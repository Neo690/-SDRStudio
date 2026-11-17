// SDRStudio - portable DSP core: channelizer, demodulators, spectrum.
// Shared by the Windows front-end and the Android front-end (via JNI).
#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fft.h"

namespace sdr {

enum class Mode : int { Wbfm = 0, Nbfm, Am, Usb, Lsb, Cw };
constexpr int kModeCount = 6;

const char* modeAbbrev(Mode m);
// Audio lowpass cutoff for a mode, in Hz (at the 48 kHz audio rate).
double modeAudioCutoff(Mode m);

// Windowed-sinc lowpass, unity DC gain, odd tap count.
std::vector<float> designLowpass(double fs, double cutoffHz, int numTaps);

// Complex FIR decimator (channel filter). Output positions are tied to the
// absolute input sample index, so results do not depend on how the stream is
// split into blocks.
class FirDecimatorComplex {
public:
    void design(double fs, double cutoffHz, int numTaps, int decimFactor);
    size_t process(const cfloat* in, size_t n, cfloat* out, size_t maxOut);
    int decim() const { return decim_; }
    void reset();

private:
    std::vector<float> taps_;
    std::vector<cfloat> tail_;
    std::vector<cfloat> work_;
    int decim_ = 1;
    size_t absPos_ = 0;
};

// Real FIR decimator (audio path).
class FirDecimatorReal {
public:
    void design(double fs, double cutoffHz, int numTaps, int decimFactor);
    size_t process(const float* in, size_t n, float* out, size_t maxOut);
    int decim() const { return decim_; }
    void reset();

private:
    std::vector<float> taps_;
    std::vector<float> tail_;
    std::vector<float> work_;
    int decim_ = 1;
    size_t absPos_ = 0;
};

// Hann-windowed power spectrum with exponential averaging. The FFT is computed
// at most `framesPerSecond` times per second; input samples in between are
// skipped, which keeps the display refresh free of wasted FFT work.
class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(int fftSize = 4096, double inputRate = 2.4e6, double framesPerSecond = 40.0);

    void process(const cfloat* in, size_t n);

    const std::vector<float>& binsDb() const { return binsDb_; }
    int size() const { return fftSize_; }
    double binHz() const { return inputRate_ / static_cast<double>(fftSize_); }
    double peakHz() const; // strongest bin offset from the centre, in Hz
    float peakDb() const { return peakDb_; }
    uint64_t frames() const { return frames_; }
    void reset();

private:
    void computeFrame();

    int fftSize_ = 4096;
    double inputRate_ = 2.4e6;
    size_t hop_ = 4096;
    size_t skip_ = 0;
    size_t fill_ = 0;
    uint64_t frames_ = 0;
    bool primed_ = false;
    float peakDb_ = -200.0f;
    float windowGain_ = 1.0f;
    Fft fft_;
    std::vector<cfloat> acc_;
    std::vector<cfloat> fftBuf_;
    std::vector<float> win_;
    std::vector<float> binsDb_;
};

// Full receive chain: DC block -> tuner NCO -> 2-stage decimation to 240 kS/s ->
// demodulation -> 48 kS/s audio, with squelch and AGC.
class Receiver {
public:
    explicit Receiver(double inputRate = 2.4e6);

    void setMode(Mode m);
    // Baseband offset inside the captured band (also used to move around in a
    // recorded file). Clamped to +/-maxOffsetHz().
    void setOffsetHz(double hz);
    double offsetHz() const { return offsetHz_; }
    // Largest offset that still keeps the whole channel inside the band.
    double maxOffsetHz() const { return std::max(0.0, 0.5 * (inputRate_ - channelRate_)); }
    void setSquelchDb(double db);
    void setSquelchEnabled(bool on);
    void setVolume(float v);          // 0..1
    void setDeemphasisMicros(int us); // 75 (EU) / 50 (US) / 0 = off
    void setAgcEnabled(bool on);

    // WBFM stereo: 19 kHz pilot PLL driving the 38 kHz subcarrier. Only
    // processStereo() consumes the L-R channel; process() stays pure mono for
    // every mode, whether stereo is enabled or not.
    void setStereoEnabled(bool on);
    bool stereoEnabled() const { return stereoRequested_; }
    // True once a 19 kHz pilot is tracked and L-R is actually mixed in.
    bool stereoLocked() const { return stereoMix_ > 0.5f; }

    // in: raw IQ at inputRate. Audio (mono, audioRate) is appended to audio.
    void process(const cfloat* in, size_t n, std::vector<float>& audio);
    // Same chain, interleaved L/R at audioRate. Without a locked pilot (or with
    // stereo disabled) both channels carry the mono signal.
    void processStereo(const cfloat* in, size_t n, std::vector<float>& audio);

    Mode mode() const { return mode_; }
    double inputRate() const { return inputRate_; }
    double channelRate() const { return channelRate_; }
    double audioRate() const { return audioRate_; }
    double channelPowerDb() const { return powerDb_; }
    // Channel above the squelch threshold. Independent of the audio gate, so a
    // scan can use it as its stop criterion whatever the gate state is.
    bool squelchOpen() const { return powerPrimed_ && powerDb_ > squelchDb_; }
    double outputPeak() const { return outPeak_; }
    void reset();

private:
    // Fixed internal chunk: state carries across chunks, so the public block
    // size never changes the result and never overflows a scratch buffer.
    static constexpr size_t kChunk = 8192;

    void rebuildFilters();
    void updateAudioFilter();
    // mono: one sample per frame. stereo: L,R interleaved. Both may be null.
    void processChunk(const cfloat* in, size_t n, std::vector<float>* mono,
                      std::vector<float>* stereo);
    // Drives the pilot PLL over one chunk and fills subDem_ with L-R.
    void decodeStereo(size_t n);

    double inputRate_ = 2.4e6;
    double channelRate_ = 240000.0;
    double audioRate_ = 48000.0;
    double offsetHz_ = 0.0;
    Mode mode_ = Mode::Wbfm;

    // Tuner mixer: recursive complex rotation, renormalised periodically.
    cfloat mixStep_{1.0f, 0.0f};
    cfloat mixState_{1.0f, 0.0f};

    // DC blocker (one-pole high-pass, removes the RTL zero-IF spike).
    cfloat dcX1_{0.0f, 0.0f};
    cfloat dcY1_{0.0f, 0.0f};

    FirDecimatorComplex stageA_;
    FirDecimatorComplex stageB_;
    FirDecimatorReal audioFir_;

    // Demodulator state.
    cfloat prevSample_{0.0f, 0.0f};
    cfloat ssbStep_{1.0f, 0.0f};
    cfloat ssbState_{1.0f, 0.0f};

    // De-emphasis (one state per channel).
    float deempA_ = 1.0f;
    float deempZL_ = 0.0f;
    float deempZR_ = 0.0f;

    // Stereo decoder: pilot PLL at the channel rate.
    bool stereoRequested_ = false;
    double pilotOmega_ = 0.0;   // nominal 19 kHz step, rad/sample
    float pilotPhase_ = 0.0f;
    float pilotFreq_ = 0.0f;    // PLL frequency correction, rad/sample
    cfloat pilotLp_{0.0f, 0.0f}; // narrowband 19 kHz detector output
    float pilotMag_ = 0.0f;     // smoothed pilot amplitude (lock indicator)
    float stereoMix_ = 0.0f;    // 0 = mono, 1 = full stereo

    // AGC / squelch / metering.
    bool agc_ = true;
    float env_ = 0.0f;
    float gain_ = 1.0f;
    float volume_ = 0.7f;
    float gate_ = 0.0f;
    float gateTarget_ = 0.0f;
    bool squelchEnabled_ = false;
    double squelchDb_ = -70.0;
    double powerDb_ = -200.0;
    bool powerPrimed_ = false;
    float outPeak_ = 0.0f;
    float amDc_ = 0.0f;

    // Scratch buffers, sized from kChunk (no per-block allocation).
    std::vector<cfloat> dc_;
    std::vector<cfloat> chA_;
    std::vector<cfloat> chB_;
    std::vector<float> dem_;
    std::vector<float> aud_;
    std::vector<float> subDem_;  // L-R at channel rate
    std::vector<float> sub_;     // L-R at audio rate, aligned with aud_
    FirDecimatorReal subFir_;
};

} // namespace sdr
