// SDRStudio - recorders implementation.
#include "recorder.h"

#include <algorithm>
#include <cstring>

namespace sdr {
namespace {

void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeHeader(std::FILE* f, int sampleRate, int channels, uint32_t dataBytes) {
    uint8_t h[44] = {0};
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    put16(h + 20, 1);            // PCM
    put16(h + 22, static_cast<uint16_t>(channels));
    put32(h + 24, static_cast<uint32_t>(sampleRate));
    put32(h + 28, static_cast<uint32_t>(sampleRate * channels * 2));
    put16(h + 32, static_cast<uint16_t>(channels * 2));
    put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
}

} // namespace

bool WavWriter::open(const std::string& path, int sampleRate, int channels, std::string& err) {
    close();
    file_ = std::fopen(path.c_str(), "wb+");
    if (!file_) {
        err = "impossible de creer " + path;
        return false;
    }
    path_ = path;
    sampleRate_ = sampleRate;
    channels_ = std::max(1, channels);
    frames_ = 0;
    uint8_t zero[44] = {0};
    std::fwrite(zero, 1, 44, file_);
    return true;
}

void WavWriter::write(const float* mono, size_t frames) {
    if (!file_ || frames == 0) return;
    std::vector<int16_t> pcm(frames * static_cast<size_t>(channels_));
    for (size_t i = 0; i < frames; ++i) {
        float v = std::clamp(mono[i], -1.0f, 1.0f);
        const int16_t s = static_cast<int16_t>(v * 32767.0f);
        for (int c = 0; c < channels_; ++c) pcm[i * static_cast<size_t>(channels_) + static_cast<size_t>(c)] = s;
    }
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file_);
    frames_ += frames;
}

void WavWriter::close() {
    if (!file_) return;
    const uint32_t dataBytes = static_cast<uint32_t>(frames_ * static_cast<uint64_t>(channels_) * 2ull);
    writeHeader(file_, sampleRate_, channels_, dataBytes);
    std::fclose(file_);
    file_ = nullptr;
}

bool IqWriter::open(const std::string& path, std::string& err) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        err = "impossible de creer " + path;
        return false;
    }
    path_ = path;
    samples_ = 0;
    return true;
}

void IqWriter::write(const cfloat* iq, size_t n) {
    if (!file_ || n == 0) return;
    scratch_.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float re = std::clamp(iq[i].real(), -1.0f, 1.0f) * 127.4f + 127.4f;
        const float im = std::clamp(iq[i].imag(), -1.0f, 1.0f) * 127.4f + 127.4f;
        scratch_[i * 2 + 0] = static_cast<uint8_t>(std::clamp(re, 0.0f, 255.0f));
        scratch_[i * 2 + 1] = static_cast<uint8_t>(std::clamp(im, 0.0f, 255.0f));
    }
    std::fwrite(scratch_.data(), 1, scratch_.size(), file_);
    samples_ += n;
}

void IqWriter::close() {
    if (!file_) return;
    std::fclose(file_);
    file_ = nullptr;
}

} // namespace sdr
