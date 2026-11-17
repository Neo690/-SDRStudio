// SDRStudio - WAV audio recorder and raw IQ recorder.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "fft.h"

namespace sdr {

class WavWriter {
public:
    ~WavWriter() { close(); }
    bool open(const std::string& path, int sampleRate, int channels, std::string& err);
    void write(const float* mono, size_t frames);
    void close();
    bool isOpen() const { return file_ != nullptr; }
    uint64_t frames() const { return frames_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    int sampleRate_ = 48000;
    int channels_ = 1;
    uint64_t frames_ = 0;
};

// Raw 8-bit unsigned IQ (RTL-SDR native format, replayable with --file).
class IqWriter {
public:
    ~IqWriter() { close(); }
    bool open(const std::string& path, std::string& err);
    void write(const cfloat* iq, size_t n);
    void close();
    bool isOpen() const { return file_ != nullptr; }
    uint64_t samples() const { return samples_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    uint64_t samples_ = 0;
    std::vector<uint8_t> scratch_;
};

} // namespace sdr
