// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PcAudioModule.hpp
 * @brief PC IAudioModule built on AbstractAudioModule.
 *
 * Wraps RtAudio output devices (PcAudioOutput) as IAudioStream instances
 * and runs the audio pipeline on a dedicated std::thread. DSP is done in
 * float by the node chain inherited from AbstractAudioModule; conversion
 * to the stream's native format happens only at push boundary.
 *
 * Optional AudioMixerEngine override: when set, process() bypasses the
 * default node chain and hands rendering to mixer.render(out0, out1, frames),
 * giving the mixer its full per-stream routing matrix in a single-writer
 * topology. AbstractAudioModule.bus_ is unused in that mode; per-stream
 * float buffers live here as mixerBus_.
 */

#include <crosspad/audio/AbstractAudioModule.hpp>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>

class PcAudioOutput;
class PcAudioInput;
class AudioMixerEngine;

namespace crosspad_pc {

/**
 * @brief IAudioStream wrapping a PcAudioOutput device.
 *
 * PcAudioOutput::write takes int16 interleaved, so this stream reports
 * Int16 as the only supported format. AbstractAudioModule converts from
 * the float bus at the push boundary.
 */
class PcRtAudioOutputStream : public crosspad::IAudioStream {
public:
    explicit PcRtAudioOutputStream(PcAudioOutput* device = nullptr) : device_(device) {}

    void setDevice(PcAudioOutput* device) { device_ = device; }
    PcAudioOutput* getDevice() const { return device_; }

    uint32_t supportedFormats() const override;
    uint32_t pushSamples(const void* samples, crosspad::AudioFormat fmt, uint32_t frames) override;
    bool isOpen() const override;
    uint32_t getSampleRate() const override;

private:
    PcAudioOutput* device_ = nullptr;
};

/**
 * @brief PC audio processing pipeline.
 *
 * Inherits the float bus + node chain default pipeline from
 * AbstractAudioModule. Owns two output streams (OUT1/OUT2). Runs
 * process() on a dedicated thread paced to frame duration.
 */
class PcAudioModule : public crosspad::AbstractAudioModule {
public:
    static constexpr uint8_t NUM_OUTPUTS = 2;

    PcAudioModule() = default;
    ~PcAudioModule() override;

    bool setup(const crosspad::AudioModuleConfig& config) override;
    void teardown() override;
    void process() override;

    crosspad::IAudioStream* getOutputStream(uint8_t index) override;

    // ── PC-specific ───────────────────────────────────────────────

    /// Set output device (call before or after setup)
    void setOutputDevice(uint8_t index, PcAudioOutput* device);

    /// Install the mixer engine. When set, process() routes through
    /// mixer.render(out0, out1, frames) instead of the default node chain.
    /// Pass nullptr to fall back to the node chain.
    void setMixerEngine(AudioMixerEngine* mixer) { mixer_ = mixer; }

    /// Start the audio processing thread
    void start();

    /// Stop the audio processing thread
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    PcRtAudioOutputStream outputs_[NUM_OUTPUTS];
    AudioMixerEngine* mixer_ = nullptr;

    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;

    // Per-stream float buses for mixer-driven mode (sized in setup).
    std::vector<float>   mixerBus_[NUM_OUTPUTS];
    std::vector<int16_t> pushScratchInt16_[NUM_OUTPUTS];

    // Ring overflow tracking and rate-limited logging
    uint32_t overflowCount_ = 0;
    uint32_t overflowLogEvery_ = 400;  // ~1 second at 128 frames/48kHz

    void audioThreadFunc();
    void processMixer();
};

} // namespace crosspad_pc
