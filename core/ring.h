// SDRStudio - lock-free single-producer / single-consumer ring buffer.
// Used between the capture thread, the DSP thread and the audio thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace sdr {

template <typename T>
class SpscRing {
public:
    // capacity is rounded up to a power of two.
    explicit SpscRing(size_t capacity = 1 << 16) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    size_t capacity() const { return buf_.size(); }

    // Producer side. Returns the number of elements actually written.
    size_t write(const T* src, size_t n) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t free_ = capacity() - (h - t);
        if (n > free_) n = free_;
        if (n == 0) return 0;
        const size_t idx = h & mask_;
        const size_t first = (n < capacity() - idx) ? n : capacity() - idx;
        std::memcpy(&buf_[idx], src, first * sizeof(T));
        if (n > first) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(T));
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Returns the number of elements actually read.
    size_t read(T* dst, size_t n) {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t avail = h - t;
        if (n > avail) n = avail;
        if (n == 0) return 0;
        const size_t idx = t & mask_;
        const size_t first = (n < capacity() - idx) ? n : capacity() - idx;
        std::memcpy(dst, &buf_[idx], first * sizeof(T));
        if (n > first) std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(T));
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::vector<T> buf_;
    size_t mask_ = 0;
    std::atomic<size_t> head_{0}; // written by the producer
    std::atomic<size_t> tail_{0}; // written by the consumer
};

} // namespace sdr
