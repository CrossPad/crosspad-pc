// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file audio_test_helpers.hpp
 * @brief Mocks for audio pipeline tests (Tier 2 + Tier 3).
 */

#include <crosspad/audio/IAudioModule.hpp>
#include <crosspad/synth/IAudioInput.hpp>
#include <crosspad/synth/ISynthEngine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace crosspad_test {

// ── Mock IAudioInput — returns a constant int16 sample on read() ─────────

class ConstAudioInput : public crosspad::IAudioInput {
public:
    explicit ConstAudioInput(int16_t sample = 0) : sample_(sample) {}

    uint32_t read(int16_t* dst, uint32_t frames) override {
        const uint32_t n = frames * 2;
        for (uint32_t i = 0; i < n; ++i) dst[i] = sample_;
        ++readCalls;
        return frames;
    }
    uint32_t getSampleRate() const override { return 48000; }
    uint32_t getBufferSize() const override { return 256; }
    void getInputLevel(int16_t& l, int16_t& r) const override { l = 0; r = 0; }

    void setSample(int16_t s) { sample_ = s; }

    int readCalls = 0;
private:
    int16_t sample_;
};

// ── Minimal ISynthEngine that fills float buffer with constant value ─────

class ConstSynth : public crosspad::ISynthEngine {
public:
    explicit ConstSynth(float v = 0.0f) : value_(v) {}

    void process(float* dst, uint32_t frames) override {
        for (uint32_t i = 0; i < frames * 2; ++i) dst[i] = value_;
        ++processCalls;
    }

    void setValue(float v) { value_ = v; }
    int processCalls = 0;

    // Stubs for pure-virtuals
    void init() override {}
    void cleanup() override {}
    void noteOn(uint8_t, uint8_t) override {}
    void noteOff(uint8_t) override {}
    void setPitchBend(int16_t) override {}
    void setAttack(float) override {}
    void setDecay(float) override {}
    void setSustain(uint8_t) override {}
    uint8_t getSustain() override { return 0; }
    void setRelease(float) override {}
    void setFilterCutoff(float) override {}
    void setFilterReso(float) override {}
    void setWaveform(uint8_t, uint8_t) override {}
    void setOscVolume(uint8_t, float) override {}
    void setOscPitch(uint8_t, float) override {}
    void setDelayEnabled(bool) override {}
    void setDelayTime(float) override {}
    void setDelayFeedback(float) override {}
    void setDelayMix(float) override {}
    void setReverbEnabled(bool) override {}
    void setReverbDecay(float) override {}
    void setReverbMix(float) override {}
    void getLevel(int16_t& l, int16_t& r) override { l = 0; r = 0; }

private:
    float value_;
};

// ── ISynthEngine that emits a deterministic sine wave (for goldens) ──────

class SineSynth : public crosspad::ISynthEngine {
public:
    SineSynth(float freqHz = 440.0f, float amplitude = 0.5f, uint32_t sampleRate = 48000)
        : freq_(freqHz), amp_(amplitude), sr_(sampleRate) {}

    void process(float* dst, uint32_t frames) override {
        const float twoPi = 6.283185307179586f;
        const float dPhase = twoPi * freq_ / static_cast<float>(sr_);
        for (uint32_t i = 0; i < frames; ++i) {
            const float v = amp_ * std::sin(phase_);
            dst[i * 2 + 0] = v;
            dst[i * 2 + 1] = v;
            phase_ += dPhase;
            if (phase_ > twoPi) phase_ -= twoPi;
        }
    }

    void resetPhase() { phase_ = 0.0f; }
    void setSampleRate(uint32_t sr) { sr_ = sr; }

    // Pure-virtual stubs
    void init() override {}
    void cleanup() override {}
    void noteOn(uint8_t, uint8_t) override {}
    void noteOff(uint8_t) override {}
    void setPitchBend(int16_t) override {}
    void setAttack(float) override {}
    void setDecay(float) override {}
    void setSustain(uint8_t) override {}
    uint8_t getSustain() override { return 0; }
    void setRelease(float) override {}
    void setFilterCutoff(float) override {}
    void setFilterReso(float) override {}
    void setWaveform(uint8_t, uint8_t) override {}
    void setOscVolume(uint8_t, float) override {}
    void setOscPitch(uint8_t, float) override {}
    void setDelayEnabled(bool) override {}
    void setDelayTime(float) override {}
    void setDelayFeedback(float) override {}
    void setDelayMix(float) override {}
    void setReverbEnabled(bool) override {}
    void setReverbDecay(float) override {}
    void setReverbMix(float) override {}
    void getLevel(int16_t& l, int16_t& r) override { l = 0; r = 0; }

private:
    float freq_, amp_;
    uint32_t sr_;
    float phase_ = 0.0f;
};

// ── Mock IAudioStream that captures pushed Int16 frames ──────────────────

class CapturingInt16Stream : public crosspad::IAudioStream {
public:
    uint32_t supportedFormats() const override {
        return crosspad::audioFormatMask(crosspad::AudioFormat::Int16);
    }
    uint32_t pushSamples(const void* samples, crosspad::AudioFormat fmt, uint32_t frames) override {
        if (fmt != crosspad::AudioFormat::Int16) return 0;
        const int16_t* src = static_cast<const int16_t*>(samples);
        captured.insert(captured.end(), src, src + frames * 2);
        ++pushes;
        return frames;
    }
    bool isOpen() const override { return open; }
    uint32_t getSampleRate() const override { return 48000; }

    bool open = true;
    int  pushes = 0;
    std::vector<int16_t> captured;
};

} // namespace crosspad_test
