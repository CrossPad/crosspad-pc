/**
 * @file PulseMonitorCapture.cpp
 * @brief Background-thread PulseAudio/PipeWire monitor source capture.
 */

#ifdef __linux__

#include "PulseMonitorCapture.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/sample.h>

namespace crosspad_pc {

PulseMonitorCapture::PulseMonitorCapture() = default;

PulseMonitorCapture::~PulseMonitorCapture() {
    stop();
}

bool PulseMonitorCapture::start(const std::string& sourceName,
                                 uint32_t sampleRate, uint32_t bufferFrames) {
    if (running_.load(std::memory_order_acquire)) return true;
    if (sourceName.empty()) return false;

    sourceName_   = sourceName;
    sampleRate_   = sampleRate;
    bufferFrames_ = bufferFrames;

    pa_sample_spec ss{};
    ss.format   = PA_SAMPLE_S16LE;
    ss.rate     = sampleRate;
    ss.channels = 2;

    // Keep server-side buffering small so end-to-end latency stays under 20ms.
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(bufferFrames) * 4 /*bytes/frame*/ * 8;
    attr.fragsize  = static_cast<uint32_t>(bufferFrames) * 4;
    attr.tlength   = static_cast<uint32_t>(-1);
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = static_cast<uint32_t>(-1);

    int err = 0;
    pa_ = pa_simple_new(
        nullptr,                  // default server
        "CrossPad",               // application name
        PA_STREAM_RECORD,
        sourceName.c_str(),       // exact source (e.g. "crosspad_vin1.monitor")
        "CrossPad virtual input", // stream description
        &ss,
        nullptr,                  // default channel map
        &attr,
        &err
    );
    if (!pa_) {
        printf("[PulseCap] pa_simple_new('%s') failed: %s\n",
               sourceName.c_str(), pa_strerror(err));
        return false;
    }

    // Ring buffer sized for ~170ms of stereo audio at the requested buffer
    // depth — matches PcAudioInput's behaviour, enough to absorb jitter when
    // the consumer (mixer thread) is momentarily delayed.
    ring_.resize(bufferFrames_ * 2 * 32);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&PulseMonitorCapture::captureLoop, this);

    printf("[PulseCap] Capture started on '%s' (%u Hz, %u frames/buffer)\n",
           sourceName.c_str(), sampleRate_, bufferFrames_);
    return true;
}

void PulseMonitorCapture::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (thread_.joinable()) thread_.join();
    if (pa_) {
        pa_simple_free(pa_);
        pa_ = nullptr;
    }
    ring_.reset();
}

void PulseMonitorCapture::detachForShutdown() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    // Leak pa_ on purpose: freeing it while captureLoop() is still blocked
    // inside pa_simple_read() would be a use-after-free race. The process is
    // _Exit()-ing right after, so the kernel reclaims everything.
    if (thread_.joinable()) thread_.detach();
    pa_ = nullptr;
}

uint32_t PulseMonitorCapture::read(int16_t* out, uint32_t frameCount) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    size_t want = static_cast<size_t>(frameCount) * 2;
    size_t avail = ring_.available() & ~size_t(1);
    if (want > avail) want = avail;
    size_t got = ring_.read(out, want);
    return static_cast<uint32_t>(got / 2);
}

void PulseMonitorCapture::getInputLevel(int16_t& left, int16_t& right) const {
    left  = peakL_.load(std::memory_order_relaxed);
    right = peakR_.load(std::memory_order_relaxed);
}

void PulseMonitorCapture::captureLoop() {
    const size_t samples = static_cast<size_t>(bufferFrames_) * 2;
    std::vector<int16_t> buf(samples);

    while (running_.load(std::memory_order_acquire)) {
        int err = 0;
        if (pa_simple_read(pa_, buf.data(), samples * sizeof(int16_t), &err) < 0) {
            printf("[PulseCap] pa_simple_read failed: %s\n", pa_strerror(err));
            break;
        }

        ring_.write(buf.data(), samples);

        // Peak meter
        int16_t mL = 0, mR = 0;
        for (uint32_t i = 0; i < bufferFrames_; ++i) {
            int16_t l = buf[i * 2];
            int16_t r = buf[i * 2 + 1];
            if (l < 0) l = (l == INT16_MIN) ? INT16_MAX : -l;
            if (r < 0) r = (r == INT16_MIN) ? INT16_MAX : -r;
            if (l > mL) mL = l;
            if (r > mR) mR = r;
        }
        peakL_.store(mL, std::memory_order_relaxed);
        peakR_.store(mR, std::memory_order_relaxed);
    }
}

} // namespace crosspad_pc

#endif // __linux__
