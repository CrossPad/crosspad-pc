// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PcAudioModule.hpp
 * @brief PC implementation of IAudioModule using RtAudio streams.
 *
 * Wraps dual PcAudioOutput devices as IAudioStreams and runs the
 * audio processing pipeline (pull inputs → DSP/mix → push outputs)
 * on a dedicated std::thread — the PC equivalent of Marcel's
 * audio_task on 2playerCrosspad (P4).
 */

#include <crosspad/audio/IAudioModule.hpp>
#include <atomic>
#include <thread>
#include <memory>

class PcAudioOutput;
class PcAudioInput;

namespace crosspad {
class ISynthEngine;
}

/**
 * @brief RtAudio-backed IAudioStream — adapts PcAudioOutput to the shared stream interface.
 */
class PcOutputStream : public crosspad::IAudioStream {
public:
    explicit PcOutputStream(PcAudioOutput* device = nullptr) : device_(device) {}

    void setDevice(PcAudioOutput* device) { device_ = device; }
    PcAudioOutput* getDevice() const { return device_; }

    uint32_t pushSamples(const int32_t* samples, uint32_t frames) override;
    bool isOpen() const override;

private:
    PcAudioOutput* device_ = nullptr;
};

/**
 * @brief RtAudio-backed input stream — adapts PcAudioInput to the shared stream interface.
 */
class PcInputStream : public crosspad::IAudioStream {
public:
    explicit PcInputStream(PcAudioInput* device = nullptr) : device_(device) {}

    void setDevice(PcAudioInput* device) { device_ = device; }
    PcAudioInput* getDevice() const { return device_; }

    uint32_t pushSamples(const int32_t*, uint32_t) override { return 0; }
    uint32_t pullSamples(int32_t* samples, uint32_t frames) override;
    bool isOpen() const override;

private:
    PcAudioInput* device_ = nullptr;
};

/**
 * @brief PC audio processing pipeline.
 *
 * Implements the same setup/process pattern as Marcel's audio_module on P4,
 * but uses RtAudio instead of I2S and std::thread instead of FreeRTOS task.
 *
 * Streams:
 *   Output 0 = OUT1 (main, also feeds VU meter)
 *   Output 1 = OUT2 (secondary)
 *   Input 0  = IN1
 *   Input 1  = IN2
 */
class PcAudioModule : public crosspad::IAudioModule {
public:
    static constexpr uint8_t NUM_OUTPUTS = 2;
    static constexpr uint8_t NUM_INPUTS  = 2;

    PcAudioModule() = default;
    ~PcAudioModule() override;

    // ── IAudioModule ──────────────────────────────────────────────

    bool setup(const crosspad::AudioModuleConfig& config) override;
    void process() override;
    void teardown() override;

    const crosspad::AudioModuleConfig& getConfig() const override { return config_; }

    crosspad::IAudioStream* getOutputStream(uint8_t index) override;
    crosspad::IAudioStream* getInputStream(uint8_t index) override;

    void getOutputLevel(uint8_t stream, int16_t& left, int16_t& right) const override;
    void getInputLevel(uint8_t stream, int16_t& left, int16_t& right) const override;

    // ── PC-specific ───────────────────────────────────────────────

    /// Set output device (call before or after setup)
    void setOutputStream(uint8_t index, PcAudioOutput* device);

    /// Set input device
    void setInputStream(uint8_t index, PcAudioInput* device);

    /// Set synth engine (for DSP generate step)
    void setSynthEngine(crosspad::ISynthEngine* synth) { synth_ = synth; }

    /// Start the audio processing thread
    void start();

    /// Stop the audio processing thread
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    crosspad::AudioModuleConfig config_{};

    PcOutputStream outputs_[NUM_OUTPUTS];
    PcInputStream  inputs_[NUM_INPUTS];

    crosspad::ISynthEngine* synth_ = nullptr;

    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;

    // DSP buffers (allocated in setup)
    static constexpr uint32_t MAX_FRAMES = 512;
    int32_t dspBuffer_[NUM_OUTPUTS][2][MAX_FRAMES]{}; // [stream][channel][frame]
    int32_t i2sBuffer_[MAX_FRAMES][2]{};              // interleaved [frame][channel]
    int16_t synthBuf_[MAX_FRAMES * 2]{};              // synth int16 interleaved

    // Peak metering (mutable: read-and-reset from const getLevel methods)
    mutable std::atomic<int16_t> outPeakL_[NUM_OUTPUTS]{};
    mutable std::atomic<int16_t> outPeakR_[NUM_OUTPUTS]{};
    mutable std::atomic<int16_t> inPeakL_[NUM_INPUTS]{};
    mutable std::atomic<int16_t> inPeakR_[NUM_INPUTS]{};

    void audioThreadFunc();

    // Process sub-steps (mirrors Marcel's pattern)
    void pullInputs();
    void generate();
    void control();
    void pushOutputs();
    void interleave(uint8_t stream);
    void updatePeaks(uint8_t stream);
};
