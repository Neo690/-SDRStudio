// SDRStudio - portable DSP core
// Radix-2 complex FFT. No dependencies, no platform APIs.
#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace sdr {

constexpr double kPi = 3.14159265358979323846;

using cfloat = std::complex<float>;

// In-place iterative radix-2 FFT. Size must be a power of two.
class Fft {
public:
    explicit Fft(int size) : size_(size), twiddles_(static_cast<size_t>(size / 2)) {
        for (int i = 0; i < size_ / 2; ++i) {
            const double a = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(size_);
            twiddles_[static_cast<size_t>(i)] =
                cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
        }
    }

    int size() const { return size_; }

    void forward(cfloat* d) const {
        for (int i = 1, j = 0; i < size_; ++i) {
            int bit = size_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                const cfloat t = d[i];
                d[i] = d[j];
                d[j] = t;
            }
        }
        for (int len = 2; len <= size_; len <<= 1) {
            const int half = len >> 1;
            const int step = size_ / len;
            for (int base = 0; base < size_; base += len) {
                for (int k = 0; k < half; ++k) {
                    const cfloat w = twiddles_[static_cast<size_t>(k * step)];
                    const cfloat u = d[base + k];
                    const cfloat v = d[base + k + half] * w;
                    d[base + k] = u + v;
                    d[base + k + half] = u - v;
                }
            }
        }
    }

private:
    int size_ = 0;
    std::vector<cfloat> twiddles_;
};

} // namespace sdr
