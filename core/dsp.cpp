// SDRStudio - portable DSP core implementation.
#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sdr {
namespace {

constexpr double kTwoPi = 2.0 * kPi;

// Keeps a recursive rotation at unit length; limits numeric drift.
inline cfloat renormalize(cfloat v) {
    const float mag = std::sqrt(v.real() * v.real() + v.imag() * v.imag());
    if (mag < 1e-12f) return cfloat(1.0f, 0.0f);
    return v / mag;
}

} // namespace

const char* modeAbbrev(Mode m) {
    switch (m) {
        case Mode::Wbfm: return "WBFM";
        case Mode::Nbfm: return "NBFM";
        case Mode::Am:   return "AM";
        case Mode::Usb:  return "USB";
        case Mode::Lsb:  return "LSB";
        case Mode::Cw:   return "CW";
    }
    return "?";
}

double modeAudioCutoff(Mode m) {
    switch (m) {
        case Mode::Wbfm: return 15000.0;
        case Mode::Nbfm: return 3200.0;
        case Mode::Am:   return 4500.0;
        case Mode::Usb:  return 2700.0;
        case Mode::Lsb:  return 2700.0;
        case Mode::Cw:   return 600.0;
    }
    return 4000.0;
}

std::vector<float> designLowpass(double fs, double cutoffHz, int numTaps) {
    if (numTaps < 3) numTaps = 3;
    if ((numTaps & 1) == 0) ++numTaps; // odd => symmetric delay
    std::vector<float> taps(static_cast<size_t>(numTaps), 0.0f);
    const int mid = (numTaps - 1) / 2;
    const double fc = cutoffHz / fs; // cycles per sample

    double sum = 0.0;
    for (int k = 0; k < numTaps; ++k) {
        const int m = k - mid;
        const double x = 2.0 * fc * static_cast<double>(m);
        const double sinc = (m == 0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
        // Blackman window: ~74 dB stopband, enough for a receive chain.
        const double t = static_cast<double>(k) / static_cast<double>(numTaps - 1);
        const double w = 0.42 - 0.5 * std::cos(kTwoPi * t) + 0.08 * std::cos(2.0 * kTwoPi * t);
        taps[static_cast<size_t>(k)] = static_cast<float>(2.0 * fc * sinc * w);
        sum += taps[static_cast<size_t>(k)];
    }
    // Normalise to unity DC gain so the demods see a calibrated amplitude.
    if (std::fabs(sum) > 1e-12) {
        const float inv = static_cast<float>(1.0 / sum);
        for (float& t : taps) t *= inv;
    }
    return taps;
}

// ------------------------------------------------------------------ complex FIR

void FirDecimatorComplex::design(double fs, double cutoffHz, int numTaps, int decimFactor) {
    taps_ = designLowpass(fs, cutoffHz, numTaps);
    decim_ = std::max(1, decimFactor);
    tail_.assign(taps_.size() - 1, cfloat(0.0f, 0.0f));
    work_.clear();
    absPos_ = 0;
}

void FirDecimatorComplex::reset() {
    std::fill(tail_.begin(), tail_.end(), cfloat(0.0f, 0.0f));
    absPos_ = 0;
}

size_t FirDecimatorComplex::process(const cfloat* in, size_t n, cfloat* out, size_t maxOut) {
    if (taps_.empty() || n == 0) return 0;
    const size_t t = taps_.size();
    const size_t prefix = t - 1;

    work_.resize(prefix + n);
    std::memcpy(work_.data(), tail_.data(), prefix * sizeof(cfloat));
    std::memcpy(work_.data() + prefix, in, n * sizeof(cfloat));

    // Output sample p exists for every p multiple of decim. Its position inside
    // work_ is prefix + (p - absPos_), which is exactly what makes the result
    // independent of where the previous block ended.
    const size_t rem = absPos_ % static_cast<size_t>(decim_);
    const size_t jStart = rem ? static_cast<size_t>(decim_) - rem : 0;

    const float* h = taps_.data();
    size_t produced = 0;
    for (size_t j = jStart; j < n && produced < maxOut; j += static_cast<size_t>(decim_)) {
        const cfloat* w = work_.data() + prefix + j;
        float re = 0.0f, im = 0.0f;
        for (size_t k = 0; k < t; ++k) {
            const float g = h[k];
            const cfloat v = w[-static_cast<ptrdiff_t>(k)];
            re += g * v.real();
            im += g * v.imag();
        }
        out[produced++] = cfloat(re, im);
    }

    absPos_ += n;
    std::memcpy(tail_.data(), work_.data() + work_.size() - prefix, prefix * sizeof(cfloat));
    return produced;
}

// --------------------------------------------------------------------- real FIR

void FirDecimatorReal::design(double fs, double cutoffHz, int numTaps, int decimFactor) {
    taps_ = designLowpass(fs, cutoffHz, numTaps);
    decim_ = std::max(1, decimFactor);
    tail_.assign(taps_.size() - 1, 0.0f);
    work_.clear();
    absPos_ = 0;
}

void FirDecimatorReal::reset() {
    std::fill(tail_.begin(), tail_.end(), 0.0f);
    absPos_ = 0;
}

size_t FirDecimatorReal::process(const float* in, size_t n, float* out, size_t maxOut) {
    if (taps_.empty() || n == 0) return 0;
    const size_t t = taps_.size();
    const size_t prefix = t - 1;

    work_.resize(prefix + n);
    std::memcpy(work_.data(), tail_.data(), prefix * sizeof(float));
    std::memcpy(work_.data() + prefix, in, n * sizeof(float));

    const size_t rem = absPos_ % static_cast<size_t>(decim_);
    const size_t jStart = rem ? static_cast<size_t>(decim_) - rem : 0;

    const float* h = taps_.data();
    size_t produced = 0;
    for (size_t j = jStart; j < n && produced < maxOut; j += static_cast<size_t>(decim_)) {
        const float* w = work_.data() + prefix + j;
        float acc = 0.0f;
        for (size_t k = 0; k < t; ++k) acc += h[k] * w[-static_cast<ptrdiff_t>(k)];
        out[produced++] = acc;
    }

    absPos_ += n;
    std::memcpy(tail_.data(), work_.data() + work_.size() - prefix, prefix * sizeof(float));
    return produced;
}

// ------------------------------------------------------------------- spectrum

SpectrumAnalyzer::SpectrumAnalyzer(int fftSize, double inputRate, double framesPerSecond)
    : fftSize_(fftSize), inputRate_(inputRate), fft_(fftSize) {
    acc_.assign(static_cast<size_t>(fftSize_), cfloat(0.0f, 0.0f));
    fftBuf_.assign(static_cast<size_t>(fftSize_), cfloat(0.0f, 0.0f));
    binsDb_.assign(static_cast<size_t>(fftSize_), -200.0f);
    win_.resize(static_cast<size_t>(fftSize_));

    double wsum = 0.0;
    for (int i = 0; i < fftSize_; ++i) {
        const double v = 0.5 - 0.5 * std::cos(kTwoPi * i / fftSize_);
        win_[static_cast<size_t>(i)] = static_cast<float>(v);
        wsum += v;
    }
    windowGain_ = static_cast<float>(wsum);

    const double rate = framesPerSecond > 0.0 ? framesPerSecond : 40.0;
    const size_t wanted = static_cast<size_t>(std::llround(inputRate_ / rate));
    hop_ = std::max(static_cast<size_t>(fftSize_), wanted);
    skip_ = 0;
    fill_ = 0;
}

void SpectrumAnalyzer::reset() {
    std::fill(acc_.begin(), acc_.end(), cfloat(0.0f, 0.0f));
    fill_ = 0;
    skip_ = 0;
    primed_ = false;
    frames_ = 0;
    peakDb_ = -200.0f;
}

void SpectrumAnalyzer::computeFrame() {
    for (int i = 0; i < fftSize_; ++i)
        fftBuf_[static_cast<size_t>(i)] = acc_[static_cast<size_t>(i)] * win_[static_cast<size_t>(i)];
    fft_.forward(fftBuf_.data());

    // |X|² / (sum(window))² gives 0 dBFS for a unit-amplitude complex tone.
    const float norm = 1.0f / (windowGain_ * windowGain_);
    const float alpha = primed_ ? 0.45f : 1.0f;
    peakDb_ = -200.0f;
    for (int i = 0; i < fftSize_; ++i) {
        const cfloat v = fftBuf_[static_cast<size_t>(i)];
        const float p = (v.real() * v.real() + v.imag() * v.imag()) * norm;
        const float db = 10.0f * std::log10(p + 1e-20f);
        float& bin = binsDb_[static_cast<size_t>(i)];
        bin += alpha * (db - bin);
        // Bins 0..2 and the mirrored edge hold the DC spike of a zero-IF front-end.
        if (i > 2 && i < fftSize_ - 2 && bin > peakDb_) peakDb_ = bin;
    }
    primed_ = true;
    ++frames_;
    fill_ = 0;
    skip_ = hop_ - static_cast<size_t>(fftSize_);
}

void SpectrumAnalyzer::process(const cfloat* in, size_t n) {
    size_t offset = 0;
    while (offset < n) {
        if (skip_ > 0) {
            const size_t take = std::min(skip_, n - offset);
            skip_ -= take;
            offset += take;
            continue;
        }
        const size_t take = std::min(n - offset, acc_.size() - fill_);
        std::memcpy(acc_.data() + fill_, in + offset, take * sizeof(cfloat));
        fill_ += take;
        offset += take;
        if (fill_ == acc_.size()) computeFrame();
    }
}

double SpectrumAnalyzer::peakHz() const {
    if (!primed_) return 0.0;
    int best = 0;
    float bestDb = -1e9f;
    for (int i = 3; i < fftSize_ - 3; ++i) {
        if (binsDb_[static_cast<size_t>(i)] > bestDb) {
            bestDb = binsDb_[static_cast<size_t>(i)];
            best = i;
        }
    }
    const int shifted = (best <= fftSize_ / 2) ? best : best - fftSize_;
    return shifted * binHz();
}

// ------------------------------------------------------------------- receiver

Receiver::Receiver(double inputRate) : inputRate_(inputRate) {
    rebuildFilters();
    updateAudioFilter();
    setDeemphasisMicros(75);
    setOffsetHz(0.0);
    reset();
}

void Receiver::rebuildFilters() {
    // 2.4 MS/s -> 480 kS/s -> 240 kS/s: a 200 kHz WBFM channel fits with margin.
    const int decimA = std::max(1, static_cast<int>(std::lround(inputRate_ / 480000.0)));
    const int decimB = std::max(1, static_cast<int>(std::lround((inputRate_ / decimA) / 240000.0)));
    channelRate_ = inputRate_ / static_cast<double>(decimA) / static_cast<double>(decimB);
    audioRate_ = channelRate_ / std::max(1, static_cast<int>(std::lround(channelRate_ / 48000.0)));

    stageA_.design(inputRate_, std::min(115000.0, 0.45 * (inputRate_ / decimA)), 121, decimA);
    stageB_.design(inputRate_ / decimA, 105000.0, 71, decimB);

    // Scratch buffers only ever see one kChunk of input.
    dc_.assign(kChunk, cfloat(0.0f, 0.0f));
    chA_.assign(kChunk + 8, cfloat(0.0f, 0.0f));
    chB_.assign(kChunk + 8, cfloat(0.0f, 0.0f));
    dem_.assign(kChunk + 8, 0.0f);
    aud_.assign(kChunk + 8, 0.0f);
    // Voie difference du decodeur stereo : meme longueur que la voie mono.
    subDem_.assign(kChunk + 8, 0.0f);
    sub_.assign(kChunk + 8, 0.0f);
}

void Receiver::updateAudioFilter() {
    const int decim = std::max(1, static_cast<int>(std::lround(channelRate_ / audioRate_)));
    double cutoff = modeAudioCutoff(mode_);
    cutoff = std::min(cutoff, 0.90 * (channelRate_ / (2.0 * decim)));
    // WBFM : 301 coefficients pour que le pilote 19 kHz tombe franchement dans
    // la bande coupee (161 le laissait a ~20 dB pres, ce qui limitait la
    // separation stereo et faisait siffler le mono).
    const int taps = (mode_ == Mode::Cw || mode_ == Mode::Wbfm) ? 301 : 161;
    audioFir_.design(channelRate_, cutoff, taps, decim);
    audioFir_.reset();
    // Meme filtre et meme decimation pour L-R : les deux voies restent
    // alignees echantillon par echantillon (indispensable pour le matricage).
    subFir_.design(channelRate_, cutoff, taps, decim);
    subFir_.reset();
    pilotOmega_ = kTwoPi * 19000.0 / channelRate_;
    prevSample_ = cfloat(0.0f, 0.0f);
    deempZL_ = 0.0f;
    deempZR_ = 0.0f;

    // SSB/CW: move the wanted sideband edge down to baseband, then keep I.
    double shift = 0.0;
    if (mode_ == Mode::Usb) shift = -1450.0;
    else if (mode_ == Mode::Lsb) shift = 1450.0;
    else if (mode_ == Mode::Cw) shift = -700.0;
    const double inc = kTwoPi * shift / channelRate_;
    ssbStep_ = cfloat(static_cast<float>(std::cos(inc)), static_cast<float>(std::sin(inc)));
    ssbState_ = cfloat(1.0f, 0.0f);
}

void Receiver::setMode(Mode m) {
    if (m == mode_) return;
    mode_ = m;
    updateAudioFilter();
    gate_ = 0.0f;
    // Le pilote n'a de sens qu'en WBFM : on repart d'un PLL neuf.
    pilotPhase_ = 0.0f;
    pilotFreq_ = 0.0f;
    pilotLp_ = cfloat(0.0f, 0.0f);
    pilotMag_ = 0.0f;
    stereoMix_ = 0.0f;
}

void Receiver::setStereoEnabled(bool on) {
    if (on == stereoRequested_) return;
    stereoRequested_ = on;
    if (!on) stereoMix_ = 0.0f;
}

void Receiver::setOffsetHz(double hz) {
    const double limit = maxOffsetHz();
    offsetHz_ = std::clamp(hz, -limit, limit);
    const double inc = -kTwoPi * offsetHz_ / inputRate_;
    mixStep_ = cfloat(static_cast<float>(std::cos(inc)), static_cast<float>(std::sin(inc)));
}

void Receiver::setSquelchDb(double db) { squelchDb_ = db; }
void Receiver::setSquelchEnabled(bool on) { squelchEnabled_ = on; }
void Receiver::setVolume(float v) { volume_ = std::clamp(v, 0.0f, 1.0f); }
void Receiver::setAgcEnabled(bool on) { agc_ = on; }

void Receiver::setDeemphasisMicros(int us) {
    // One-pole low-pass: y = a*x + (1-a)*y with a = dt / (tau + dt).
    const double dt = 1.0 / audioRate_;
    const double tau = static_cast<double>(us) * 1e-6;
    deempA_ = (tau <= 0.0) ? 1.0f : static_cast<float>(dt / (tau + dt));
}

void Receiver::reset() {
    stageA_.reset();
    stageB_.reset();
    audioFir_.reset();
    dcX1_ = dcY1_ = cfloat(0.0f, 0.0f);
    mixState_ = cfloat(1.0f, 0.0f);
    ssbState_ = cfloat(1.0f, 0.0f);
    prevSample_ = cfloat(0.0f, 0.0f);
    deempZL_ = 0.0f;
    deempZR_ = 0.0f;
    subFir_.reset();
    pilotPhase_ = 0.0f;
    pilotFreq_ = 0.0f;
    pilotLp_ = cfloat(0.0f, 0.0f);
    pilotMag_ = 0.0f;
    stereoMix_ = 0.0f;
    env_ = 0.0f;
    gain_ = 1.0f;
    gate_ = 0.0f;
    gateTarget_ = 0.0f;
    outPeak_ = 0.0f;
    amDc_ = 0.0f;
    powerDb_ = -200.0;
    powerPrimed_ = false;
}

void Receiver::process(const cfloat* in, size_t n, std::vector<float>& audio) {
    size_t offset = 0;
    while (offset < n) {
        const size_t take = std::min(kChunk, n - offset);
        processChunk(in + offset, take, &audio, nullptr);
        offset += take;
    }
}

void Receiver::processStereo(const cfloat* in, size_t n, std::vector<float>& audio) {
    size_t offset = 0;
    while (offset < n) {
        const size_t take = std::min(kChunk, n - offset);
        processChunk(in + offset, take, nullptr, &audio);
        offset += take;
    }
}

// PLL sur le pilote 19 kHz + demodulation coherente du sous-porteure 38 kHz.
// dem_ contient le multiplex composite : (L+R) + pilote + (L-R) module.
void Receiver::decodeStereo(size_t n) {
    // Boucle du 2e ordre, bande ~20 Hz : assez etroite pour ne pas accrocher
    // sur le multiplex lui-meme, assez rapide pour verrouiller en ~50 ms.
    const float kLoopFilter = 0.002f; // ~76 Hz a 240 kHz
    const float kPhase = 7.5e-4f;
    const float kFreq = 2.75e-7f;
    float phase = pilotPhase_;
    float freq = pilotFreq_;
    cfloat lp = pilotLp_;
    float level = pilotMag_;

    // Le NCO accroche le pilote a 90 deg : la phase du pilote vaut arg(lp)+90,
    // donc le 38 kHz s'ecrit -sin(2*phase + 2*arg(lp)). Reconstruire la
    // sous-porteuse depuis le pilote mesure annule tout ecart statique (7 deg
    // suffisaient a perdre 3 % de L-R, soit 6 dB de separation).
    const float phi = std::atan2(lp.imag(), lp.real());
    const float c2phi = std::cos(2.0f * phi);
    const float s2phi = std::sin(2.0f * phi);

    for (size_t i = 0; i < n; ++i) {
        const float mpx = dem_[i];
        const float c = std::cos(phase);
        const float s = std::sin(phase);
        // mpx * conj(exp(j*phase)) : la composante 19 kHz revient en continu.
        const cfloat e(mpx * c, -mpx * s);
        lp += (e - lp) * kLoopFilter;
        // sin(erreur de phase), borne a +/-1 : un atan2 sur un detecteur
        // bruite ferait emballer l'integrateur avant le verrouillage.
        const float mag = std::abs(lp);
        const float err = (mag > 1e-6f) ? (lp.imag() / mag) : 0.0f;
        freq = std::clamp(freq + kFreq * err, -0.004f, 0.004f); // +/-150 Hz
        phase += static_cast<float>(pilotOmega_) + freq + kPhase * err;
        if (phase > kTwoPi) phase -= static_cast<float>(kTwoPi);
        else if (phase < 0.0f) phase += static_cast<float>(kTwoPi);
        // Sous-porteure 38 kHz : le signal utile est en (L-R)*sin(2*phase), le
        // NCO accroche le pilote a 90 deg, donc sin(2*phase) = -sin(2*phase_pilote).
        // Multiplier par un cosinus renverrait tout a 76 kHz, hors bande audio.
        const float s2 = 2.0f * c * s;       // sin(2*phase NCO)
        const float c2 = c * c - s * s;      // cos(2*phase NCO)
        const float ref = -(s2 * c2phi + c2 * s2phi); // 38 kHz, en phase pilote
        subDem_[i] = 2.0f * mpx * ref;
        level += 0.0005f * (mag - level); // indicateur de verrouillage
    }

    pilotPhase_ = phase;
    pilotFreq_ = freq;
    pilotLp_ = lp;
    pilotMag_ = level;
}

void Receiver::processChunk(const cfloat* in, size_t n, std::vector<float>* mono,
                            std::vector<float>* stereo) {
    // 1. DC block + tuner mix, single pass over the chunk.
    cfloat s = mixState_;
    const cfloat step = mixStep_;
    cfloat x1 = dcX1_;
    cfloat y1 = dcY1_;
    for (size_t i = 0; i < n; ++i) {
        const cfloat x = in[i];
        const cfloat y = x - x1 + 0.9995f * y1; // ~190 Hz high-pass at 2.4 MS/s
        x1 = x;
        y1 = y;
        dc_[i] = y * s;
        s *= step;
        if ((i & 255) == 255) s = renormalize(s);
    }
    dcX1_ = x1;
    dcY1_ = y1;
    mixState_ = s;

    // 2. Channelizer.
    const size_t nA = stageA_.process(dc_.data(), n, chA_.data(), chA_.size());
    if (nA == 0) return;
    const size_t nB = stageB_.process(chA_.data(), nA, chB_.data(), chB_.size());
    if (nB == 0) return;

    // 3. Channel power for the squelch and the meter.
    double power = 0.0;
    for (size_t i = 0; i < nB; ++i) power += std::norm(chB_[i]);
    power /= static_cast<double>(nB);
    const double db = 10.0 * std::log10(power + 1e-20);
    powerDb_ = powerPrimed_ ? powerDb_ + 0.25 * (db - powerDb_) : db;
    powerPrimed_ = true;

    // 4. Demodulate to real audio at the channel rate.
    size_t demN = 0;
    switch (mode_) {
        case Mode::Wbfm:
        case Mode::Nbfm: {
            // atan2 of the conjugate product = instantaneous frequency.
            const double deviation = (mode_ == Mode::Wbfm) ? 75000.0 : 5000.0;
            const float scale = static_cast<float>(channelRate_ / (kTwoPi * deviation));
            cfloat prev = prevSample_;
            for (size_t i = 0; i < nB; ++i) {
                const cfloat cur = chB_[i];
                const cfloat prod = cur * std::conj(prev);
                dem_[demN++] = std::atan2(prod.imag(), prod.real()) * scale;
                prev = cur;
            }
            prevSample_ = prev;
            break;
        }
        case Mode::Am: {
            for (size_t i = 0; i < nB; ++i) dem_[demN++] = std::abs(chB_[i]);
            break;
        }
        case Mode::Usb:
        case Mode::Lsb:
        case Mode::Cw: {
            cfloat st = ssbState_;
            const cfloat stp = ssbStep_;
            for (size_t i = 0; i < nB; ++i) {
                dem_[demN++] = (chB_[i] * st).real();
                st *= stp;
                if ((i & 255) == 255) st = renormalize(st);
            }
            ssbState_ = st;
            break;
        }
    }

    // 4b. Stereo (WBFM seulement) : pilote 19 kHz puis sous-porteure 38 kHz.
    const bool wantStereo = stereoRequested_ && mode_ == Mode::Wbfm;
    if (wantStereo) decodeStereo(demN);
    // Fondu mono <-> stereo, avec hysteresis : sans pilote exploitable on
    // garde le mono, et on ne redescend qu'une fois le pilote vraiment perdu.
    const float threshold = (stereoMix_ > 0.5f) ? 0.012f : 0.020f;
    const float lockTarget = (wantStereo && pilotMag_ > threshold) ? 1.0f : 0.0f;
    stereoMix_ += 0.12f * (lockTarget - stereoMix_);

    // 5. Audio lowpass + decimation to 48 kHz.
    const size_t nAud = audioFir_.process(dem_.data(), demN, aud_.data(), aud_.size());
    if (nAud == 0) return;
    // L-R subit exactement le meme filtrage et la meme decimation.
    const size_t nSub = wantStereo ? subFir_.process(subDem_.data(), demN, sub_.data(), sub_.size()) : 0;

    // 6. De-emphasis, squelch gate, AGC, volume.
    const bool deemphasise = (mode_ == Mode::Wbfm);
    const bool amEnvelope = (mode_ == Mode::Am);
    const float hydro = deemphasise ? deempA_ : 1.0f;
    gateTarget_ = (!squelchEnabled_ || powerDb_ > squelchDb_) ? 1.0f : 0.0f;

    // Le matricage n'est applique qu'a la sortie stereo : process() reste mono.
    const float mix = (stereo != nullptr && nSub > 0) ? stereoMix_ : 0.0f;

    if (mono) mono->reserve(mono->size() + nAud);
    if (stereo) stereo->reserve(stereo->size() + 2 * nAud);
    float peak = 0.0f;
    for (size_t i = 0; i < nAud; ++i) {
        float v = aud_[i];

        if (amEnvelope) {
            // The envelope detector leaves the carrier as DC: remove it slowly.
            amDc_ += 0.0005f * (v - amDc_);
            v -= amDc_;
        }

        // L = S(1 - m/2) + (m/2)(L-R), R = S(1 - m/2) - (m/2)(L-R).
        // A m = 0 les deux voies valent S : c'est exactement le mono d'avant.
        const float d = (i < nSub) ? sub_[i] : 0.0f;
        const float half = 0.5f * mix;
        float l = v * (1.0f - half) + half * d;
        float r = v * (1.0f - half) - half * d;

        l = hydro * l + (1.0f - hydro) * deempZL_;
        deempZL_ = l;
        r = hydro * r + (1.0f - hydro) * deempZR_;
        deempZR_ = r;

        const float a = std::fabs(v); // AGC sur la somme mono : image stable
        env_ += (a > env_ ? 0.010f : 0.0004f) * (a - env_);
        if (agc_) gain_ += 0.02f * (std::clamp(0.35f / std::max(env_, 1e-4f), 0.05f, 400.0f) - gain_);
        else gain_ = 1.0f;

        gate_ += 0.0025f * (gateTarget_ - gate_); // ~8 ms ramp at 48 kHz
        const float g = gain_ * gate_ * volume_;
        const float outL = std::clamp(l * g, -1.0f, 1.0f);
        if (mono) mono->push_back(outL);
        if (stereo) {
            const float outR = std::clamp(r * g, -1.0f, 1.0f);
            stereo->push_back(outL);
            stereo->push_back(outR);
            peak = std::max(peak, std::max(std::fabs(outL), std::fabs(outR)));
        } else {
            peak = std::max(peak, std::fabs(outL));
        }
    }
    outPeak_ += 0.3f * (peak - outPeak_);
}

} // namespace sdr
