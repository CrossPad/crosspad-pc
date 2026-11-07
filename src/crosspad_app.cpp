/**
 * @file crosspad_app.cpp
 * @brief Shared CrossPad application initialization.
 *
 * Extracted from main.cpp so that both the standard main loop and the
 * FreeRTOS main can share the same GUI setup with zero duplication.
 */

#include "crosspad_app.hpp"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdlib>
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
#include "pc_stubs/GuiThread.hpp"
#include "updater/PcUpdater.hpp"

// crosspad-core
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/control/AppName.hpp"
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
#include "crosspad-gui/fw/IFwViewService.h"
#ifdef __EMSCRIPTEN__
void fw_view_service_install(void);   // the board's (platform-idf main/fw_view_service.cpp)
#else
#include "fw/PcFirmware.hpp"
#include "fw/PcFwViewService.hpp"
#endif
#include "crosspad-gui/components/boot_splash.h"

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
#include "InstanceEnv.hpp"
#include "audio/audio_platform.hpp"
#include "audio/OutputRoute.hpp"
#include "audio/sampler/PcInstrumentHost.hpp"
#include "audio/sampler/PcPitchedPort.hpp"
#include "audio/sampler/PcSamplerPort.hpp"
#include <crosspad-sampler/logic/kit_gate.hpp>
#ifdef USE_VIRTUAL_AUDIO
#include "audio/virtual/IVirtualSinkManager.hpp"
#ifdef USE_PIPEWIRE
#include "audio/pipewire/PwContext.hpp"
#include "audio/pipewire/PwDefaultSinkGuard.hpp"
#include "audio/pipewire/PwVirtualSource.hpp"
#include "audio/pipewire/PwVirtualSinkCapture.hpp"
#endif
#endif
#include "crosspad-gui/components/vu_meter.h"
#include "crosspad/audio/PeakMeter.hpp"
#include "crosspad/audio/SynthEngineNode.hpp"
#include "synth/MlPianoSynth.hpp"
#if __has_include("crosspad-mixer/AudioMixerEngine.hpp")
#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "sequencer/PcSequencer.hpp"
#include "song/PcSongEnginePort.hpp"
#include "song/PcSongSounds.hpp"
#include "sampletools/PcSampleToolsPort.hpp"
#define HAS_MIXER 1
#endif
#include <RtAudio.h>
#endif

#include "uart/PcUart.hpp"
#include <ArduinoJson.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* ── Constants ────────────────────────────────────────────────────────── */

#define APP_BUTTON_SIZE 64
#define APP_BUTTON_NAME_VISIBLE true

/// Pad animation tick, the board's pad_anim worker period.
static constexpr uint32_t kPadAnimTickMs = 10;

/// The boot worker (see "Boot" below): the LVGL task's priority, so the two
/// share the CPU while the splash plays, and its stack depth -- the code it
/// runs ran on the LVGL task until now.
static constexpr uint32_t    kBootTaskStack    = 8192;
static constexpr UBaseType_t kBootTaskPriority = 1;

/* ── State ────────────────────────────────────────────────────────────── */

/* The app container, by the name the board's gui.h gives it (its views open apps in it). */
lv_obj_t* app_c = nullptr;
static lv_obj_t* s_lcdContainer = nullptr;
static Stm32EmuWindow stm32Emu;
static bool s_lcdOnly = false;

void crosspad_app_set_lcd_only(bool on) { s_lcdOnly = on; }
bool crosspad_app_lcd_only() { return s_lcdOnly; }
lv_obj_t* crosspad_app_container() { return app_c; }

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

/// The rate the audio module renders at, once it is set up: an output opened
/// on a sink later has to run at it. 0 before.
static std::atomic<uint32_t> s_outRate{0};
#ifdef HAS_MIXER
#include "crosspad/audio/AudioInputNode.hpp"
#include "pc_stubs/pc_platform.h"
static AudioMixerEngine s_mixerEngine;
// Adapters registered as mixer channels in IN1, IN2, SYNTH order so
// MixerInput::IN1=0, IN2=1, SYNTH=2 stay valid against the dynamic API.
// The indexed getter form picks up hot-swapped virtual inputs every cycle;
// the sample tools' getter hands every block read to them as well.
static crosspad::AudioInputNode s_in1Node{&crosspad_pc::sample_tools_input, 0, "Input 1"};
static crosspad::AudioInputNode s_in2Node{&crosspad_pc::sample_tools_input, 1, "Input 2"};
static crosspad::SynthEngineNode s_mixerSynthNode;
/// The capture bus after every render: a bounce's, and a PADS take's.
static void capture_tap(const float* bus, uint32_t frames) {
    crosspad_pc::song_engine_rt_capture(bus, frames);
    crosspad_pc::sample_tools_rt_bus(bus, frames);
}
#endif
#ifdef USE_VIRTUAL_AUDIO
static std::unique_ptr<crosspad_pc::IVirtualSinkManager> s_virtualSinkManager;
static std::atomic<bool> s_shutdownRan{false};
// The virtual-sink monitor capturers now live in audio_platform (Linux-only
// PulseMonitorCapture), reached through startVirtualCapture / virtualCaptureOpen
// so this file carries no Linux capture type.
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
    // Opt-in: every other program's sound then goes into the mixer's Input 1,
    // which a fresh mixer does not route to any output, so a new user would
    // hear their desktop go quiet the moment the simulator starts.
    bool        pwTakeoverDefault = false;
    // Sink name to restore on shutdown. Non-empty at startup means the previous
    // run died before restoring — used for crash recovery, not the live default.
    std::string pwPrevDefaultSink;
};

static DevicePreferences s_devicePrefs;
/* The preferences are written from the LVGL thread (jack panel, SD slot,
 * BLE), the audio-device workers and the control port's thread (device
 * picks), the boot worker and the shutdown. Every write and every save
 * holds this; a save that read a string another thread was assigning, or
 * two saves writing the file at once, was a crash or a torn JSON that
 * dropped every preference at the next start. */
static std::recursive_mutex s_prefsMutex;

/// Change the preferences in memory, from any thread; saveDevicePrefs()
/// writes them out.
template <typename F>
static void setDevicePrefs(F&& change) {
    std::lock_guard<std::recursive_mutex> lock(s_prefsMutex);
    change(s_devicePrefs);
}

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
    std::lock_guard<std::recursive_mutex> lock(s_prefsMutex);
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

    // Beside it, then over it: a process killed mid-write leaves the old file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) {
            printf("[DevPrefs] Failed to write %s\n", tmp.c_str());
            return;
        }
        serializeJsonPretty(doc, f);
        if (!f.flush()) {
            printf("[DevPrefs] Failed to write %s\n", tmp.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        printf("[DevPrefs] Failed to replace %s: %s\n", path.c_str(), ec.message().c_str());
        return;
    }
    printf("[DevPrefs] Saved to %s\n", path.c_str());
}

/* ── Audio/MIDI device enumeration helpers ───────────────────────────── */

#ifdef USE_AUDIO
/// Names of the output devices with 2+ output channels. A device is named,
/// never numbered: RtAudio numbers devices per instance, so an id from this
/// enumeration can mean another device, or none, to the instance that opens
/// it (PcAudioOutput::begin resolves the name itself).
static std::vector<std::string> enumerateAudioOutputDevices() {
    std::vector<std::string> result;
    RtAudio probe;
    for (unsigned int id : probe.getDeviceIds()) {
        RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
        if (info.outputChannels >= 2) result.push_back(info.name);
    }
    return result;
}

/// Names of the input devices with 2+ input channels.
static std::vector<std::string> enumerateAudioInputDevices() {
    std::vector<std::string> result;
    RtAudio probe;
    for (unsigned int id : probe.getDeviceIds()) {
        RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
        if (info.inputChannels >= 2) result.push_back(info.name);
    }
    return result;
}

/// Devices that play into the SYSTEM DEFAULT sink, wherever it is.
static bool isLoopbackRiskDevice(const std::string& name) {
    return name.find("Default ALSA Device") != std::string::npos ||
           name.find("PulseAudio Sound Server") != std::string::npos;
}

/// The boot opens the outputs before the takeover runs; while it is still to
/// come, the default counts as CrossPad's already.
static std::atomic<bool> s_takeoverPending{false};

/// The system default sink is one of CrossPad's own (this run's takeover, or
/// another simulator's), so an output following it would play into a
/// CrossPad input: silence for the user and a feedback path.
static bool defaultIsCrossPads() {
#ifdef USE_PIPEWIRE
    if (s_pwGuard.active() || s_takeoverPending.load()) return true;
#endif
    return crosspad_pc::out_route::isCrossPadSink(
        crosspad_pc::audio_platform::currentDefaultSink());
}

/* ── Output routes ────────────────────────────────────────────────────────
 * What an output is open on. `wanted` is the user's pick (what the
 * preferences hold); `open` is what plays, which differs while the pick is
 * unusable: a device that will not open, a sink that went away, or the
 * system default while that is a CrossPad sink. An output is never left on a
 * CrossPad sink: a route that would follow such a default is replaced by a
 * named real sink (out_route::safeSinks), and the route watch puts it back
 * on its pick once that works again. */
struct OutRoute {
    enum class Kind { None, Default, Device, Sink };
    Kind        kind = Kind::None;
    std::string id;     ///< Device: RtAudio device name. Sink: sink name.
    std::string label;  ///< what the jack panel shows
    /// Sink: it was the default sink when the stream was created. The sound
    /// server then makes the stream follow the default instead of the sink
    /// (PipeWire's pulse layer does), so it moves with the default after all.
    bool        viaDefault = false;
    bool same(const OutRoute& o) const { return kind == o.kind && id == o.id; }
    bool followsDefault() const {
        return kind == Kind::Default || (kind == Kind::Device && isLoopbackRiskDevice(id))
            || (kind == Kind::Sink && viaDefault);
    }
};
static OutRoute s_outWanted[2];
static OutRoute s_outOpen[2];
/// A picked sink that is listed but would not open (one that never plays, an
/// HDMI port with nothing on it): the route watch does not go back to it
/// until it has gone and come back, or every attempt would drop the output.
static std::string s_outRefusedSink[2];

/// The real sink @p sink is listed by the sound server right now.
static bool sinkListed(const std::string& sink) {
    for (auto& s : crosspad_pc::audio_platform::outputSinkCandidates())
        if (s.name == sink) return true;
    return false;
}

static PcAudioOutput& outputOf(int slot) { return slot == 0 ? pcAudio : pcAudio2; }

/// Open exactly @p r on the slot. Refuses a sink that is not among the real
/// ones the server lists (gone, unplugged, a CrossPad sink), and a route that
/// follows the default while the default is a CrossPad sink.
static bool openRouteExactly(int slot, OutRoute r) {
    auto& out = outputOf(slot);
    out.end();
    s_outOpen[slot] = {};
    bool ok = false;
    switch (r.kind) {
    case OutRoute::Kind::None:
        return false;
    case OutRoute::Kind::Default:
    case OutRoute::Kind::Device:
        if (r.followsDefault() && defaultIsCrossPads()) return false;
        ok = out.begin(r.kind == OutRoute::Kind::Device ? r.id : std::string{},
                       out.getSampleRate(), out.getBufferSize());
        if (ok) r.label = out.getCurrentDeviceName();
        break;
    case OutRoute::Kind::Sink: {
        if (crosspad_pc::out_route::isCrossPadSink(r.id)) return false;
        bool listed = false;
        for (auto& c : crosspad_pc::audio_platform::outputSinkCandidates()) {
            if (c.name != r.id) continue;
            listed = true;
            if (r.label.empty()) r.label = c.description;
        }
        if (!listed) return false;
        r.viaDefault = r.id == crosspad_pc::audio_platform::currentDefaultSink();
        // The module's rate; before it is set up OUT2 follows OUT1, which
        // the module takes its rate from.
        uint32_t rate = s_outRate.load();
        if (rate == 0) rate = (slot == 1 && pcAudio.isOpen()) ? pcAudio.getSampleRate() : 48000;
        ok = out.beginOnSink(r.id, rate, out.getBufferSize());
        break;
    }
    }
    if (ok) s_outOpen[slot] = r;
    return ok;
}

/// Open the slot's pick, or, when it cannot be used, the first real sink of
/// out_route::safeSinks that opens, then the system default if that is safe.
/// Says which in the log. False only when nothing opens.
static bool openOutput(int slot, const OutRoute& want) {
    if (want.kind == OutRoute::Kind::None) {
        outputOf(slot).end();
        s_outOpen[slot] = {};
        return false;
    }
    if (openRouteExactly(slot, want)) {
        s_outRefusedSink[slot].clear();
        return true;
    }
    if (want.kind == OutRoute::Kind::Sink && sinkListed(want.id)) s_outRefusedSink[slot] = want.id;
    const std::string what = want.kind == OutRoute::Kind::Default ? "the system default"
                           : "'" + want.id + "'";
    if (want.followsDefault() && defaultIsCrossPads())
        printf("[Audio] OUT%d: %s is CrossPad's own input, not following it\n", slot + 1,
               what.c_str());
    else
        printf("[Audio] OUT%d: %s would not open\n", slot + 1, what.c_str());

    std::string userDefault;
#ifdef USE_PIPEWIRE
    if (s_pwGuard.active()) userDefault = s_pwGuard.previousSink();
#endif
    if (userDefault.empty()) userDefault = crosspad_pc::audio_platform::currentDefaultSink();
    std::string beforeCrash;
    setDevicePrefs([&](DevicePreferences& p) { beforeCrash = p.pwPrevDefaultSink; });
    std::vector<std::string> real;
    for (auto& s : crosspad_pc::audio_platform::outputSinkCandidates()) real.push_back(s.name);
    for (const auto& c : crosspad_pc::out_route::safeSinks(userDefault, beforeCrash, real)) {
        if (want.kind == OutRoute::Kind::Sink && c.sink == want.id) continue;
        if (openRouteExactly(slot, {OutRoute::Kind::Sink, c.sink, {}})) {
            printf("[Audio] OUT%d -> sink '%s' (%s)\n", slot + 1, c.sink.c_str(),
                   crosspad_pc::out_route::describe(c.why));
            return true;
        }
    }
    if (want.kind != OutRoute::Kind::Default &&
        openRouteExactly(slot, {OutRoute::Kind::Default, {}, {}})) {
        printf("[Audio] OUT%d -> the system default (%s)\n", slot + 1,
               s_outOpen[slot].label.c_str());
        return true;
    }
    printf("[Audio] OUT%d: no output would open\n", slot + 1);
    return false;
}

/// One selectable entry of the OUT1/OUT2 dropdowns: an RtAudio device
/// (paSinkName empty), or a sound-server sink RtAudio does not list.
struct OutUiEntry {
    std::string label;
    std::string paSinkName;
};

/// Single source of truth for what the OUT dropdowns offer (minus "(None)").
/// Applies the feedback-loop guard and the PA-sink overlap dedupe so the
/// populate site and the selection callback can never disagree on labels.
static std::vector<OutUiEntry> buildOutDeviceUiEntries() {
    std::vector<OutUiEntry> entries;
    auto outDevices = enumerateAudioOutputDevices();
    const bool guard = defaultIsCrossPads();
    for (auto& d : outDevices) {
        if (guard && isLoopbackRiskDevice(d)) continue;
        entries.push_back({d, {}});
    }
    // Add sound-server sinks RtAudio's backend missed — typically the
    // motherboard analog jack and the active HDMI display. The platform layer
    // supplies the candidate list (empty off-Linux / when virtual audio is
    // off); the overlap dedupe below is portable string work. Token-overlap on
    // long, hardware-identifying tokens only; generic words would falsely flag
    // e.g. the Pro Audio sink as a duplicate.
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
                if (d.find(w) != std::string::npos) return true;
            }
        }
        return false;
    };
    for (auto& s : crosspad_pc::audio_platform::outputSinkCandidates()) {
        if (!overlapsRtAudio(s.description))
            entries.push_back({s.description, s.name});
    }
    // Stable, enumeration-order-independent ordering, so the list a client
    // shows and the list it picks from agree on positions.
    std::sort(entries.begin(), entries.end(),
              [](const OutUiEntry& a, const OutUiEntry& b) { return a.label < b.label; });
    return entries;
}

/* ── Cached UI device lists + thread-safe selection ─────────────────────
 * The dropdowns are populated from these caches, and a pick is resolved by
 * its label, never by its position: a refresh between showing a list and
 * clicking it (the control port lists too) may have moved every entry. The
 * caches refresh on every populate and on every remote *_list. */
struct InUiEntry {
    std::string  label;              ///< what the dropdown shows
    std::string  deviceName;         ///< RtAudio device name; empty for a virtual sink
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
// thread never blocks on an ALSA/PulseAudio open, the remote-control server
// runs it on its TCP thread and the route watch on its own — all go through
// this lock.
static std::mutex s_audioSelMutex;

/// IN dropdown entries: physical RtAudio inputs relabelled with PipeWire
/// source descriptions (parity with the OUT dropdown), the hw:/plughw:/
/// sysdefault: aliases of one card collapsed to a single entry, then
/// CrossPad's virtual sinks appended at the end.
static std::vector<InUiEntry> buildInDeviceUiEntries() {
    std::vector<InUiEntry> entries;
    auto inDevices = enumerateAudioInputDevices();
    // Relabel with PipeWire source descriptions where available; portable
    // builds get the raw RtAudio names back. Labels line up with inDevices.
    auto labels = crosspad_pc::audio_platform::prettyInputLabels(inDevices);
    std::set<std::string> seen;
    for (size_t i = 0; i < inDevices.size(); ++i) {
        const std::string& label = labels[i];
        if (!seen.insert(label).second) continue;   // dedupe aliases of one card
        entries.push_back({label, inDevices[i], {}, false});
    }
#if defined(USE_VIRTUAL_AUDIO)
    // CrossPad's own virtual sinks (system-wide mixer inputs). The manager is
    // a no-op off-Linux, so list() is empty there and nothing is appended.
    if (s_virtualSinkManager) {
        for (auto& v : s_virtualSinkManager->list())
            entries.push_back({v.displayName, {}, v.captureDeviceName, true});
    }
#endif
    // Same enumeration-order-independence as the OUT list.
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

/// The cached entry shown as @p label, the cache rebuilt once when it has
/// none (it may predate the entry). Call with s_audioSelMutex held.
template <typename Entry>
static const Entry* findUiEntry(std::vector<Entry>& cache, std::vector<Entry> (*build)(),
                                const std::string& label) {
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& e : cache)
            if (e.label == label) return &e;
        if (pass == 0) cache = build();
    }
    return nullptr;
}

/// Apply an OUT pick, named by its dropdown label ("" = "(None)"). A pick
/// that will not open is refused and the slot goes back to what it played
/// (or a safe fallback), so a bad pick never leaves the output silent; only
/// a pick that opened is saved. Blocking (RtAudio/PA open) — never call from
/// the LVGL thread; wrap it in a worker.
/// @return true if a device is connected on the slot afterwards; @p outLabel
///         is what it plays.
static bool appAudioOutSelect(int slot, const std::string& label, std::string& outLabel) {
    std::lock_guard<std::mutex> lk(s_audioSelMutex);
    OutRoute want;
    if (!label.empty()) {
        const OutUiEntry* sel = findUiEntry(s_outUiCache, &buildOutDeviceUiEntries, label);
        if (!sel) {
            printf("[Audio] OUT%d: '%s' is not on the list any more\n", slot + 1, label.c_str());
            outLabel = s_outOpen[slot].label;
            return outputOf(slot).isOpen();
        }
        want = sel->paSinkName.empty()
                   ? OutRoute{OutRoute::Kind::Device, sel->label, sel->label}
                   : OutRoute{OutRoute::Kind::Sink, sel->paSinkName, sel->label};
    }
    bool connected = openRouteExactly(slot, want);
    if (connected || want.kind == OutRoute::Kind::None) {
        s_outWanted[slot] = want;
        setDevicePrefs([&](DevicePreferences& p) {
            std::string& name = slot == 0 ? p.audioOut1 : p.audioOut2;
            std::string& pa   = slot == 0 ? p.audioOut1Pa : p.audioOut2Pa;
            name = want.kind == OutRoute::Kind::None ? std::string{} : want.label;
            pa   = want.kind == OutRoute::Kind::Sink ? want.id : std::string{};
        });
        saveDevicePrefs();
    } else {
        printf("[Audio] OUT%d: '%s' refused, back to the previous output\n", slot + 1,
               label.c_str());
        connected = openOutput(slot, s_outWanted[slot]);
    }
    outLabel = connected ? s_outOpen[slot].label : std::string{};
    return connected;
}

/// The same, by position in the current OUT list (the control port's form).
static bool appAudioOutSelectIdx(int slot, int uiIndex, std::string& outLabel) {
    std::string label;
    {
        std::lock_guard<std::mutex> lk(s_audioSelMutex);
        if (uiIndex > 0 && static_cast<size_t>(uiIndex) <= s_outUiCache.size())
            label = s_outUiCache[uiIndex - 1].label;
        else if (uiIndex > 0) {
            outLabel = s_outOpen[slot].label;
            return outputOf(slot).isOpen();
        }
    }
    return appAudioOutSelect(slot, label, outLabel);
}

/// Apply an IN pick, named by its dropdown label ("" = "(None)"). Blocking —
/// same worker-thread rule as appAudioOutSelect.
static bool appAudioInSelect(int slot, const std::string& label, std::string& outLabel) {
    std::lock_guard<std::mutex> lk(s_audioSelMutex);
    auto& input = (slot == 0) ? pcAudioIn1 : pcAudioIn2;
    // Tear down whichever capturer was bound to this slot — saves juggling
    // state when switching between physical mics and virtual sinks. Both the
    // RtAudio input and the (platform-owned) virtual capturer get stopped.
    input.end();
    crosspad_pc::audio_platform::stopVirtualCapture(slot);
    if (label.empty()) {
        outLabel.clear();
        pc_platform_set_audio_input(slot, nullptr);
        setDevicePrefs([&](DevicePreferences& p) {
            if (slot == 0) p.audioIn1.clear(); else p.audioIn2.clear();
        });
        saveDevicePrefs();
        return false;
    }
    const InUiEntry* found = findUiEntry(s_inUiCache, &buildInDeviceUiEntries, label);
    if (!found) { saveDevicePrefs(); return false; }
    const InUiEntry sel = *found;
    bool connected = false;
    if (!sel.isVirtual && !sel.deviceName.empty()) {
        input.switchDevice(sel.deviceName);
        connected = input.isOpen();
        if (connected) pc_platform_set_audio_input(slot, &input);
        outLabel = sel.label;   // store the shown label, not the raw device name
        setDevicePrefs([&](DevicePreferences& p) {
            if (slot == 0) p.audioIn1 = sel.label; else p.audioIn2 = sel.label;
        });
    }
    else if (sel.isVirtual) {
        uint32_t rate = pcAudio.isOpen() ? pcAudio.getSampleRate() : 44100;
        if (auto* cap = crosspad_pc::audio_platform::startVirtualCapture(
                slot, sel.captureDeviceName, rate)) {
            connected = true;
            outLabel = sel.label;
            pc_platform_set_audio_input(slot, cap);
            setDevicePrefs([&](DevicePreferences& p) {
                if (slot == 0) p.audioIn1 = sel.label; else p.audioIn2 = sel.label;
            });
        }
    }
    saveDevicePrefs();
    return connected;
}

/// The same, by position in the current IN list (the control port's form).
static bool appAudioInSelectIdx(int slot, int uiIndex, std::string& outLabel) {
    std::string label;
    {
        std::lock_guard<std::mutex> lk(s_audioSelMutex);
        if (uiIndex > 0 && static_cast<size_t>(uiIndex) <= s_inUiCache.size())
            label = s_inUiCache[uiIndex - 1].label;
        else if (uiIndex > 0)
            return false;
    }
    return appAudioInSelect(slot, label, outLabel);
}

#ifdef USE_VIRTUAL_AUDIO
/// The route watch: every couple of seconds put an output back on its pick
/// once that works again, and take it off a route that broke -- its sink went
/// away (the server then moves the stream to the default), or the default it
/// follows became a CrossPad sink. The jack panel is told what plays.
static std::atomic<bool> s_routeWatchRun{false};
static std::atomic<bool> s_routeWatchBusy{false};

static void routeWatchTick() {
    for (int slot = 0; slot < 2; ++slot) {
        std::string label;
        bool connected = false;
        {
            std::unique_lock<std::mutex> lk(s_audioSelMutex, std::try_to_lock);
            if (!lk.owns_lock()) return;   // a pick is being applied
            const OutRoute want = s_outWanted[slot];
            const OutRoute open = s_outOpen[slot];
            if (want.kind == OutRoute::Kind::None || open.kind == OutRoute::Kind::None) continue;
            const bool broken = (open.followsDefault() && defaultIsCrossPads())
                || (open.kind == OutRoute::Kind::Sink && !sinkListed(open.id));
            // Back to the pick: its sink is there again (and was not refused
            // while there), or the default it follows is no longer CrossPad's
            // (only then was it replaced by a sink).
            bool canReturn = false;
            if (!open.same(want) && want.kind == OutRoute::Kind::Sink) {
                if (!sinkListed(want.id)) s_outRefusedSink[slot].clear();
                else canReturn = s_outRefusedSink[slot] != want.id;
            } else if (!open.same(want)) {
                canReturn = want.followsDefault() && open.kind == OutRoute::Kind::Sink
                         && !defaultIsCrossPads();
            }
            if (!broken && !canReturn) continue;
            printf("[Audio] OUT%d: %s\n", slot + 1,
                   broken ? "its sink went away, or the default it follows is CrossPad's; re-routing"
                          : "its pick works again, going back to it");
            connected = openOutput(slot, want);
            label = connected ? s_outOpen[slot].label : std::string{};
        }
        auto* res = new AudioJackUiResult{slot == 0 ? EmuJackPanel::AUDIO_OUT1
                                                    : EmuJackPanel::AUDIO_OUT2,
                                          connected, std::move(label)};
        pc_gui::post([](void* ud) {
            auto* r = static_cast<AudioJackUiResult*>(ud);
            auto& jp = stm32Emu.getJackPanel();
            jp.setConnected(static_cast<EmuJackPanel::JackId>(r->jackId), r->connected);
            jp.setDeviceName(static_cast<EmuJackPanel::JackId>(r->jackId), r->label);
            delete r;
        }, res);
    }
}

/// Detached, so a process that ends without the shutdown is not stopped by a
/// joinable thread; routeWatchStop() waits for a tick in progress instead.
static void routeWatchStart() {
    if (s_routeWatchRun.exchange(true)) return;
    std::thread([] {
        while (s_routeWatchRun.load()) {
            for (int i = 0; i < 20 && s_routeWatchRun.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            s_routeWatchBusy.store(true);
            if (s_routeWatchRun.load()) routeWatchTick();
            s_routeWatchBusy.store(false);
        }
    }).detach();
}

static void routeWatchStop() {
    s_routeWatchRun.store(false);
    while (s_routeWatchBusy.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
#endif

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

#ifdef USE_AUDIO
/* What crosspad-sampler's kit gate asks of the simulator. The kit scan runs
 * inside the mount, so there is no "still scanning" here. */
static crosspad_sampler::KitGateHost kit_gate_host() {
    crosspad_sampler::KitGateHost host;
    host.appContainer = []() { return app_c; };
    host.cardPresent  = []() { return !pc_platform_get_sdcard_path().empty(); };
    host.log          = [](const char* line) { printf("[sampler] %s\n", line); };
    host.selectorIcon = "notepick.png";
    return host;
}
#endif

static void InitializeOrchestrator() {
    crosspad_gui::OrchestratorConfig config;
    config.app_factory = crosspad_gui::defaultAppFactory<App>;
    config.icon_resolver = pc_icon_resolver;
#ifdef USE_AUDIO
    // An app that needs a kit and has none gets the kit selector first — the
    // same gate the device puts in front of the sampler.
    config.pre_launch = crosspad_sampler::kit_gate_pre_launch;
#endif
    crosspad_gui::AppOrchestrator::getInstance().init(config);
}

static void LoadMainScreen(lv_obj_t* parent) {
    if (parent == nullptr) parent = lv_screen_active();

    auto& orch = crosspad_gui::AppOrchestrator::getInstance();

    crosspad_gui::MainScreenConfig config;
    config.encoder        = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    config.on_select      = crosspad_gui::AppOrchestrator::onAppSelected;
    config.on_close       = crosspad_gui::AppOrchestrator::onPopupClose;
    config.button_size    = APP_BUTTON_SIZE;
    config.button_spacing = 10;
    config.show_names     = APP_BUTTON_NAME_VISIBLE;
    config.style          = crosspad::CrosspadSettings::getInstance()->launcherStyle;
    config.bg_color       = lv_color_black();
    // REC/DUB/PLAY/STOP live on the transport bar since the mini-daw merge;
    // without it the Sequencer and Arrange cannot take or play anything here.
    // A kit without a top bar (a game) gives its app the whole panel.
    config.transport_bar  = CROSSPAD_FW_TOP_BAR;

#ifdef USE_AUDIO
    // The rebuild below cleans the whole screen, kit-selector overlay
    // included; a launch that was waiting behind that gate must not land on
    // the launcher that replaces it.
    crosspad_sampler::kit_gate_reset();
#endif
    auto result = orch.loadMainScreen(parent, config);
    // Built all the same (its drawer and timers keep working), only not shown.
    if (!CROSSPAD_FW_TOP_BAR && result.status_bar) lv_obj_add_flag(result.status_bar, LV_OBJ_FLAG_HIDDEN);

    app_c    = result.app_container;

    crosspad_app_update_pad_icon();
    printf("[GUI] Main screen loaded with %zu apps\n", orch.getAppInfos().size());
}

/* ── Public API ───────────────────────────────────────────────────────── */

/* ── Boot ──────────────────────────────────────────────────────────────
 *
 * The devices take seconds to come up -- RtAudio opens and enumerates every
 * card, the virtual sinks go through PipeWire or pactl, a saved output pin
 * waits up to 4 s per output for its stream, BLE starts an adapter, the
 * sampler scans the card's kits -- and all of it ran on the LVGL task before
 * its first frame, so the window stayed black until it was over and only then
 * did the splash play. Now the splash is on the panel at once and plays while
 * a worker task brings the devices up; the launcher is built once they are,
 * and the splash leaves when both that and its own sequence are done.
 *
 * The worker is a FreeRTOS task, as the LVGL task is, so everything it calls
 * runs where it always ran. It holds s_bootLock, which the LVGL task takes
 * for each lv_timer_handler() pass while booting, except around the calls
 * that only wait on devices (BootUnlocked): there the splash animates. Under
 * the lock nothing of LVGL, the pads or the platform services moves under the
 * worker's feet -- the splash, the pad tick and a key or click on the device
 * body all run inside lv_timer_handler(). */

static SemaphoreHandle_t  s_bootLock = nullptr;
static std::atomic<bool>  s_devicesUp{false};
static bool               s_booted = false;
static lv_obj_t*          s_splashHolder = nullptr;
#ifdef USE_AUDIO
static std::vector<std::string> s_bootOutNames;
static std::vector<std::string> s_bootInNames;
#endif

/// The worker's lock let go for a scope that only waits on devices.
class BootUnlocked {
public:
    BootUnlocked() { xSemaphoreGive(s_bootLock); }
    ~BootUnlocked() { xSemaphoreTake(s_bootLock, portMAX_DELAY); }
    BootUnlocked(const BootUnlocked&) = delete;
    BootUnlocked& operator=(const BootUnlocked&) = delete;
};

/* Over the LCD's content area, on the top layer: LoadMainScreen() cleans the
 * LCD container, and the splash must outlive that. Not clickable, as the
 * splash itself is not: a click goes to whatever is under it. */
static void boot_splash_begin()
{
    lv_obj_update_layout(s_lcdContainer);
    lv_area_t a;
    lv_obj_get_content_coords(s_lcdContainer, &a);
    s_splashHolder = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_splashHolder);
    lv_obj_set_pos(s_splashHolder, a.x1, a.y1);
    lv_obj_set_size(s_splashHolder, lv_area_get_width(&a), lv_area_get_height(&a));
    lv_obj_remove_flag(s_splashHolder, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    boot_splash_start(s_splashHolder, crosspad::CrosspadSettings::getInstance()->LCDbrightness,
                      nullptr, false);
}

static void boot_devices()
{
#ifdef USE_MIDI
    // The handlers before the ports open: RtMidi's thread calls them from the
    // first message on.
    midi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        struct D { uint8_t ch, note, vel; };
        auto* d = new D{channel, note, velocity};
        pc_gui::post([](void* ud) {
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
        pc_gui::post([](void* ud) {
            auto* n = static_cast<D*>(ud);
            auto& pm = crosspad::getPadManager();
            uint8_t padIdx = pm.getPadForMidiNote(n->note);
            if (padIdx < 16) pm.handlePadRelease(padIdx);
            else pm.handleMidiNoteOff(n->ch, n->note);
            delete n;
        }, d);
    });
    /* Like the notes, on the LVGL thread: the encoder's state is what the
     * emulator's timer draws and turns into LVGL key events, and a SysEx frame
     * drives the PadManager, its LEDs and the event bus -- a FreeRTOS queue,
     * which a thread outside the scheduler must not touch at all. */
    midi.setHandleControlChange([](uint8_t channel, uint8_t cc, uint8_t value) {
        printf("[MIDI IN] CC      ch=%u cc=%u  val=%u\n", channel + 1, cc, value);
        if (channel != 0 || (cc != 1 && cc != 64)) return;
        struct D { uint8_t cc, value; };
        pc_gui::post([](void* ud) {
            auto* d = static_cast<D*>(ud);
            if (d->cc == 1) stm32Emu.handleEncoderCC(d->value, 31, 18);
            else            stm32Emu.handleEncoderPress(d->value >= 64);
            delete d;
        }, new D{cc, value});
    });
    midi.setHandleSystemExclusive([](uint8_t* data, unsigned size) {
        auto* frame = new std::vector<uint8_t>(data, data + size);
        pc_gui::post([](void* ud) {
            auto* f = static_cast<std::vector<uint8_t>*>(ud);
            stm32Handler.handleMessage(f->data(), static_cast<unsigned>(f->size()));
            delete f;
        }, frame);
    });

    {
        BootUnlocked unlocked;
        int outPort = findMidiPortByName(s_devicePrefs.midiOut, true);
        int inPort  = findMidiPortByName(s_devicePrefs.midiIn, false);

        if (outPort >= 0 && inPort >= 0) {
            midi.begin((unsigned)outPort, (unsigned)inPort);
            midi.setAutoConnectKeyword("CrossPad");
            printf("[MIDI] Connected from saved prefs: out=%d in=%d\n", outPort, inPort);
        } else {
            midi.beginAutoConnect("CrossPad");
        }
    }
    s_midiHandler.setUsbOutput(&midi);
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
        {
            BootUnlocked unlocked;
            bleMidi.begin(mode);
        }

        // Wire into routing system
        crosspad::getPlatformServices().setBleMidi(&bleMidi);
        s_midiHandler.setBleOutput(&bleMidi);  // Route pad output to BLE

        // Input callbacks — dispatch to LVGL thread via pc_gui::post().
        // RtMidi callbacks run on a separate thread, but PadManager/LED updates
        // touch LVGL which is NOT thread-safe. Must dispatch to the LVGL task.
        // Echo is already filtered by PcBleMidi's anti-loopback ring buffer.
        bleMidi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
            printf("[BLE IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
            ble_settings_log_midi_in(0x90 | channel, note, velocity);

            struct NoteData { uint8_t ch; uint8_t note; uint8_t vel; };
            auto* d = new NoteData{channel, note, velocity};
            pc_gui::post([](void* ud) {
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
            pc_gui::post([](void* ud) {
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
        // On SimpleBLE's thread; the status bar reads the flag on the LVGL
        // thread, and the preferences are written from there too.
        bleMidi.setOnConnectionChanged([](bool connected, const crosspad::BleMidiDevice& dev) {
            printf("[BLE MIDI] %s: %s (%s)\n",
                   connected ? "Connected" : "Disconnected",
                   dev.name.c_str(), dev.address.c_str());
            struct D { bool connected; std::string address; };
            pc_gui::post([](void* ud) {
                auto* d = static_cast<D*>(ud);
                auto* st = crosspad::getPlatformServices().status;
                if (st) st->bluetoothConnected = d->connected;
                if (d->connected) {
                    setDevicePrefs([&](DevicePreferences& p) { p.bleDevice = d->address; });
                    saveDevicePrefs();
                }
                delete d;
            }, new D{connected, dev.address});
        });
    }
#endif

#ifdef USE_AUDIO
    // ── Audio OUT1/OUT2: the saved pick, or the default ──
    //
    // The preferences hold the pick: a sink (audioOutNPa, the stream created
    // on it), an RtAudio device by name, or nothing (the default). An output
    // that would follow a default that is, or is about to be, CrossPad's own
    // goes to a named real sink instead (openOutput). A fallback is not
    // written back, so the pick returns once it works again (the route watch).
    if (crosspad_pc::instance::audioEnabled()) {
        DevicePreferences prefs;
        setDevicePrefs([&](DevicePreferences& p) { prefs = p; });
#ifdef USE_PIPEWIRE
        // The takeover below runs when this holds (and the native backend
        // makes the virtual sinks, which PwContext::init() answers).
        s_takeoverPending = crosspad_pc::instance::defaultSinkTakeoverAllowed() &&
                            prefs.pwTakeoverDefault && crosspad_pc::PwContext::instance().init();
#endif
        auto pick = [](const std::string& name, const std::string& pa) {
            if (!pa.empty())   return OutRoute{OutRoute::Kind::Sink, pa, name};
            if (!name.empty()) return OutRoute{OutRoute::Kind::Device, name, name};
            return OutRoute{OutRoute::Kind::Default, {}, {}};
        };
        BootUnlocked unlocked;
        std::lock_guard<std::mutex> lk(s_audioSelMutex);
        s_outWanted[0] = pick(prefs.audioOut1, prefs.audioOut1Pa);
        s_outWanted[1] = pick(prefs.audioOut2, prefs.audioOut2Pa);
        openOutput(0, s_outWanted[0]);
        openOutput(1, s_outWanted[1]);
    }
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
    if (crosspad_pc::instance::audioEnabled()) {
        BootUnlocked unlocked;
        s_virtualSinkManager = crosspad_pc::makeVirtualSinkManager();
        if (s_virtualSinkManager) {
            if (!s_virtualSinkManager->setup(2)) {
                printf("[VirtSink] Virtual sinks not available: %s\n",
                       s_virtualSinkManager->errorHint().c_str());
            }
        }
    }
#endif

    {
        BootUnlocked unlocked;
#if defined(USE_VIRTUAL_AUDIO)
        // Prefer virtual sinks over saved physical inputs — the whole point of
        // the feature is that CrossPad becomes a system-wide mixer by default.
        // Off-Linux the manager is a no-op (empty list, null input) and the
        // monitor capturer returns nullptr, so both slots fall through to
        // RtAudio without a platform #ifdef here.
        bool in1Virtual = false, in2Virtual = false;
        if (s_virtualSinkManager) {
            auto sinks = s_virtualSinkManager->list();
            // Native PW backend exposes each sink directly as an IAudioInput —
            // no libpulse monitor round-trip. input() returns nullptr for the
            // pactl/RtAudio fallback backend, in which case we use the platform
            // monitor capturer below. (input() is on the base interface.)
            crosspad::IAudioInput* nativeIn1 = s_virtualSinkManager->input(0);
            crosspad::IAudioInput* nativeIn2 = s_virtualSinkManager->input(1);
            if (nativeIn1) {
                printf("[Audio] IN1 connected to native virtual sink (%s)\n", crosspad_pc::instance::nodeName("crosspad_vin1").c_str());
                pc_platform_set_audio_input(0, nativeIn1);
                in1Virtual = true;
            } else if (sinks.size() >= 1) {
                if (auto* cap = crosspad_pc::audio_platform::startVirtualCapture(
                        0, sinks[0].captureDeviceName, outSampleRate)) {
                    printf("[Audio] IN1 connected to virtual sink: %s\n",
                           sinks[0].displayName.c_str());
                    pc_platform_set_audio_input(0, cap);
                    in1Virtual = true;
                }
            }
            if (nativeIn2) {
                printf("[Audio] IN2 connected to native virtual sink (%s)\n", crosspad_pc::instance::nodeName("crosspad_vin2").c_str());
                pc_platform_set_audio_input(1, nativeIn2);
                in2Virtual = true;
            } else if (sinks.size() >= 2) {
                if (auto* cap = crosspad_pc::audio_platform::startVirtualCapture(
                        1, sinks[1].captureDeviceName, outSampleRate)) {
                    printf("[Audio] IN2 connected to virtual sink: %s\n",
                           sinks[1].displayName.c_str());
                    pc_platform_set_audio_input(1, cap);
                    in2Virtual = true;
                }
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

        auto inDevices = crosspad_pc::instance::audioEnabled()
                             ? enumerateAudioInputDevices()
                             : decltype(enumerateAudioInputDevices()){};
        // Restore a saved IN pick. New prefs store the UI label (a PipeWire
        // description); prefs written before that stored the raw RtAudio name.
        // Try the raw match first for back-compat, then resolve the label
        // through the same builder the dropdown uses.
        auto restoreInSlot = [&](PcAudioInput& in, int slot, const std::string& saved) {
            if (saved.empty() || in.isOpen()) return;
            if (std::find(inDevices.begin(), inDevices.end(), saved) != inDevices.end()) {
                in.begin(saved, outSampleRate);
                if (in.isOpen()) {
                    pc_platform_set_audio_input(slot, &in);
                    printf("[Audio] IN%d auto-connected: %s @ %u Hz\n", slot + 1,
                           in.getCurrentDeviceName().c_str(), in.getSampleRate());
                }
                return;
            }
            std::string lbl;
            if (appAudioInSelect(slot, saved, lbl))
                printf("[Audio] IN%d auto-connected: %s\n", slot + 1, lbl.c_str());
        };
        if (crosspad_pc::instance::audioEnabled()) {
            if (!in1Skip) restoreInSlot(pcAudioIn1, 0, s_devicePrefs.audioIn1);
            if (!in2Skip) restoreInSlot(pcAudioIn2, 1, s_devicePrefs.audioIn2);
        }
    }

#ifdef USE_PIPEWIRE
    // Make CrossPad IN#1 the system default sink so audio from other apps
    // flows into the mixer without manual routing. Only when the native PW
    // backend is active (input(0) != nullptr) and the pref allows it.
    {
        BootUnlocked unlocked;
        // Only the primary instance takes the desktop's default sink: a test
        // instance (CROSSPAD_INSTANCE / CROSSPAD_AUDIO) must leave it, and the
        // crash-recovery marker, alone.
        const bool pwMayTouchDefault = crosspad_pc::instance::defaultSinkTakeoverAllowed();
        const bool pwTakeoverWillRun = pwMayTouchDefault &&
                                       s_devicePrefs.pwTakeoverDefault &&
                                       s_virtualSinkManager &&
                                       s_virtualSinkManager->input(0) != nullptr;
        if (pwTakeoverWillRun) {
            // Crash recovery: a leftover pwPrevDefaultSink means the previous run
            // died before restore — that value, not the current default (which is
            // still our own sink), is what we must eventually restore.
            if (!s_devicePrefs.pwPrevDefaultSink.empty())
                s_pwGuard.setPreviousSink(s_devicePrefs.pwPrevDefaultSink);
            if (s_pwGuard.takeover(crosspad_pc::instance::nodeName("crosspad_vin1").c_str())) {
                setDevicePrefs([](DevicePreferences& p) { p.pwPrevDefaultSink = s_pwGuard.previousSink(); });
                saveDevicePrefs();
                printf("[Audio] system default sink -> CrossPad IN#1 (was: %s)\n",
                       s_devicePrefs.pwPrevDefaultSink.c_str());
            }
        } else if (pwMayTouchDefault && !s_devicePrefs.pwPrevDefaultSink.empty()) {
            // Takeover is NOT running this launch (pref off, or native PW backend
            // unavailable) but a previous run left a crash-recovery marker — the
            // configured default sink is still pointed at our virtual sink. Do a
            // one-shot restore of that stale marker so we don't strand the user's
            // default on crosspad_vin1. Only clear the marker on success; if the
            // daemon is unreachable, keep it and retry next launch.
            if (s_pwGuard.restoreStale(s_devicePrefs.pwPrevDefaultSink)) {
                printf("[Audio] restored stale default sink -> %s (no takeover)\n",
                       s_devicePrefs.pwPrevDefaultSink.c_str());
                setDevicePrefs([](DevicePreferences& p) { p.pwPrevDefaultSink.clear(); });
                saveDevicePrefs();
            }
        }
        // Taken over or not, the guard says from here on (a failed takeover
        // leaves the default real, and the route watch returns the outputs).
        s_takeoverPending = false;
        // An output put on the user's default sink above was created while
        // that was the default, so it follows the default -- now CrossPad's.
        // Re-open it before the module starts playing, while the default is
        // no longer that sink.
        routeWatchTick();
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
        s_mixerEngine.setup(mixerFrames < 128 ? 128 : mixerFrames, mixerSr,
                            crosspad_pc::PcAudioModule::NUM_BUSES);
    }
    s_mixerEngine.setDefaults();

#ifdef CROSSPAD_BOARD_FIRMWARE
    /* The board's desk, in its order and under its names (platform-idf
     * crosspad-platform-idf/CrosspadPlatform.cpp): the Mixer lists and files
     * channels by both. No host plays into a simulator: the USB channel is
     * there, silent. */
    static crosspad::AudioInputNode s_usbNode{static_cast<crosspad::IAudioInput*>(nullptr), "USB"};
    s_samplerNode = crosspad_pc::sampler_port_init(kit_gate_host());
    if (s_samplerNode) {
        const auto ch = s_mixerEngine.addChannel(s_samplerNode, "Sampler");
        for (uint8_t o = 0; o < crosspad_pc::PcAudioModule::NUM_BUSES; ++o) s_mixerEngine.setRouteEnabled(ch, o, true);
    }
    s_pitchedNode = crosspad_pc::pitched_port_init(mixerSr);
    if (s_pitchedNode) {
        const auto ch = s_mixerEngine.addChannel(s_pitchedNode, "Pitched");
        for (uint8_t o = 0; o < crosspad_pc::PcAudioModule::NUM_BUSES; ++o) s_mixerEngine.setRouteEnabled(ch, o, true);
    }
    {
        const auto ch = s_mixerEngine.addChannel(&s_usbNode, "USB");
        for (uint8_t o = 0; o < crosspad_pc::PcAudioModule::NUM_OUTPUTS; ++o) s_mixerEngine.setRouteEnabled(ch, o, true);
    }
    crosspad_pc::sequencer_init(s_mixerEngine, crosspad_pc::PcAudioModule::NUM_OUTPUTS);
    s_mixerEngine.addChannel(&s_in1Node, "IN1");
    s_mixerEngine.addChannel(&s_in2Node, "IN2");
    s_mixerEngine.setOutputName(0, "Out 1/2");
    s_mixerEngine.setOutputName(1, "Out 3/4");
    s_mixerEngine.setOutputName(crosspad_pc::PcAudioModule::CAPTURE_BUS, "USB rec");
    s_mixerEngine.setOutputHeard(crosspad_pc::PcAudioModule::CAPTURE_BUS, false);
#else
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
    s_samplerNode = crosspad_pc::sampler_port_init(kit_gate_host());
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

    // The click and the pattern sequencer ride the mixer, as on the board,
    // and so does the song engine that plays clips and bounces scenes.
    crosspad_pc::sequencer_init(s_mixerEngine, crosspad_pc::PcAudioModule::NUM_OUTPUTS);
#endif
    crosspad_pc::song_engine_init(s_mixerEngine, crosspad_pc::PcAudioModule::NUM_OUTPUTS,
                                  crosspad_pc::PcAudioModule::CAPTURE_BUS);
    // The Recorder's and the Slicer's: takes, auditions, kits from a loop.
    crosspad_pc::sample_tools_init(s_mixerEngine, crosspad_pc::PcAudioModule::NUM_OUTPUTS);

    // Load mixer state AFTER channel slots exist so saved per-channel routing
    // applies; loadState only patches existing slots, never creates them.
    s_mixerEngine.loadState(getMixerStatePath());

#ifndef CROSSPAD_BOARD_FIRMWARE   /* the board's desk names and routes its own */
    // The pads alone on the capture bus a bounce records, whatever an older
    // saved state (two outputs) said about routes it did not know.
    s_mixerEngine.setOutputName(crosspad_pc::PcAudioModule::CAPTURE_BUS, "Rec (pads)");
    if (s_samplerNode) {
        s_mixerEngine.setRouteEnabled(static_cast<MixerInput>(3),
                                      static_cast<MixerOutput>(crosspad_pc::PcAudioModule::CAPTURE_BUS), true);
    }
    if (s_pitchedNode) {
        s_mixerEngine.setRouteEnabled(static_cast<MixerInput>(4),
                                      static_cast<MixerOutput>(crosspad_pc::PcAudioModule::CAPTURE_BUS), true);
    }
#endif

    // Engine-level state-changed sink — MixerApp.cpp (cross-platform) calls
    // notifyStateChanged() after every GUI mutation; this hook flushes to disk.
    s_mixerEngine.setStateChangedCallback([]() {
        s_mixerEngine.saveState(getMixerStatePath());
    });
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
        s_audioModule.setCaptureTap(capture_tap);
#else
        s_synthNode.setEngine(&fmSynth);
        s_audioModule.addNode(&s_synthNode);
        // Without the mixer component the sampler is a plain generator on the
        // node chain; it is brought up here because there is no addChannel().
        s_samplerNode = crosspad_pc::sampler_port_init(kit_gate_host());
        if (s_samplerNode) s_audioModule.addNode(s_samplerNode);
        s_pitchedNode = crosspad_pc::pitched_port_init(cfg.sampleRate);
        if (s_pitchedNode) s_audioModule.addNode(s_pitchedNode);
#endif
        crosspad::getPlatformServices().setAudioModule(&s_audioModule);
        // Apps borrow the pitched engine through it (an Arrange Pitch track, a
        // song's instrument): after the sampler and the pitched engine.
        crosspad_pc::instrument_host_init();
        s_audioModule.start();
        s_outRate = cfg.sampleRate;

#ifdef USE_PIPEWIRE
        // Expose OUT1 as a virtual source ("CrossPad Out") that OBS/DAWs can
        // capture directly. Attach as the module's aux stream (OUT1 tap) only
        // after the source connects; setAuxStream stores a non-owning pointer
        // read by the audio thread.
        if (crosspad_pc::instance::audioEnabled()) {
            BootUnlocked unlocked;
            if (s_pwSource.start(crosspad_pc::instance::nodeName("crosspad_out").c_str(),
                                 crosspad_pc::instance::description("CrossPad Out").c_str(),
                                 cfg.sampleRate)) {
                s_audioModule.setAuxStream(&s_pwSource);
                printf("[Audio] OUT1 tap -> virtual source 'CrossPad Out' @ %u Hz\n",
                       cfg.sampleRate);
            } else {
                printf("[Audio] virtual source 'CrossPad Out' unavailable\n");
            }
        }
#endif
    }

    // Save preferences now that we know actual connected device names
    saveDevicePrefs();

    // What the jack panel's dropdowns offer: an enumeration that opens every
    // device, so here rather than on the LVGL thread. CROSSPAD_AUDIO=off opens
    // none, so it offers none.
    if (crosspad_pc::instance::audioEnabled()) {
        BootUnlocked unlocked;
        s_bootOutNames = refreshOutUiCache();
        s_bootInNames  = refreshInUiCache();
    }
#endif
}

static void boot_devices_run()
{
    xSemaphoreTake(s_bootLock, portMAX_DELAY);
    boot_devices();
    xSemaphoreGive(s_bootLock);
    s_devicesUp.store(true);
}

static void boot_devices_task(void*)
{
    boot_devices_run();
    vTaskDelete(nullptr);
}

/// The rest of the boot, on the LVGL thread once the devices are up.
static void boot_finish()
{
#ifdef USE_MIDI
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
#ifdef HAS_MIXER
    // The open song's kit and instrument, now that the orchestrator can say
    // which app is in front.
    crosspad_pc::song_sounds_start();
#endif
    LoadMainScreen(s_lcdContainer);

    /* Wire keyboard shortcuts: Escape→go home, Space/Ctrl→power button
     * (tap = contextual back, hold 0.5 s = quick settings), V→volume overlay.
     * Once there is a launcher to go home to: the splash takes keys while
     * the devices come up, and Escape then rebuilt a launcher whose
     * orchestrator did not exist yet. */
    stm32Emu.getKeyboardCapture().setEscapeCallback(
        CROSSPAD_FW_STANDALONE_APP[0] ? [] { crosspad_gui::powerBack(); } : crosspad_app_go_home);
    stm32Emu.getKeyboardCapture().setPowerCallback(crosspad_gui::powerButtonPress);
    stm32Emu.getKeyboardCapture().setPowerReleaseCallback(crosspad_gui::powerButtonRelease);
    stm32Emu.getKeyboardCapture().setVolumeCallback(crosspad_gui::volume_overlay_toggle);

    /* The board's boot-time decisions, made here as its platform makes them,
     * whatever app the firmware is: pad 16 held through the boot goes back
     * to the main firmware, a trial slot confirms itself. */
#ifndef __EMSCRIPTEN__   /* a twin's slots are the board's (TWIN_FW) */
    if (pc_fw::bootFinished(crosspad::getPadManager().isPadPressed(pc_fw::kRecoveryPad))) return;
#endif

    /* A single-app kit (DOOM) starts into its app; there is no launcher to
     * go back to, Escape is the power click. */
    if (CROSSPAD_FW_STANDALONE_APP[0] && !crosspad_app_start(CROSSPAD_FW_STANDALONE_APP))
        printf("[CrossPad] kit %s: app %s is not registered\n", CROSSPAD_FW_KIT_ID, CROSSPAD_FW_STANDALONE_APP);

    /* ── Auto-check for updates (background) ─────────────────────────── */
#ifdef __EMSCRIPTEN__
    if (false) {   // no GitHub in the browser: the board checks for itself
#else
    if (pc_platform_get_auto_check_updates()) {
#endif
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
        // Audio OUT1/OUT2 — what each plays, and whether it is open
        std::string outLabel[2];
        {
            std::lock_guard<std::mutex> lk(s_audioSelMutex);
            outLabel[0] = s_outOpen[0].label;
            outLabel[1] = s_outOpen[1].label;
        }
        jp.setDeviceName(EmuJackPanel::AUDIO_OUT1, outLabel[0]);
        jp.setConnected(EmuJackPanel::AUDIO_OUT1, pcAudio.isOpen());

        jp.setDeviceName(EmuJackPanel::AUDIO_OUT2, outLabel[1]);
        jp.setConnected(EmuJackPanel::AUDIO_OUT2, pcAudio2.isOpen());

        // Audio IN1/IN2 — current device. When a virtual sink serves the slot
        // the input is the platform monitor capturer, not PcAudioInput, so we
        // report the virtual sink's display name instead of the empty RtAudio
        // device name. virtualCaptureOpen() is false off-Linux.
        std::string in1Name = pcAudioIn1.getCurrentDeviceName();
        std::string in2Name = pcAudioIn2.getCurrentDeviceName();
        bool in1Connected = pcAudioIn1.isOpen();
        bool in2Connected = pcAudioIn2.isOpen();
#if defined(USE_VIRTUAL_AUDIO)
        if (crosspad_pc::audio_platform::virtualCaptureOpen(0) && s_virtualSinkManager) {
            auto sinks = s_virtualSinkManager->list();
            if (sinks.size() >= 1) { in1Name = sinks[0].displayName; in1Connected = true; }
        }
        if (crosspad_pc::audio_platform::virtualCaptureOpen(1) && s_virtualSinkManager) {
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
            const auto& outNames = s_bootOutNames;
            jp.setDeviceList(EmuJackPanel::AUDIO_OUT1, outNames,
                             findDropdownIndex(outNames, outLabel[0]));
            jp.setDeviceList(EmuJackPanel::AUDIO_OUT2, outNames,
                             findDropdownIndex(outNames, outLabel[1]));

            const auto& inNames = s_bootInNames;
            // IN prefs store the shown label (a PipeWire description), so the
            // selected index is matched against the saved label, not the raw
            // RtAudio device name getCurrentDeviceName() would return.
            jp.setDeviceList(EmuJackPanel::AUDIO_IN1, inNames,
                             findDropdownIndex(inNames, s_devicePrefs.audioIn1));
            jp.setDeviceList(EmuJackPanel::AUDIO_IN2, inNames,
                             findDropdownIndex(inNames, s_devicePrefs.audioIn2));
        }
#ifdef USE_VIRTUAL_AUDIO
        if (crosspad_pc::instance::audioEnabled()) routeWatchStart();
#endif
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
                        setDevicePrefs([&](DevicePreferences& p) { p.uartPort = port; });
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
                // result back with pc_gui::post() so only widget mutation — which
                // must stay on the LVGL thread — happens there. The pick is
                // the label the dropdown showed at that position, resolved by
                // name: positions shift whenever a list is rebuilt.
                const bool isOut = (jackId == EmuJackPanel::AUDIO_OUT1 ||
                                    jackId == EmuJackPanel::AUDIO_OUT2);
                const int  slot  = (jackId == EmuJackPanel::AUDIO_OUT1 ||
                                    jackId == EmuJackPanel::AUDIO_IN1) ? 0 : 1;
                const auto& shown = isOut ? s_bootOutNames : s_bootInNames;
                std::string pick;
                if (deviceIndex > 0 && deviceIndex < shown.size()) pick = shown[deviceIndex];
                std::thread([jackId, slot, isOut, pick]() {
                    std::string label;
                    bool connected = isOut ? appAudioOutSelect(slot, pick, label)
                                           : appAudioInSelect(slot, pick, label);
                    auto* res = new AudioJackUiResult{jackId, connected, std::move(label)};
                    pc_gui::post([](void* ud) {
                        auto* r = static_cast<AudioJackUiResult*>(ud);
                        auto& jp = stm32Emu.getJackPanel();
                        const auto id = static_cast<EmuJackPanel::JackId>(r->jackId);
                        jp.setConnected(id, r->connected);
                        jp.setDeviceName(id, r->label);
                        // A refused pick leaves the slot on another output:
                        // the dropdown says which.
                        if (id == EmuJackPanel::AUDIO_OUT1 || id == EmuJackPanel::AUDIO_OUT2)
                            jp.setDeviceList(id, s_bootOutNames,
                                             findDropdownIndex(s_bootOutNames, r->label));
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
                    setDevicePrefs([&](DevicePreferences& p) { p.midiOut.clear(); });
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
                        setDevicePrefs([&](DevicePreferences& p) { p.midiOut = name; });
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
                    setDevicePrefs([&](DevicePreferences& p) { p.midiIn.clear(); });
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
                        setDevicePrefs([&](DevicePreferences& p) { p.midiIn = name; });
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
                    setDevicePrefs([&](DevicePreferences& p) { p.uartPort.clear(); });
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
                            setDevicePrefs([&](DevicePreferences& p) { p.uartPort = comPorts[portIdx]; });
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
                setDevicePrefs([&](DevicePreferences& p) { p.uartPort = port; });
                saveDevicePrefs();
                crosspad::addPlatformCapability(crosspad::Capability::Usb);
                printf("[USB] Auto-reconnected CrossPad on %s\n", port.c_str());
                break;
            }
        }
    }, 5000, nullptr);

    printf("[CrossPad] App init complete\n");
}

void crosspad_app_init()
{
#if defined(USE_AUDIO) && defined(USE_VIRTUAL_AUDIO)
    /* Make sure virtual sinks get unloaded whatever the exit path.
       SDL_QUIT, Power off and the updater call crosspad_app_shutdown()
       directly; SIGINT/SIGTERM go through pc_install_shutdown_signals()
       (main()), atexit catches a normal return.
       kill -9 is unrecoverable — sinks will be cleaned up at next launch
       by LinuxPipewireSinks::cleanupStaleSinks(). */
    std::atexit(crosspad_app_shutdown);
#endif

#ifndef __EMSCRIPTEN__
    /* Which slot this binary runs, and that firmware's own settings in place
     * before they are read. */
    pc_fw::boot();
#endif

    /* Platform stubs (event bus, settings, pad manager, GUI) */
    pc_platform_init();

    /* The Firmware views and an app that leaves the firmware (DOOM's Quit)
     * switch through the one service. */
#ifdef __EMSCRIPTEN__
    /* The board's own service, over what the twin answers for its ESP half
     * (src/wasm/twin_*.cpp: TWIN_FW, an idle WiFi link, an updater that
     * leaves the work to the board). */
    fw_view_service_install();
#else
    crosspad_gui::setFwViewService(&pcFwViewService());
#endif

    /* The pads' animations (progress bars, showFrame) and the LED controller
     * advance on this tick, as on the board's pad_anim worker. On the LVGL
     * thread: the pad clicks and the marshalled MIDI input that also drive the
     * LEDs run here too. */
    lv_timer_create([](lv_timer_t*) { crosspad::getPadManager().update(); },
                    kPadAnimTickMs, nullptr);

    /* Register go-home callback for shared Settings UI */
    pc_platform_set_go_home([]() { LoadMainScreen(s_lcdContainer); });

    /* Load saved device preferences */
    loadDevicePrefs();

    /* STM32 emulator window — returns 320x240 LCD container. With --lcd the
     * display is the LCD itself: the GUI takes the active screen as the board's
     * does, and the body is built on a screen never shown (its jacks and card
     * slot are still what the rest of the simulator talks to). */
    s_lcdContainer = s_lcdOnly ? (stm32Emu.init(lv_obj_create(nullptr)), lv_screen_active())
                               : stm32Emu.init();

    /* Virtual SD card slot — auto-mount from saved preferences */
#ifdef __EMSCRIPTEN__
    // The page mounts the card (IndexedDB) at /sdcard (crosspad-web-twin web/simtwin.js).
    if (s_devicePrefs.sdcardPath.empty()) s_devicePrefs.sdcardPath = "/sdcard";
#endif
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
            setDevicePrefs([&](DevicePreferences& p) { p.sdcardPath.clear(); });
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
        setDevicePrefs([&](DevicePreferences& p) { p.sdcardPath = path; });
        saveDevicePrefs();
    });
    stm32Emu.getSdCardSlot().setOnUnmount([]() {
        pc_platform_set_sdcard_path("");
#ifdef USE_AUDIO
        crosspad_pc::sampler_port_set_sdcard("");
#endif
        setDevicePrefs([&](DevicePreferences& p) { p.sdcardPath.clear(); });
        saveDevicePrefs();
    });

    /* Overlay layer on lv_layer_top(), exactly over the LCD area. It sat at
     * y = 20 while the panel is drawn at LCD_Y, so every overlay -- the MODE
     * map, dialogs, the kit loader -- landed 38 px high, and one aligned to
     * the panel lost its bottom 38 px to the layer's clip. With --lcd the top
     * layer is the LCD already, as on the board. */
    if (!s_lcdOnly) {
        lv_obj_t* overlayLayer = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(overlayLayer);
        lv_obj_set_pos(overlayLayer, Stm32EmuWindow::LCD_X, Stm32EmuWindow::LCD_Y);
        lv_obj_set_size(overlayLayer, Stm32EmuWindow::LCD_W, Stm32EmuWindow::LCD_H);
        lv_obj_remove_flag(overlayLayer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        crosspad_gui::setOverlayParent(overlayLayer);
    }

    /* The board's cold-boot splash, the first thing on the panel: the devices
     * come up behind it, the launcher is built behind it, and it leaves once
     * both are done and it has played. CROSSPAD_NO_SPLASH skips it (the GUI
     * test harness starts at the launcher). */
    if (!std::getenv("CROSSPAD_NO_SPLASH")) boot_splash_begin();

    // Init MidiInputHandler — central routing brain (same pattern as ESP32).
    // Routes pad output to USB + BLE + STM32 based on KeypadSettings flags.
    s_midiHandler.init(crosspad::getPadManager(), crosspad::getEventBus(),
                       crosspad::CrosspadSettings::getInstance());
    crosspad::getPlatformServices().setMidiOutput(&s_midiHandler);

#ifdef USE_MIDI
    // Initialize STM32 message handler
    stm32Handler.init(crosspad::getPadManager(), status);
    /* DAW Control takes host frames through the platform's catalog decoder,
     * as on the board. */
    crosspad::getPlatformServices().stm32MessageHandler = &stm32Handler;
#endif

    s_bootLock = xSemaphoreCreateMutex();
    if (xTaskCreate(boot_devices_task, "boot_dev", kBootTaskStack, nullptr,
                    kBootTaskPriority, nullptr) != pdPASS) {
        printf("[CrossPad] boot worker not created; bringing the devices up here\n");
        boot_devices_run();
    }
}

bool crosspad_app_tick()
{
    if (s_booted) {
        lv_timer_handler();
        if (s_splashHolder && !boot_splash_active()) {
            lv_obj_delete(s_splashHolder);
            s_splashHolder = nullptr;
            boot_splash_reveal();
        }
        return false;
    }
    xSemaphoreTake(s_bootLock, portMAX_DELAY);
    lv_timer_handler();
    const bool up = s_devicesUp.load();
    if (up) {
        boot_finish();
        s_booted = true;
    }
    xSemaphoreGive(s_bootLock);
    return up;
}

void crosspad_app_go_home()
{
    LoadMainScreen(s_lcdContainer);
}

bool crosspad_app_start(const char* name)
{
    for (const auto& app : crosspad_gui::AppOrchestrator::getInstance().getApps()) {
        if (app && crosspad::control::appNameMatches(app->getName(), name)) {
            crosspad_gui::AppOrchestrator::onAppSelected(app.get(), app_c);
            return true;
        }
    }
    return false;
}

void crosspad_app_update_pad_icon()
{
#ifdef CROSSPAD_BOARD_FIRMWARE
    return;   // a simulator's glyph; the board's status bar has none
#endif
    std::string active = crosspad::getPadManager().getActivePadLogic();

    if (active == "MLPiano") {
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

    // Nothing re-routes an output while the default and the sinks go away.
    routeWatchStop();

#ifdef USE_PIPEWIRE
    // Restore the user's original default sink and clear the crash-recovery
    // marker BEFORE tearing down our nodes — so a clean exit leaves no stale
    // pwPrevDefaultSink behind. Detach the aux stream (non-owning pointer read
    // by the audio thread) BEFORE stopping/destroying the source.
    s_pwGuard.restore();
    setDevicePrefs([&](DevicePreferences& p) { p.pwPrevDefaultSink.clear(); });
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

    // Non-blocking: the process is about to _Exit(0), so abandoning the
    // capture threads is preferable to hanging in a join(). No-op off-Linux.
    crosspad_pc::audio_platform::detachVirtualCaptureForShutdown();
    pcAudioIn1.end();
    pcAudioIn2.end();
#ifdef USE_PIPEWIRE
    // Tear down the shared pw_thread_loop LAST — after every proxy/stream that
    // lives on it (guard, source, native sink captures) is already stopped.
    crosspad_pc::PwContext::instance().shutdown();
#endif
#endif
}
