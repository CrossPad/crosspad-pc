// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PwVirtualSource.hpp
 * @brief Native PipeWire Audio/Source/Virtual stream — exposes CrossPad's
 *        OUT1 mix as a virtual microphone ("CrossPad Out") that OBS/DAWs
 *        can pick up directly, without a loopback/monitor round-trip.
 *
 * Direction is OUTPUT (we produce audio; the graph pulls from us), the
 * inverse of PwVirtualSinkCapture's Audio/Sink INPUT direction. See
 * PwVirtualSinkCapture.hpp for the shared PipeWire stream lifecycle notes
 * (bounded state wait, stop()/stopLocked() idempotency).
 *
 * Only PipeWire-gated translation units include this header (guarded by
 * __linux__), so pulling in <pipewire/stream.h> here is acceptable — it
 * never leaks into non-Linux or non-PipeWire builds.
 */

#ifdef __linux__

#include <crosspad/audio/IAudioModule.hpp>
#include <crosspad/audio/AudioRingBuffer.hpp>

#include <pipewire/stream.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace crosspad_pc {

class PwVirtualSource : public crosspad::IAudioStream {
public:
    PwVirtualSource() = default;
    ~PwVirtualSource() override;

    PwVirtualSource(const PwVirtualSource&) = delete;
    PwVirtualSource& operator=(const PwVirtualSource&) = delete;

    /// Create and connect an Audio/Source/Virtual pw_stream named @p nodeName.
    /// Blocks (bounded) until the stream reaches PAUSED/STREAMING. Returns
    /// false on daemon-unreachable, stream-connect failure, or timeout while
    /// still CONNECTING.
    bool start(const char* nodeName, const char* description,
               uint32_t sampleRate, uint32_t bufferFrames = 256);

    /// Disconnects + destroys the stream. Idempotent.
    void stop();

    // crosspad::IAudioStream
    uint32_t supportedFormats() const override;
    uint32_t pushSamples(const void* samples, crosspad::AudioFormat fmt, uint32_t frames) override;
    bool isOpen() const override { return open_.load(std::memory_order_acquire); }
    uint32_t getSampleRate() const override { return sampleRate_; }

private:
    // pw_stream_events callbacks — RT thread. Ring buffer + atomics only:
    // no allocation, no locking, no logging from onProcess().
    static void onProcess(void* userdata);
    static void onStateChanged(void* userdata, enum pw_stream_state old,
                                enum pw_stream_state state, const char* error);

    // Tears the stream down assuming the PwContext lock is already held (or
    // not yet needed, e.g. mid-construction failure paths in start()).
    void stopLocked();

    struct pw_stream* stream_ = nullptr;
    struct spa_hook   listener_{};
    struct pw_stream_events events_{};

    crosspad::AudioRingBuffer<int16_t> ring_;
    std::atomic<bool> open_{false};

    uint32_t    sampleRate_   = 48000;
    uint32_t    bufferFrames_ = 256;
    std::string nodeName_;
};

} // namespace crosspad_pc

#endif // __linux__
