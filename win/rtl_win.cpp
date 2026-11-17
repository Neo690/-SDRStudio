// SDRStudio - Windows RTL-SDR backend (dynamically loads librtlsdr.dll).
// The app runs without the DLL: hardware is simply unavailable.
#include "../core/source.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace sdr {
namespace {

struct rtlsdr_dev;
using rtlsdr_dev_t = struct rtlsdr_dev;

struct RtlApi {
    HMODULE mod = nullptr;
    int (*open)(rtlsdr_dev_t**, uint32_t) = nullptr;
    int (*close)(rtlsdr_dev_t*) = nullptr;
    int (*setSampleRate)(rtlsdr_dev_t*, uint32_t) = nullptr;
    uint32_t (*getSampleRate)(rtlsdr_dev_t*) = nullptr;
    int (*setCenterFreq)(rtlsdr_dev_t*, uint32_t) = nullptr;
    uint32_t (*getCenterFreq)(rtlsdr_dev_t*) = nullptr;
    int (*setTunerGainMode)(rtlsdr_dev_t*, int) = nullptr;
    int (*setTunerGain)(rtlsdr_dev_t*, int) = nullptr;
    int (*getTunerGains)(rtlsdr_dev_t*, int*) = nullptr;
    int (*setFreqCorrection)(rtlsdr_dev_t*, int) = nullptr;
    int (*resetBuffer)(rtlsdr_dev_t*) = nullptr;
    int (*readSync)(rtlsdr_dev_t*, void*, int, int*) = nullptr;
    uint32_t (*getDeviceCount)() = nullptr;
    const char* (*getDeviceName)(uint32_t) = nullptr;
    int (*setBiasTee)(rtlsdr_dev_t*, int) = nullptr;
    int (*getTunerType)(rtlsdr_dev_t*) = nullptr;

    bool load(std::string& err) {
        if (mod) return true;
        std::vector<std::string> candidates;

        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        const size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir.resize(slash + 1);

        for (const char* name : {"rtlsdr.dll", "librtlsdr.dll"}) {
            candidates.push_back(dir + name);
            candidates.push_back(dir + "vendor\\" + name);
        }
        candidates.push_back("rtlsdr.dll");
        candidates.push_back("librtlsdr.dll");

        for (const auto& path : candidates) {
            mod = LoadLibraryA(path.c_str());
            if (mod) break;
        }
        if (!mod) {
            err = "librtlsdr.dll introuvable (placez-la a cote de l'exe)";
            return false;
        }

        auto bind = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(mod, name));
            return fn != nullptr;
        };
        bool ok = true;
        ok &= bind(open, "rtlsdr_open");
        ok &= bind(close, "rtlsdr_close");
        ok &= bind(setSampleRate, "rtlsdr_set_sample_rate");
        ok &= bind(getSampleRate, "rtlsdr_get_sample_rate");
        ok &= bind(setCenterFreq, "rtlsdr_set_center_freq");
        ok &= bind(getCenterFreq, "rtlsdr_get_center_freq");
        ok &= bind(setTunerGainMode, "rtlsdr_set_tuner_gain_mode");
        ok &= bind(setTunerGain, "rtlsdr_set_tuner_gain");
        ok &= bind(getTunerGains, "rtlsdr_get_tuner_gains");
        ok &= bind(setFreqCorrection, "rtlsdr_set_freq_correction");
        ok &= bind(resetBuffer, "rtlsdr_reset_buffer");
        ok &= bind(readSync, "rtlsdr_read_sync");
        // Optional symbols (present in most builds, not required).
        bind(getDeviceCount, "rtlsdr_get_device_count");
        bind(getDeviceName, "rtlsdr_get_device_name");
        bind(setBiasTee, "rtlsdr_set_bias_tee_gpio");
        bind(getTunerType, "rtlsdr_get_tuner_type");

        if (!ok) {
            err = "librtlsdr.dll chargee mais incomplete";
            FreeLibrary(mod);
            mod = nullptr;
            return false;
        }
        return true;
    }
};

RtlApi& api() {
    static RtlApi instance;
    return instance;
}

class RtlSource : public IqSource {
public:
    explicit RtlSource(const SourceRequest& req) : req_(req) {}
    ~RtlSource() override { close(); }

    std::string open(std::string& err) override {
        if (!api().load(err)) return std::string();
        const uint32_t count = api().getDeviceCount ? api().getDeviceCount() : 1u;
        if (count == 0) {
            err = "aucune cle RTL-SDR detectee (pilote WinUSB installe ?)";
            return std::string();
        }
        if (api().open(&dev_, static_cast<uint32_t>(std::max(0, req_.index))) != 0 || !dev_) {
            err = "ouverture de la cle impossible (pilote WinUSB / Zadig ?)";
            return std::string();
        }
        info_.hardware = true;
        info_.driver = "librtlsdr";
        const char* name = api().getDeviceName ? api().getDeviceName(static_cast<uint32_t>(std::max(0, req_.index))) : nullptr;
        info_.name = name ? name : "RTL-SDR";
        open_ = true;

        std::string ignored;
        setSampleRate(req_.sampleRate > 0 ? req_.sampleRate : 2.4e6, ignored);
        setCenterHz(101.5e6, ignored);
        setGainDb(-1.0, ignored);  // auto
        setPpm(0, ignored);
        api().resetBuffer(dev_);

        if (api().getTunerType) {
            switch (api().getTunerType(dev_)) {
                case 5: info_.name += " (R820T)"; break;
                case 6: info_.name += " (R820T2)"; break;
                default: break;
            }
        }
        return info_.name;
    }

    void close() override {
        if (dev_ && api().close) api().close(dev_);
        dev_ = nullptr;
        open_ = false;
    }

    size_t read(cfloat* dst, size_t maxSamples, std::string& err) override {
        if (!dev_) return 0;
        // librtlsdr reads in 512-byte blocks: request a multiple of 512 bytes.
        size_t samples = maxSamples & ~static_cast<size_t>(255);
        if (samples == 0) samples = 256;
        raw_.resize(samples * 2);
        int nread = 0;
        const int rc = api().readSync(dev_, raw_.data(), static_cast<int>(raw_.size()), &nread);
        if (rc != 0 || nread <= 0) {
            err = "lecture USB interrompue (cle debranchee ?)";
            return 0;
        }
        const size_t n = static_cast<size_t>(nread) / 2;
        for (size_t i = 0; i < n; ++i) {
            dst[i] = cfloat((static_cast<float>(raw_[i * 2]) - 127.4f) / 127.4f,
                            (static_cast<float>(raw_[i * 2 + 1]) - 127.4f) / 127.4f);
        }
        return n;
    }

    bool setCenterHz(double hz, std::string& err) override {
        if (!dev_) {
            err = "cle non ouverte";
            return false;
        }
        const uint32_t f = static_cast<uint32_t>(hz);
        if (api().setCenterFreq(dev_, f) != 0) {
            err = "frequence refusee par la cle";
            return false;
        }
        info_.centerHz = api().getCenterFreq ? api().getCenterFreq(dev_) : f;
        return true;
    }

    bool setSampleRate(double rate, std::string& err) override {
        if (!dev_) {
            err = "cle non ouverte";
            return false;
        }
        if (api().setSampleRate(dev_, static_cast<uint32_t>(rate)) != 0) {
            err = "debit d'echantillonnage refuse";
            return false;
        }
        info_.sampleRate = api().getSampleRate ? api().getSampleRate(dev_) : rate;
        return true;
    }

    bool setGainDb(double db, std::string& err) override {
        if (!dev_) {
            err = "cle non ouverte";
            return false;
        }
        if (db < 0.0) {
            if (api().setTunerGainMode(dev_, 1) != 0) {
                err = "mode gain auto refuse";
                return false;
            }
            info_.gainDb = -1.0;
            return true;
        }
        const std::vector<double> steps = gainsDb();
        double nearest = db;
        if (!steps.empty()) {
            nearest = steps.front();
            for (double g : steps)
                if (std::fabs(g - db) < std::fabs(nearest - db)) nearest = g;
        }
        if (api().setTunerGainMode(dev_, 0) != 0 || api().setTunerGain(dev_, static_cast<int>(nearest * 10.0)) != 0) {
            err = "reglage de gain refuse";
            return false;
        }
        info_.gainDb = nearest;
        return true;
    }

    bool setPpm(int ppm, std::string& err) override {
        if (!dev_) {
            err = "cle non ouverte";
            return false;
        }
        if (api().setFreqCorrection(dev_, ppm) != 0) {
            err = "correction ppm refusee";
            return false;
        }
        return true;
    }

    bool setBiasTee(bool on, std::string& err) override {
        if (!api().setBiasTee) {
            err = "bias tee non disponible dans cette librtlsdr";
            return false;
        }
        if (api().setBiasTee(dev_, on ? 1 : 0) != 0) {
            err = "bias tee refuse";
            return false;
        }
        return true;
    }

    const SourceInfo& info() const override { return info_; }

    std::vector<double> gainsDb() const override {
        std::vector<double> out;
        if (!dev_ || !api().getTunerGains) return out;
        int raw[64] = {0};
        const int n = api().getTunerGains(dev_, raw);
        for (int i = 0; i < n && i < 64; ++i) out.push_back(raw[i] / 10.0);
        return out;
    }

private:
    SourceRequest req_;
    SourceInfo info_;
    rtlsdr_dev_t* dev_ = nullptr;
    bool open_ = false;
    std::vector<uint8_t> raw_;
};

} // namespace

std::unique_ptr<IqSource> createRtlSource(const SourceRequest& req, std::string& err) {
    (void)err;
    return std::make_unique<RtlSource>(req);
}

bool rtlBackendProbe(std::string& detail) {
    std::string err;
    if (!api().load(err)) {
        detail = err;
        return false;
    }
    const uint32_t count = api().getDeviceCount ? api().getDeviceCount() : 0u;
    detail = std::to_string(count);
    if (count == 0) {
        detail += " peripherique(s) RTL-SDR, aucun detecte";
        return false;
    }
    detail = std::to_string(count) + " peripherique(s): ";
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = api().getDeviceName ? api().getDeviceName(i) : "RTL-SDR";
        detail += std::string(name ? name : "RTL-SDR");
        if (i + 1 < count) detail += ", ";
    }
    return true;
}

} // namespace sdr
