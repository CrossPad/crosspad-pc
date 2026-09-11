/**
 * @file crosspad_app.cpp
 * @brief Shared CrossPad application initialization.
 *
 * Extracted from main.cpp so that both the standard main loop and the
 * FreeRTOS main can share the same GUI setup with zero duplication.
 */

#include "crosspad_app.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <set>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <string>

#include "lvgl/lvgl.h"

#include "pc_stubs/pc_platform.h"
#include "pc_stubs/PcApp.hpp"
#include "updater/PcUpdater.hpp"

// crosspad-core
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"

// STM32 hardware emulator window
#include "stm32_emu/Stm32EmuWindow.hpp"
#include "remote/RemoteControl.hpp"

// crosspad-gui
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/components/app_launcher.h"
#include "crosspad-gui/components/main_screen.h"
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/components/volume_overlay.h"
#include "crosspad-gui/components/power_gesture.h"

#ifdef USE_MIDI
#include "midi/PcMidi.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#endif

#include "crosspad/midi/MidiInputHandler.hpp"

#ifdef USE_BLE
#include "midi/PcBleMidi.hpp"
#include "apps/settings/settings_app.h"
#endif

#ifdef USE_AUDIO
#include "audio/PcAudio.hpp"
#include "audio/PcAudioInput.hpp"
#include "audio/PcAudioModule.hpp"
#include "audio/sampler/PcPitchedPort.hpp"
#include "audio/sampler/PcSamplerPort.hpp"
#ifdef USE_VIRTUAL_AUDIO
#include "audio/virtual/IVirtualSinkManager.hpp"
#ifdef __linux__
#include "audio/virtual/PulseMonitorCapture.hpp"
#endif
#ifdef USE_PIPEWIRE
#include "audio/pipewire/PwContext.hpp"
#include "audio/pipewire/PwDefaultSinkGuard.hpp"
#include "audio/pipewire/PwVirtualSource.hpp"
#include "audio/pipewire/PwVirtualSinkCapture.hpp"
#include "audio/pipewire/PwSinkEnumerator.hpp"
#endif
#include <csignal>
#endif
#include "crosspad-gui/components/vu_meter.h"
#include "crosspad/audio/PeakMeter.hpp"
#include "crosspad/audio/SynthEngineNode.hpp"
#include "synth/MlPianoSynth.hpp"
#if __has_include("crosspad-mixer/AudioMixerEngine.hpp")
#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "crosspad-mixer/MixerPadLogic.hpp"
#define HAS_MIXER 1
#endif
#include <RtAudio.h>
#endif

#include "uart/PcUart.hpp"
#include <ArduinoJson.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define APP_BUTTON_SIZE 64
#define APP_BUTTON_NAME_VISIBLE true

/* ── State ────────────────────────────────────────────────────────────── */

extern lv_obj_t* status_c;

static lv_obj_t* app_c = nullptr;
static lv_obj_t* s_lcdContainer = nullptr;
static Stm32EmuWindow stm32Emu;

// Central MIDI router — distributes pad output to USB + BLE + STM32
static crosspad::MidiInputHandler s_midiHandler;

namespace crosspad {
MidiInputHandler& getMidiInputHandler() { return s_midiHandler; }
}

#ifdef USE_MIDI
static PcMidi midi;
static crosspad::Stm32MessageHandler stm32Handler;
extern CrosspadStatus status;
#endif

#ifdef USE_BLE
PcBleMidi bleMidi;
#endif

#ifdef USE_AUDIO
static PcAudioOutput pcAudio;      // OUT1
static PcAudioOutput pcAudio2;     // OUT2
static PcAudioInput  pcAudioIn1;   // IN1
static PcAudioInput  pcAudioIn2;   // IN2
static MlPianoSynth fmSynth;
static crosspad_pc::PcAudioModule s_audioModule;
static crosspad::SynthEngineNode s_synthNode;
// Owned by the sampler port; null when the engine failed to start.
static crosspad::IAudioNode* s_samplerNode = nullptr;
// The second sample engine: one PitchedInstrument, silent until a kit whose
// kit.json says "engine": "pitched" is loaded.
static crosspad::IAudioNode* s_pitchedNode = nullptr;

/// Sink each output wants to be pinned to, applied once audio is flowing.
/// Empty means "wherever the sound server puts it".
static std::string s_pendingPin[2];
#ifdef HAS_MIXER
#include "crosspad/audio/AudioInputNode.hpp"
#include "pc_stubs/pc_platform.h"
static AudioMixerEngine s_mixerEngine;
static std::shared_ptr<MixerPadLogic> s_mixerPadLogic;
// Adapters registered as mixer channels in IN1, IN2, SYNTH order so
// MixerInput::IN1=0, IN2=1, SYNTH=2 stay valid against the dynamic API.
// The indexed getter form picks up hot-swapped virtual inputs every cycle.
static crosspad::AudioInputNode s_in1Node{&pc_platform_get_audio_input, 0, "Input 1"};
static crosspad::AudioInputNode s_in2Node{&pc_platform_get_audio_input, 1, "Input 2"};
static crosspad::SynthEngineNode s_mixerSynthNode;
#endif
#ifdef USE_VIRTUAL_AUDIO
static std::unique_ptr<crosspad_pc::IVirtualSinkManager> s_virtualSinkManager;
static std::atomic<bool> s_shutdownRan{false};
#ifdef __linux__
// Two dedicated libpulse-simple capturers for the virtual sink monitors —
// keeps CrossPad decoupled from RtAudio's PULSE quirks (it aggregates
// sinks-per-card and hides per-sink sources). When active these are what
// gets registered with PlatformServices instead of PcAudioInput.
static crosspad_pc::PulseMonitorCapture s_pulseCap1;
static crosspad_pc::PulseMonitorCapture s_pulseCap2;
#endif
#ifdef USE_PIPEWIRE
// Native PipeWire session orchestration: default-sink takeover guard and the
// OUT1-tap virtual source ("CrossPad Out"). Only meaningful when the native
// PW backend is active (s_virtualSinkManager->input(i) != nullptr).
static crosspad_pc::PwDefaultSinkGuard s_pwGuard;
static crosspad_pc::PwVirtualSource s_pwSource;
#endif
#endif
#endif

/* ── Virtual USB/UART ─────────────────────────────────────────────────── */

static PcUart pcUart;

/* ── Device Preferences ──────────────────────────────────────────────── */

struct DevicePreferences {
    std::string audioOut1;
    std::string audioOut2;
    // Technical PA sink name when OUT was routed via `pactl move-sink-input`
    // to a PulseAudio-only sink (e.g. motherboard analog not exposed by ALSA).
    // Empty if OUT is bound directly via RtAudio. Persisted across restarts so
    // the "Family 17h Pro" / "Rembrandt HDMI" pick is restored on next launch.
    std::string audioOut1Pa;
    std::string audioOut2Pa;
    std::string audioIn1;
    std::string audioIn2;
    std::string midiOut;
    std::string midiIn;
    std::string sdcardPath;
    std::string uartPort;
    uint32_t    uartBaud = 115200;
    std::string bleDevice;    // Last connected BLE MIDI device address
    uint8_t     bleMode = 0;  // 0=Host, 1=Server
    // Native PipeWire: make CrossPad IN#1 the system default sink while active.
    bool        pwTakeoverDefault = true;
    // Sink name to restore on shutdown. Non-empty at startup means the previous
    // run died before restoring — used for crash recovery, not the live default.
    std::string pwPrevDefaultSink;
};

static DevicePreferences s_devicePrefs;

static std::string getDevicePrefsPath() {
    const char* dir = pc_platform_get_profile_dir();
    return std::string(dir) + "/device_preferences.json";
}

static std::string getMixerStatePath() {
    const char* dir = pc_platform_get_profile_dir();
    return std::string(dir) + "/mixer_state.json";
}

static void loadDevicePrefs() {
    std::string path = getDevicePrefsPath();
    std::ifstream f(path);
    if (!f.is_open()) {
        printf("[DevPrefs] No saved preferences at %s\n", path.c_str());
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    if (err) {
        printf("[DevPrefs] Parse error: %s\n", err.c_str());
        return;
    }

    if (doc["audio_out1"].is<const char*>()) s_devicePrefs.audioOut1 = doc["audio_out1"].as<const char*>();
    if (doc["audio_out2"].is<const char*>()) s_devicePrefs.audioOut2 = doc["audio_out2"].as<const char*>();
    if (doc["audio_out1_pa"].is<const char*>()) s_devicePrefs.audioOut1Pa = doc["audio_out1_pa"].as<const char*>();
    if (doc["audio_out2_pa"].is<const char*>()) s_devicePrefs.audioOut2Pa = doc["audio_out2_pa"].as<const char*>();
    if (doc["audio_in1"].is<const char*>())  s_devicePrefs.audioIn1  = doc["audio_in1"].as<const char*>();
    if (doc["audio_in2"].is<const char*>())  s_devicePrefs.audioIn2  = doc["audio_in2"].as<const char*>();
    if (doc["midi_out"].is<const char*>())   s_devicePrefs.midiOut   = doc["midi_out"].as<const char*>();
    if (doc["midi_in"].is<const char*>())    s_devicePrefs.midiIn    = doc["midi_in"].as<const char*>();
    if (doc["sdcard_path"].is<const char*>()) s_devicePrefs.sdcardPath = doc["sdcard_path"].as<const char*>();
    if (doc["uart_port"].is<const char*>())  s_devicePrefs.uartPort   = doc["uart_port"].as<const char*>();
    if (doc["uart_baud"].is<uint32_t>())     s_devicePrefs.uartBaud   = doc["uart_baud"].as<uint32_t>();
    if (doc["ble_device"].is<const char*>()) s_devicePrefs.bleDevice  = doc["ble_device"].as<const char*>();
    if (doc["ble_mode"].is<uint8_t>())       s_devicePrefs.bleMode    = doc["ble_mode"].as<uint8_t>();
    if (doc["pw_takeover_default"].is<bool>()) s_devicePrefs.pwTakeoverDefault = doc["pw_takeover_default"].as<bool>();
    if (doc["pw_prev_default_sink"].is<const char*>()) s_devicePrefs.pwPrevDefaultSink = doc["pw_prev_default_sink"].as<const char*>();

    printf("[DevPrefs] Loaded: out1='%s' out2='%s' in1='%s' in2='%s' midiOut='%s' midiIn='%s' sd='%s' uart='%s@%u'\n",
           s_devicePrefs.audioOut1.c_str(), s_devicePrefs.audioOut2.c_str(),
           s_devicePrefs.audioIn1.c_str(), s_devicePrefs.audioIn2.c_str(),
           s_devicePrefs.midiOut.c_str(), s_devicePrefs.midiIn.c_str(),
           s_devicePrefs.sdcardPath.c_str(),
           s_devicePrefs.uartPort.c_str(), s_devicePrefs.uartBaud);
}

static void saveDevicePrefs() {
    std::string path = getDevicePrefsPath();
    JsonDocument doc;
    doc["audio_out1"] = s_devicePrefs.audioOut1;
    doc["audio_out2"] = s_devicePrefs.audioOut2;
    doc["audio_out1_pa"] = s_devicePrefs.audioOut1Pa;
    doc["audio_out2_pa"] = s_devicePrefs.audioOut2Pa;
    doc["audio_in1"]  = s_devicePrefs.audioIn1;
    doc["audio_in2"]  = s_devicePrefs.audioIn2;
    doc["midi_out"]    = s_devicePrefs.midiOut;
    doc["midi_in"]     = s_devicePrefs.midiIn;
    doc["sdcard_path"] = s_devicePrefs.sdcardPath;
    doc["uart_port"]   = s_devicePrefs.uartPort;
    doc["uart_baud"]   = s_devicePrefs.uartBaud;
    doc["ble_device"]  = s_devicePrefs.bleDevice;
    doc["ble_mode"]    = s_devicePrefs.bleMode;
    doc["pw_takeover_default"]  = s_devicePrefs.pwTakeoverDefault;
    doc["pw_prev_default_sink"] = s_devicePrefs.pwPrevDefaultSink;

    std::ofstream f(path);
    if (!f.is_open()) {
        printf("[DevPrefs] Failed to write %s\n", path.c_str());
        return;
    }
    serializeJsonPretty(doc, f);
    printf("[DevPrefs] Saved to %s\n", path.c_str());
}

/* ── Audio/MIDI device enumeration helpers ───────────────────────────── */

#ifdef USE_AUDIO
struct AudioDeviceEntry {
    unsigned int rtAudioId;
    std::string name;
};

/// Enumerate output devices with 2+ output channels.
static std::vector<AudioDeviceEntry> enumerateAudioOutputDevices() {
    std::vector<AudioDeviceEntry> result;
    RtAudio probe;
    for (unsigned int id : probe.getDeviceIds()) {
        RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
        if (info.outputChannels >= 2) {
            result.push_back({id, info.name});
        }
    }
    return result;
}

/// Enumerate input devices with 2+ input channels.
static std::vector<AudioDeviceEntry> enumerateAudioInputDevices() {
    std::vector<AudioDeviceEntry> result;
    RtAudio probe;
    for (unsigned int id : probe.getDeviceIds()) {
        RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
        if (info.inputChannels >= 2) {
            result.push_back({id, info.name});
        }
    }
    return result;
}

/// Devices that route into the SYSTEM DEFAULT sink. With default-sink
/// takeover active the default IS crosspad_vin1, so opening OUT on one of
/// these feeds CrossPad's own output back into its own input — feedback loop.
static bool isLoopbackRiskDevice(const std::string& name) {
    return name.find("Default ALSA Device") != std::string::npos ||
           name.find("PulseAudio Sound Server") != std::string::npos;
}

static bool loopGuardActive() {
#if defined(USE_PIPEWIRE)
    return s_devicePrefs.pwTakeoverDefault;
#else
    return false;
#endif
}

/// One selectable entry of the OUT1/OUT2 dropdowns.
/// rtAudioId != 0 → a real RtAudio device; otherwise paSinkName names a
/// PulseAudio/PipeWire sink reached via move-sink-input.
struct OutUiEntry {
    std::string  label;
    unsigned int rtAudioId = 0;
    std::string  paSinkName;
};

/// Single source of truth for what the OUT dropdowns offer (minus "(None)").
/// Applies the feedback-loop guard and the PA-sink overlap dedupe so the
/// populate site and the selection callback can never disagree on indices.
static std::vector<OutUiEntry> buildOutDeviceUiEntries() {
    std::vector<OutUiEntry> entries;
    auto outDevices = enumerateAudioOutputDevices();
    const bool guard = loopGuardActive();
    for (auto& d : outDevices) {
        if (guard && isLoopbackRiskDevice(d.name)) continue;
        entries.push_back({d.name, d.rtAudioId, {}});
    }
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
    // Add only PulseAudio sinks that RtAudio's ALSA backend missed — typically
    // the motherboard analog jack and the active HDMI display. Token-overlap
    // dedupe on long, hardware-identifying tokens only; generic words would
    // falsely flag e.g. the Pro Audio sink as a duplicate.
    static const std::vector<std::string> kStopWords = {
        "Audio", "HD-Audio", "Generic", "Output", "Stereo", "Default",
        "Pro", "Sound", "Server", "Pulse", "PulseAudio", "ALSA",
        "Device", "Analog", "Digital", "Family"
    };
    auto isStopWord = [](const std::string& w) {
        for (auto& sw : kStopWords) if (w == sw) return true;
        return false;
    };
    auto overlapsRtAudio = [&](const std::string& paDesc) {
        std::istringstream tok(paDesc);
        std::string w;
        while (tok >> w) {
            if (w.size() < 6 || isStopWord(w)) continue;
            for (auto& d : outDevices) {
                if (d.name.find(w) != std::string::npos) return true;
            }
        }
        return false;
    };
    // Sink list: prefer the native PipeWire registry (node.description is
    // always present — no locale-fragile pactl parsing, no raw
    // "alsa_output.pci-..." labels when a description lookup misses).
    // Fall back to the pactl parser when the daemon is unreachable.
    struct SinkOption { std::string name, description; };
    std::vector<SinkOption> sinks;
#if defined(USE_PIPEWIRE)
    for (auto& s : crosspad_pc::pwEnumerateSinks())
        sinks.push_back({s.name, s.description});
#endif
    if (sinks.empty()) {
        for (auto& s : crosspad_pc::enumeratePulseSinks())
            sinks.push_back({s.name, s.description});
    }
    for (auto& s : sinks) {
        if (!overlapsRtAudio(s.description))
            entries.push_back({s.description, 0, s.name});
    }
#endif
    // Stable, enumeration-order-independent ordering. RtAudio does not
    // guarantee the same device order between two enumerations, so without
    // this a rebuild between showing the dropdown and clicking it (the UI
    // populate timer, or a second remote *_list) could move a device to a
    // different index and open the wrong one. Sorting by label pins each
    // device to a fixed slot.
    std::sort(entries.begin(), entries.end(),
              [](const OutUiEntry& a, const OutUiEntry& b) { return a.label < b.label; });
    return entries;
}

/* ── Cached UI device lists + thread-safe selection ─────────────────────
 * The dropdowns are populated from these caches and the selection resolves
 * the chosen index against the SAME cache — never a fresh enumeration.
 * Rebuilding the list between showing it and clicking it was how a graph
 * change (a sink appearing, RtAudio reordering its ids) shifted indices and
 * opened a different device than the one the label named. The caches refresh
 * on every populate and on every remote *_list. */
struct InUiEntry {
    std::string  label;              ///< what the dropdown shows
    unsigned int rtAudioId = 0;      ///< 0 => not a physical RtAudio device
    std::string  captureDeviceName;  ///< virtual-sink capture target (Pulse)
    bool         isVirtual = false;
};

static std::vector<OutUiEntry> s_outUiCache;
static std::vector<InUiEntry>  s_inUiCache;

// Carries a completed device switch from the worker thread back to the LVGL
// thread, where the jack-panel widgets may be touched.
struct AudioJackUiResult {
    int         jackId;
    bool        connected;
    std::string label;
};
// Serialises device switching: the UI runs it on a worker thread so the LVGL
// thread never blocks on an ALSA/PulseAudio open, and the remote-control
// server runs it on its TCP thread — both go through this lock.
static std::mutex s_audioSelMutex;

/// IN dropdown entries: physical RtAudio inputs relabelled with PipeWire
/// source descriptions (parity with the OUT dropdown), the hw:/plughw:/
/// sysdefault: aliases of one card collapsed to a single entry, then
/// CrossPad's virtual sinks appended at the end.
static std::vector<InUiEntry> buildInDeviceUiEntries() {
    std::vector<InUiEntry> entries;
    auto inDevices = enumerateAudioInputDevices();
#if defined(USE_PIPEWIRE)
    auto pwSources = crosspad_pc::pwEnumerateSources();
    auto prettyName = [&](const std::string& raw) -> std::string {
        std::istringstream tok(raw);
        std::string w;
        while (tok >> w) {
            if (w.size() < 6) continue;
            for (auto& s : pwSources)
                if (s.description.find(w) != std::string::npos) return s.description;
        }
        return raw;
    };
#else
    auto prettyName = [](const std::string& raw) { return raw; };
#endif
    std::set<std::string> seen;
    for (auto& d : inDevices) {
        std::string label = prettyName(d.name);
        if (!seen.insert(label).second) continue;   // dedupe aliases of one card
        entries.push_back({label, d.rtAudioId, {}, false});
    }
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
    if (s_virtualSinkManager) {
        for (auto& v : s_virtualSinkManager->list())
            entries.push_back({v.displayName, 0, v.captureDeviceName, true});
    }
#endif
    // Same enumeration-order-independence as the OUT list: pin each entry to a
    // fixed index by label so a rebuild never re-slots a device.
    std::sort(entries.begin(), entries.end(),
              [](const InUiEntry& a, const InUiEntry& b) { return a.label < b.label; });
    return entries;
}

/// Rebuild the OUT cache and return the dropdown labels ("(None)" at index 0).
/// Locked because the remote-control thread lists while the LVGL thread may
/// be populating, and the selection functions read the cache under the lock.
static std::vector<std::string> refreshOutUiCache() {
    std::lock_guard<std::mutex> lk(s_audioSelMutex);
    s_outUiCache = buildOutDeviceUiEntries();
    std::vector<std::string> names;
    names.push_back("(None)");
    for (auto& e : s_outUiCache) names.push_back(e.label);
    return names;
}

/// Rebuild the IN cache and return the dropdown labels ("(None)" at index 0).
static std::vector<std::string> refreshInUiCache() {
    std::lock_guard<std::mutex> lk(s_audioSelMutex);
    s_inUiCache = buildInDeviceUiEntries();
    std::vector<std::string> names;
    names.push_back("(None)");
    for (auto& e : s_inUiCache) names.push_back(e.label);
    return names;
}

/// Apply an OUT selection by dropdown index against the cached list. Blocking
/// (RtAudio/PA open) — never call from the LVGL thread; wrap it in a worker.
/// @return true if a device is connected on the slot afterwards.
static bool appAudioOutSelectIdx(int slot, int uiIndex, std::string& outLabel) {
    std::lock_guard<std::mutex> lk(s_audioSelMutex);
    auto& output = (slot == 0) ? pcAudio : pcAudio2;
    if (uiIndex <= 0) {
        output.end();
        outLabel.clear();
        if (slot == 0) { s_devicePrefs.audioOut1.clear(); s_devicePrefs.audioOut1Pa.clear(); }
        else           { s_devicePrefs.audioOut2.clear(); s_devicePrefs.audioOut2Pa.clear(); }
        saveDevicePrefs();
        return false;
    }
    size_t realIdx = static_cast<size_t>(uiIndex) - 1;
    if (realIdx >= s_outUiCache.size()) return output.isOpen();
    const auto& sel = s_outUiCache[realIdx];
    bool connected = false;
    if (sel.rtAudioId != 0) {
        output.switchDevice(sel.rtAudioId);
        connected = output.isOpen();
        outLabel = output.getCurrentDeviceName();
        if (slot == 0) { s_devicePrefs.audioOut1 = outLabel; s_devicePrefs.audioOut1Pa.clear(); }
        else           { s_devicePrefs.audioOut2 = outLabel; s_devicePrefs.audioOut2Pa.clear(); }
    }
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
    else {
        // PA-only sink — open the RtAudio stream on the PulseAudio server
        // device, then move the sink-input onto the chosen sink.
        if (!output.isOpen()) {
            for (auto& d : enumerateAudioOutputDevices()) {
                if (d.name.find("PulseAudio") != std::string::npos) {
                    output.switchDevice(d.rtAudioId);
                    break;
                }
            }
        }
        if (crosspad_pc::movePulseOutputToSink(slot, sel.paSinkName)) {
            connected = true;
            outLabel = sel.label;
            if (slot == 0) { s_devicePrefs.audioOut1 = sel.label; s_devicePrefs.audioOut1Pa = sel.paSinkName; }
            else           { s_devicePrefs.audioOut2 = sel.label; s_devicePrefs.audioOut2Pa = sel.paSinkName; }
        }
    }
#endif
    saveDevicePrefs();
    return connected;
}

/// Apply an IN selection by dropdown index against the cached list. Blocking —
/// same worker-thread rule as appAudioOutSelectIdx.
static bool appAudioInSelectIdx(int slot, int uiIndex, std::string& outLabel) {
    std::lock_guard<std::mutex> lk(s_audioSelMutex);
    auto& input = (slot == 0) ? pcAudioIn1 : pcAudioIn2;
    // Tear down whichever capturer was bound to this slot — saves juggling
    // state when switching between physical mics and virtual sinks.
    input.end();
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
    auto& cap = (slot == 0) ? s_pulseCap1 : s_pulseCap2;
    cap.stop();
#endif
    if (uiIndex <= 0) {
        outLabel.clear();
        pc_platform_set_audio_input(slot, nullptr);
        if (slot == 0) s_devicePrefs.audioIn1.clear(); else s_devicePrefs.audioIn2.clear();
        saveDevicePrefs();
        return false;
    }
    size_t realIdx = static_cast<size_t>(uiIndex) - 1;
    if (realIdx >= s_inUiCache.size()) { saveDevicePrefs(); return false; }
    const auto& sel = s_inUiCache[realIdx];
    bool connected = false;
    if (!sel.isVirtual && sel.rtAudioId != 0) {
        input.switchDevice(sel.rtAudioId);
        connected = input.isOpen();
        if (connected) pc_platform_set_audio_input(slot, &input);
        outLabel = sel.label;   // store the shown label, not the raw device id
        if (slot == 0) s_devicePrefs.audioIn1 = sel.label; else s_devicePrefs.audioIn2 = sel.label;
    }
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
    else if (sel.isVirtual) {
        uint32_t rate = pcAudio.isOpen() ? pcAudio.getSampleRate() : 44100;
        if (cap.start(sel.captureDeviceName, rate, 256)) {
            connected = true;
            outLabel = sel.label;
            pc_platform_set_audio_input(slot, &cap);
            if (slot == 0) s_devicePrefs.audioIn1 = sel.label; else s_devicePrefs.audioIn2 = sel.label;
        }
    }
#endif
    saveDevicePrefs();
    return connected;
}

/// Find device ID by name in a list. Returns 0 (default) if not found.
static unsigned int findDeviceByName(const std::vector<AudioDeviceEntry>& devices,
                                      const std::string& name) {
    if (name.empty()) return 0;
    for (auto& d : devices) {
        if (d.name == name) return d.rtAudioId;
    }
    return 0;
}

/// Find dropdown index (1-based, 0 = None) for a device name.
static int findDropdownIndex(const std::vector<std::string>& list, const std::string& name) {
    if (name.empty()) return 0;
    for (size_t i = 1; i < list.size(); i++) {
        if (list[i] == name) return (int)i;
    }
    return 0;
}
#endif

#ifdef USE_MIDI
/// Find port index by name substring (case-insensitive).
static int findMidiPortByName(const std::string& name, bool isOutput) {
    if (name.empty()) return -1;

    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (isOutput) {
        RtMidiOut probe;
        for (unsigned int i = 0; i < probe.getPortCount(); i++) {
            std::string pn = probe.getPortName(i);
            std::string pnLower = pn;
            std::transform(pnLower.begin(), pnLower.end(), pnLower.begin(), ::tolower);
            if (pnLower.find(lower) != std::string::npos || lower.find(pnLower) != std::string::npos
                || pn == name) {
                return (int)i;
            }
        }
    } else {
        RtMidiIn probe;
        for (unsigned int i = 0; i < probe.getPortCount(); i++) {
            std::string pn = probe.getPortName(i);
            std::string pnLower = pn;
            std::transform(pnLower.begin(), pnLower.end(), pnLower.begin(), ::tolower);
            if (pnLower.find(lower) != std::string::npos || lower.find(pnLower) != std::string::npos
                || pn == name) {
                return (int)i;
            }
        }
    }
    return -1;
}
#endif

/* ── App Orchestrator setup ───────────────────────────────────────────── */

static void LoadMainScreen(lv_obj_t* parent);

/// Resolve short icon names (e.g. "info.png") to full asset paths.
/// Icons that already contain a path separator are left unchanged.
static std::vector<std::string> s_resolvedIcons;
static const char* pc_icon_resolver(size_t /*index*/, const char* icon) {
    if (!icon || !*icon) return icon;
    // Already a full path?
    if (strchr(icon, '/') || strchr(icon, '\\')) return icon;
    // LVGL symbols (UTF-8 private use area, start with 0xEF)
    if ((unsigned char)icon[0] >= 0xC0) return icon;
    // Prepend asset prefix
    std::string resolved = crosspad_gui::getGuiPlatform().assetPathPrefix();
    resolved += icon;
    s_resolvedIcons.push_back(std::move(resolved));
    return s_resolvedIcons.back().c_str();
}

static void InitializeOrchestrator() {
    crosspad_gui::OrchestratorConfig config;
    config.app_factory = crosspad_gui::defaultAppFactory<App>;
    config.icon_resolver = pc_icon_resolver;
#ifdef USE_AUDIO
    // An app that needs a kit and has none gets the kit selector first — the
    // same gate the device puts in front of the sampler.
    config.pre_launch = crosspad_pc::sampler_port_pre_launch;
#endif
    crosspad_gui::AppOrchestrator::getInstance().init(config);
}

static void LoadMainScreen(lv_obj_t* parent) {
    if (parent == nullptr) parent = lv_screen_active();

    crosspad::getPadManager().setActivePadLogic("Mixer");

    auto& orch = crosspad_gui::AppOrchestrator::getInstance();

    crosspad_gui::MainScreenConfig config;
    config.encoder        = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    config.on_select      = crosspad_gui::AppOrchestrator::onAppSelected;
    config.on_close       = crosspad_gui::AppOrchestrator::onPopupClose;
    config.button_size    = APP_BUTTON_SIZE;
    config.button_spacing = 10;
    config.show_names     = APP_BUTTON_NAME_VISIBLE;
    config.bg_color       = lv_color_black();

    auto result = orch.loadMainScreen(parent, config);

    status_c = result.status_bar;
    app_c    = result.app_container;

    crosspad_app_update_pad_icon();
    printf("[GUI] Main screen loaded with %zu apps\n", orch.getAppInfos().size());
}

/* ── Public API ───────────────────────────────────────────────────────── */

void crosspad_app_init()
{
#if defined(USE_AUDIO) && defined(USE_VIRTUAL_AUDIO)
    /* Make sure virtual sinks get unloaded whatever the exit path.
       SDL_QUIT calls crosspad_app_shutdown() directly; atexit and signals
       catch the remaining cases (normal return, SIGINT, SIGTERM).
       kill -9 is unrecoverable — sinks will be cleaned up at next launch
       by LinuxPipewireSinks::cleanupStaleSinks(). */
    std::atexit(crosspad_app_shutdown);
    auto sigHandler = [](int sig) { crosspad_app_shutdown(); std::signal(sig, SIG_DFL); std::raise(sig); };
    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);
#endif

    /* Platform stubs (event bus, settings, pad manager, GUI) */
    pc_platform_init();

    /* Register go-home callback for shared Settings UI */
    pc_platform_set_go_home([]() { LoadMainScreen(s_lcdContainer); });

    /* Load saved device preferences */
    loadDevicePrefs();

    /* STM32 emulator window — returns 320x240 LCD container */
    lv_obj_t* lcdContainer = stm32Emu.init();
    s_lcdContainer = lcdContainer;

    /* Wire keyboard shortcuts: Escape→go home, Space/Ctrl→power button
     * (tap = contextual back, hold 0.5 s = quick settings), V→volume overlay */
    stm32Emu.getKeyboardCapture().setEscapeCallback(crosspad_app_go_home);
    stm32Emu.getKeyboardCapture().setPowerCallback(crosspad_gui::powerButtonPress);
    stm32Emu.getKeyboardCapture().setPowerReleaseCallback(crosspad_gui::powerButtonRelease);
    stm32Emu.getKeyboardCapture().setVolumeCallback(crosspad_gui::volume_overlay_toggle);

    /* Virtual SD card slot — auto-mount from saved preferences */
    if (!s_devicePrefs.sdcardPath.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::exists(s_devicePrefs.sdcardPath, ec)) {
            pc_platform_set_sdcard_path(s_devicePrefs.sdcardPath);
#ifdef USE_AUDIO
            crosspad_pc::sampler_port_set_sdcard(s_devicePrefs.sdcardPath);
#endif
            stm32Emu.getSdCardSlot().setMounted(true, s_devicePrefs.sdcardPath);
            printf("[SDCard] Auto-mounted from saved prefs: %s\n", s_devicePrefs.sdcardPath.c_str());
        } else {
            printf("[SDCard] Saved path no longer exists: %s\n", s_devicePrefs.sdcardPath.c_str());
            s_devicePrefs.sdcardPath.clear();
            saveDevicePrefs();
        }
    }

    /* Wire SD card slot mount/unmount callbacks */
    stm32Emu.getSdCardSlot().setOnMount([](const std::string& path) {
        pc_platform_set_sdcard_path(path);
#ifdef USE_AUDIO
        // Kits and samples live on the card; rescanning here is what makes a
        // card mounted mid-session show up in the kit selector.
        crosspad_pc::sampler_port_set_sdcard(path);
#endif
        s_devicePrefs.sdcardPath = path;
        saveDevicePrefs();
    });
    stm32Emu.getSdCardSlot().setOnUnmount([]() {
        pc_platform_set_sdcard_path("");
#ifdef USE_AUDIO
        crosspad_pc::sampler_port_set_sdcard("");
#endif
        s_devicePrefs.sdcardPath.clear();
        saveDevicePrefs();
    });

    /* Overlay layer on lv_layer_top(), positioned over the LCD area. */
    lv_obj_t* overlayLayer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlayLayer);
    lv_obj_set_pos(overlayLayer, (Stm32EmuWindow::WIN_W - 320) / 2, 20);
    lv_obj_set_size(overlayLayer, 320, 240);
    lv_obj_remove_flag(overlayLayer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    crosspad_gui::setOverlayParent(overlayLayer);

    // Init MidiInputHandler — central routing brain (same pattern as ESP32).
    // Routes pad output to USB + BLE + STM32 based on KeypadSettings flags.
    s_midiHandler.init(crosspad::getPadManager(), crosspad::getEventBus(),
                       crosspad::CrosspadSettings::getInstance());
    crosspad::getPlatformServices().setMidiOutput(&s_midiHandler);

#ifdef USE_MIDI
    // Initialize STM32 message handler
    stm32Handler.init(crosspad::getPadManager(), status);

    // Auto-connect MIDI from saved preferences or fall back to "CrossPad" keyword
    {
        int outPort = findMidiPortByName(s_devicePrefs.midiOut, true);
        int inPort  = findMidiPortByName(s_devicePrefs.midiIn, false);

        if (outPort >= 0 && inPort >= 0) {
            midi.begin((unsigned)outPort, (unsigned)inPort);
            midi.setAutoConnectKeyword("CrossPad");
            printf("[MIDI] Connected from saved prefs: out=%d in=%d\n", outPort, inPort);
        } else {
            midi.beginAutoConnect("CrossPad");
        }
        s_midiHandler.setUsbOutput(&midi);
    }

    midi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        struct D { uint8_t ch, note, vel; };
        auto* d = new D{channel, note, velocity};
        lv_async_call([](void* ud) {
            auto* n = static_cast<D*>(ud);
            auto& pm = crosspad::getPadManager();
            uint8_t padIdx = pm.getPadForMidiNote(n->note);
            if (padIdx < 16) pm.handlePadPress(padIdx, n->vel);
            else pm.handleMidiNoteOn(n->ch, n->note, n->vel);
            delete n;
        }, d);
    });
    midi.setHandleNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOff ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        struct D { uint8_t ch, note; };
        auto* d = new D{channel, note};
        lv_async_call([](void* ud) {
            auto* n = static_cast<D*>(ud);
            auto& pm = crosspad::getPadManager();
            uint8_t padIdx = pm.getPadForMidiNote(n->note);
            if (padIdx < 16) pm.handlePadRelease(padIdx);
            else pm.handleMidiNoteOff(n->ch, n->note);
            delete n;
        }, d);
    });
    midi.setHandleControlChange([](uint8_t channel, uint8_t cc, uint8_t value) {
        printf("[MIDI IN] CC      ch=%u cc=%u  val=%u\n", channel + 1, cc, value);
        if (channel == 0 && cc == 1) {
            stm32Emu.handleEncoderCC(value, 31, 18);
        } else if (channel == 0 && cc == 64) {
            stm32Emu.handleEncoderPress(value >= 64);
        }
    });
    midi.setHandleSystemExclusive([](uint8_t* data, unsigned size) {
        stm32Handler.handleMessage(data, size);
    });

    // Periodic MIDI reconnect check (3s).
    // IMPORTANT: RtMidi port enumeration is BLOCKING (100-500ms on Windows).
    // We only check connection state on the LVGL thread — the actual reconnect
    // (with its blocking enumeration) runs on a background thread.
    static std::atomic<bool> s_midiReconnecting{false};
    lv_timer_create([](lv_timer_t*) {
        // Reconnect if output died, OR if we don't have a keyword match
        // (a new CrossPad device may have appeared). reconnect() itself
        // short-circuits if nothing changed, so this is cheap.
        bool needsReconnect = !midi.isOutputOpen() || !midi.isKeywordConnected();
        if (needsReconnect && !s_midiReconnecting.load()) {
            s_midiReconnecting.store(true);
            std::thread([]() {
                midi.reconnect();
                s_midiReconnecting.store(false);
            }).detach();
        }

        // Update jack panel from current MIDI state (no enumeration — just read cached names)
        auto& jp = stm32Emu.getJackPanel();
        jp.setConnected(EmuJackPanel::MIDI_OUT, midi.isOutputOpen());
        jp.setConnected(EmuJackPanel::MIDI_IN, midi.isInputOpen());
    }, 3000, nullptr);
#endif

#ifdef USE_BLE
    // ── BLE MIDI — same pattern as USB MIDI ──
    {
        auto* settings = crosspad::CrosspadSettings::getInstance();
        // Use saved mode from device prefs if available
        if (s_devicePrefs.bleMode <= 1) {
            settings->wireless.bleMidiMode = s_devicePrefs.bleMode;
        }
        auto mode = settings->wireless.bleMidiMode == 0
            ? crosspad::BleMidiMode::Host : crosspad::BleMidiMode::Server;

        bleMidi.setNoteOffset(settings->wireless.bleMidiNoteOffset);
        bleMidi.begin(mode);

        // Wire into routing system
        crosspad::getPlatformServices().setBleMidi(&bleMidi);
        s_midiHandler.setBleOutput(&bleMidi);  // Route pad output to BLE

        // Input callbacks — dispatch to LVGL thread via lv_async_call.
        // RtMidi callbacks run on a separate thread, but PadManager/LED updates
        // touch LVGL which is NOT thread-safe. Must dispatch to the LVGL task.
        // Echo is already filtered by PcBleMidi's anti-loopback ring buffer.
        bleMidi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
            printf("[BLE IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
            ble_settings_log_midi_in(0x90 | channel, note, velocity);

            struct NoteData { uint8_t ch; uint8_t note; uint8_t vel; };
            auto* d = new NoteData{channel, note, velocity};
            lv_async_call([](void* ud) {
                auto* n = static_cast<NoteData*>(ud);
                auto& pm = crosspad::getPadManager();
                uint8_t padIdx = pm.getPadForMidiNote(n->note);
                if (padIdx < 16)
                    pm.handlePadPress(padIdx, n->vel);
                else
                    pm.handleMidiNoteOn(n->ch, n->note, n->vel);
                delete n;
            }, d);
        });
        bleMidi.setHandleNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
            printf("[BLE IN] NoteOff ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
            ble_settings_log_midi_in(0x80 | channel, note, velocity);

            struct NoteData { uint8_t ch; uint8_t note; };
            auto* d = new NoteData{channel, note};
            lv_async_call([](void* ud) {
                auto* n = static_cast<NoteData*>(ud);
                auto& pm = crosspad::getPadManager();
                uint8_t padIdx = pm.getPadForMidiNote(n->note);
                if (padIdx < 16)
                    pm.handlePadRelease(padIdx);
                else
                    pm.handleMidiNoteOff(n->ch, n->note);
                delete n;
            }, d);
        });
        bleMidi.setHandleControlChange([](uint8_t channel, uint8_t cc, uint8_t value) {
            printf("[BLE IN] CC      ch=%u cc=%u  val=%u\n", channel + 1, cc, value);
            ble_settings_log_midi_in(0xB0 | channel, cc, value);
        });

        // Connection state → status bar bluetooth icon + persist device address
        bleMidi.setOnConnectionChanged([](bool connected, const crosspad::BleMidiDevice& dev) {
            printf("[BLE MIDI] %s: %s (%s)\n",
                   connected ? "Connected" : "Disconnected",
                   dev.name.c_str(), dev.address.c_str());
            auto* st = crosspad::getPlatformServices().status;
            if (st) st->bluetoothConnected = connected;

            if (connected) {
                s_devicePrefs.bleDevice = dev.address;
                saveDevicePrefs();
            }
        });
    }
#endif

#ifdef USE_AUDIO
    // ── Audio OUT1/OUT2: auto-connect saved device or default ──
    //
    // Preference resolution order per slot:
    //   1. If audioOutNPa is set — saved pick was a PulseAudio-only sink
    //      (motherboard analog / HDMI port hidden from RtAudio's ALSA backend).
    //      Open RtAudio on "PulseAudio Sound Server" so we have a real
    //      sink-input, then move it onto the remembered PA sink.
    //   2. Else if audioOutN matches an RtAudio device name — open that directly.
    //   3. Else — RtAudio default device.
    auto openOutputSlot = [&](PcAudioOutput& output, int slot,
                              std::string& savedName, std::string& savedPa) {
        auto outDevices = enumerateAudioOutputDevices();
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
        if (!savedPa.empty()) {
            // Pinning moves the stream this open creates, so the device has to
            // be one that appears in the sound server's graph: RtAudio's
            // default goes through pipewire-alsa and does, while opening a
            // hardware device directly does not, and on a PipeWire host the
            // ALSA "pulse" plugin can open without ever creating a node.
            //
            // The move itself cannot happen here. The node only materialises
            // once audio is actually written, and nothing writes until the
            // audio module starts, which is much later in init — attempting it
            // at open time simply never finds the stream. The pin is recorded
            // and applied after start(); see s_pendingPin below.
            output.begin(0);
            if (output.isOpen()) {
                s_pendingPin[slot] = savedPa;
                return;
            }
            printf("[Audio] OUT%d: default device would not open, cannot pin to '%s'\n",
                   slot + 1, savedPa.c_str());
            savedPa.clear();
        }
#endif
        unsigned int devId = findDeviceByName(outDevices, savedName);
        if (loopGuardActive()) {
            // Saved pick itself is a loopback risk → treat as not found.
            if (devId != 0 && isLoopbackRiskDevice(savedName)) devId = 0;
            // No (usable) saved device: RtAudio's default would be the
            // "Default ALSA Device" → our own virtual sink. Pick the first
            // real hardware device instead of the default.
            if (devId == 0) {
                for (auto& d : outDevices) {
                    if (!isLoopbackRiskDevice(d.name)) { devId = d.rtAudioId; break; }
                }
            }
        }
        output.begin(devId);
        if (output.isOpen()) {
            savedName = output.getCurrentDeviceName();
        }
    };

    openOutputSlot(pcAudio,  0, s_devicePrefs.audioOut1, s_devicePrefs.audioOut1Pa);
    openOutputSlot(pcAudio2, 1, s_devicePrefs.audioOut2, s_devicePrefs.audioOut2Pa);
    pc_platform_set_audio_output_2(&pcAudio2);

    // ── Audio IN1/IN2: auto-connect saved devices ──
    // Use output sample rate so mixer doesn't need sample rate conversion
    uint32_t outSampleRate = pcAudio.isOpen() ? pcAudio.getSampleRate() : 44100;
    pc_platform_set_audio_input(0, &pcAudioIn1);
    pc_platform_set_audio_input(1, &pcAudioIn2);

#ifdef USE_VIRTUAL_AUDIO
    // Create OS-visible virtual sinks ("CrossPad virtual IN#1/#2") that other
    // apps and DAWs can route audio to. RtAudio's monitor-source discovery
    // runs afterwards — the sinks must exist before enumerateAudioInputDevices.
    s_virtualSinkManager = crosspad_pc::makeVirtualSinkManager();
    if (s_virtualSinkManager) {
        if (!s_virtualSinkManager->setup(2)) {
            printf("[VirtSink] Virtual sinks not available: %s\n",
                   s_virtualSinkManager->errorHint().c_str());
        }
    }
#endif

    {
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
        // Prefer virtual sinks over saved physical inputs — the whole point of
        // the feature is that CrossPad becomes a system-wide mixer by default.
        // We bind directly via libpulse-simple; see PulseMonitorCapture.
        bool in1Virtual = false, in2Virtual = false;
        if (s_virtualSinkManager) {
            auto sinks = s_virtualSinkManager->list();
            // Native PW backend exposes each sink directly as an IAudioInput —
            // no libpulse monitor round-trip. input() returns nullptr for the
            // pactl/RtAudio fallback backend, in which case we use the Pulse
            // capturer path below. (input() is on the base interface.)
            crosspad::IAudioInput* nativeIn1 = s_virtualSinkManager->input(0);
            crosspad::IAudioInput* nativeIn2 = s_virtualSinkManager->input(1);
            if (nativeIn1) {
                printf("[Audio] IN1 connected to native virtual sink (crosspad_vin1)\n");
                pc_platform_set_audio_input(0, nativeIn1);
                in1Virtual = true;
            } else if (sinks.size() >= 1 &&
                       s_pulseCap1.start(sinks[0].captureDeviceName, outSampleRate)) {
                printf("[Audio] IN1 connected to virtual sink: %s\n",
                       sinks[0].displayName.c_str());
                pc_platform_set_audio_input(0, &s_pulseCap1);
                in1Virtual = true;
            }
            if (nativeIn2) {
                printf("[Audio] IN2 connected to native virtual sink (crosspad_vin2)\n");
                pc_platform_set_audio_input(1, nativeIn2);
                in2Virtual = true;
            } else if (sinks.size() >= 2 &&
                       s_pulseCap2.start(sinks[1].captureDeviceName, outSampleRate)) {
                printf("[Audio] IN2 connected to virtual sink: %s\n",
                       sinks[1].displayName.c_str());
                pc_platform_set_audio_input(1, &s_pulseCap2);
                in2Virtual = true;
            }
        }
        // Block RtAudio fallback for slots that are already serviced by the
        // Pulse capturer — no need to race two capture paths for the same
        // logical input.
        const bool in1Skip = in1Virtual;
        const bool in2Skip = in2Virtual;
#else
        const bool in1Skip = false;
        const bool in2Skip = false;
#endif

        auto inDevices = enumerateAudioInputDevices();
        // Restore a saved IN pick. New prefs store the UI label (a PipeWire
        // description); prefs written before that stored the raw RtAudio name.
        // Try the raw match first for back-compat, then resolve the label
        // through the same builder the dropdown uses.
        auto restoreInSlot = [&](PcAudioInput& in, int slot, const std::string& saved) {
            if (saved.empty() || in.isOpen()) return;
            unsigned int devId = findDeviceByName(inDevices, saved);
            if (devId != 0) {
                in.begin(devId, outSampleRate);
                if (in.isOpen()) {
                    pc_platform_set_audio_input(slot, &in);
                    printf("[Audio] IN%d auto-connected: %s @ %u Hz\n", slot + 1,
                           in.getCurrentDeviceName().c_str(), in.getSampleRate());
                }
                return;
            }
            s_inUiCache = buildInDeviceUiEntries();
            for (size_t i = 0; i < s_inUiCache.size(); ++i) {
                if (s_inUiCache[i].label != saved) continue;
                std::string lbl;
                if (appAudioInSelectIdx(slot, (int)i + 1, lbl))
                    printf("[Audio] IN%d auto-connected: %s\n", slot + 1, lbl.c_str());
                return;
            }
        };
        if (!in1Skip) restoreInSlot(pcAudioIn1, 0, s_devicePrefs.audioIn1);
        if (!in2Skip) restoreInSlot(pcAudioIn2, 1, s_devicePrefs.audioIn2);
    }

#ifdef USE_PIPEWIRE
    // Make CrossPad IN#1 the system default sink so audio from other apps
    // flows into the mixer without manual routing. Only when the native PW
    // backend is active (input(0) != nullptr) and the pref allows it.
    const bool pwTakeoverWillRun = s_devicePrefs.pwTakeoverDefault &&
                                   s_virtualSinkManager &&
                                   s_virtualSinkManager->input(0) != nullptr;
    if (pwTakeoverWillRun) {
        // Crash recovery: a leftover pwPrevDefaultSink means the previous run
        // died before restore — that value, not the current default (which is
        // still our own sink), is what we must eventually restore.
        if (!s_devicePrefs.pwPrevDefaultSink.empty())
            s_pwGuard.setPreviousSink(s_devicePrefs.pwPrevDefaultSink);
        if (s_pwGuard.takeover("crosspad_vin1")) {
            s_devicePrefs.pwPrevDefaultSink = s_pwGuard.previousSink();
            saveDevicePrefs();
            printf("[Audio] system default sink -> CrossPad IN#1 (was: %s)\n",
                   s_devicePrefs.pwPrevDefaultSink.c_str());
        }
    } else if (!s_devicePrefs.pwPrevDefaultSink.empty()) {
        // Takeover is NOT running this launch (pref off, or native PW backend
        // unavailable) but a previous run left a crash-recovery marker — the
        // configured default sink is still pointed at our virtual sink. Do a
        // one-shot restore of that stale marker so we don't strand the user's
        // default on crosspad_vin1. Only clear the marker on success; if the
        // daemon is unreachable, keep it and retry next launch.
        if (s_pwGuard.restoreStale(s_devicePrefs.pwPrevDefaultSink)) {
            printf("[Audio] restored stale default sink -> %s (no takeover)\n",
                   s_devicePrefs.pwPrevDefaultSink.c_str());
            s_devicePrefs.pwPrevDefaultSink.clear();
            saveDevicePrefs();
        }
    }
#endif

    // Initialize FM synth engine at the audio device's actual sample rate
    fmSynth.setSampleRate(pcAudio.getSampleRate());
    fmSynth.init();
    crosspad::getPlatformServices().setSynthEngine(&fmSynth);

#ifdef HAS_MIXER
    // Mixer engine setup: 2 outputs (OUT1, OUT2) at the active SR/frameCount.
    // mixerSr also feeds the pitched engine below — both have to render at
    // the same rate the mixer was set up with.
    uint32_t mixerSr = 44100;
    {
        auto* settings = crosspad::CrosspadSettings::getInstance();
        mixerSr = pcAudio.isOpen() ? pcAudio.getSampleRate()
                 : (settings ? settings->audioEngine.sampleRate : 44100);
        const uint32_t mixerFrames = settings ? settings->audioEngine.frameCount : 256;
        s_mixerEngine.setup(mixerFrames < 128 ? 128 : mixerFrames, mixerSr, 2);
    }
    s_mixerEngine.setDefaults();

    // Register predefined PC channels in IN1=0, IN2=1, SYNTH=2 order.
    s_mixerSynthNode.setEngine(&fmSynth);
    s_mixerEngine.addChannel(&s_in1Node, "Input 1");
    s_mixerEngine.addChannel(&s_in2Node, "Input 2");
    s_mixerEngine.addChannel(&s_mixerSynthNode, "Synth");
    // Default routing: SYNTH → OUT1 (matches legacy behavior).
    s_mixerEngine.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);

    // The sampler joins as a fourth channel, after the three whose indices
    // MixerInput::IN1/IN2/SYNTH pin down. Registered before loadState() so a
    // saved routing for it is applied rather than dropped — loadState only
    // patches slots that already exist.
    s_samplerNode = crosspad_pc::sampler_port_init();
    if (s_samplerNode) {
        s_mixerEngine.addChannel(s_samplerNode, "Sampler");
        s_mixerEngine.setRouteEnabled(static_cast<MixerInput>(3), MixerOutput::OUT1, true);
    }

    // The pitched engine joins as a fifth channel, on the same terms as the
    // sampler: registered before loadState() so a saved routing for it is
    // applied rather than dropped — loadState only patches slots that
    // already exist.
    s_pitchedNode = crosspad_pc::pitched_port_init(mixerSr);
    if (s_pitchedNode) {
        s_mixerEngine.addChannel(s_pitchedNode, "Pitched");
        s_mixerEngine.setRouteEnabled(static_cast<MixerInput>(4), MixerOutput::OUT1, true);
    }

    // Load mixer state AFTER channel slots exist so saved per-channel routing
    // applies; loadState only patches existing slots, never creates them.
    s_mixerEngine.loadState(getMixerStatePath());

    // Engine-level state-changed sink — MixerApp.cpp (cross-platform) calls
    // notifyStateChanged() after every GUI mutation; this hook flushes to disk.
    s_mixerEngine.setStateChangedCallback([]() {
        s_mixerEngine.saveState(getMixerStatePath());
    });

    // Register mixer pad logic globally (always available)
    s_mixerPadLogic = std::make_shared<MixerPadLogic>(s_mixerEngine);
    s_mixerPadLogic->setOnStateChanged([]() {
        s_mixerEngine.saveState(getMixerStatePath());
    });
    crosspad::getPadManager().registerPadLogic("Mixer", s_mixerPadLogic);
    crosspad::getPadManager().setActivePadLogic("Mixer");
#endif

    // ── Audio module pipeline (float bus + node chain or mixer override) ──
    // When HAS_MIXER, the AudioMixerEngine is plugged in as a sync renderer
    // (no separate mixer thread) — single-writer to OUT1/OUT2, single FmSynth
    // call site, eliminates the distortion seen with parallel mixer + audio
    // threads.
    {
        auto* settings = crosspad::CrosspadSettings::getInstance();
        crosspad::AudioModuleConfig cfg;
        cfg.sampleRate   = pcAudio.isOpen() ? pcAudio.getSampleRate()
                          : (settings ? settings->audioEngine.sampleRate : 44100);
        // PC: floor at 128 so the audio thread always generates enough per call
        // to keep RtAudio's async-callback ringbuffer fed. Below that we'd see
        // underrun-driven distortion. Embedded I2S keeps lower values OK because
        // its blocking write provides natural backpressure.
        {
            uint32_t userFrames = settings ? settings->audioEngine.frameCount : 256;
            cfg.frameCount = userFrames < 128 ? 128 : userFrames;
            if (cfg.frameCount != userFrames) {
                printf("[PcAudioModule] frameCount %u clamped to %u (RtAudio min)\n",
                       userFrames, cfg.frameCount);
            }
        }
        cfg.channelCount = 2;
        cfg.streamCount  = 2;
        s_audioModule.setOutputDevice(0, &pcAudio);
        s_audioModule.setOutputDevice(1, &pcAudio2);
        s_audioModule.setup(cfg);
#ifdef HAS_MIXER
        s_audioModule.setMixerEngine(&s_mixerEngine);
#else
        s_synthNode.setEngine(&fmSynth);
        s_audioModule.addNode(&s_synthNode);
        // Without the mixer component the sampler is a plain generator on the
        // node chain; it is brought up here because there is no addChannel().
        s_samplerNode = crosspad_pc::sampler_port_init();
        if (s_samplerNode) s_audioModule.addNode(s_samplerNode);
        s_pitchedNode = crosspad_pc::pitched_port_init(cfg.sampleRate);
        if (s_pitchedNode) s_audioModule.addNode(s_pitchedNode);
#endif
        crosspad::getPlatformServices().setAudioModule(&s_audioModule);
        s_audioModule.start();

#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
        // Now that the module is writing, the output streams exist in the
        // sound server and can be pinned. This matters most when the
        // virtual-sink takeover is on: the default sink is then CrossPad's own
        // input, so an unpinned output would feed straight back into it.
        for (uint8_t s = 0; s < 2; ++s) {
            if (s_pendingPin[s].empty()) continue;
            if (crosspad_pc::movePulseOutputToSinkWithin(s, s_pendingPin[s], 4000)) {
                printf("[Audio] OUT%u pinned to sink '%s'\n",
                       unsigned(s + 1), s_pendingPin[s].c_str());
            } else {
                printf("[Audio] OUT%u could not be pinned to '%s' — it will follow "
                       "the default sink\n", unsigned(s + 1), s_pendingPin[s].c_str());
            }
        }
#endif

#ifdef USE_PIPEWIRE
        // Expose OUT1 as a virtual source ("CrossPad Out") that OBS/DAWs can
        // capture directly. Attach as the module's aux stream (OUT1 tap) only
        // after the source connects; setAuxStream stores a non-owning pointer
        // read by the audio thread.
        if (s_pwSource.start("crosspad_out", "CrossPad Out", cfg.sampleRate)) {
            s_audioModule.setAuxStream(&s_pwSource);
            printf("[Audio] OUT1 tap -> virtual source 'CrossPad Out' @ %u Hz\n",
                   cfg.sampleRate);
        } else {
            printf("[Audio] virtual source 'CrossPad Out' unavailable\n");
        }
#endif
    }

    // Save preferences now that we know actual connected device names
    saveDevicePrefs();
#endif

    /* Register all apps (auto-generated by cmake/generate_registry.cmake) */
    {
        extern void AppRegistry_InitAll();
        AppRegistry_InitAll();
    }
    crosspad_gui::launcher_register_power_off();

    /* Styles, orchestrator, main screen */
    initStyles();
    InitializeOrchestrator();
    crosspad_gui::AppOrchestrator::getInstance().populateApps(app_c);
    LoadMainScreen(lcdContainer);

    /* ── Auto-check for updates (background) ─────────────────────────── */
    if (pc_platform_get_auto_check_updates()) {
        std::thread([]() {
            PcUpdater updater;
            auto info = updater.checkForUpdate();
            pc_updater_set_cached_check_result(info);
            if (info.updateAvailable)
                printf("[Updater] Update available: v%s\n", info.latestVersion.c_str());
        }).detach();
    }

    /* ── Jack panel wiring ────────────────────────────────────────────── */
    {
        auto& jp = stm32Emu.getJackPanel();

#ifdef USE_AUDIO
        // Audio OUT1/OUT2 — show device name and connection status
        jp.setDeviceName(EmuJackPanel::AUDIO_OUT1, pcAudio.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_OUT1, pcAudio.isOpen());

        jp.setDeviceName(EmuJackPanel::AUDIO_OUT2, pcAudio2.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_OUT2, pcAudio2.isOpen());

        // Audio IN1/IN2 — current device. On Linux with active virtual sinks
        // the input slot is served by PulseMonitorCapture, not PcAudioInput,
        // so we report the virtual sink's display name instead of the empty
        // RtAudio device name.
        std::string in1Name = pcAudioIn1.getCurrentDeviceName();
        std::string in2Name = pcAudioIn2.getCurrentDeviceName();
        bool in1Connected = pcAudioIn1.isOpen();
        bool in2Connected = pcAudioIn2.isOpen();
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
        if (s_pulseCap1.isOpen() && s_virtualSinkManager) {
            auto sinks = s_virtualSinkManager->list();
            if (sinks.size() >= 1) { in1Name = sinks[0].displayName; in1Connected = true; }
        }
        if (s_pulseCap2.isOpen() && s_virtualSinkManager) {
            auto sinks = s_virtualSinkManager->list();
            if (sinks.size() >= 2) { in2Name = sinks[1].displayName; in2Connected = true; }
        }
#endif
        jp.setDeviceName(EmuJackPanel::AUDIO_IN1, in1Name);
        jp.setConnected(EmuJackPanel::AUDIO_IN1, in1Connected);
        jp.setDeviceName(EmuJackPanel::AUDIO_IN2, in2Name);
        jp.setConnected(EmuJackPanel::AUDIO_IN2, in2Connected);

        // Populate device lists for all audio jacks. refreshOut/InUiCache
        // fill the caches the selection callback resolves against, so the
        // list shown and the list clicked are byte-for-byte the same.
        {
            auto outNames = refreshOutUiCache();
            jp.setDeviceList(EmuJackPanel::AUDIO_OUT1, outNames,
                             findDropdownIndex(outNames, pcAudio.getCurrentDeviceName()));
            jp.setDeviceList(EmuJackPanel::AUDIO_OUT2, outNames,
                             findDropdownIndex(outNames, pcAudio2.getCurrentDeviceName()));

            auto inNames = refreshInUiCache();
            // IN prefs store the shown label (a PipeWire description), so the
            // selected index is matched against the saved label, not the raw
            // RtAudio device name getCurrentDeviceName() would return.
            jp.setDeviceList(EmuJackPanel::AUDIO_IN1, inNames,
                             findDropdownIndex(inNames, s_devicePrefs.audioIn1));
            jp.setDeviceList(EmuJackPanel::AUDIO_IN2, inNames,
                             findDropdownIndex(inNames, s_devicePrefs.audioIn2));
        }
#endif

#ifdef USE_MIDI
        // Populate MIDI port lists and set selected device
        {
            std::vector<std::string> midiOutPorts;
            midiOutPorts.push_back("(None)");
            int outSelected = 0;
            for (unsigned int i = 0; i < midi.getOutputPortCount(); i++) {
                std::string name = midi.getOutputPortName(i);
                midiOutPorts.push_back(name);
                if (midi.isOutputOpen() && name == s_devicePrefs.midiOut)
                    outSelected = (int)i + 1; // +1 for "(None)" at index 0
            }
            // If no saved pref match but output is open, find by auto-connect
            if (outSelected == 0 && midi.isOutputOpen()) {
                for (unsigned int i = 0; i < midi.getOutputPortCount(); i++) {
                    std::string name = midi.getOutputPortName(i);
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find("crosspad") != std::string::npos) {
                        outSelected = (int)i + 1;
                        break;
                    }
                }
            }
            jp.setDeviceList(EmuJackPanel::MIDI_OUT, midiOutPorts, outSelected);
            if (outSelected > 0) {
                jp.setDeviceName(EmuJackPanel::MIDI_OUT, midiOutPorts[outSelected]);
                jp.setConnected(EmuJackPanel::MIDI_OUT, true);
            }

            std::vector<std::string> midiInPorts;
            midiInPorts.push_back("(None)");
            int inSelected = 0;
            for (unsigned int i = 0; i < midi.getInputPortCount(); i++) {
                std::string name = midi.getInputPortName(i);
                midiInPorts.push_back(name);
                if (midi.isInputOpen() && name == s_devicePrefs.midiIn)
                    inSelected = (int)i + 1;
            }
            if (inSelected == 0 && midi.isInputOpen()) {
                for (unsigned int i = 0; i < midi.getInputPortCount(); i++) {
                    std::string name = midi.getInputPortName(i);
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find("crosspad") != std::string::npos) {
                        inSelected = (int)i + 1;
                        break;
                    }
                }
            }
            jp.setDeviceList(EmuJackPanel::MIDI_IN, midiInPorts, inSelected);
            if (inSelected > 0) {
                jp.setDeviceName(EmuJackPanel::MIDI_IN, midiInPorts[inSelected]);
                jp.setConnected(EmuJackPanel::MIDI_IN, true);
            }
        }
#endif

        // USB/UART — enumerate COM ports and auto-connect
        {
            auto comPorts = PcUart::enumeratePorts();
            std::vector<std::string> usbPorts;
            usbPorts.push_back("(None)");
            int currentIdx = 0;
            for (size_t i = 0; i < comPorts.size(); i++) {
                usbPorts.push_back(comPorts[i]);
                if (comPorts[i] == s_devicePrefs.uartPort)
                    currentIdx = (int)(i + 1);
            }
            jp.setDeviceList(EmuJackPanel::USB, usbPorts, currentIdx);

            // Auto-connect: saved port first, then VID/PID detection
            bool connected = false;
            auto baud = static_cast<PcUart::BaudRate>(s_devicePrefs.uartBaud);

            if (!s_devicePrefs.uartPort.empty()) {
                connected = pcUart.open(s_devicePrefs.uartPort, baud);
            }

            if (!connected && pc_platform_get_usb_autoconnect()) {
                auto crosspadPorts = PcUart::findPortsByVidPid(
                    PcUart::CROSSPAD_VID, PcUart::CROSSPAD_PID);
                for (auto& port : crosspadPorts) {
                    if (pcUart.open(port, baud)) {
                        s_devicePrefs.uartPort = port;
                        saveDevicePrefs();
                        printf("[USB] Auto-connected CrossPad on %s\n", port.c_str());
                        connected = true;
                        break;
                    }
                }
            }

            if (connected) {
                jp.setConnected(EmuJackPanel::USB, true);
                jp.setDeviceName(EmuJackPanel::USB, pcUart.getPortName());
                crosspad::addPlatformCapability(crosspad::Capability::Usb);
            }
        }

        // Device selection callback — handles all jack types
        jp.setOnDeviceSelected([](int jackId, unsigned int deviceIndex) {
            auto& jp = stm32Emu.getJackPanel();

            switch (jackId) {
#ifdef USE_AUDIO
            case EmuJackPanel::AUDIO_OUT1:
            case EmuJackPanel::AUDIO_OUT2:
            case EmuJackPanel::AUDIO_IN1:
            case EmuJackPanel::AUDIO_IN2: {
                // Opening an ALSA/PulseAudio device blocks for 100–500 ms; doing
                // it inline here froze the whole UI (this callback runs on the
                // LVGL thread). Run the switch on a worker thread and push the
                // result back with lv_async_call so only widget mutation — which
                // must stay on the LVGL thread — happens there. Selection
                // resolves against the cached list the dropdown was built from,
                // never a fresh enumeration whose indices may have shifted.
                const bool isOut = (jackId == EmuJackPanel::AUDIO_OUT1 ||
                                    jackId == EmuJackPanel::AUDIO_OUT2);
                const int  slot  = (jackId == EmuJackPanel::AUDIO_OUT1 ||
                                    jackId == EmuJackPanel::AUDIO_IN1) ? 0 : 1;
                const int  uiIndex = static_cast<int>(deviceIndex);
                std::thread([jackId, slot, isOut, uiIndex]() {
                    std::string label;
                    bool connected = isOut ? appAudioOutSelectIdx(slot, uiIndex, label)
                                           : appAudioInSelectIdx(slot, uiIndex, label);
                    auto* res = new AudioJackUiResult{jackId, connected, std::move(label)};
                    lv_async_call([](void* ud) {
                        auto* r = static_cast<AudioJackUiResult*>(ud);
                        auto& jp = stm32Emu.getJackPanel();
                        jp.setConnected(static_cast<EmuJackPanel::JackId>(r->jackId),
                                        r->connected);
                        jp.setDeviceName(static_cast<EmuJackPanel::JackId>(r->jackId),
                                         r->label);
                        delete r;
                    }, res);
                }).detach();
                break;
            }
#endif

#ifdef USE_MIDI
            case EmuJackPanel::MIDI_OUT: {
                if (deviceIndex == 0) {
                    printf("[JackPanel] MIDI OUT disconnected\n");
                    jp.setConnected(EmuJackPanel::MIDI_OUT, false);
                    jp.setDeviceName(EmuJackPanel::MIDI_OUT, "");
                    s_devicePrefs.midiOut.clear();
                } else {
                    unsigned int newOut = deviceIndex - 1;
                    // Preserve current input port
                    int curIn = findMidiPortByName(s_devicePrefs.midiIn, false);
                    midi.end();
                    midi.begin(newOut, curIn >= 0 ? (unsigned)curIn : 0);
                    jp.setConnected(EmuJackPanel::MIDI_OUT, midi.isOutputOpen());
                    if (midi.isOutputOpen()) {
                        std::string name = midi.getOutputPortName(newOut);
                        jp.setDeviceName(EmuJackPanel::MIDI_OUT, name);
                        s_devicePrefs.midiOut = name;
                    }
                }
                saveDevicePrefs();
                break;
            }
            case EmuJackPanel::MIDI_IN: {
                if (deviceIndex == 0) {
                    printf("[JackPanel] MIDI IN disconnected\n");
                    jp.setConnected(EmuJackPanel::MIDI_IN, false);
                    jp.setDeviceName(EmuJackPanel::MIDI_IN, "");
                    s_devicePrefs.midiIn.clear();
                } else {
                    unsigned int newIn = deviceIndex - 1;
                    // Preserve current output port
                    int curOut = findMidiPortByName(s_devicePrefs.midiOut, true);
                    midi.end();
                    midi.begin(curOut >= 0 ? (unsigned)curOut : 0, newIn);
                    jp.setConnected(EmuJackPanel::MIDI_IN, midi.isInputOpen());
                    if (midi.isInputOpen()) {
                        std::string name = midi.getInputPortName(newIn);
                        jp.setDeviceName(EmuJackPanel::MIDI_IN, name);
                        s_devicePrefs.midiIn = name;
                    }
                }
                saveDevicePrefs();
                break;
            }
#endif

            case EmuJackPanel::USB: {
                if (deviceIndex == 0) {
                    printf("[JackPanel] USB/UART disconnected\n");
                    pcUart.close();
                    jp.setConnected(EmuJackPanel::USB, false);
                    jp.setDeviceName(EmuJackPanel::USB, "");
                    s_devicePrefs.uartPort.clear();
                    crosspad::removePlatformCapability(crosspad::Capability::Usb);
                } else {
                    auto comPorts = PcUart::enumeratePorts();
                    unsigned int portIdx = deviceIndex - 1;
                    if (portIdx < comPorts.size()) {
                        pcUart.close();
                        auto baud = static_cast<PcUart::BaudRate>(s_devicePrefs.uartBaud);
                        if (pcUart.open(comPorts[portIdx], baud)) {
                            jp.setConnected(EmuJackPanel::USB, true);
                            jp.setDeviceName(EmuJackPanel::USB, comPorts[portIdx]);
                            s_devicePrefs.uartPort = comPorts[portIdx];
                            crosspad::addPlatformCapability(crosspad::Capability::Usb);
                        } else {
                            jp.setConnected(EmuJackPanel::USB, false);
                            jp.setDeviceName(EmuJackPanel::USB, "");
                        }
                    }
                }
                saveDevicePrefs();
                break;
            }

            default:
                break;
            }
        });
    }

#ifdef USE_AUDIO
    // Hand the audio-device list/select entry points to the remote-control
    // server so the simulator can be driven through the same audio-selector
    // path the Jack panel uses — the sim's equivalent of an AUDIO_* CDC verb.
    remote::set_audio_device_ctl({ refreshOutUiCache, refreshInUiCache,
                                   appAudioOutSelectIdx, appAudioInSelectIdx });
#endif

#ifdef USE_AUDIO
    // ── VU meter timer: feed levels to jack panel bars + main VU meter ──
    static crosspad::PeakMeter s_vuMeter(238);    // main VU (slower decay)
    static crosspad::PeakMeter s_jpOut1(230);      // jack panel OUT1
    static crosspad::PeakMeter s_jpOut2(230);      // jack panel OUT2
    static crosspad::PeakMeter s_jpIn1(230);       // jack panel IN1
    static crosspad::PeakMeter s_jpIn2(230);       // jack panel IN2

    lv_timer_create([](lv_timer_t*) {
        auto& jp = stm32Emu.getJackPanel();

        // OUT1 levels → main VU meter + jack panel
        {
            int16_t rawL, rawR;
            pcAudio.getOutputLevel(rawL, rawR);
            s_vuMeter.update(rawL, rawR);
            crosspad_gui::vu_set_levels(s_vuMeter.left(), s_vuMeter.right());
            s_jpOut1.update(rawL, rawR);
            jp.setLevel(EmuJackPanel::AUDIO_OUT1, s_jpOut1.left(), s_jpOut1.right());
        }

        // OUT2 levels → jack panel
        {
            int16_t rawL, rawR;
            pcAudio2.getOutputLevel(rawL, rawR);
            s_jpOut2.update(rawL, rawR);
            jp.setLevel(EmuJackPanel::AUDIO_OUT2, s_jpOut2.left(), s_jpOut2.right());
        }

        // IN1/IN2 levels → jack panel. Read polymorphically through the
        // platform accessor: on Linux with USE_VIRTUAL_AUDIO the live input
        // is a PulseMonitorCapture (not pcAudioIn*), and the panel VU bars
        // would otherwise stay flat even though the mixer sees the signal.
        if (auto* in0 = pc_platform_get_audio_input(0)) {
            int16_t rawL, rawR;
            in0->getInputLevel(rawL, rawR);
            s_jpIn1.update(rawL, rawR);
            jp.setLevel(EmuJackPanel::AUDIO_IN1, s_jpIn1.left(), s_jpIn1.right());
        }
        if (auto* in1 = pc_platform_get_audio_input(1)) {
            int16_t rawL, rawR;
            in1->getInputLevel(rawL, rawR);
            s_jpIn2.update(rawL, rawR);
            jp.setLevel(EmuJackPanel::AUDIO_IN2, s_jpIn2.left(), s_jpIn2.right());
        }

        // Audio-health watchdog: every ~5 s check the drop counters and log
        // ONLY when something got worse since the last check. Silent when
        // healthy; a growing count = audible crackle source identified
        // (ring starvation, ALSA xrun or virtual-sink ring overflow).
        static uint32_t s_diagTick = 0;
        if (++s_diagTick % 300 == 0) {
            static uint32_t lastShort1 = 0, lastShort2 = 0;
            static uint32_t lastXrun1 = 0, lastXrun2 = 0;
            static uint32_t lastVinOver = 0;
            uint32_t short1 = pcAudio.diagRingShortfalls_.load();
            uint32_t short2 = pcAudio2.diagRingShortfalls_.load();
            uint32_t xrun1  = pcAudio.diagAlsaUnderflows_.load();
            uint32_t xrun2  = pcAudio2.diagAlsaUnderflows_.load();
            uint32_t vinOver = 0;
#if defined(USE_PIPEWIRE)
            if (s_virtualSinkManager) {
                for (int i = 0; i < 2; ++i) {
                    auto* cap = dynamic_cast<crosspad_pc::PwVirtualSinkCapture*>(
                        s_virtualSinkManager->input(i));
                    if (cap) vinOver += cap->diagRingOverflows_.load();
                }
            }
#endif
            if (short1 != lastShort1 || short2 != lastShort2 ||
                xrun1 != lastXrun1 || xrun2 != lastXrun2 || vinOver != lastVinOver) {
                uint32_t cyc = s_audioModule.diagCycles_.load();
                printf("[AudioHealth] OUT1 ringShort=%u(+%u) xrun=%u | OUT2 ringShort=%u(+%u) xrun=%u"
                       " | vinRingOver=%u | loop avg=%lluus max=%uus\n",
                       short1, short1 - lastShort1, xrun1,
                       short2, short2 - lastShort2, xrun2, vinOver,
                       cyc ? (unsigned long long)(s_audioModule.diagPeriodUsSum_.load() / cyc) : 0ULL,
                       s_audioModule.diagPeriodUsMax_.load());
                lastShort1 = short1; lastShort2 = short2;
                lastXrun1 = xrun1;   lastXrun2 = xrun2;
                lastVinOver = vinOver;
            }
        }
    }, 16, nullptr);
#endif

    // ── USB/UART periodic reconnect (5s) — auto-detect CrossPad by VID/PID ──
    lv_timer_create([](lv_timer_t*) {
        if (pcUart.isOpen()) return;          // already connected
        if (!pc_platform_get_usb_autoconnect()) return;

        auto crosspadPorts = PcUart::findPortsByVidPid(
            PcUart::CROSSPAD_VID, PcUart::CROSSPAD_PID);
        if (crosspadPorts.empty()) return;

        auto baud = static_cast<PcUart::BaudRate>(s_devicePrefs.uartBaud);
        for (auto& port : crosspadPorts) {
            if (pcUart.open(port, baud)) {
                auto& jp = stm32Emu.getJackPanel();
                jp.setConnected(EmuJackPanel::USB, true);
                jp.setDeviceName(EmuJackPanel::USB, port);
                s_devicePrefs.uartPort = port;
                saveDevicePrefs();
                crosspad::addPlatformCapability(crosspad::Capability::Usb);
                printf("[USB] Auto-reconnected CrossPad on %s\n", port.c_str());
                break;
            }
        }
    }, 5000, nullptr);

    printf("[CrossPad] App init complete\n");
}

void crosspad_app_go_home()
{
    LoadMainScreen(s_lcdContainer);
}

void crosspad_app_update_pad_icon()
{
    std::string active = crosspad::getPadManager().getActivePadLogic();

    if (active == "Mixer") {
        crosspad_gui::statusbar_add_icon("pad_logic", LV_SYMBOL_SHUFFLE,
                                          lv_color_hex(0x0099AA),
                                          crosspad_gui::StatusIconSide::Right);
    } else if (active == "MLPiano") {
        crosspad_gui::statusbar_add_icon("pad_logic", LV_SYMBOL_KEYBOARD,
                                          lv_color_hex(0x9966FF),
                                          crosspad_gui::StatusIconSide::Right);
    } else if (active.empty()) {
        crosspad_gui::statusbar_remove_icon("pad_logic");
    } else {
        // Unknown pad logic — show generic icon with name
        crosspad_gui::statusbar_add_icon("pad_logic", LV_SYMBOL_LIST,
                                          lv_color_hex(0xAAAAAA),
                                          crosspad_gui::StatusIconSide::Right);
    }
}

/* ── Audio device accessors & global mixer ────────────────────────────── */

#ifdef USE_AUDIO
PcAudioOutput* pc_platform_get_audio_output(int index)
{
    if (index == 0) return &pcAudio;
    if (index == 1) return &pcAudio2;
    return nullptr;
}

#ifdef HAS_MIXER
AudioMixerEngine& getMixerEngine()
{
    return s_mixerEngine;
}
#endif

void pc_platform_save_mixer_state()
{
#ifdef HAS_MIXER
    s_mixerEngine.saveState(getMixerStatePath());
#endif
}
#else
PcAudioOutput* pc_platform_get_audio_output(int /*index*/) { return nullptr; }
void pc_platform_save_mixer_state() {}
#endif

/* ── UART accessor ────────────────────────────────────────────────────── */

PcUart& pc_platform_get_uart() { return pcUart; }

/* ── Graceful shutdown ────────────────────────────────────────────────── */

void crosspad_app_shutdown()
{
#if defined(USE_AUDIO) && defined(USE_VIRTUAL_AUDIO)
    bool expected = false;
    if (!s_shutdownRan.compare_exchange_strong(expected, true)) return;

#ifdef USE_PIPEWIRE
    // Restore the user's original default sink and clear the crash-recovery
    // marker BEFORE tearing down our nodes — so a clean exit leaves no stale
    // pwPrevDefaultSink behind. Detach the aux stream (non-owning pointer read
    // by the audio thread) BEFORE stopping/destroying the source.
    s_pwGuard.restore();
    s_devicePrefs.pwPrevDefaultSink.clear();
    saveDevicePrefs();
    s_audioModule.setAuxStream(nullptr);
    s_pwSource.stop();
#endif

    // Order matters: unload the PulseAudio null-sink modules FIRST so they
    // don't leak into the user's system. In theory this should also sever
    // in-flight pa_simple_read() calls in our capture threads, but on PipeWire's
    // pulse shim the read can stay blocked indefinitely even after the module
    // goes away — so below we detach instead of join.
    if (s_virtualSinkManager) {
        s_virtualSinkManager->teardown();
    }

#ifdef __linux__
    // Non-blocking: the process is about to _Exit(0), so abandoning the
    // capture threads is preferable to hanging in a join().
    s_pulseCap1.detachForShutdown();
    s_pulseCap2.detachForShutdown();
#endif
    pcAudioIn1.end();
    pcAudioIn2.end();
#ifdef USE_PIPEWIRE
    // Tear down the shared pw_thread_loop LAST — after every proxy/stream that
    // lives on it (guard, source, native sink captures) is already stopped.
    crosspad_pc::PwContext::instance().shutdown();
#endif
#endif
}
