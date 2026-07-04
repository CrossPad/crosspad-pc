// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PwVirtualSinkCapture.hpp
 * @brief Native PipeWire Audio/Sink stream — exposes a CrossPad virtual
 *        input directly as an IAudioInput, without going through a
 *        PulseAudio ".monitor" source round-trip.
 *
 * Only PipeWire-gated translation units include this header (guarded by
 * __linux__), so pulling in <pipewire/stream.h> here is acceptable — it
 * never leaks into non-Linux or non-PipeWire builds.
 */

#ifdef __linux__

#include <crosspad/synth/IAudioInput.hpp>
#include <crosspad/audio/AudioRingBuffer.hpp>

#include <pipewire/stream.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace crosspad_pc {

class PwVirtualSinkCapture : public crosspad::IAudioInput {
public:
    PwVirtualSinkCapture() = default;
    ~PwVirtualSinkCapture() override;

    PwVirtualSinkCapture(const PwVirtualSinkCapture&) = delete;
    PwVirtualSinkCapture& operator=(const PwVirtualSinkCapture&) = delete;

    /// Create and connect an Audio/Sink pw_stream named @p nodeName. Blocks
    /// (bounded) until the stream reaches PAUSED/STREAMING. Returns false on
    /// daemon-unreachable, stream-connect failure, or timeout while still
    /// CONNECTING.
    bool start(const char* nodeName, const char* description,
               uint32_t sampleRate, uint32_t bufferFrames = 256);

    /// Disconnects + destroys the stream. Idempotent.
    void stop();

    bool isOpen() const { return open_.load(std::memory_order_acquire); }

    // crosspad::IAudioInput
    uint32_t read(int16_t* interleavedSamples, uint32_t frameCount) override;
    uint32_t getSampleRate() const override { return sampleRate_; }
    uint32_t getBufferSize() const override { return bufferFrames_; }
    void getInputLevel(int16_t& left, int16_t& right) const override;

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

    std::atomic<int16_t> peakL_{0};
    std::atomic<int16_t> peakR_{0};

public:
    // Diagnostic counters (RT thread increments; non-RT readers report).
    std::atomic<uint32_t> diagRingOverflows_{0};  // onProcess couldn't write all samples
    std::atomic<uint32_t> diagProcessCalls_{0};
};

} // namespace crosspad_pc

#endif // __linux__
