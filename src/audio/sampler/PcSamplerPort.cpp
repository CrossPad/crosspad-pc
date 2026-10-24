// SPDX-License-Identifier: GPL-3.0-or-later

#include "PcSamplerPort.hpp"
#include "PcInstrumentHost.hpp"
#include "PcPitchedPort.hpp"
#include "PcSamplerEngine.hpp"

#include "pc_stubs/pc_platform.h"

#include <crosspad/event/EventData.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/kit/PortableKitLoader.hpp>
#include <crosspad/pad/IPadMix.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <crosspad-gui/components/file_explorer.h>
#include <crosspad-gui/components/status_bar.h>

#include "remote/RemoteControl.hpp"
#include "sequencer/PcSequencer.hpp"
#include "song/PcSongSounds.hpp"

#include <crosspad-sampler/SamplerApp.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/logic/kit_gate.hpp>
#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/logic/pad_color_manager.hpp>
#include <crosspad-sampler/ui/sampler_panel.hpp>
#include <crosspad-sampler/waveform/waveform_cache.hpp>
#include <crosspad-sampler/waveform/waveform_loader.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace crosspad_pc {
namespace {

crosspad::PortableKitLoader s_kitLoader("");
std::string               s_sdRoot;
bool                      s_engineUp = false;

/* What the file browser auditions at: the kit's unity. */
constexpr int kPreviewVolume = 100;

// ── Path helpers ─────────────────────────────────────────────────────────
//
// Kits store device-absolute logical paths ("/crosspad/kits/X/kit.json").
// PortableKitLoader maps those through its root prefix, the engine through
// its own; anything here that opens a file itself has to do the same.

std::string fsPath(const std::string& logical) {
    return pc_platform_resolve_sdcard_path(logical);
}

// ── Player callbacks ─────────────────────────────────────────────────────
//
// Core's SamplerEngine (PcSamplerEngine) holds every sampler kit; a pitched
// kit is PcPitchedPort's. Kits store pan as a signed offset, 0 the centre,
// and the engine glue takes it that way.

void platform_noteOn(uint8_t padIdx, uint8_t velocity) {
    native_sampler_note_on(padIdx, velocity, 0, crosspad::kSamplePlaybackOwnKey);
}

void platform_setVolume(uint8_t padIdx, int volume) {
    native_sampler_set_pad_volume(padIdx, volume);
}

void platform_setPan(uint8_t padIdx, int pan) {
    native_sampler_set_pad_pan(padIdx, pan);
}

/* Pad Mixer's levels, split as the board's sampler splits them: a pitched
 * kit's pads are the instrument's, a sampler kit's the sample engine's. */
class PcPadMix final : public crosspad::IPadMix {
public:
    void setPadMix(uint8_t pad, int volume, int pan) override {
        auto* km  = crosspad::getKitManager();
        auto* kit = km ? km->getCurrentKit() : nullptr;
        if (kit && kit->engine == crosspad::KitEngine::Pitched) {
            if (auto* inst = crosspad::getPlatformServices().pitchedInstrument) {
                inst->setPadMix(pad, volume, pan);
            }
            return;
        }
        platform_setVolume(pad, volume);
        platform_setPan(pad, pan);
    }
};

/* The editor's loop button writes playbackMode into the kit model and then
 * says so; the engine reads the mode out of that model, so both calls are
 * the same question to it. */
void platform_setLoopEnd(uint8_t padIdx, uint32_t) {
    native_sampler_sync_pad_mode(padIdx);
}

void platform_clearLoop(uint8_t padIdx) {
    native_sampler_sync_pad_mode(padIdx);
}

void platform_wipeOut(uint8_t padIdx) {
    native_sampler_stop_pad(padIdx);
}

int platform_getFreeSlotCount() {
    return native_sampler_free_slots();
}

uint32_t platform_liveScope(float* outData, uint32_t maxSamples,
                            float* fundamentalHz, uint32_t* sampleRate) {
    if (fundamentalHz) *fundamentalHz = 0.0f; // a drum mix has no single note
    return native_sampler_scope_snapshot(outData, maxSamples, sampleRate);
}

void platform_setLiveScopeEnabled(bool enabled) {
    native_sampler_scope_enable(enabled);
}

/* The engine transposes in the voice, so a pitch-only pad wants the original
 * file and a step -- not a copy. Filters and reverse it does not do. */
bool platform_engineHandlesFx(int, uint16_t hpHz, uint16_t lpHz, bool reverse) {
    if (hpHz || lpHz || reverse) return false;
    return native_sampler_active();
}

bool platform_getSampleInfo(const char* path, crosspad_sampler::SampleInfo* outInfo) {
    if (!path || !outInfo) return false;
    uint32_t frames = 0, rate = 0;
    uint8_t channels = 0, bits = 0;
    if (!native_sampler_describe(path, &frames, &channels, &rate, &bits)) return false;
    outInfo->sampleCount      = frames;
    outInfo->sampleRate       = rate;
    outInfo->numberOfChannels = channels;
    outInfo->bitsPerSample    = bits;
    return true;
}

/* No path: the browser closed, the audition stops. */
void platform_previewSample(const char* path, uint32_t start, uint32_t end,
                            int volume, int pan) {
    native_sampler_preview(path, start, end, volume, pan);
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

    // While an app holds the pitched engine (PcInstrumentHost) the bank is
    // its own: a kit load leaves it alone, and release() reloads the kit.
    const bool appBank = instrument_host_owns_engine();
    if (kit->engine == crosspad::KitEngine::Pitched) {
        // The sample engine lets go of the last sampler kit, so a hit during
        // the load never sounds it; the whole bank is built once, on pad 0.
        native_sampler_release();
        if (padIdx == 0 && !appBank) pitched_port_load(*kit);
        return;
    }

    // Back on a sampler kit: the pitched bank goes first, so a hit never
    // sounds a zone of a kit that is no longer current.
    if (!appBank && pitched_port_active()) pitched_port_unload();

    // The first pad of a kit the engine does not hold yet takes it whole;
    // after that each pad swaps in under whatever is playing.
    native_sampler_swap_pad(padIdx, crosspad_sampler::kit_load_task_is_worker());
    const uint8_t named = kit->pads[padIdx].sampleCount;
    if (native_sampler_pad_ready(padIdx)) {
        ++s_padsLoaded;
        s_layersLoaded += named;
    } else {
        s_layersFailed += named;
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
    // The open song is made with this kit from now on.
    song_sounds_kit_loaded();
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
    // Only a request: the scenes store reads the card on its own task.
    sequencer_kit_changed(kit ? kit->path : std::string());
}

// ── Waveform callbacks ───────────────────────────────────────────────────

bool platform_wf_getFileInfo(const char* path, crosspad_sampler::AudioFileInfo* outInfo) {
    if (!path || !outInfo) return false;
    uint32_t frames = 0, rate = 0;
    uint8_t channels = 0, bits = 0;
    if (!native_sampler_describe(path, &frames, &channels, &rate, &bits)) return false;
    outInfo->sampleCount = frames;
    outInfo->sampleRate  = rate;
    outInfo->channels    = channels;
    outInfo->bitDepth    = bits;
    return true;
}

bool platform_wf_getWaveformSamples(const char* path, int16_t* outBuf, uint32_t outSamples,
                                    uint32_t startSample, uint32_t endSample) {
    return native_sampler_waveform(path, outBuf, outSamples, startSample, endSample);
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
        // The bank is an app's (PcInstrumentHost), not this kit's.
        if (instrument_host_owns_engine()) return;
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

    if (d->isOn) {
        // Velocity 0 is a note-off to the engine, so a pad reporting a
        // zero-velocity hit still has to make a sound.
        native_sampler_note_on(d->note, d->velocity ? d->velocity : 1, d->transpose, d->key);
    } else {
        native_sampler_note_off(d->note, d->key);
    }
}

/* A finger's hit straight from the pad logic, off the event queue, while the
 * sample engine holds the kit; an app holding the pitched engine, or a
 * pitched kit, keeps the event. */
bool native_engine_plays_pads() {
    if (instrument_host_owns_engine()) return false;
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit && kit->engine == crosspad::KitEngine::Pitched) return false;
    return native_sampler_active();
}

bool platform_armPad(uint8_t touchPad, uint8_t soundPad, int8_t transpose, uint8_t key) {
    return native_engine_plays_pads() && native_sampler_arm(touchPad, soundPad, transpose, key);
}

bool platform_playPad(uint8_t touchPad, uint8_t soundPad, uint8_t velocity, int8_t transpose,
                      uint8_t key) {
    if (!native_engine_plays_pads()) return false;
    native_sampler_play(touchPad, soundPad, velocity ? velocity : 1, transpose, key);
    return true;
}

/* The engine never takes a pad apart under a voice -- a voice of the old
 * sample plays out -- so the loader has nothing to wait for. */
bool platform_padSounding(uint8_t padIdx) {
    if (native_sampler_active()) return false;
    return crosspad::getPadManager().isPadPlaying(padIdx);
}

/* Whether a hit on the pad sounds, for its LED and the load's verdict. An
 * app holding the pitched engine plays the kit its own way. */
bool platform_padPlayable(uint8_t padIdx) {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit || padIdx >= KIT_PADS) return false;
    if (instrument_host_owns_engine()) return true;
    if (kit->engine == crosspad::KitEngine::Pitched) return pitched_port_active();
    return native_sampler_pad_ready(padIdx);
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

crosspad::IAudioNode* sampler_port_init(const crosspad_sampler::KitGateHost& gate) {
    crosspad::IAudioNode* node = native_sampler_node();
    s_engineUp = true;

    crosspad::getPlatformServices().kitManager = &s_kitLoader;
    static PcPadMix padMix;
    crosspad::getPlatformServices().padMix = &padMix;

    crosspad_sampler::SamplerPlatformCallbacks cb{};

    crosspad_sampler::kit_gate_install(gate, cb.kitSelector);

    cb.audio.startLoadKit      = platform_startLoadKit;
    cb.audio.onKitLoadFinished = platform_onKitLoadFinished;
    cb.audio.reloadPad         = platform_reloadPad;
    cb.audio.reloadPads        = platform_reloadPads;
    cb.audio.hasCacheFile      = platform_hasCacheFile;
    cb.audio.padSounding       = platform_padSounding;
    cb.audio.padPlayable       = platform_padPlayable;

    cb.player.noteOn           = platform_noteOn;
    cb.player.armPad           = platform_armPad;
    cb.player.playPad          = platform_playPad;
    cb.player.disarmPad        = native_sampler_disarm;
    cb.player.engineHandlesFx  = platform_engineHandlesFx;
    cb.player.setPadPitch      = native_sampler_set_pad_pitch;
    cb.player.previewSample    = platform_previewSample;
    cb.player.getSampleInfo    = platform_getSampleInfo;
    cb.player.setVolume        = platform_setVolume;
    cb.player.setPan           = platform_setPan;
    cb.player.setLoopEnd       = platform_setLoopEnd;
    cb.player.clearLoop        = platform_clearLoop;
    cb.player.wipeOut          = platform_wipeOut;
    cb.player.getFreeSlotCount = platform_getFreeSlotCount;
    cb.player.getLiveScopeData = platform_liveScope;
    cb.player.setLiveScopeEnabled = platform_setLiveScopeEnabled;

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
        platform_previewSample(path, 0, crosspad_sampler::SAMPLE_END_MAX, kPreviewVolume, 0);
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

    return node;
}

void sampler_port_load_kit(int kitId) {
    platform_startLoadKit(kitId, /*onlyModified=*/false);
}

void sampler_port_unload_kit() {
    native_sampler_release();
    if (!instrument_host_owns_engine() && pitched_port_active()) pitched_port_unload();
    s_kitLoader.clearCurrentKit();
    crosspad_sampler::getPadColorManager().loadColorsFromKit(crosspad::KitInfo{});
}

void sampler_port_set_sdcard(const std::string& root) {
    s_sdRoot = root;
    s_kitLoader.setRootPrefix(root);
    native_sampler_set_root(root);
    if (!s_engineUp) return;   // init() replays this

    rescanKits();
}

} // namespace crosspad_pc
