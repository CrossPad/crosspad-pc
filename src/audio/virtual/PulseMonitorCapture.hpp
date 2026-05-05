#pragma once

/**
 * @file PulseMonitorCapture.hpp
 * @brief Captures audio from a named PulseAudio/PipeWire source (e.g. a
 *        virtual sink's ".monitor" source) using libpulse-simple.
 *
 * We bypass RtAudio here because its PULSE backend aggregates sinks by card
 * and cannot address individual null-sink monitor sources by name. Using
 * pa_simple_new() directly lets us bind exactly to "crosspad_vin1.monitor"
 * and "crosspad_vin2.monitor", which is what the virtual-mixer design needs.
 */

#ifdef __linux__

#include <crosspad/synth/IAudioInput.hpp>
#include <crosspad/audio/AudioRingBuffer.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct pa_simple;

namespace crosspad_pc {

class PulseMonitorCapture : public crosspad::IAudioInput {
public:
    PulseMonitorCapture();
    ~PulseMonitorCapture() override;

    PulseMonitorCapture(const PulseMonitorCapture&) = delete;
    PulseMonitorCapture& operator=(const PulseMonitorCapture&) = delete;

    /// Open @p sourceName (e.g. "crosspad_vin1.monitor") and start a
    /// background capture thread. Returns false if the source can't be opened.
    bool start(const std::string& sourceName,
               uint32_t sampleRate = 48000,
               uint32_t bufferFrames = 256);

    void stop();

    /// Non-blocking shutdown for process-exit paths. Signals the capture thread
    /// to stop and detaches it — safe when the process is about to _Exit().
    /// pa_simple_read() has no cancellation primitive on PipeWire's pulse shim,
    /// so a plain join() can deadlock even after the null-sink module is
    /// unloaded; detaching lets the kernel reap the thread on process exit.
    void detachForShutdown();

    bool isOpen() const { return running_.load(std::memory_order_acquire); }
    const std::string& sourceName() const { return sourceName_; }

    // crosspad::IAudioInput
    uint32_t read(int16_t* interleavedSamples, uint32_t frameCount) override;
    uint32_t getSampleRate() const override { return sampleRate_; }
    uint32_t getBufferSize() const override { return bufferFrames_; }
    void getInputLevel(int16_t& left, int16_t& right) const override;

private:
    void captureLoop();

    pa_simple*              pa_ = nullptr;
    std::thread             thread_;
    std::atomic<bool>       running_{false};
    crosspad::AudioRingBuffer<int16_t> ring_;

    uint32_t    sampleRate_    = 48000;
    uint32_t    bufferFrames_  = 256;
    std::string sourceName_;

    std::atomic<int16_t> peakL_{0};
    std::atomic<int16_t> peakR_{0};
};

} // namespace crosspad_pc

#endif // __linux__
