// SPDX-License-Identifier: MIT

#include "PcAudioModule.hpp"
#include "PcAudio.hpp"
#include "PcAudioInput.hpp"

#include <crosspad/synth/ISynthEngine.hpp>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ── PcOutputStream (int32 → int16 adapter for PcAudioOutput) ──────────────

uint32_t PcOutputStream::pushSamples(const int32_t* samples, uint32_t frames) {
    if (!device_ || !device_->isOpen()) return 0;

    // Convert int32 interleaved [frame][2] → int16 interleaved for PcAudioOutput
    // int32 is the I2S-native format; we right-shift 16 bits for int16 output
    static thread_local int16_t convBuf[512 * 2];
    uint32_t todo = std::min(frames, (uint32_t)512);

    for (uint32_t i = 0; i < todo * 2; i++) {
        int32_t s = samples[i] >> 16;
        convBuf[i] = static_cast<int16_t>(std::clamp(s, -32768, 32767));
    }

    return device_->write(convBuf, todo);
}

bool PcOutputStream::isOpen() const {
    return device_ && device_->isOpen();
}

// ── PcInputStream (int16 → int32 adapter for PcAudioInput) ────────────────

uint32_t PcInputStream::pullSamples(int32_t* samples, uint32_t frames) {
    if (!device_ || !device_->isOpen()) return 0;

    static thread_local int16_t convBuf[512 * 2];
    uint32_t todo = std::min(frames, (uint32_t)512);

    uint32_t read = device_->read(convBuf, todo);
    for (uint32_t i = 0; i < read * 2; i++) {
        samples[i] = static_cast<int32_t>(convBuf[i]) << 16;
    }
    return read;
}

bool PcInputStream::isOpen() const {
    return device_ && device_->isOpen();
}

// ── PcAudioModule ─────────────────────────────────────────────────────────

PcAudioModule::~PcAudioModule() {
    stop();
}

bool PcAudioModule::setup(const crosspad::AudioModuleConfig& config) {
    config_ = config;

    if (config_.frameCount > MAX_FRAMES) {
        printf("[PcAudioModule] frameCount %u exceeds MAX_FRAMES %u, clamping\n",
               config_.frameCount, MAX_FRAMES);
        config_.frameCount = MAX_FRAMES;
    }

    // Zero all DSP buffers
    std::memset(dspBuffer_, 0, sizeof(dspBuffer_));
    std::memset(i2sBuffer_, 0, sizeof(i2sBuffer_));
    std::memset(synthBuf_, 0, sizeof(synthBuf_));

    printf("[PcAudioModule] Setup: %u Hz, %u frames, %u streams, %u channels\n",
           config_.sampleRate, config_.frameCount,
           config_.streamCount, config_.channelCount);
    return true;
}

void PcAudioModule::teardown() {
    stop();
}

crosspad::IAudioStream* PcAudioModule::getOutputStream(uint8_t index) {
    return (index < NUM_OUTPUTS) ? &outputs_[index] : nullptr;
}

crosspad::IAudioStream* PcAudioModule::getInputStream(uint8_t index) {
    return (index < NUM_INPUTS) ? &inputs_[index] : nullptr;
}

void PcAudioModule::getOutputLevel(uint8_t stream, int16_t& left, int16_t& right) const {
    if (stream < NUM_OUTPUTS) {
        left  = outPeakL_[stream].exchange(0, std::memory_order_relaxed);
        right = outPeakR_[stream].exchange(0, std::memory_order_relaxed);
    } else {
        left = right = 0;
    }
}

void PcAudioModule::getInputLevel(uint8_t stream, int16_t& left, int16_t& right) const {
    if (stream < NUM_INPUTS) {
        left  = inPeakL_[stream].exchange(0, std::memory_order_relaxed);
        right = inPeakR_[stream].exchange(0, std::memory_order_relaxed);
    } else {
        left = right = 0;
    }
}

void PcAudioModule::setOutputStream(uint8_t index, PcAudioOutput* device) {
    if (index < NUM_OUTPUTS) {
        outputs_[index].setDevice(device);
    }
}

void PcAudioModule::setInputStream(uint8_t index, PcAudioInput* device) {
    if (index < NUM_INPUTS) {
        inputs_[index].setDevice(device);
    }
}

// ── Thread management ─────────────────────────────────────────────────────

void PcAudioModule::start() {
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::make_unique<std::thread>(&PcAudioModule::audioThreadFunc, this);
    printf("[PcAudioModule] Audio thread started\n");
}

void PcAudioModule::stop() {
    running_.store(false);
    if (thread_ && thread_->joinable()) {
        thread_->join();
        thread_.reset();
        printf("[PcAudioModule] Audio thread stopped\n");
    }
}

void PcAudioModule::audioThreadFunc() {
    // Pace the loop based on frame duration (same as Marcel's tight loop + I2S blocking)
    const auto frameDuration = std::chrono::microseconds(
        (uint64_t)config_.frameCount * 1000000 / config_.sampleRate);

    while (running_.load(std::memory_order_relaxed)) {
        auto start = std::chrono::steady_clock::now();

        process();

        // Sleep for remainder of frame period (avoid busy-wait)
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remaining = frameDuration - elapsed;
        if (remaining.count() > 0) {
            std::this_thread::sleep_for(remaining);
        }
    }
}

// ── Process cycle (mirrors Marcel's AudioModule_Process) ──────────────────

void PcAudioModule::process() {
    pullInputs();
    generate();
    control();
    pushOutputs();
}

void PcAudioModule::pullInputs() {
    const uint32_t frames = config_.frameCount;

    for (uint8_t i = 0; i < NUM_INPUTS; i++) {
        if (inputs_[i].isOpen()) {
            int32_t tempBuf[MAX_FRAMES][2];
            uint32_t read = inputs_[i].pullSamples(&tempBuf[0][0], frames);

            // Track input peaks
            int16_t peakL = 0, peakR = 0;
            for (uint32_t f = 0; f < read; f++) {
                int16_t l = static_cast<int16_t>(tempBuf[f][0] >> 16);
                int16_t r = static_cast<int16_t>(tempBuf[f][1] >> 16);
                if (std::abs(l) > peakL) peakL = std::abs(l);
                if (std::abs(r) > peakR) peakR = std::abs(r);
            }

            // Update peak atomics (max of current and new)
            int16_t curL = inPeakL_[i].load(std::memory_order_relaxed);
            if (peakL > curL) inPeakL_[i].store(peakL, std::memory_order_relaxed);
            int16_t curR = inPeakR_[i].load(std::memory_order_relaxed);
            if (peakR > curR) inPeakR_[i].store(peakR, std::memory_order_relaxed);
        }
    }
}

void PcAudioModule::generate() {
    const uint32_t frames = config_.frameCount;

    // Clear DSP buffers
    std::memset(dspBuffer_, 0, sizeof(int32_t) * NUM_OUTPUTS * 2 * frames);

    // Run synth engine if available — generates into stream 0
    if (synth_) {
        // ISynthEngine doesn't have a process() method that fills a buffer;
        // it's note-driven. The actual sample generation happens in the synth's
        // internal thread. We just track levels here.
        // TODO: When ISynthEngine gets a process(buffer, frames) method,
        //       call it here to fill dspBuffer_[0][ch][frame].
    }
}

void PcAudioModule::control() {
    // Runtime control logic placeholder
    // (Marcel uses this for volume ramps, mute-after-timeout, etc.)
}

void PcAudioModule::pushOutputs() {
    const uint8_t streamCount = std::min(config_.streamCount, NUM_OUTPUTS);

    for (uint8_t s = 0; s < streamCount; s++) {
        if (!outputs_[s].isOpen()) continue;

        interleave(s);
        updatePeaks(s);
        outputs_[s].pushSamples(&i2sBuffer_[0][0], config_.frameCount);
    }
}

void PcAudioModule::interleave(uint8_t stream) {
    // Convert separate channel buffers → interleaved [frame][channel]
    // Mirrors Marcel's AudioDSP_Interleave()
    const uint32_t frames = config_.frameCount;

    for (uint32_t f = 0; f < frames; f++) {
        i2sBuffer_[f][0] = dspBuffer_[stream][0][f];
        i2sBuffer_[f][1] = dspBuffer_[stream][1][f];
    }
}

void PcAudioModule::updatePeaks(uint8_t stream) {
    const uint32_t frames = config_.frameCount;
    int16_t peakL = 0, peakR = 0;

    for (uint32_t f = 0; f < frames; f++) {
        int16_t l = static_cast<int16_t>(i2sBuffer_[f][0] >> 16);
        int16_t r = static_cast<int16_t>(i2sBuffer_[f][1] >> 16);
        if (std::abs(l) > peakL) peakL = std::abs(l);
        if (std::abs(r) > peakR) peakR = std::abs(r);
    }

    // Store max peak (read resets to 0 via exchange)
    int16_t curL = outPeakL_[stream].load(std::memory_order_relaxed);
    if (peakL > curL) outPeakL_[stream].store(peakL, std::memory_order_relaxed);
    int16_t curR = outPeakR_[stream].load(std::memory_order_relaxed);
    if (peakR > curR) outPeakR_[stream].store(peakR, std::memory_order_relaxed);
}
