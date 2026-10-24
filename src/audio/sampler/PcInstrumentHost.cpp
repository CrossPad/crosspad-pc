// SPDX-License-Identifier: GPL-3.0-or-later

#include "PcInstrumentHost.hpp"
#include "PcPitchedPort.hpp"

#include "pc_stubs/GuiThread.hpp"
#include "pc_stubs/pc_platform.h"
#include "song/PcSongSounds.hpp"

#include <crosspad/instrument/IInstrumentHost.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/PortableKitLoader.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <crosspad-sampler/logic/kit_load_task.hpp>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace crosspad_pc {
namespace {

constexpr const char* kLibrary = "/crosspad/pitchplay";
constexpr uint32_t kStack = 8192;
/// The LVGL task's priority (1), as every worker here: on the POSIX port a
/// task blocked on a std::mutex still counts as running.
constexpr UBaseType_t kPriority = 1;
/// How long a load waits for a kit load already under way.
constexpr int kKitLoadWaitTicks = 300;
constexpr uint32_t kKitLoadWaitMs = 100;

enum class Job : uint8_t { None, Load, Open };

class PcInstrumentHost final : public crosspad::IInstrumentHost {
public:
    bool requestLoad(const crosspad::KitInfo& spec) override { return submit(Job::Load, &spec, nullptr); }
    bool requestOpen(const std::string& path) override { return submit(Job::Open, nullptr, &path); }

    bool requestSave(const crosspad::KitInfo&) override
    {
        std::lock_guard<std::mutex> l(mu_);
        message_ = "Saving an instrument is not available here";
        return false;
    }

    void release() override
    {
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_ = Job::None;   // not started: not wanted any more
        }
        releasePending_.store(true);
        xSemaphoreGive(wake_);
    }

    State state() const override { return state_.load(); }

    crosspad::KitInfo instrument() const override
    {
        std::lock_guard<std::mutex> l(mu_);
        return instrument_;
    }

    std::string message() const override
    {
        std::lock_guard<std::mutex> l(mu_);
        return message_;
    }

    std::string libraryRoot() const override { return kLibrary; }

    bool owns() const { return owned_.load(); }

    bool start()
    {
        wake_ = xSemaphoreCreateBinary();
        return wake_ && xTaskCreate(task, "inst_host", kStack, this, kPriority, nullptr) == pdPASS;
    }

private:
    bool submit(Job job, const crosspad::KitInfo* spec, const std::string* path)
    {
        {
            /* Newest wins: a request made while another runs replaces
             * whatever was waiting, and the one running is not interrupted. */
            std::lock_guard<std::mutex> l(mu_);
            pending_ = job;
            if (spec) spec_ = *spec;
            if (path) path_ = *path;
            message_.clear();
        }
        // A load asked for before an earlier release was carried out keeps
        // the engine: it is the same app, back again.
        releasePending_.store(false);
        state_.store(State::Busy);
        xSemaphoreGive(wake_);
        return true;
    }

    static void task(void* self) { static_cast<PcInstrumentHost*>(self)->run(); }

    void run()
    {
        for (;;) {
            xSemaphoreTake(wake_, portMAX_DELAY);
            for (;;) {
                Job job;
                crosspad::KitInfo spec;
                std::string path;
                {
                    std::lock_guard<std::mutex> l(mu_);
                    job = pending_;
                    pending_ = Job::None;
                    if (job != Job::None) {
                        spec = spec_;
                        path = path_;
                    }
                }
                if (job != Job::None) {
                    const bool ok = job == Job::Load ? load(spec) : open(path);
                    /* Before Ready is published: whoever takes the instrument
                     * then takes the path the song kept. */
                    if (ok) keep_in_song();
                    std::lock_guard<std::mutex> l(mu_);
                    // Busy until the last request is through: a caller that
                    // saw Ready here would take an instrument already replaced.
                    if (pending_ == Job::None) {
                        state_.store(ok ? (owned_.load() ? State::Ready : State::Idle) : State::Failed);
                    }
                    continue;
                }
                if (releasePending_.exchange(false)) {
                    give_back();
                    continue;
                }
                break;
            }
        }
    }

    void keep_in_song()
    {
        crosspad::KitInfo spec;
        {
            std::lock_guard<std::mutex> l(mu_);
            spec = instrument_;
        }
        song_sounds_keep_instrument(spec, nullptr);
    }

    void fail(const std::string& why)
    {
        std::printf("[inst_host] %s\n", why.c_str());
        std::lock_guard<std::mutex> l(mu_);
        message_ = why;
    }

    /// The bank is the app's from here. A kit load already running finishes
    /// first, or a pitched kit's bank would be built over the app's.
    void take_engine()
    {
        if (owned_.exchange(true)) return;
        for (int i = 0; i < kKitLoadWaitTicks && crosspad_sampler::kit_load_task_is_loading(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(kKitLoadWaitMs));
        }
    }

    bool load(crosspad::KitInfo spec)
    {
        if (spec.instrument.zones.empty()) {
            fail("No samples in the instrument");
            return false;
        }
        spec.engine = crosspad::KitEngine::Pitched;
        take_engine();
        /* As the board builds an app's bank: zones kept at their own rate
         * and brought to one level, each at most kMaxZoneSeconds. */
        PitchedPortOptions options;
        options.maxZoneSeconds = kMaxZoneSeconds;
        options.convertRate = false;
        options.normalize = true;
        if (!pitched_port_load(spec, options)) {
            fail("The samples could not be loaded");
            return false;
        }
        std::lock_guard<std::mutex> l(mu_);
        instrument_ = spec;
        message_ = pitched_port_last_report().warning;
        return true;
    }

    bool open(const std::string& path)
    {
        crosspad::PortableKitLoader reader(pc_platform_get_sdcard_path());
        crosspad::KitInfo spec;
        spec.path = path;
        const auto result = reader.loadKit(spec);
        if (!result.success) {
            fail(result.error.empty() ? "The instrument could not be read" : result.error);
            return false;
        }
        if (spec.engine != crosspad::KitEngine::Pitched) {
            fail("Not an instrument: " + path);
            return false;
        }
        return load(spec);
    }

    void give_back()
    {
        if (!owned_.load()) return;
        pitched_port_unload();
        owned_.store(false);
        state_.store(State::Idle);
        auto* kits = crosspad::getKitManager();
        if (kits && kits->getCurrentKit()) {
            /* The kit as it was: a pitched kit builds its own bank again. */
            pc_gui::post([](void*) { crosspad_sampler::kit_load_task_start(false, -1); }, nullptr);
        }
    }

    SemaphoreHandle_t wake_ = nullptr;
    mutable std::mutex mu_;
    Job pending_ = Job::None;
    crosspad::KitInfo spec_;
    std::string path_;
    crosspad::KitInfo instrument_;
    std::string message_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> owned_{false};
    std::atomic<bool> releasePending_{false};
};

PcInstrumentHost* s_host = nullptr;

} // namespace

void instrument_host_init()
{
    if (s_host) return;
    static PcInstrumentHost host;
    if (!host.start()) {
        std::printf("[inst_host] worker not started; no app can borrow the engine\n");
        return;
    }
    s_host = &host;
    crosspad::getPlatformServices().instrumentHost = &host;
}

bool instrument_host_owns_engine()
{
    return s_host && s_host->owns();
}

} // namespace crosspad_pc
