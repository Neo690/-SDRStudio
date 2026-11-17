// SDRStudio - pont JNI : meme coeur DSP que la version Windows.
// Deux sources : cle RTL-SDR (descripteur fourni par UsbManager) ou generateur interne.
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "recorder.h"
#include "ring.h"
#include "rtl_android.h"
#include "source.h"

namespace {

constexpr size_t kBlock = 8192;

struct Engine {
    std::unique_ptr<sdr::IqSource> source;
    std::unique_ptr<sdr::Receiver> receiver;
    std::unique_ptr<sdr::SpectrumAnalyzer> spectrum;
    std::thread worker;
    std::atomic<bool> running{false};

    // Le worker demodule : les changements de reglage passent par ce verrou.
    std::mutex control;
    // Bins en dB lus par le thread UI.
    std::mutex spectrumLock;
    std::vector<float> binsDb;
    // Thread DSP -> thread audio.
    sdr::SpscRing<float> audio{1 << 15};

    // Enregistreurs : ecrits par le worker, ouverts/fermes par le thread Java.
    std::mutex recordLock;
    sdr::WavWriter wav;
    sdr::IqWriter iq;
};

Engine& engine() {
    static Engine instance;
    return instance;
}

void workerLoop() {
    Engine& e = engine();
    std::vector<sdr::cfloat> block(kBlock);
    std::vector<float> audio;
    std::string error;

    while (e.running.load(std::memory_order_relaxed)) {
        const size_t n = e.source->read(block.data(), block.size(), error);
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // IQ brut tel que recu (rejouable avec --file).
        {
            std::lock_guard<std::mutex> lock(e.recordLock);
            if (e.iq.isOpen()) e.iq.write(block.data(), n);
        }

        e.spectrum->process(block.data(), n);

        audio.clear();
        {
            std::lock_guard<std::mutex> lock(e.control);
            e.receiver->process(block.data(), n, audio);
        }

        // Audio demodule, avant la mise en file pour AudioTrack.
        {
            std::lock_guard<std::mutex> lock(e.recordLock);
            if (e.wav.isOpen() && !audio.empty()) e.wav.write(audio.data(), audio.size());
        }

        for (size_t offset = 0; offset < audio.size();) {
            const size_t written = e.audio.write(audio.data() + offset, audio.size() - offset);
            if (written == 0) break; // thread audio en retard : on abandonne le reste
            offset += written;
        }

        {
            std::lock_guard<std::mutex> lock(e.spectrumLock);
            e.binsDb = e.spectrum->binsDb();
        }
    }
}

// Demarre la chaine de reception sur la source demandee ("rtl" = materiel seul,
// "auto" = materiel si un descripteur attend, sinon generateur interne).
bool startEngine(const char* kind, const std::string& path = std::string(),
                 double captureRate = 2.4e6) {
    Engine& e = engine();
    if (e.running.load()) return true;

    sdr::SourceRequest request;
    request.kind = kind;
    request.path = path;
    request.sampleRate = captureRate;

    std::string error;
    e.source = sdr::createSource(request, error);
    if (!e.source) return false;
    const double rate = e.source->info().sampleRate;
    if (e.source->open(error).empty()) return false;

    e.receiver = std::make_unique<sdr::Receiver>(rate);
    e.receiver->setMode(sdr::Mode::Wbfm);
    e.receiver->setDeemphasisMicros(75);
    e.receiver->setVolume(0.7f);

    e.spectrum = std::make_unique<sdr::SpectrumAnalyzer>(4096, rate);
    {
        std::lock_guard<std::mutex> lock(e.spectrumLock);
        e.binsDb.assign(static_cast<size_t>(e.spectrum->size()), -120.0f);
    }
    e.audio.clear();

    e.running.store(true);
    e.worker = std::thread(workerLoop);
    return true;
}

std::string jstringToStd(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* utf = env->GetStringUTFChars(s, nullptr);
    std::string out(utf ? utf : "");
    if (utf) env->ReleaseStringUTFChars(s, utf);
    return out;
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeStart(JNIEnv*, jobject) {
    return startEngine("auto") ? JNI_TRUE : JNI_FALSE;
}

// A appeler apres autorisation UsbManager : le descripteur est consomme par
// createRtlSource() et appartient ensuite a libusb.
JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeStartUsb(JNIEnv*, jobject, jint fd) {
    sdr::setPendingUsbFd(static_cast<int>(fd));
    return startEngine("rtl") ? JNI_TRUE : JNI_FALSE;
}

// Rejeu d'un enregistrement IQ (.cu8). Le fichier est relu en boucle ; le
// deplacement dans la bande se fait avec nativeSetOffsetHz().
JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeStartFile(JNIEnv* env, jobject, jstring path, jdouble rate) {
    const std::string file = jstringToStd(env, path);
    if (file.empty()) return JNI_FALSE;
    const double r = (rate > 1e5) ? static_cast<double>(rate) : 2.4e6;
    return startEngine("file", file, r) ? JNI_TRUE : JNI_FALSE;
}

// Decalage du canal dans la bande capturee (meme reglage que la version Windows).
JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetOffsetHz(JNIEnv*, jobject, jdouble hz) {
    Engine& e = engine();
    if (!e.receiver) return;
    std::lock_guard<std::mutex> lock(e.control);
    e.receiver->setOffsetHz(static_cast<double>(hz));
}

JNIEXPORT jdouble JNICALL
Java_com_sdrstudio_SdrEngine_nativeOffsetHz(JNIEnv*, jobject) {
    Engine& e = engine();
    return e.receiver ? e.receiver->offsetHz() : 0.0;
}

// |offset| maximal : le canal entier doit rester dans la bande enregistree.
JNIEXPORT jdouble JNICALL
Java_com_sdrstudio_SdrEngine_nativeMaxOffsetHz(JNIEnv*, jobject) {
    Engine& e = engine();
    return e.receiver ? e.receiver->maxOffsetHz() : 0.0;
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeStop(JNIEnv*, jobject) {
    Engine& e = engine();
    e.running.store(false);
    if (e.worker.joinable()) e.worker.join();
    // Ferme les enregistrements avant de lacher la source (l'en-tete WAV doit
    // etre ecrit meme si l'app est arretee en pleine capture).
    {
        std::lock_guard<std::mutex> lock(e.recordLock);
        e.wav.close();
        e.iq.close();
    }
    // Idempotent : le service de premier plan et l'activite peuvent demander
    // l'arret tous les deux (bouton Arreter de la notification, changement de
    // source). On lache les pointeurs pour qu'un second appel ne referme pas un
    // descripteur USB deja rendu a libusb.
    if (e.source) {
        e.source->close();
        e.source.reset();
    }
    e.receiver.reset();
    e.audio.clear();
}

JNIEXPORT jstring JNICALL
Java_com_sdrstudio_SdrEngine_nativeSourceName(JNIEnv* env, jobject) {
    Engine& e = engine();
    const std::string name = e.source ? e.source->info().name : std::string("aucune");
    return env->NewStringUTF(name.c_str());
}

JNIEXPORT jint JNICALL
Java_com_sdrstudio_SdrEngine_nativeGetSpectrum(JNIEnv* env, jobject, jfloatArray out) {
    Engine& e = engine();
    const jsize length = env->GetArrayLength(out);
    if (length <= 0) return 0;
    std::lock_guard<std::mutex> lock(e.spectrumLock);
    const jsize count = std::min<jsize>(length, static_cast<jsize>(e.binsDb.size()));
    if (count > 0) env->SetFloatArrayRegion(out, 0, count, e.binsDb.data());
    return count;
}

JNIEXPORT jint JNICALL
Java_com_sdrstudio_SdrEngine_nativeReadAudio(JNIEnv* env, jobject, jfloatArray out) {
    const jsize length = env->GetArrayLength(out);
    if (length <= 0) return 0;
    jfloat* buffer = env->GetFloatArrayElements(out, nullptr);
    if (!buffer) return 0;
    const size_t got = engine().audio.read(buffer, static_cast<size_t>(length));
    env->ReleaseFloatArrayElements(out, buffer, 0);
    return static_cast<jint>(got);
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetMode(JNIEnv*, jobject, jint mode) {
    Engine& e = engine();
    if (!e.receiver) return;
    std::lock_guard<std::mutex> lock(e.control);
    e.receiver->setMode(static_cast<sdr::Mode>(mode));
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetVolume(JNIEnv*, jobject, jfloat volume) {
    Engine& e = engine();
    if (!e.receiver) return;
    std::lock_guard<std::mutex> lock(e.control);
    e.receiver->setVolume(volume);
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetFrequency(JNIEnv*, jobject, jdouble hz) {
    Engine& e = engine();
    if (!e.source) return;
    std::string error;
    e.source->setCenterHz(static_cast<double>(hz), error);
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetGain(JNIEnv*, jobject, jdouble db) {
    Engine& e = engine();
    if (!e.source) return;
    std::string error;
    e.source->setGainDb(static_cast<double>(db), error); // < 0 => automatique
}

JNIEXPORT jfloatArray JNICALL
Java_com_sdrstudio_SdrEngine_nativeGains(JNIEnv* env, jobject) {
    Engine& e = engine();
    std::vector<double> gains;
    if (e.source) gains = e.source->gainsDb();
    jfloatArray out = env->NewFloatArray(static_cast<jsize>(gains.size()));
    if (!out) return nullptr;
    std::vector<float> flat(gains.begin(), gains.end());
    if (!flat.empty()) env->SetFloatArrayRegion(out, 0, static_cast<jsize>(flat.size()), flat.data());
    return out;
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetPpm(JNIEnv*, jobject, jint ppm) {
    Engine& e = engine();
    if (!e.source) return;
    std::string error;
    e.source->setPpm(static_cast<int>(ppm), error);
}

// Signal au-dessus du seuil de squelch : critere d'arret du scan.
JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeSquelchOpen(JNIEnv*, jobject) {
    Engine& e = engine();
    if (!e.receiver) return JNI_FALSE;
    return e.receiver->squelchOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeSetSquelch(JNIEnv*, jobject, jboolean on, jfloat db) {
    Engine& e = engine();
    if (!e.receiver) return;
    std::lock_guard<std::mutex> lock(e.control);
    e.receiver->setSquelchDb(static_cast<double>(db));
    e.receiver->setSquelchEnabled(on == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeStartWav(JNIEnv* env, jobject, jstring path) {
    Engine& e = engine();
    if (!e.receiver) return JNI_FALSE;
    const std::string file = jstringToStd(env, path);
    if (file.empty()) return JNI_FALSE;
    std::string error;
    std::lock_guard<std::mutex> lock(e.recordLock);
    const bool ok = e.wav.open(file, static_cast<int>(e.receiver->audioRate()), 1, error);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeStopWav(JNIEnv*, jobject) {
    Engine& e = engine();
    std::lock_guard<std::mutex> lock(e.recordLock);
    e.wav.close();
}

// Filet de securite : le worker ecrit dans le fichier, ce getter permet a l'UI
// de savoir si l'enregistrement tourne encore (arret depuis nativeStop()).
JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeIsRecordingWav(JNIEnv*, jobject) {
    Engine& e = engine();
    std::lock_guard<std::mutex> lock(e.recordLock);
    return e.wav.isOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeStartIq(JNIEnv* env, jobject, jstring path) {
    Engine& e = engine();
    if (!e.source) return JNI_FALSE;
    const std::string file = jstringToStd(env, path);
    if (file.empty()) return JNI_FALSE;
    std::string error;
    std::lock_guard<std::mutex> lock(e.recordLock);
    const bool ok = e.iq.open(file, error);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_sdrstudio_SdrEngine_nativeStopIq(JNIEnv*, jobject) {
    Engine& e = engine();
    std::lock_guard<std::mutex> lock(e.recordLock);
    e.iq.close();
}

JNIEXPORT jboolean JNICALL
Java_com_sdrstudio_SdrEngine_nativeIsRecordingIq(JNIEnv*, jobject) {
    Engine& e = engine();
    std::lock_guard<std::mutex> lock(e.recordLock);
    return e.iq.isOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jdouble JNICALL
Java_com_sdrstudio_SdrEngine_nativeChannelDb(JNIEnv*, jobject) {
    Engine& e = engine();
    if (!e.receiver) return -200.0;
    return e.receiver->channelPowerDb();
}

} // extern "C"
