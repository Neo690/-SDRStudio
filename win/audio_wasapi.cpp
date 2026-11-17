// SDRStudio - WASAPI renderer: 20 ms shared mode buffer, event driven.
#include "audio_wasapi.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// GUIDs are declared locally so the build never depends on uuid libraries.
const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const GUID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

} // namespace

struct AudioOutput::Impl {
    sdr::SpscRing<float>* ring = new sdr::SpscRing<float>(1 << 17);
    HANDLE thread = nullptr;
    HANDLE event = nullptr;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> ready{false};
    std::atomic<unsigned> underruns{0};
    std::atomic<unsigned> bufferFrames{0};
    std::string error;

    static DWORD WINAPI entry(LPVOID self) {
        static_cast<Impl*>(self)->run();
        return 0;
    }

    void run();
};

void AudioOutput::Impl::run() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;

    auto fail = [&](const char* message) {
        error = message;
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (render) render->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (SUCCEEDED(comHr)) CoUninitialize();
        ready = true;
    };

    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                reinterpret_cast<void**>(&enumerator))) || !enumerator) {
        fail("initialisation audio impossible (COM)");
        return;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
        fail("aucune sortie audio par defaut");
        return;
    }
    if (FAILED(device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) || !client) {
        fail("pilote audio indisponible");
        return;
    }
    if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
        fail("format audio du systeme illisible");
        return;
    }

    WAVEFORMATEXTENSIBLE want{};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = 2;
    want.Format.nSamplesPerSec = 48000;
    want.Format.wBitsPerSample = 32;
    want.Format.nBlockAlign = static_cast<WORD>(want.Format.nChannels * want.Format.wBitsPerSample / 8);
    want.Format.nAvgBytesPerSec = want.Format.nSamplesPerSec * want.Format.nBlockAlign;
    want.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    want.Samples.wValidBitsPerSample = 32;
    want.dwChannelMask = 0x3; // front left + front right
    want.SubFormat = kSubtypeFloat;
    CoTaskMemFree(mixFormat);
    mixFormat = nullptr;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    // 10 ms requested buffer (in 100 ns units); the shared engine may enlarge it.
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0,
                                    reinterpret_cast<WAVEFORMATEX*>(&want), nullptr);
    if (FAILED(hr)) {
        fail("ouverture du flux audio refusee");
        return;
    }

    UINT32 bufFrames = 0;
    client->GetBufferSize(&bufFrames);
    bufferFrames = bufFrames;

    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event || FAILED(client->SetEventHandle(event))) {
        fail("evenement audio impossible");
        return;
    }
    if (FAILED(client->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(&render))) || !render) {
        fail("service de rendu audio indisponible");
        return;
    }
    if (FAILED(client->Start())) {
        fail("demarrage audio refuse");
        return;
    }

    ready = true;
    std::string localError;
    std::vector<float> mono;
    while (!stopRequested.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForSingleObject(event, 200);
        if (wait != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 available = bufFrames > padding ? bufFrames - padding : 0;
        if (available == 0) continue;

        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(available, &data))) break;
        mono.resize(available);
        const size_t got = ring->read(mono.data(), available);
        float* out = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < available; ++i) {
            const float v = (i < got) ? mono[i] : 0.0f;
            out[2u * i + 0u] = v;
            out[2u * i + 1u] = v;
        }
        if (got < available) underruns.fetch_add(1, std::memory_order_relaxed);
        render->ReleaseBuffer(available, 0);
    }

    client->Stop();
    render->Release();
    client->Release();
    device->Release();
    enumerator->Release();
    if (event) CloseHandle(event);
    event = nullptr;
    if (SUCCEEDED(comHr)) CoUninitialize();
}

AudioOutput::AudioOutput() : impl_(new Impl()) {}

AudioOutput::~AudioOutput() {
    stop();
    delete impl_->ring;
    delete impl_;
    impl_ = nullptr;
}

bool AudioOutput::start(std::string& err) {
    if (impl_->thread) return true;
    impl_->stopRequested = false;
    impl_->ready = false;
    impl_->error.clear();
    impl_->thread = CreateThread(nullptr, 0, &Impl::entry, impl_, 0, nullptr);
    if (!impl_->thread) {
        err = "thread audio impossible";
        return false;
    }
    for (int i = 0; i < 300 && !impl_->ready.load(); ++i) Sleep(5);
    if (!impl_->ready.load() || !impl_->error.empty()) {
        err = impl_->error.empty() ? "audio non demarre" : impl_->error;
        stop();
        return false;
    }
    return true;
}

void AudioOutput::stop() {
    if (!impl_ || !impl_->thread) return;
    impl_->stopRequested = true;
    if (impl_->event) SetEvent(impl_->event);
    WaitForSingleObject(impl_->thread, 2000);
    CloseHandle(impl_->thread);
    impl_->thread = nullptr;
}

bool AudioOutput::running() const { return impl_ && impl_->thread != nullptr; }

sdr::SpscRing<float>& AudioOutput::ring() { return *impl_->ring; }

unsigned AudioOutput::underruns() const { return impl_->underruns.load(); }

unsigned AudioOutput::bufferFrames() const { return impl_->bufferFrames.load(); }
