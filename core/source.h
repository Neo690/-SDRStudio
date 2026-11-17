// SDRStudio - IQ source abstraction (hardware, synthetic, file).
#pragma once

#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "fft.h"

namespace sdr {

struct SourceInfo {
    std::string name = "aucune";
    std::string driver = "none";
    double sampleRate = 2.4e6;
    double centerHz = 100e6;
    double minHz = 24e6;
    double maxHz = 1766e6;
    double gainDb = 0.0;
    bool hardware = false;
    bool txCapable = false;
};

class IqSource {
public:
    virtual ~IqSource() = default;

    // Returns a human readable name, empty string on failure (err filled).
    virtual std::string open(std::string& err) = 0;
    virtual void close() = 0;

    // Blocking read of up to maxSamples complex samples (integers already
    // converted to float in [-1, 1]). Returns 0 on end of stream.
    virtual size_t read(cfloat* dst, size_t maxSamples, std::string& err) = 0;

    virtual bool setCenterHz(double hz, std::string& err) = 0;
    virtual bool setSampleRate(double rate, std::string& err) = 0;
    virtual bool setGainDb(double db, std::string& err) = 0; // < 0 => automatic
    virtual bool setPpm(int ppm, std::string& err) = 0;
    virtual bool setBiasTee(bool on, std::string& err) {
        (void)on;
        err = "bias tee non supporte par cette source";
        return false;
    }

    virtual const SourceInfo& info() const = 0;
    virtual std::vector<double> gainsDb() const { return {}; }
    virtual bool atEnd() const { return false; }
};

struct SourceRequest {
    std::string kind = "auto"; // auto | rtl | synthetic | file
    std::string path;
    int index = 0;
    double sampleRate = 2.4e6;
};

// Portable sources.
std::unique_ptr<IqSource> createSyntheticSource(const SourceRequest& req);
std::unique_ptr<IqSource> createFileSource(const SourceRequest& req, std::string& err);

// Platform backends (implemented per front-end).
std::unique_ptr<IqSource> createRtlSource(const SourceRequest& req, std::string& err);
bool rtlBackendProbe(std::string& detail);

// Auto: hardware first, synthetic fallback.
std::unique_ptr<IqSource> createSource(const SourceRequest& req, std::string& err);

} // namespace sdr
