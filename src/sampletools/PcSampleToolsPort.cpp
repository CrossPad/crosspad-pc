// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample tools on PC: a FreeRTOS worker task (a take with the song asks
// the song engine to play, which only a FreeRTOS task may do), malloc'd
// rings, the simulated SD card's folder as the card, IN1/IN2 as the mics and
// the line-in, and the capture bus as the pads. No codec ADC to hold and no
// USB audio profile: a USB take is refused as unavailable.

#include "PcSampleToolsPort.hpp"

#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/SampleToolsEngine.hpp"
#include "crosspad/synth/IAudioInput.hpp"
#include "pc_stubs/pc_platform.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr uint32_t kWorkerStack = 8192;
/// The LVGL task's priority (1), not above it as on the board: the worker and
/// the UI share std::mutexes, and on the POSIX port a task blocked on one
/// still counts as running -- above LVGL it would wait forever for a mutex
/// LVGL never gets the CPU to release (PcSongEnginePort.cpp).
constexpr UBaseType_t kWorkerPriority = 1;
/// Mixer inputs the Recorder can record: IN1 as MIC, IN2 as LINE.
constexpr int kInputs = 2;

class PcToolsPlatform final : public crosspad::ISampleToolsPlatform {
public:
    PcToolsPlatform() : wake_(xSemaphoreCreateBinary()) {}

    bool startWorker(void (*entry)(void*), void* arg) override {
        return xTaskCreate(entry, "tools_io", kWorkerStack, arg, kWorkerPriority, nullptr) == pdPASS;
    }
    void joinWorker() override {}   // the engine lives until the process's _Exit()
    void wake() override { xSemaphoreGive(wake_); }
    void waitWake(uint32_t ms) override {
        xSemaphoreTake(wake_, ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(ms));
    }
    void sleepMs(uint32_t ms) override { vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1); }
    int64_t nowUs() override {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void* allocRingSegment(size_t bytes, uint32_t, bool, bool& lent) override {
        lent = false;
        return std::malloc(bytes);
    }
    void freeRingSegment(void* p) override { std::free(p); }
    void* allocScratch(size_t bytes) override { return std::malloc(bytes); }

    bool storageReady() override { return !pc_platform_get_sdcard_path().empty(); }
    void yieldIo() override {}
    std::string cardRoot() override { return pc_platform_get_sdcard_path(); }

    void log(bool warning, const char* msg) override {
        std::fprintf(stderr, "[SampleTools]%s %s\n", warning ? " warning:" : "", msg);
    }

private:
    SemaphoreHandle_t wake_;
};

crosspad::SampleToolsEngine* s_tools = nullptr;

/// One of the simulator's inputs as the mixer reads it, every block handed
/// to the sample tools too: the one place a block of IN1/IN2 passes through.
class TappedInput final : public crosspad::IAudioInput {
public:
    explicit TappedInput(int index) : index_(index) {}

    uint32_t read(int16_t* interleaved, uint32_t frames) override {
        crosspad::IAudioInput* in = pc_platform_get_audio_input(index_);
        const uint32_t got = in ? in->read(interleaved, frames) : 0;
        if (got && s_tools) s_tools->rtInput(index_, interleaved, got);
        return got;
    }
    uint32_t getSampleRate() const override {
        crosspad::IAudioInput* in = pc_platform_get_audio_input(index_);
        return in ? in->getSampleRate() : 0;
    }
    uint32_t getBufferSize() const override {
        crosspad::IAudioInput* in = pc_platform_get_audio_input(index_);
        return in ? in->getBufferSize() : 0;
    }
    void getInputLevel(int16_t& left, int16_t& right) const override {
        left = right = 0;
        if (crosspad::IAudioInput* in = pc_platform_get_audio_input(index_)) in->getInputLevel(left, right);
    }

private:
    int index_;
};

TappedInput s_inputs[kInputs] = {TappedInput(0), TappedInput(1)};

} // namespace

namespace crosspad_pc {

void sample_tools_init(AudioMixerEngine& mixer, uint8_t audibleOutputs)
{
    /* For the life of the process, which ends with _Exit(): the audio thread
     * may call into it until the very end. */
    static PcToolsPlatform platform;
    static crosspad::SampleToolsEngine tools(platform);
    s_tools = &tools;
    const auto ch = mixer.addChannel(&tools, "Preview");
    if (ch != kMixerInvalidChannelId) {
        /* Audible outputs only: an audition is never in a take of the pads. */
        for (uint8_t o = 0; o < audibleOutputs; ++o) mixer.setRouteEnabled(ch, o, true);
    }
    crosspad::getPlatformServices().sampleTools = &tools;
}

void sample_tools_rt_bus(const float* bus, uint32_t frames)
{
    if (!s_tools) return;
    s_tools->rtBus(bus, frames);
    /* The next block begins here: nothing is read between this tap and the
     * next render, so the take's length now is where that block starts. */
    s_tools->rtBlockBegin();
}

crosspad::IAudioInput* sample_tools_input(int index)
{
    return index >= 0 && index < kInputs ? &s_inputs[index] : pc_platform_get_audio_input(index);
}

} // namespace crosspad_pc
