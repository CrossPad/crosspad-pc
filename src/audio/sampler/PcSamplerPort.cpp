// SPDX-License-Identifier: MIT

#include "PcSamplerPort.hpp"
#include "PcPitchedPort.hpp"
#include "PcSampleNode.hpp"
#include "SampleStreamPlayer.hpp"
#include "SampleStreamEngine.hpp"

#include "pc_stubs/pc_platform.h"
#include "pc_stubs/PcApp.hpp"

#include <crosspad/event/EventData.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/kit/PortableKitLoader.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <crosspad-gui/components/app_orchestrator.h>
#include <crosspad-gui/components/file_explorer.h>
#include <crosspad-gui/components/status_bar.h>

#include "remote/RemoteControl.hpp"

#include <crosspad-sampler/SamplerApp.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/ui/kit_selector_app.hpp>
#include <crosspad-sampler/waveform/waveform_cache.hpp>
#include <crosspad-sampler/waveform/waveform_loader.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace crosspad_pc {
namespace {

PcSampleNode              s_node;
crosspad::PortableKitLoader s_kitLoader("");
std::string               s_sdRoot;
bool                      s_engineUp = false;

// ── Path helpers ─────────────────────────────────────────────────────────
//
// Kits store device-absolute logical paths ("/crosspad/kits/X/kit.json").
// PortableKitLoader maps those through its root prefix, the engine through
// its own; anything here that opens a file itself has to do the same.

std::string fsPath(const std::string& logical) {
    return pc_platform_resolve_sdcard_path(logical);
}

// ── Player callbacks ─────────────────────────────────────────────────────

// Kit files store pan as a signed offset with 0 meaning centre — every pad of
// every kit on a real card holds 0, and hardware sounds them centred. The
// player API is the MIDI convention (0..127, 64 centre), so the conversion
// belongs here at the boundary rather than in the kit data.
static uint8_t toPlayerPan(int kitPan) {
    const int v = kitPan + 64;
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 127 ? 127 : v));
}

void platform_noteOn(uint8_t padIdx, uint8_t velocity) {
    if (!SampleStreamPlayer_IsReady()) return;
    SampleStreamPlayer_NoteOn(padIdx, velocity);
}

void platform_setVolume(uint8_t padIdx, int volume) {
    SampleStreamPlayer_SetVolume(padIdx, static_cast<uint8_t>(volume));
}

void platform_setPan(uint8_t padIdx, int pan) {
    SampleStreamPlayer_SetPan(padIdx, toPlayerPan(pan));
}

void platform_setLoopEnd(uint8_t padIdx, uint32_t endPos) {
    SampleStreamPlayer_SetLoopEnd(padIdx, endPos);
}

void platform_clearLoop(uint8_t padIdx) {
    SampleStreamPlayer_SetLoopClear(padIdx);
}

void platform_wipeOut(uint8_t padIdx) {
    SampleStreamPlayer_WipeOut(padIdx);
}

int platform_getFreeSlotCount() {
    return SampleStreamPlayer_GetFreeWavCnt();
}

bool platform_getSampleInfo(const char* path, crosspad_sampler::SampleInfo* outInfo) {
    if (!path || !outInfo) return false;
    sample_info_from_file info{};
    if (!SampleStreamPlayer_GetInfo(&info, path)) return false;
    outInfo->sampleCount      = info.sampleCount;
    outInfo->sampleRate       = info.sampleRate;
    outInfo->numberOfChannels = info.numberOfChannels;
    outInfo->bitsPerSample    = info.bitsPerSample;
    return true;
}

void platform_previewSample(const char* path, uint32_t start, uint32_t end,
                            int volume, int pan) {
    if (!path || !path[0]) return;
    SampleStreamPlayer_Preview(path, start, end);
    SampleStreamPlayer_SetVolume(SAMPLE_STREAM_PLY_PREVIEW_NOTE,
                                 static_cast<uint8_t>(volume > 0 ? volume : 100));
    SampleStreamPlayer_SetPan(SAMPLE_STREAM_PLY_PREVIEW_NOTE, toPlayerPan(pan));
}

// ── Kit reload ───────────────────────────────────────────────────────────

// Counted per kit load and reported once when it finishes. Without this a kit
// that parsed but whose samples never reached the engine is indistinguishable
// from one that plays — the pads light up either way, because that is the pad
// logic, not the sound.
int s_padsLoaded = 0, s_layersLoaded = 0, s_layersFailed = 0;

void platform_reloadPad(uint8_t padIdx) {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit || padIdx >= KIT_PADS) return;

    SampleStreamPlayer_WipeOut(padIdx);

    if (kit->engine == crosspad::KitEngine::Pitched) {
        // The WipeOut above is all the streaming engine has to do for a pitched
        // kit. The whole bank is built once, on pad 0; the other fifteen pads
        // carry no per-engine state (their volume/pan come from the bank swap).
        if (padIdx == 0) pitched_port_load(*kit);
        return;
    }

    // Back on a sampler kit: the pitched bank goes first, so the two engines
    // never hold their sample memory at once.
    if (padIdx == 0 && pitched_port_active()) pitched_port_unload();

    auto& pad = kit->pads[padIdx];
    int ok = 0;
    for (uint8_t i = 0; i < pad.sampleCount; ++i) {
        const std::string resolved =
            crosspad::resolveKitSamplePath(kit->path, pad.samples[i].sample);
        if (resolved.empty()) { ++s_layersFailed; continue; }

        // end == 0 means "to the end of the file"; the player takes that as
        // its own sentinel, so it is passed through rather than resolved here.
        if (SampleStreamPlayer_SetupSample(padIdx, resolved.c_str(), pad.choke_group, i,
                                           pad.samples[i].start, pad.samples[i].end)) {
            ++ok;
            ++s_layersLoaded;
        } else {
            ++s_layersFailed;
        }
    }
    if (ok) ++s_padsLoaded;
    SampleStreamPlayer_SetVolume(padIdx, static_cast<uint8_t>(pad.volume));
    SampleStreamPlayer_SetPan(padIdx, toPlayerPan(pad.pan));
    if (pad.playbackMode == 2) {
        SampleStreamPlayer_SetLoopEnd(padIdx, crosspad_sampler::SAMPLE_END_MAX);
    } else {
        SampleStreamPlayer_SetLoopClear(padIdx);
    }
}

void platform_reloadPads() {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit) return;

    for (uint8_t i = 0; i < KIT_PADS; ++i) {
        if (!kit->pads[i].dirty) continue;
        platform_reloadPad(i);
        kit->pads[i].dirty = false;
    }
}

bool platform_hasCacheFile(int kitId) {
    auto* mgr = crosspad::getKitManager();
    if (!mgr) return false;
    auto* kit = mgr->getKit(kitId);
    if (!kit || kit->path.empty()) return false;
    const std::string cache = crosspad::kitAssetPath(kit->path, "waveform.cache");
    std::error_code ec;
    return std::filesystem::exists(cache, ec);
}

void platform_startLoadKit(int kitId, bool onlyModified) {
    s_padsLoaded = s_layersLoaded = s_layersFailed = 0;
    // Parsing a kit is file I/O; it belongs on the load worker, not on the
    // LVGL thread that asked for it.
    crosspad_sampler::kit_load_task_start(onlyModified, kitId);
}

// Queue every layer of the freshly-loaded kit that is not cached yet. The
// loader worker is long-lived, so "already running" is not a reason to skip
// this — that was what left a kit loaded any way other than through the
// selector with no waveforms at all.
void platform_topUpWaveformCache() {
    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getCurrentKit() : nullptr;
    if (!kit) return;

    crosspad_sampler::waveform_cache_clear_if_kit_changed(kit, kit->path.c_str());

    int total = 0, missing = 0;
    for (int i = 0; i < crosspad_sampler::PAD_COUNT; ++i) {
        for (const auto& sample : kit->pads[i].samples) {
            const std::string p = crosspad::resolveKitSamplePath(kit->path, sample.sample);
            if (p.empty()) continue;
            ++total;
            if (!crosspad_sampler::waveform_cache_find(p.c_str())) ++missing;
        }
    }
    if (missing == 0) return;
    crosspad_sampler::totalWaveformsToCache = total;
    crosspad_sampler::waveformsCached       = total - missing;
    crosspad_sampler::waveform_loader_start();
    crosspad_sampler::waveform_loader_preload_kit(crosspad_sampler::PAD_COUNT);
}

void platform_onKitLoadFinished(bool success) {
    if (!success) {
        // "No card" is the common cause and the orchestrator cannot know it,
        // so say which failure this was.
        if (pc_platform_get_sdcard_path().empty()) {
            crosspad_gui::statusbar_process_loader_status(100, true, true, "No SD card");
        }
        return;
    }
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    std::printf("[sampler] kit ready: '%s' — %d pads, %d layers in the engine%s\n",
                kit ? kit->name.c_str() : "?", s_padsLoaded, s_layersLoaded,
                s_layersFailed ? " (some layers refused, see above)" : "");
    // A pitched zone that did not fit the bank budget still plays; how much of
    // it was dropped is only visible here.
    if (pitched_port_active()) {
        const auto& report = pitched_port_last_report();
        if (report.truncatedZones > 0 && report.warning[0]) {
            crosspad_gui::statusbar_show_loader_message("Warning", report.warning, false);
        }
    }
    platform_topUpWaveformCache();
}

// ── Waveform callbacks ───────────────────────────────────────────────────

bool platform_wf_getFileInfo(const char* path, crosspad_sampler::AudioFileInfo* outInfo) {
    if (!path || !outInfo) return false;
    sample_info_from_file info{};
    if (!SampleStreamPlayer_GetInfo(&info, path)) return false;
    outInfo->sampleCount = info.sampleCount;
    outInfo->sampleRate  = info.sampleRate;
    outInfo->channels    = static_cast<uint8_t>(info.numberOfChannels);
    outInfo->bitDepth    = static_cast<uint8_t>(info.bitsPerSample);
    return true;
}

bool platform_wf_getWaveformSamples(const char* path, int16_t* outBuf, uint32_t outSamples,
                                    uint32_t startSample, uint32_t endSample) {
    return SampleStreamPlayer_GetWaveform(path, outBuf, outSamples, startSample, endSample);
}

bool platform_wf_writeFile(const char* path, const uint8_t* data, uint32_t size) {
    if (!path) return false;
    FILE* f = fopen(fsPath(path).c_str(), "wb");
    if (!f) return false;
    const size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

uint32_t platform_wf_readFile(const char* path, uint8_t* outBuf, uint32_t maxSize) {
    if (!path) return 0;
    FILE* f = fopen(fsPath(path).c_str(), "rb");
    if (!f) return 0;
    const size_t got = fread(outBuf, 1, maxSize, f);
    fclose(f);
    return static_cast<uint32_t>(got);
}

void* platform_wf_alloc(uint32_t bytes) { return malloc(bytes); }
void  platform_wf_free(void* ptr) { free(ptr); }

// ── EventBus → engine bridge ─────────────────────────────────────────────
//
// SamplerPadLogic posts SamplePlayback on a pad hit; this is what turns that
// into sound. Without it the GUI reacts to every pad and nothing is heard.

void onSamplePlaybackEvent(const void* data, crosspad::EventType, void*) {
    auto* d = static_cast<const crosspad::SamplePlaybackData*>(data);
    if (d->source == crosspad::EventSource::SamplePlayback) return;   // engine feedback

    // Pitched kits: the note field carries the pad index (SamplerPadLogic posts
    // the pad), and the kit's pad_notes map decides what it sounds. The
    // streaming engine is not involved and need not be ready.
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit && kit->engine == crosspad::KitEngine::Pitched) {
        // No bank: the load failed, or a failed load dropped the previous one.
        // Falling through to the streaming engine is not a fallback —
        // reloadPad() wiped every slot for this kit and put nothing back, so it
        // holds whatever an earlier kit left where the wipe did not reach, keyed
        // by a note map belonging to neither kit. Silence is the honest answer.
        if (!pitched_port_active()) return;
        const uint8_t pad = d->note;
        if (pad >= KIT_PADS) return;
        auto& inst = pitched_port_instrument();
        const uint8_t note = kit->instrument.noteForPad(pad);
        if (d->isOn) {
            inst.noteOn(note, d->velocity ? d->velocity : 1, pad);
        } else {
            inst.noteOff(note);
        }
        return;
    }

    if (!SampleStreamPlayer_IsReady()) return;

    if (d->isOn) {
        // Velocity 0 is a note-off to the engine, so a pad reporting a
        // zero-velocity hit still has to make a sound.
        SampleStreamPlayer_NoteOn(d->note, d->velocity ? d->velocity : 1);
    } else {
        SampleStreamPlayer_NoteOff(d->note);
    }
}

// ── Kit-selector gate ────────────────────────────────────────────────────

App*      s_overlay       = nullptr;
App*      s_pendingApp    = nullptr;
lv_obj_t* s_pendingParent = nullptr;
bool      s_shouldLaunch  = false;

const char* kKitRequiredApps[] = {"Sampler", "Sequencer"};

bool app_requires_kit(const char* name) {
    if (!name) return false;
    for (const char* n : kKitRequiredApps) {
        if (std::strcmp(name, n) == 0) return true;
    }
    return false;
}

void on_kit_loaded() {
    s_shouldLaunch = true;
    crosspad_gui::statusbar_process_loader_status(100, true, false, "Kit loaded");
}

// The selector reports here once it has torn itself down — both when a kit
// finished loading and when the user backed out. Only the first launches what
// was waiting behind the gate.
void on_selector_closed() {
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    if (s_pendingApp && s_shouldLaunch && s_pendingParent) {
        s_pendingApp->start(s_pendingParent);
        s_pendingApp->resume();
        orch.setRunningApp(s_pendingApp);
    }
    s_pendingApp   = nullptr;
    s_shouldLaunch = false;
}

// ── Kit discovery ────────────────────────────────────────────────────────

void rescanKits() {
    if (s_sdRoot.empty()) {
        s_kitLoader.kits().clear();
        s_kitLoader.setCurrentKitId(-1);
        return;
    }
    s_kitLoader.listKits(crosspad_sampler::DEFAULT_KIT_PATH);
    std::printf("[KitLoader] Discovered %d kits under %s\n",
                s_kitLoader.getKitCount(), s_sdRoot.c_str());
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────

crosspad::IAudioNode* sampler_port_init() {
    if (!SampleStreamPlayer_Init()) {
        std::printf("[sampler] engine init failed — pads will not sound\n");
        return nullptr;
    }
    s_engineUp = true;

    // Sixteen pads plus the audition slot is what the grid can ask for at
    // once; the board sets the same cap for the same reason.
    SampleStreamPlayer_SetMaxPolyphony(16);

    crosspad::getPlatformServices().kitManager = &s_kitLoader;

    crosspad_sampler::SamplerPlatformCallbacks cb{};

    cb.kitSelector.onKitLoaded = on_kit_loaded;
    cb.kitSelector.onClosed    = on_selector_closed;

    cb.audio.startLoadKit      = platform_startLoadKit;
    cb.audio.onKitLoadFinished = platform_onKitLoadFinished;
    cb.audio.reloadPad         = platform_reloadPad;
    cb.audio.reloadPads        = platform_reloadPads;
    cb.audio.hasCacheFile      = platform_hasCacheFile;

    cb.player.noteOn           = platform_noteOn;
    cb.player.previewSample    = platform_previewSample;
    cb.player.getSampleInfo    = platform_getSampleInfo;
    cb.player.setVolume        = platform_setVolume;
    cb.player.setPan           = platform_setPan;
    cb.player.setLoopEnd       = platform_setLoopEnd;
    cb.player.clearLoop        = platform_clearLoop;
    cb.player.wipeOut          = platform_wipeOut;
    cb.player.getFreeSlotCount = platform_getFreeSlotCount;

    cb.waveform.getFileInfo        = platform_wf_getFileInfo;
    cb.waveform.getWaveformSamples = platform_wf_getWaveformSamples;
    cb.waveform.writeFile          = platform_wf_writeFile;
    cb.waveform.readFile           = platform_wf_readFile;
    cb.waveform.allocWaveformMem   = platform_wf_alloc;
    cb.waveform.freeWaveformMem    = platform_wf_free;

    crosspad_sampler::sampler_set_platform_callbacks(cb);

    // Audition files while browsing them — the explorer debounces the
    // selection, so this just plays whatever it rests on.
    file_explorer_set_preview_callback([](const char* path) {
        platform_previewSample(path, 0, crosspad_sampler::SAMPLE_END_MAX, 100, 64);
    });

    crosspad::getEventBus().subscribe(crosspad::EventType::SamplePlayback,
                                      onSamplePlaybackEvent, nullptr);

    // The sampler is the system instrument: pads play samples everywhere, not
    // only inside its own app. Other instruments preempt it and hand it back.
    crosspad_sampler::sampler_service_init();

    // Give the control port a kit_load that goes through the same worker the
    // browser uses, so a test does not have to click its way through a list of
    // seventy kits to get a deterministic starting state.
    remote::set_kit_loader(
        [](int kitId) { platform_startLoadKit(kitId, /*onlyModified=*/false); },
        []() { return crosspad_sampler::kit_load_task_is_loading(); });

    // A card mounted before this ran only recorded its path; apply it now.
    if (!s_sdRoot.empty()) sampler_port_set_sdcard(s_sdRoot);

    return &s_node;
}

void sampler_port_set_sdcard(const std::string& root) {
    s_sdRoot = root;
    s_kitLoader.setRootPrefix(root);
    if (!s_engineUp) return;   // init() replays this

    SampleStreamPlayer_SetRootPrefix(root.c_str());
    rescanKits();
}

bool sampler_port_pre_launch(crosspad_gui::ILvglApp* app, lv_obj_t* container) {
    if (!app || !app_requires_kit(app->getName())) return true;

    auto* mgr = crosspad::getKitManager();
    if (mgr && mgr->getCurrentKit() != nullptr) return true;

    s_pendingApp    = static_cast<App*>(app);
    s_pendingParent = container;
    s_shouldLaunch  = false;

    // Built once and reused. The overlay is not owned by the app factory, so
    // nothing would ever delete it — re-creating it per prompt leaked one App
    // each time. destroyApp() clears its root, so start() rebuilds the UI on
    // the same instance.
    if (!s_overlay) {
        s_overlay = new App(container, "Kit selector", "notepick.png",
                            lv_CreateKitSelector, lv_KitSelector_destroy);
    }
    s_overlay->start(container);
    s_overlay->resume();
    return false;
}

} // namespace crosspad_pc
