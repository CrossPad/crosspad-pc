// The song engine on PC: a std::thread worker, malloc'd rings, projects in
// <sdcard_path>/SONGS, the current project in SONGS/.current, and a bounce
// records the capture bus PcAudioModule renders beside the two it plays.

#include "PcSongEnginePort.hpp"

#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/song/SongEngine.hpp"
#include "sequencer/PcSequencer.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

const std::string& pc_platform_get_sdcard_path();

namespace {

constexpr const char* kSongsDir = "/SONGS";
constexpr const char* kCurrentFile = "/.current";

class PcSongPlatform final : public crosspad::ISongEnginePlatform {
public:
    PcSongPlatform(AudioMixerEngine& mixer, uint8_t captureBus) : mixer_(mixer), bus_(captureBus) {}

    bool startWorker(void (*entry)(void*), void* arg) override {
        worker_ = std::thread(entry, arg);
        return true;
    }
    void joinWorker() override { if (worker_.joinable()) worker_.join(); }
    void wake() override {
        { std::lock_guard<std::mutex> l(m_); woken_ = true; }
        cv_.notify_one();
    }
    void waitWake(uint32_t ms) override {
        std::unique_lock<std::mutex> l(m_);
        if (ms == UINT32_MAX) cv_.wait(l, [&] { return woken_; });
        else cv_.wait_for(l, std::chrono::milliseconds(ms), [&] { return woken_; });
        woken_ = false;
    }
    void sleepMs(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms ? ms : 1)); }
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
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    bool woken_ = false;
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
