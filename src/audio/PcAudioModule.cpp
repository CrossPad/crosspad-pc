// SPDX-License-Identifier: MIT

#include "PcAudioModule.hpp"
#include "PcAudio.hpp"

#include <crosspad/audio/AudioFormatConvert.hpp>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace crosspad_pc {

// ── PcRtAudioOutputStream ──────────────────────────────────────────────────

uint32_t PcRtAudioOutputStream::supportedFormats() const {
    // PcAudioOutput::write takes int16 interleaved.
    return crosspad::audioFormatMask(crosspad::AudioFormat::Int16);
}

uint32_t PcRtAudioOutputStream::pushSamples(const void* samples,
                                            crosspad::AudioFormat fmt,
                                            uint32_t frames) {
    if (!device_ || !device_->isOpen()) return 0;
    if (fmt != crosspad::AudioFormat::Int16) return 0;
    return device_->write(static_cast<const int16_t*>(samples), frames);
}

bool PcRtAudioOutputStream::isOpen() const {
    return device_ && device_->isOpen();
}

uint32_t PcRtAudioOutputStream::getSampleRate() const {
    return device_ ? device_->getSampleRate() : 0;
}

// ── PcAudioModule ──────────────────────────────────────────────────────────

PcAudioModule::~PcAudioModule() {
    stop();
}

bool PcAudioModule::setup(const crosspad::AudioModuleConfig& config) {
    if (!AbstractAudioModule::setup(config)) return false;
    printf("[PcAudioModule] Setup: %u Hz, %u frames, %u streams, %u channels\n",
           config_.sampleRate, config_.frameCount,
           config_.streamCount, config_.channelCount);
    return true;
}

void PcAudioModule::teardown() {
    stop();
    AbstractAudioModule::teardown();
}

crosspad::IAudioStream* PcAudioModule::getOutputStream(uint8_t index) {
    return (index < NUM_OUTPUTS) ? &outputs_[index] : nullptr;
}

void PcAudioModule::setOutputDevice(uint8_t index, PcAudioOutput* device) {
    if (index < NUM_OUTPUTS) {
        outputs_[index].setDevice(device);
    }
}

// ── Thread management ──────────────────────────────────────────────────────

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
    const auto frameDuration = std::chrono::microseconds(
        static_cast<uint64_t>(config_.frameCount) * 1000000 / config_.sampleRate);

    while (running_.load(std::memory_order_relaxed)) {
        auto start = std::chrono::steady_clock::now();

        process();

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remaining = frameDuration - elapsed;
        if (remaining.count() > 0) {
            std::this_thread::sleep_for(remaining);
        }
    }
}

} // namespace crosspad_pc
