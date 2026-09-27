// The song engine on PC: a FreeRTOS worker task (its sequencer calls post on
// the event bus, which only a FreeRTOS task may do), malloc'd rings, projects
// in <sdcard_path>/SONGS, the current project in SONGS/.current, and a bounce
// records the capture bus PcAudioModule renders beside the two it plays.

#include "PcSongEnginePort.hpp"

#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/song/SongEngine.hpp"
#include "pc_stubs/pc_platform.h"
#include "sequencer/PcSequencer.hpp"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace {

constexpr const char* kSongsDir = "/SONGS";
constexpr const char* kCurrentFile = "/.current";
constexpr uint32_t kWorkerStack = 8192;
/// The LVGL task's priority (1), not above it as on the board: the worker and
/// the UI share std::mutexes (the song's), and on the POSIX port a task
/// blocked on one still counts as running -- above LVGL it would wait forever
/// for a mutex LVGL never gets the CPU to release. At equal priority the tick
/// hands LVGL its turn.
constexpr UBaseType_t kWorkerPriority = 1;

class PcSongPlatform final : public crosspad::ISongEnginePlatform {
public:
    PcSongPlatform(AudioMixerEngine& mixer, uint8_t captureBus)
        : mixer_(mixer), bus_(captureBus), wake_(xSemaphoreCreateBinary()) {}

    bool startWorker(void (*entry)(void*), void* arg) override {
        return xTaskCreate(entry, "song_io", kWorkerStack, arg, kWorkerPriority, nullptr) == pdPASS;
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

    void* allocRingSegment(size_t bytes, uint32_t) override { return std::malloc(bytes); }
    void freeRingSegment(void* p) override { std::free(p); }
    void* allocScratch(size_t bytes) override { return std::malloc(bytes); }
    size_t freeMemoryBytes() override { return SIZE_MAX; }

    bool storageReady() override { return !pc_platform_get_sdcard_path().empty(); }
    void yieldIo() override {}
    const char* songsRoot() override {
        std::lock_guard<std::mutex> l(rootMutex_);
        root_ = pc_platform_get_sdcard_path() + kSongsDir;
        return root_.c_str();
    }
    std::string loadCurrent() override {
        std::ifstream f(std::string(songsRoot()) + kCurrentFile);
        std::string name;
        std::getline(f, name);
        return name;
    }
    void saveCurrent(const std::string& name) override {
        std::ofstream(std::string(songsRoot()) + kCurrentFile) << name << '\n';
    }

    void beginPadsCapture() override {
        gain_ = mixer_.getOutputVolume(bus_);
        muted_ = mixer_.isOutputMuted(bus_);
        mixer_.setOutputVolume(bus_, 1.0f);
        mixer_.setOutputMute(bus_, false);   // a muted bus records silence
    }
    void endPadsCapture() override {
        mixer_.setOutputVolume(bus_, gain_);
        mixer_.setOutputMute(bus_, muted_);
    }

    void log(bool warning, const char* msg) override {
        std::fprintf(stderr, "[Song]%s %s\n", warning ? " warning:" : "", msg);
    }

private:
    AudioMixerEngine& mixer_;
    uint8_t bus_;
    SemaphoreHandle_t wake_;
    std::mutex rootMutex_;
    std::string root_;
    float gain_ = 1.0f;
    bool muted_ = false;
};

crosspad::SongEngine* s_engine = nullptr;

} // namespace

namespace crosspad_pc {

void song_engine_init(AudioMixerEngine& mixer, uint8_t audibleOutputs, uint8_t captureBus)
{
    /* For the life of the process, which ends with _Exit(): the audio thread
     * may call rtCapture() until the very end. */
    static PcSongPlatform platform(mixer, captureBus);
    static crosspad::SongEngine engine(platform, getMetronome(), getSequencer());
    s_engine = &engine;
    const auto ch = mixer.addChannel(&engine, "Song");
    if (ch != kMixerInvalidChannelId) {
        /* Audible outputs only: the song must not end up inside its own clips. */
        for (uint8_t o = 0; o < audibleOutputs; ++o) mixer.setRouteEnabled(ch, o, true);
    }
    crosspad::getPlatformServices().songEngine = &engine;
}

void song_engine_rt_capture(const float* bus, uint32_t frames)
{
    if (s_engine) s_engine->rtCapture(bus, frames);
}

} // namespace crosspad_pc
