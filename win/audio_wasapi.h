// SDRStudio - event driven WASAPI renderer (48 kHz, 32-bit float, stereo).
#pragma once

#include <atomic>
#include <string>

#include "../core/ring.h"

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // Starts the render thread. Fails if no output device can be opened.
    bool start(std::string& err);
    void stop();
    bool running() const;

    // Mono float samples written by the DSP thread, consumed by WASAPI.
    sdr::SpscRing<float>& ring();
    unsigned underruns() const;
    unsigned bufferFrames() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
