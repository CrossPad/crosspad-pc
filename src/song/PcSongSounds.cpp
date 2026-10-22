// SPDX-License-Identifier: GPL-3.0-or-later
//
// The open song's sounds on PC: core's SongSounds over the sampler port's kit
// loader, the instrument host and a toast, stepped from an LVGL timer -- the
// board does the same in platform-idf's main/song_sounds.cpp.

#include "PcSongSounds.hpp"
#include "PcSongEnginePort.hpp"

#include "audio/sampler/PcInstrumentHost.hpp"
#include "audio/sampler/PcSamplerPort.hpp"
#include "pc_stubs/pc_platform.h"

#include <crosspad/song/SongSounds.hpp>

#include <crosspad-gui/components/app_orchestrator.h>
#include <crosspad-gui/components/ui_toast.h>
#include <crosspad-sampler/logic/kit_load_task.hpp>

#include "lvgl.h"

#include <cstdio>
#include <string>

namespace crosspad_pc {
namespace {

constexpr uint32_t kToastMs = 4000;

class PcSongSoundsHost final : public crosspad::ISongSoundsHost {
public:
    std::string cardRoot() override { return pc_platform_get_sdcard_path(); }
    /* The kit list is read where the card is mounted, before this runs. */
    bool kitsSettled() override { return !crosspad_sampler::kit_load_task_is_loading(); }
    void loadKit(int kitId) override { sampler_port_load_kit(kitId); }
    void unloadKit() override { sampler_port_unload_kit(); }
    bool instrumentOwnsEngine() override { return instrument_host_owns_engine(); }

    const char* foregroundApp() override
    {
        crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
        return fg ? fg->getName() : nullptr;
    }

    void warn(const char* msg) override { ui_toast_show(msg, kToastMs, UI_TOAST_WARNING); }

    void log(bool warning, const char* msg) override
    {
        std::printf("[song_sounds]%s %s\n", warning ? " warning:" : "", msg);
    }
};

crosspad::SongSounds& sounds()
{
    static PcSongSoundsHost host;
    static crosspad::SongSounds s(host);
    return s;
}

lv_timer_t* s_timer = nullptr;

} // namespace

void song_sounds_start()
{
    if (s_timer) return;
    song_engine_set_open_handler([](const crosspad::AudioSong&) { sounds().songOpened(); });
    s_timer = lv_timer_create([](lv_timer_t*) { sounds().step(); }, crosspad::SongSounds::kStepMs, nullptr);
    sounds().start();
}

void song_sounds_kit_loaded() { sounds().kitLoaded(); }

void song_sounds_keep_instrument(const crosspad::KitInfo& spec,
                                 const std::function<bool(const crosspad::KitInfo&)>& save)
{
    sounds().keepInstrument(spec, save);
}

} // namespace crosspad_pc
