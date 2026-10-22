// SPDX-License-Identifier: GPL-3.0-or-later

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

#ifdef __EMSCRIPTEN__
extern "C" void wasm_audio_add_producer(void (*fn)(void*), void* ctx, uint32_t block);
extern "C" void wasm_audio_remove_producer(void* ctx);
#endif

PcAudioModule::~PcAudioModule() {
    stop();
}

bool PcAudioModule::setup(const crosspad::AudioModuleConfig& config) {
    if (!AbstractAudioModule::setup(config)) return false;
    const size_t samples = static_cast<size_t>(config_.frameCount) * 2;
    for (uint8_t s = 0; s < NUM_BUSES; ++s) mixerBus_[s].assign(samples, 0.0f);
    for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) pushScratchInt16_[s].assign(samples, 0);
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

void PcAudioModule::addNode(crosspad::IAudioNode* node) {
    if (!mixer_) {
        AbstractAudioModule::addNode(node);
        return;
    }
    if (!node) return;
    if (config_.sampleRate > 0) node->onPrepare(config_.frameCount, config_.sampleRate);
    for (auto& slot : appNodes_) {
        crosspad::IAudioNode* empty = nullptr;
        if (slot.compare_exchange_strong(empty, node, std::memory_order_acq_rel)) return;
    }
    printf("[PcAudioModule] no room for node %s\n", node->name());
}

void PcAudioModule::removeNode(crosspad::IAudioNode* node) {
    for (auto& slot : appNodes_) {
        crosspad::IAudioNode* expected = node;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
    AbstractAudioModule::removeNode(node);
}

void PcAudioModule::processMixer() {
    const uint32_t frames  = config_.frameCount;
    const uint32_t samples = frames * 2;

    float* outBuses[NUM_BUSES];
    for (uint8_t s = 0; s < NUM_BUSES; ++s) outBuses[s] = mixerBus_[s].data();
    mixer_->render(outBuses, NUM_BUSES, frames);
    for (auto& slot : appNodes_) {
        crosspad::IAudioNode* n = slot.load(std::memory_order_acquire);
        if (n && n->enabled() && n->isGenerator()) n->mix(mixerBus_[0].data(), frames);
    }
    if (CaptureTap tap = captureTap_.load(std::memory_order_acquire)) {
        tap(mixerBus_[CAPTURE_BUS].data(), frames);
    }

    for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) {
        // Convert unconditionally — even with no physical stream open, the
        // int16 buffer must stay current so the aux tap (below) sees fresh
        // data every cycle rather than stale/zeroed samples.
        crosspad::floatToInt16(mixerBus_[s].data(),
                               pushScratchInt16_[s].data(),
                               samples);

        crosspad::IAudioStream* stream = getOutputStream(s);
        if (!stream || !stream->isOpen()) continue;
        uint32_t pushed = stream->pushSamples(pushScratchInt16_[s].data(),
                                            crosspad::AudioFormat::Int16, frames);
        if (pushed < frames) {
            // Ring overflow — we are producing faster than RtAudio drains. Counted
            // every time, said at most once per overflowLogPeriod with the total
            // since the last line, so a sustained overrun is one line, not a stream.
            ++overflowCount_;
            overflowFrames_ += frames - pushed;
            const auto now = std::chrono::steady_clock::now();
            if (now - overflowLastLog_ >= overflowLogPeriod) {
                overflowLastLog_ = now;
                printf("[PcAudioModule] OUT overflow: %u events, %llu frames dropped since the last report (total events %u)\n",
                       overflowCount_ - overflowReported_,
                       (unsigned long long)overflowFrames_, overflowCount_);
                overflowReported_ = overflowCount_;
                overflowFrames_ = 0;
            }
        }
    }

    // B-bus tap: mirror OUT1 into the aux stream (PipeWire virtual source),
    // regardless of whether any physical output stream is open. Load the
    // pointer once per cycle — setAuxStream() may swap it from another thread
    // (post-start wiring / shutdown detach).
    crosspad::IAudioStream* aux = aux_.load(std::memory_order_acquire);
    if (aux && aux->isOpen())
        aux->pushSamples(pushScratchInt16_[0].data(),
                         crosspad::AudioFormat::Int16, frames);

    // Peak meter on OUT1 (matches existing VU meter wiring)
    const float* bus0 = mixerBus_[0].data();
    float pl = 0.0f, pr = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float l = bus0[i * 2 + 0]; if (l < 0) l = -l;
        float r = bus0[i * 2 + 1]; if (r < 0) r = -r;
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    peakL_.store(pl, std::memory_order_relaxed);
    peakR_.store(pr, std::memory_order_relaxed);
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
#ifdef __EMSCRIPTEN__
    // The page pulls audio (src/wasm/web_io.cpp): process() runs per block it asks for.
    wasm_audio_add_producer([](void* self) { static_cast<PcAudioModule*>(self)->process(); },
                            this, config_.frameCount);
    return;
#endif
    thread_ = std::make_unique<std::thread>(&PcAudioModule::audioThreadFunc, this);
    printf("[PcAudioModule] Audio thread started\n");
}

void PcAudioModule::stop() {
    running_.store(false);
#ifdef __EMSCRIPTEN__
    wasm_audio_remove_producer(this);
#endif
    if (thread_ && thread_->joinable()) {
        thread_->join();
        thread_.reset();
        printf("[PcAudioModule] Audio thread stopped\n");
    }
}

void PcAudioModule::audioThreadFunc() {
    // Absolute-deadline pacing. A relative sleep_for(frameDuration - elapsed)
    // loses the kernel wakeup overshoot (~50-150us) on EVERY cycle — the
    // error accumulates into a ~2% production deficit that periodically
    // starves the RtAudio output rings (audible crackles). Scheduling
    // against an absolute timeline makes late wakeups self-correcting: the
    // next deadline stays fixed, so the long-run average period is exactly
    // frameCount/sampleRate.
    const auto frameDuration = std::chrono::nanoseconds(
        static_cast<uint64_t>(config_.frameCount) * 1000000000ULL / config_.sampleRate);
    // Resync threshold: after a stall (debugger, suspend, CPU starvation)
    // don't burst-produce to catch up more than this — snap the schedule.
    const auto kMaxBacklog = std::chrono::milliseconds(100);

    auto next = std::chrono::steady_clock::now();

    auto prevStart = std::chrono::steady_clock::now();
    bool first = true;

    while (running_.load(std::memory_order_relaxed)) {
        auto start = std::chrono::steady_clock::now();

        // Loop health metrics: actual period + process() duration. Cheap
        // relaxed atomics; consumed by the app-level audio-health watchdog
        // and the [pacing] regression test.
        if (!first) {
            auto periodUs = std::chrono::duration_cast<std::chrono::microseconds>(
                start - prevStart).count();
            diagPeriodUsSum_.fetch_add(static_cast<uint64_t>(periodUs),
                                       std::memory_order_relaxed);
            uint32_t p = static_cast<uint32_t>(periodUs);
            if (p > diagPeriodUsMax_.load(std::memory_order_relaxed))
                diagPeriodUsMax_.store(p, std::memory_order_relaxed);
            diagCycles_.fetch_add(1, std::memory_order_relaxed);
        }
        first = false;
        prevStart = start;

        process();

        auto elapsed = std::chrono::steady_clock::now() - start;
        {
            auto procUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            diagProcessUsSum_.fetch_add(static_cast<uint64_t>(procUs),
                                        std::memory_order_relaxed);
            uint32_t p = static_cast<uint32_t>(procUs);
            if (p > diagProcessUsMax_.load(std::memory_order_relaxed))
                diagProcessUsMax_.store(p, std::memory_order_relaxed);
        }

        next += frameDuration;
        auto now = std::chrono::steady_clock::now();
        if (next < now - kMaxBacklog) {
            // Fell too far behind — resync instead of a runaway catch-up burst.
            next = now;
        }
        // A deadline already in the past returns immediately: the loop
        // catches up by producing the next block right away.
        std::this_thread::sleep_until(next);
    }
}

} // namespace crosspad_pc
