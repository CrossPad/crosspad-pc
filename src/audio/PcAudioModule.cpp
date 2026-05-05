// SPDX-License-Identifier: MIT

#include "PcAudioModule.hpp"
#include "PcAudio.hpp"

#include <crosspad/audio/AudioFormatConvert.hpp>
#include <crosspad-mixer/AudioMixerEngine.hpp>

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
    const size_t samples = static_cast<size_t>(config_.frameCount) * 2;
    for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) {
        mixerBus_[s].assign(samples, 0.0f);
        pushScratchInt16_[s].assign(samples, 0);
    }
    printf("[PcAudioModule] Setup: %u Hz, %u frames, %u streams, %u channels\n",
           config_.sampleRate, config_.frameCount,
           config_.streamCount, config_.channelCount);
    return true;
}

void PcAudioModule::process() {
    if (mixer_) {
        processMixer();
    } else {
        AbstractAudioModule::process();
    }
}

void PcAudioModule::processMixer() {
    const uint32_t frames  = config_.frameCount;
    const uint32_t samples = frames * 2;

    mixer_->render(mixerBus_[0].data(), mixerBus_[1].data(), frames);

    for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) {
        crosspad::IAudioStream* stream = getOutputStream(s);
        if (!stream || !stream->isOpen()) continue;
        crosspad::floatToInt16(mixerBus_[s].data(),
                               pushScratchInt16_[s].data(),
                               samples);
        stream->pushSamples(pushScratchInt16_[s].data(),
                            crosspad::AudioFormat::Int16, frames);
    }

    // Peak meter on OUT1 (matches existing VU meter wiring)
    const float* bus0 = mixerBus_[0].data();
    float pl = 0.0f, pr = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float l = bus0[i * 2 + 0]; if (l < 0) l = -l;
        float r = bus0[i * 2 + 1]; if (r < 0) r = -r;
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    peakL_ = pl;
    peakR_ = pr;
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
