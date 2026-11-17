// SDRStudio - synthetic and file based IQ sources (portable, no hardware).
#include "source.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sdr {
namespace {

constexpr double kTwoPi = 2.0 * kPi;

uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ---------------------------------------------------------------- synthetic

struct DemoStation {
    double hz;        // absolute frequency
    double deviation; // FM deviation in Hz
    double tone1;     // audio tones
    double tone2;
    double level;     // linear amplitude
    double phase;     // carrier phase
    double a1;        // audio phases
    double a2;
};

class SyntheticSource : public IqSource {
public:
    explicit SyntheticSource(const SourceRequest& req) {
        info_.name = "Generateur interne (demo)";
        info_.driver = "synthetic";
        info_.hardware = false;
        info_.sampleRate = req.sampleRate > 0 ? req.sampleRate : 2.4e6;
        info_.centerHz = 101.5e6;
        info_.minHz = 1e6;
        info_.maxHz = 2000e6;
        stations_ = {
            {100.0e6, 75000.0, 440.0, 1000.0, 0.20, 0.0, 0.0, 0.0},
            {100.7e6, 70000.0, 300.0, 660.0, 0.12, 0.0, 0.0, 0.0},
            {101.5e6, 75000.0, 1000.0, 1500.0, 0.28, 0.0, 0.0, 0.0},
            {102.3e6, 40000.0, 800.0, 1200.0, 0.09, 0.0, 0.0, 0.0},
            {103.4e6, 75000.0, 520.0, 940.0, 0.16, 0.0, 0.0, 0.0},
        };
        // Deterministic random phases so successive runs look identical.
        for (size_t i = 0; i < stations_.size(); ++i)
            stations_[i].phase = 0.37 * static_cast<double>(i);
    }

    std::string open(std::string& err) override {
        (void)err;
        open_ = true;
        return info_.name;
    }
    void close() override { open_ = false; }

    size_t read(cfloat* dst, size_t maxSamples, std::string& err) override {
        (void)err;
        if (!open_) return 0;
        const double fs = info_.sampleRate;
        for (size_t n = 0; n < maxSamples; ++n) {
            if ((n & 31) == 0) {
                for (auto& s : stations_) {
                    s.a1 = std::sin(kTwoPi * s.tone1 * time_);
                    s.a2 = std::sin(kTwoPi * s.tone2 * time_);
                }
            }
            float re = 0.0f, im = 0.0f;
            for (auto& s : stations_) {
                const double offset = s.hz - info_.centerHz;
                if (std::fabs(offset) > fs * 0.5) continue;
                const double audio = 0.65 * s.a1 + 0.35 * s.a2;
                s.phase += kTwoPi * (offset + s.deviation * audio) / fs;
                if (s.phase > kTwoPi) s.phase -= kTwoPi;
                if (s.phase < -kTwoPi) s.phase += kTwoPi;
                const double lvl = s.level;
                re += static_cast<float>(lvl * std::cos(s.phase));
                im += static_cast<float>(lvl * std::sin(s.phase));
            }
            // Noise floor + a small DC term (emulates an RTL front-end).
            const float n1 = (static_cast<float>(xorshift(rng_) & 0xFFFF) / 32768.0f - 1.0f) * 0.006f;
            const float n2 = (static_cast<float>(xorshift(rng_) & 0xFFFF) / 32768.0f - 1.0f) * 0.006f;
            dst[n] = cfloat(re + n1 + 0.01f, im + n2);
            time_ += 1.0 / fs;
        }
        return maxSamples;
    }

    bool setCenterHz(double hz, std::string& err) override {
        (void)err;
        info_.centerHz = hz;
        return true;
    }
    bool setSampleRate(double rate, std::string& err) override {
        (void)err;
        info_.sampleRate = rate;
        return true;
    }
    bool setGainDb(double db, std::string& err) override {
        (void)err;
        info_.gainDb = db;
        return true;
    }
    bool setPpm(int ppm, std::string& err) override {
        (void)err;
        (void)ppm;
        return true;
    }
    const SourceInfo& info() const override { return info_; }
    std::vector<double> gainsDb() const override { return {0.0}; }

private:
    SourceInfo info_;
    bool open_ = false;
    double time_ = 0.0;
    uint32_t rng_ = 0x12345678u;
    std::vector<DemoStation> stations_;
};

// --------------------------------------------------------------------- file

class FileSource : public IqSource {
public:
    FileSource(const SourceRequest& req, std::string& err) {
        path_ = req.path;
        info_.name = req.path;
        info_.driver = "file";
        info_.sampleRate = req.sampleRate > 0 ? req.sampleRate : 2.4e6;
        info_.centerHz = 100e6;

        std::string lower = req.path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".cs16") == 0) fmt_ = Format::Cs16;
        else if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".cf32") == 0) fmt_ = Format::Cf32;
        else fmt_ = Format::Cu8;

        file_ = std::fopen(path_.c_str(), "rb");
        if (!file_) err = "impossible d'ouvrir " + path_;
    }

    ~FileSource() override { close(); }

    std::string open(std::string& err) override {
        if (!file_) {
            if (err.empty()) err = "fichier non ouvert";
            return std::string();
        }
        return info_.name;
    }

    void close() override {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    size_t read(cfloat* dst, size_t maxSamples, std::string& err) override {
        (void)err;
        if (!file_) return 0;
        size_t n = readRaw(dst, maxSamples);
        if (n == 0) {
            // Rejeu : on repart du debut plutot que de couper la reception,
            // sinon une capture de quelques secondes serait inutilisable.
            if (std::fseek(file_, 0, SEEK_SET) == 0) n = readRaw(dst, maxSamples);
            eof_ = (n == 0);
        }
        return n;
    }

    size_t readRaw(cfloat* dst, size_t maxSamples) {
        const size_t bytesPerSample = (fmt_ == Format::Cu8) ? 2 : (fmt_ == Format::Cs16) ? 4 : 8;
        raw_.resize(maxSamples * bytesPerSample);
        const size_t got = std::fread(raw_.data(), 1, raw_.size(), file_);
        const size_t n = got / bytesPerSample;
        for (size_t i = 0; i < n; ++i) {
            if (fmt_ == Format::Cu8) {
                const uint8_t* p = raw_.data() + i * 2;
                dst[i] = cfloat((static_cast<float>(p[0]) - 127.4f) / 127.4f,
                                (static_cast<float>(p[1]) - 127.4f) / 127.4f);
            } else if (fmt_ == Format::Cs16) {
                int16_t v[2];
                std::memcpy(v, raw_.data() + i * 4, 4);
                dst[i] = cfloat(static_cast<float>(v[0]) / 32768.0f,
                                static_cast<float>(v[1]) / 32768.0f);
            } else {
                float v[2];
                std::memcpy(v, raw_.data() + i * 8, 8);
                dst[i] = cfloat(v[0], v[1]);
            }
        }
        return n;
    }

    bool setCenterHz(double hz, std::string& err) override {
        (void)err;
        info_.centerHz = hz;
        return true;
    }
    bool setSampleRate(double rate, std::string& err) override {
        (void)err;
        info_.sampleRate = rate;
        return true;
    }
    bool setGainDb(double db, std::string& err) override {
        (void)err;
        (void)db;
        return true;
    }
    bool setPpm(int ppm, std::string& err) override {
        (void)err;
        (void)ppm;
        return true;
    }
    const SourceInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }

private:
    enum class Format { Cu8, Cs16, Cf32 };
    SourceInfo info_;
    std::string path_;
    std::FILE* file_ = nullptr;
    Format fmt_ = Format::Cu8;
    std::vector<uint8_t> raw_;
    bool eof_ = false;
};

} // namespace

std::unique_ptr<IqSource> createSyntheticSource(const SourceRequest& req) {
    return std::make_unique<SyntheticSource>(req);
}

std::unique_ptr<IqSource> createFileSource(const SourceRequest& req, std::string& err) {
    auto src = std::make_unique<FileSource>(req, err);
    if (!err.empty()) return nullptr;
    return src;
}

std::unique_ptr<IqSource> createSource(const SourceRequest& req, std::string& err) {
    if (req.kind == "file") return createFileSource(req, err);
    if (req.kind == "synthetic") return createSyntheticSource(req);
    if (req.kind == "rtl") return createRtlSource(req, err);

    // auto
    if (auto rtl = createRtlSource(req, err)) return rtl;
    err.clear();
    return createSyntheticSource(req);
}

} // namespace sdr
