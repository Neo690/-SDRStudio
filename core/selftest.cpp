// SDRStudio - self-test: demodulation accuracy + throughput measurement.
#include "selftest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp.h"
#include "fft.h"
#include "ring.h"

namespace sdr {
namespace {

constexpr double kFs = 2.4e6;
constexpr double kAudioFs = 48000.0;

void genFm(std::vector<cfloat>& out, size_t n, double offset, double deviation, double tone, double amp) {
    out.resize(n);
    double phase = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double audio = std::sin(2.0 * kPi * tone * t);
        phase += 2.0 * kPi * (offset + deviation * audio) / kFs;
        out[i] = cfloat(static_cast<float>(amp * std::cos(phase)), static_cast<float>(amp * std::sin(phase)));
    }
}

void genAm(std::vector<cfloat>& out, size_t n, double offset, double depth, double tone, double amp) {
    out.resize(n);
    double phase = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double audio = 1.0 + depth * std::sin(2.0 * kPi * tone * t);
        phase += 2.0 * kPi * offset / kFs;
        out[i] = cfloat(static_cast<float>(amp * audio * std::cos(phase)),
                        static_cast<float>(amp * audio * std::sin(phase)));
    }
}

double rms(const std::vector<float>& x) {
    if (x.empty()) return 0.0;
    double acc = 0.0;
    for (float v : x) acc += static_cast<double>(v) * v;
    return std::sqrt(acc / static_cast<double>(x.size()));
}

// Frequency estimate from positive-going zero crossings.
double estimateToneHz(const std::vector<float>& x, double fs) {
    if (x.size() < 4) return 0.0;
    double mean = 0.0;
    for (float v : x) mean += v;
    mean /= static_cast<double>(x.size());
    int crossings = 0;
    float prev = static_cast<float>(x[0] - mean);
    for (size_t i = 1; i < x.size(); ++i) {
        const float v = static_cast<float>(x[i] - mean);
        if (prev < 0.0f && v >= 0.0f) ++crossings;
        prev = v;
    }
    return static_cast<double>(crossings) / (static_cast<double>(x.size()) / fs);
}

std::vector<float> runChain(const std::vector<cfloat>& iq, Mode mode, double offset) {
    Receiver rx(kFs);
    rx.setMode(mode);
    rx.setDeemphasisMicros(75);
    rx.setAgcEnabled(true);
    rx.setVolume(1.0f);
    rx.setSquelchEnabled(false);
    rx.setOffsetHz(offset);
    std::vector<float> audio;
    audio.reserve(iq.size() / 12);
    const size_t block = 8192;
    std::vector<float> chunk;
    for (size_t pos = 0; pos < iq.size(); pos += block) {
        const size_t n = std::min(block, iq.size() - pos);
        chunk.clear();
        rx.process(iq.data() + pos, n, chunk);
        audio.insert(audio.end(), chunk.begin(), chunk.end());
    }
    // Drop 150 ms of filter/AGC settling.
    const size_t skip = std::min(audio.size(), static_cast<size_t>(0.15 * kAudioFs));
    return std::vector<float>(audio.begin() + static_cast<ptrdiff_t>(skip), audio.end());
}

int failures = 0;

void check(bool ok, const char* label, const char* detail) {
    std::printf("  [%s] %-22s %s\n", ok ? " OK " : "FAIL", label, detail);
    if (!ok) ++failures;
}

// Multiplex stereo complet : (L+R) sous 15 kHz, pilote 19 kHz, (L-R) sur 38 kHz.
void genFmStereo(std::vector<cfloat>& out, size_t n, double offset, double leftTone,
                 double rightTone, bool pilot) {
    out.resize(n);
    double phase = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double left = leftTone > 0.0 ? 0.4 * std::sin(2.0 * kPi * leftTone * t) : 0.0;
        const double right = rightTone > 0.0 ? 0.4 * std::sin(2.0 * kPi * rightTone * t) : 0.0;
        const double sum = 0.5 * (left + right);
        const double diff = 0.5 * (left - right);
        double mpx = sum + diff * std::sin(2.0 * kPi * 38000.0 * t);
        if (pilot) mpx += 0.09 * std::sin(2.0 * kPi * 19000.0 * t);
        phase += 2.0 * kPi * (offset + 75000.0 * mpx) / kFs;
        out[i] = cfloat(static_cast<float>(0.5 * std::cos(phase)),
                        static_cast<float>(0.5 * std::sin(phase)));
    }
}

void runStereoChain(const std::vector<cfloat>& iq, bool stereo, std::vector<float>& left,
                    std::vector<float>& right, bool& locked) {
    Receiver rx(kFs);
    rx.setMode(Mode::Wbfm);
    rx.setDeemphasisMicros(75);
    rx.setAgcEnabled(false); // gain unitaire : la mesure porte sur la separation seule
    rx.setVolume(1.0f);
    rx.setSquelchEnabled(false);
    rx.setOffsetHz(0.0);
    rx.setStereoEnabled(stereo);

    std::vector<float> interleaved;
    std::vector<float> chunk;
    const size_t block = 8192;
    for (size_t pos = 0; pos < iq.size(); pos += block) {
        const size_t n = std::min(block, iq.size() - pos);
        chunk.clear();
        rx.processStereo(iq.data() + pos, n, chunk);
        interleaved.insert(interleaved.end(), chunk.begin(), chunk.end());
    }
    locked = rx.stereoLocked();

    left.clear();
    right.clear();
    const size_t frames = interleaved.size() / 2;
    const size_t skip = std::min(frames, static_cast<size_t>(0.15 * kAudioFs));
    for (size_t f = skip; f < frames; ++f) {
        left.push_back(interleaved[f * 2]);
        right.push_back(interleaved[f * 2 + 1]);
    }
}

double separationDb(const std::vector<float>& left, const std::vector<float>& right) {
    const double l = rms(left);
    const double r = rms(right);
    const double lo = std::min(l, r);
    if (std::max(l, r) <= 0.0 || lo <= 0.0) return 0.0;
    return 20.0 * std::log10(std::max(l, r) / lo);
}

double maxChannelDiff(const std::vector<float>& left, const std::vector<float>& right) {
    double worst = 0.0;
    for (size_t i = 0; i < std::min(left.size(), right.size()); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(left[i] - right[i])));
    return worst;
}

// Separation = ecart entre le canal qui porte la tonalite et l'autre.
void testStereo() {
    std::printf("Stereo FM (pilote 19 kHz)\n");
    const size_t n = static_cast<size_t>(kFs * 0.6);
    std::vector<cfloat> iq;
    std::vector<float> left, right;
    bool locked = false;
    char detail[192];

    // 1) Tonalite 1 kHz a gauche seulement.
    genFmStereo(iq, n, 0.0, 1000.0, 0.0, true);
    runStereoChain(iq, true, left, right, locked);
    const double sepL = separationDb(left, right);
    std::snprintf(detail, sizeof(detail), "L isolee : separation %.1f dB, verrou %s, rms L %.3f",
                  sepL, locked ? "oui" : "non", rms(left));
    check(locked && rms(left) > rms(right) && sepL >= 25.0, "separation L (1 kHz)", detail);

    // 2) Tonalite 1,2 kHz a droite seulement : detecte une inversion L/R.
    genFmStereo(iq, n, 0.0, 0.0, 1200.0, true);
    runStereoChain(iq, true, left, right, locked);
    const double sepR = separationDb(left, right);
    std::snprintf(detail, sizeof(detail), "R isolee : separation %.1f dB, rms R %.3f", sepR, rms(right));
    check(rms(right) > rms(left) && sepR >= 25.0, "separation R (1,2 kHz)", detail);

    // 3) Sans pilote : repli mono, les deux canaux doivent etre identiques.
    genFmStereo(iq, n, 0.0, 1000.0, 0.0, false);
    runStereoChain(iq, true, left, right, locked);
    const double diffNoPilot = maxChannelDiff(left, right);
    std::snprintf(detail, sizeof(detail), "sans pilote : ecart L/R max %.1e, verrou %s",
                  diffNoPilot, locked ? "oui" : "non");
    check(!locked && diffNoPilot < 1e-6, "repli mono sans pilote", detail);

    // 4) Pilote present mais stereo desactive : sortie mono aussi.
    genFmStereo(iq, n, 0.0, 1000.0, 0.0, true);
    runStereoChain(iq, false, left, right, locked);
    const double diffOff = maxChannelDiff(left, right);
    std::snprintf(detail, sizeof(detail), "stereo desactive : ecart L/R max %.1e", diffOff);
    check(diffOff < 1e-6 && rms(left) > 0.02, "mono si stereo desactive", detail);

    // 5) process() reste mono meme quand le stereo est arme.
    {
        Receiver rx(kFs);
        rx.setMode(Mode::Wbfm);
        rx.setAgcEnabled(false);
        rx.setSquelchEnabled(false);
        rx.setStereoEnabled(true);
        genFmStereo(iq, n, 0.0, 1000.0, 0.0, true);
        std::vector<float> monoOut;
        std::vector<float> chunk;
        for (size_t pos = 0; pos < iq.size(); pos += 8192) {
            const size_t len = std::min<size_t>(8192, iq.size() - pos);
            chunk.clear();
            rx.process(iq.data() + pos, len, chunk);
            monoOut.insert(monoOut.end(), chunk.begin(), chunk.end());
        }
        const double level = rms(monoOut);
        std::snprintf(detail, sizeof(detail), "%.1f k ech., rms %.3f (stereo arme)",
                      monoOut.size() / 1000.0, level);
        check(level > 0.02, "sortie mono inchangee", detail);
    }
}

void testFft() {
    std::printf("FFT\n");
    const int n = 4096;
    Fft fft(n);
    std::vector<cfloat> buf(static_cast<size_t>(n));
    const int bin = 317;
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * kPi * bin * i / n;
        buf[static_cast<size_t>(i)] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    fft.forward(buf.data());
    int best = 0;
    float bestMag = -1.0f;
    for (int i = 0; i < n; ++i) {
        const float m = std::abs(buf[static_cast<size_t>(i)]);
        if (m > bestMag) {
            bestMag = m;
            best = i;
        }
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail), "pic au bin %d (attendu %d)", best, bin);
    check(best == bin, "resolution spectrale", detail);
}

void testDemod(Mode mode, const char* label, bool am, double offset, double deviation, double tone, double tolHz) {
    const size_t n = static_cast<size_t>(kFs * 0.5); // 500 ms
    std::vector<cfloat> iq;
    if (am) genAm(iq, n, offset, 0.6, tone, 0.5);
    else genFm(iq, n, offset, deviation, tone, 0.5);

    const std::vector<float> audio = runChain(iq, mode, offset);
    const double freq = estimateToneHz(audio, kAudioFs);
    const double level = rms(audio);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "tone %.1f Hz (attendu %.0f), rms %.3f", freq, tone, level);
    const bool okFreq = std::fabs(freq - tone) <= tolHz;
    const bool okLevel = level > 0.02;
    check(okFreq && okLevel, label, detail);
}

// Verifie que le resultat ne depend pas du decoupage en blocs : toute erreur de
// phase (NCO, etat du filtre, discriminateur) ferait diverger les deux sorties.
void testBlockInvariance() {
    std::printf("Coherence des calculs\n");

    Receiver probe(kFs);
    char rates[192];
    std::snprintf(rates, sizeof(rates), "canal %.0f Hz -> audio %.0f Hz (blocs de 8192)",
                  probe.channelRate(), probe.audioRate());
    check(std::fabs(probe.channelRate() - 240000.0) < 1.0 &&
              std::fabs(probe.audioRate() - 48000.0) < 1.0,
          "calcul de decimation", rates);

    const size_t n = static_cast<size_t>(kFs * 0.4);
    std::vector<cfloat> iq;
    genFm(iq, n, 250000.0, 75000.0, 1200.0, 0.5);

    auto run = [&](size_t block) {
        Receiver rx(kFs);
        rx.setMode(Mode::Wbfm);
        rx.setDeemphasisMicros(75);
        rx.setAgcEnabled(true);
        rx.setVolume(1.0f);
        rx.setSquelchEnabled(false);
        rx.setOffsetHz(250000.0);
        std::vector<float> out;
        std::vector<float> chunk;
        for (size_t pos = 0; pos < iq.size(); pos += block) {
            const size_t len = std::min(block, iq.size() - pos);
            chunk.clear();
            rx.process(iq.data() + pos, len, chunk);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    };

    const std::vector<float> whole = run(8192);
    const std::vector<float> split = run(997);
    const size_t common = std::min(whole.size(), split.size());
    double maxDiff = 0.0;
    for (size_t i = 0; i < common; ++i)
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(whole[i] - split[i])));

    char detail[192];
    std::snprintf(detail, sizeof(detail), "blocs 8192 vs 997 : ecart max %.2e sur %zu ech.", maxDiff, common);
    check(maxDiff < 1e-6 && common > static_cast<size_t>(kAudioFs * 0.3), "invariance de phase", detail);
}

void testThroughput() {
    std::printf("Debit (benchmark sur ce CPU)\n");
    const size_t n = static_cast<size_t>(kFs * 2.0); // 2 s of signal
    std::vector<cfloat> iq(n);
    // Two FM stations plus noise, so the pipeline sees realistic data.
    double ph1 = 0.0, ph2 = 0.0;
    uint32_t rng = 0x9E3779B9u;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double a1 = std::sin(2.0 * kPi * 700.0 * t);
        const double a2 = std::sin(2.0 * kPi * 1300.0 * t);
        ph1 += 2.0 * kPi * (240000.0 + 75000.0 * a1) / kFs;
        ph2 += 2.0 * kPi * (-610000.0 + 40000.0 * a2) / kFs;
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const float noise = (static_cast<float>(rng & 0xFFFF) / 32768.0f - 1.0f) * 0.01f;
        iq[i] = cfloat(static_cast<float>(0.3 * std::cos(ph1) + 0.1 * std::cos(ph2)) + noise,
                       static_cast<float>(0.3 * std::sin(ph1) + 0.1 * std::sin(ph2)) + noise);
    }

    Receiver rx(kFs);
    rx.setMode(Mode::Wbfm);
    rx.setDeemphasisMicros(75);
    rx.setOffsetHz(240000.0);
    SpectrumAnalyzer spec(4096, kFs);
    std::vector<float> audio;
    audio.reserve(n / 12);
    std::vector<float> chunk;
    const size_t block = 8192;

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < n; pos += block) {
        const size_t len = std::min(block, n - pos);
        spec.process(iq.data() + pos, len);
        chunk.clear();
        rx.process(iq.data() + pos, len, chunk);
        audio.insert(audio.end(), chunk.begin(), chunk.end());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    const double msps = static_cast<double>(n) / seconds / 1e6;
    const double realtime = msps / 2.4;

    char detail[192];
    std::snprintf(detail, sizeof(detail), "%.1f MS/s (x%.1f temps reel a 2,4 MS/s), %llu trames FFT",
                  msps, realtime, static_cast<unsigned long long>(spec.frames()));
    // 2.4 MS/s must be processed with a wide margin, otherwise audio would drop out.
    check(realtime >= 1.5, "debit chaine complete", detail);
    std::printf("  audio produit: %.1f s pour 2.0 s de signal\n",
                static_cast<double>(audio.size()) / kAudioFs);
}

} // namespace

bool runSelfTest() {
    failures = 0;
    std::printf("=== SDRStudio - auto-test du coeur DSP ===\n");
    testFft();
    std::printf("Demodulation\n");
    testDemod(Mode::Wbfm, "WBFM 1 kHz", false, 0.0, 75000.0, 1000.0, 60.0);
    testDemod(Mode::Wbfm, "WBFM 1,5 kHz decale", false, 400000.0, 75000.0, 1500.0, 60.0);
    testDemod(Mode::Nbfm, "NBFM 800 Hz", false, -200000.0, 2500.0, 800.0, 40.0);
    testDemod(Mode::Am, "AM 1,2 kHz", true, 300000.0, 0.0, 1200.0, 40.0);
    testStereo();
    testBlockInvariance();
    testThroughput();
    std::printf("=== %s (%d echec(s)) ===\n", failures == 0 ? "TOUT EST OK" : "ECHECS", failures);
    return failures == 0;
}

} // namespace sdr
