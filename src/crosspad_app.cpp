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
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <string>

#include "lvgl/lvgl.h"

#include "pc_stubs/pc_platform.h"
#include "pc_stubs/PcApp.hpp"

// crosspad-core
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"

// STM32 hardware emulator window
#include "stm32_emu/Stm32EmuWindow.hpp"

// crosspad-gui
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/components/app_launcher.h"
#include "crosspad-gui/components/main_screen.h"

#ifdef USE_MIDI
#include "midi/PcMidi.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#endif

#ifdef USE_AUDIO
#include "audio/PcAudio.hpp"
#include "audio/PcAudioInput.hpp"
#include "crosspad-gui/components/vu_meter.h"
#include "crosspad/audio/PeakMeter.hpp"
#include "synth/MlPianoSynth.hpp"
#include "apps/mixer/AudioMixerEngine.hpp"
#include "apps/mixer/MixerPadLogic.hpp"
#include <RtAudio.h>
#endif

#include <ArduinoJson.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define APP_BUTTON_SIZE 64
#define APP_BUTTON_NAME_VISIBLE true

/* ── State ────────────────────────────────────────────────────────────── */

extern lv_obj_t* status_c;

static lv_obj_t* app_c = nullptr;
static lv_obj_t* s_lcdContainer = nullptr;
static App* runningApp = nullptr;
static std::vector<App*> app_shortcuts;
static Stm32EmuWindow stm32Emu;

#ifdef USE_MIDI
static PcMidi midi;
static crosspad::Stm32MessageHandler stm32Handler;
extern CrosspadStatus status;
#endif

#ifdef USE_AUDIO
static PcAudioOutput pcAudio;      // OUT1
static PcAudioOutput pcAudio2;     // OUT2
static PcAudioInput  pcAudioIn1;   // IN1
static PcAudioInput  pcAudioIn2;   // IN2
static MlPianoSynth fmSynth;
static AudioMixerEngine s_mixerEngine;
static std::shared_ptr<MixerPadLogic> s_mixerPadLogic;
#endif

/* ── Device Preferences ──────────────────────────────────────────────── */

struct DevicePreferences {
    std::string audioOut1;
    std::string audioOut2;
    std::string audioIn1;
    std::string audioIn2;
    std::string midiOut;
    std::string midiIn;
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
    if (doc["audio_in1"].is<const char*>())  s_devicePrefs.audioIn1  = doc["audio_in1"].as<const char*>();
    if (doc["audio_in2"].is<const char*>())  s_devicePrefs.audioIn2  = doc["audio_in2"].as<const char*>();
    if (doc["midi_out"].is<const char*>())   s_devicePrefs.midiOut   = doc["midi_out"].as<const char*>();
    if (doc["midi_in"].is<const char*>())    s_devicePrefs.midiIn    = doc["midi_in"].as<const char*>();

    printf("[DevPrefs] Loaded: out1='%s' out2='%s' in1='%s' in2='%s' midiOut='%s' midiIn='%s'\n",
           s_devicePrefs.audioOut1.c_str(), s_devicePrefs.audioOut2.c_str(),
           s_devicePrefs.audioIn1.c_str(), s_devicePrefs.audioIn2.c_str(),
           s_devicePrefs.midiOut.c_str(), s_devicePrefs.midiIn.c_str());
}

static void saveDevicePrefs() {
    std::string path = getDevicePrefsPath();
    JsonDocument doc;
    doc["audio_out1"] = s_devicePrefs.audioOut1;
    doc["audio_out2"] = s_devicePrefs.audioOut2;
    doc["audio_in1"]  = s_devicePrefs.audioIn1;
    doc["audio_in2"]  = s_devicePrefs.audioIn2;
    doc["midi_out"]   = s_devicePrefs.midiOut;
    doc["midi_in"]    = s_devicePrefs.midiIn;

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

/* ── Launcher callbacks ───────────────────────────────────────────────── */

static void LoadMainScreen(lv_obj_t* parent);

static void on_app_selected(void* app_ptr, lv_obj_t* app_container) {
    App* app = (App*)app_ptr;
    if (!app) return;
    printf("[GUI] Launching app: %s\n", app->getName());
    app->start(app_container);
    app->resume();
    runningApp = app;
}

static void on_popup_close(void* app_ptr) {
    App* app = (App*)app_ptr;
    if (app) app->destroyApp();
}

/* ── Main screen ──────────────────────────────────────────────────────── */

static void LoadMainScreen(lv_obj_t* parent) {
    if (parent == nullptr) parent = lv_screen_active();

    if (runningApp != nullptr && runningApp->isStarted()) {
        runningApp->destroyApp();
        runningApp = nullptr;
    }

    crosspad::getPadManager().setActivePadLogic("Mixer");

    std::vector<crosspad_gui::LauncherAppInfo> infos;
    for (App* a : app_shortcuts) {
        if (a) infos.push_back({a->getName(), a->getIcon(), a});
    }

    crosspad_gui::MainScreenConfig config;
    config.on_select      = on_app_selected;
    config.on_close       = on_popup_close;
    config.button_size    = APP_BUTTON_SIZE;
    config.button_spacing = 10;
    config.show_names     = APP_BUTTON_NAME_VISIBLE;
    config.bg_color       = lv_color_black();

    auto result = crosspad_gui::build_main_screen(
        parent, infos.data(), infos.size(), config);

    status_c = result.status_bar;
    app_c    = result.app_container;

    crosspad_app_update_pad_icon();
    printf("[GUI] Main screen loaded with %zu apps\n", infos.size());
}

/* ── App registration ─────────────────────────────────────────────────── */

static void InitializeApps() {
    crosspad_gui::launcher_register_power_off();

    app_shortcuts.clear();
    auto& registry = crosspad::AppRegistry::getInstance();
    const auto* apps = registry.getApps();
    size_t count = registry.getAppCount();

    printf("[GUI] Found %zu registered apps\n", count);

    for (size_t i = 0; i < count; i++) {
        const auto& entry = apps[i];
        if (entry.createLVGL == nullptr) continue;

        App* app = new App(app_c, entry.name, entry.icon, entry.createLVGL, entry.destroyLVGL);
        if (!app) continue;

        if (strcmp(entry.name, "Power OFF") == 0) {
            app->AddFlag(crosspad::APP_FLAG_MSGBOX);
        }
        app_shortcuts.push_back(app);
    }

    printf("[GUI] Created %zu app shortcuts\n", app_shortcuts.size());
}

/* ── Public API ───────────────────────────────────────────────────────── */

void crosspad_app_init()
{
    /* Platform stubs (event bus, settings, pad manager, GUI) */
    pc_platform_init();

    /* Register go-home callback for shared Settings UI */
    pc_platform_set_go_home([]() { LoadMainScreen(s_lcdContainer); });

    /* Load saved device preferences */
    loadDevicePrefs();

    /* STM32 emulator window — returns 320x240 LCD container */
    lv_obj_t* lcdContainer = stm32Emu.init();
    s_lcdContainer = lcdContainer;

    /* Overlay layer on lv_layer_top(), positioned over the LCD area. */
    lv_obj_t* overlayLayer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlayLayer);
    lv_obj_set_pos(overlayLayer, (Stm32EmuWindow::WIN_W - 320) / 2, 20);
    lv_obj_set_size(overlayLayer, 320, 240);
    lv_obj_remove_flag(overlayLayer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    crosspad_gui::setOverlayParent(overlayLayer);

#ifdef USE_MIDI
    // Initialize STM32 message handler
    stm32Handler.init(crosspad::getPadManager(), status);

    // Auto-connect MIDI from saved preferences or fall back to "CrossPad" keyword
    {
        int outPort = findMidiPortByName(s_devicePrefs.midiOut, true);
        int inPort  = findMidiPortByName(s_devicePrefs.midiIn, false);

        if (outPort >= 0 || inPort >= 0) {
            // We have saved preferences — connect to those specific ports
            midi.begin(outPort >= 0 ? (unsigned)outPort : 0,
                       inPort >= 0  ? (unsigned)inPort  : 0);
            printf("[MIDI] Connected from saved prefs: out=%d in=%d\n", outPort, inPort);
        } else {
            // No saved prefs — use auto-connect keyword
            midi.beginAutoConnect("CrossPad");
        }
        pc_platform_set_midi_output(&midi);
        crosspad::addPlatformCapability(crosspad::Capability::Midi);
    }

    midi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        auto& pm = crosspad::getPadManager();
        uint8_t padIdx = pm.getPadForMidiNote(note);
        if (padIdx < 16) {
            pm.handlePadPress(padIdx, velocity);
        } else {
            pm.handleMidiNoteOn(channel, note, velocity);
        }
    });
    midi.setHandleNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOff ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        auto& pm = crosspad::getPadManager();
        uint8_t padIdx = pm.getPadForMidiNote(note);
        if (padIdx < 16) {
            pm.handlePadRelease(padIdx);
        } else {
            pm.handleMidiNoteOff(channel, note);
        }
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

    // Periodic MIDI reconnect timer (3s)
    lv_timer_create([](lv_timer_t*) {
        bool connected = midi.isKeywordConnected() && midi.isOutputOpen();
        if (!connected) {
            printf("[MIDI] Attempting reconnect...\n");
            midi.reconnect();

            // Update jack panel after reconnect
            auto& jp = stm32Emu.getJackPanel();
            if (midi.isOutputOpen()) {
                RtMidiOut probe;
                for (unsigned int i = 0; i < probe.getPortCount(); i++) {
                    jp.setDeviceName(EmuJackPanel::MIDI_OUT, probe.getPortName(i));
                    break;
                }
                jp.setConnected(EmuJackPanel::MIDI_OUT, true);
            }
            if (midi.isInputOpen()) {
                RtMidiIn probe;
                for (unsigned int i = 0; i < probe.getPortCount(); i++) {
                    jp.setDeviceName(EmuJackPanel::MIDI_IN, probe.getPortName(i));
                    break;
                }
                jp.setConnected(EmuJackPanel::MIDI_IN, true);
            }
        }
    }, 3000, nullptr);
#endif

#ifdef USE_AUDIO
    // ── Audio OUT1: auto-connect saved device or default ──
    {
        auto outDevices = enumerateAudioOutputDevices();
        unsigned int dev1 = findDeviceByName(outDevices, s_devicePrefs.audioOut1);
        pcAudio.begin(dev1);
        pc_platform_set_audio_output(&pcAudio);

        // Save actual device name if we connected
        if (pcAudio.isOpen()) {
            s_devicePrefs.audioOut1 = pcAudio.getCurrentDeviceName();
            crosspad::addPlatformCapability(crosspad::Capability::AudioOut);
        }
    }

    // ── Audio OUT2: auto-connect saved device or default ──
    {
        auto outDevices = enumerateAudioOutputDevices();
        unsigned int dev2 = findDeviceByName(outDevices, s_devicePrefs.audioOut2);
        pcAudio2.begin(dev2);
        pc_platform_set_audio_output_2(&pcAudio2);

        if (pcAudio2.isOpen()) {
            s_devicePrefs.audioOut2 = pcAudio2.getCurrentDeviceName();
        }
    }

    // ── Audio IN1/IN2: auto-connect saved devices ──
    pc_platform_set_audio_input(0, &pcAudioIn1);
    pc_platform_set_audio_input(1, &pcAudioIn2);

    {
        auto inDevices = enumerateAudioInputDevices();
        if (!s_devicePrefs.audioIn1.empty()) {
            unsigned int devId = findDeviceByName(inDevices, s_devicePrefs.audioIn1);
            if (devId != 0) {
                pcAudioIn1.begin(devId);
                if (pcAudioIn1.isOpen()) {
                    printf("[Audio] IN1 auto-connected: %s\n", pcAudioIn1.getCurrentDeviceName().c_str());
                    crosspad::addPlatformCapability(crosspad::Capability::AudioIn);
                }
            }
        }
        if (!s_devicePrefs.audioIn2.empty()) {
            unsigned int devId = findDeviceByName(inDevices, s_devicePrefs.audioIn2);
            if (devId != 0) {
                pcAudioIn2.begin(devId);
                if (pcAudioIn2.isOpen()) {
                    printf("[Audio] IN2 auto-connected: %s\n", pcAudioIn2.getCurrentDeviceName().c_str());
                }
            }
        }
    }

    // Initialize FM synth engine at the audio device's actual sample rate
    fmSynth.setSampleRate(pcAudio.getSampleRate());
    fmSynth.init();
    pc_platform_set_synth_engine(&fmSynth);
    crosspad::addPlatformCapability(crosspad::Capability::Synth);

    // Load mixer state from preferences (or set defaults)
    s_mixerEngine.loadState(getMixerStatePath());

    // Register mixer pad logic globally (always available)
    s_mixerPadLogic = std::make_shared<MixerPadLogic>(s_mixerEngine);
    s_mixerPadLogic->setOnStateChanged([]() {
        // Save mixer state on every pad-driven change
        s_mixerEngine.saveState(getMixerStatePath());
    });
    crosspad::getPadManager().registerPadLogic("Mixer", s_mixerPadLogic);
    crosspad::getPadManager().setActivePadLogic("Mixer");

    // Start the mixer engine (replaces the old default synth-only audio thread)
    s_mixerEngine.start();

    // Save preferences now that we know actual connected device names
    saveDevicePrefs();
#endif

    /* Register all apps (auto-generated by cmake/generate_registry.cmake) */
    {
        extern void AppRegistry_InitAll();
        AppRegistry_InitAll();
    }

    /* Styles, apps, main screen */
    initStyles();
    InitializeApps();
    LoadMainScreen(lcdContainer);

    /* ── Jack panel wiring ────────────────────────────────────────────── */
    {
        auto& jp = stm32Emu.getJackPanel();

#ifdef USE_AUDIO
        // Audio OUT1/OUT2 — show device name and connection status
        jp.setDeviceName(EmuJackPanel::AUDIO_OUT1, pcAudio.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_OUT1, pcAudio.isOpen());

        jp.setDeviceName(EmuJackPanel::AUDIO_OUT2, pcAudio2.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_OUT2, pcAudio2.isOpen());

        // Audio IN1/IN2 — show device name and connection status
        jp.setDeviceName(EmuJackPanel::AUDIO_IN1, pcAudioIn1.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_IN1, pcAudioIn1.isOpen());

        jp.setDeviceName(EmuJackPanel::AUDIO_IN2, pcAudioIn2.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_IN2, pcAudioIn2.isOpen());

        // Populate device lists for all audio jacks
        {
            auto outDevices = enumerateAudioOutputDevices();
            std::vector<std::string> outNames;
            outNames.push_back("(None)");
            for (auto& d : outDevices) outNames.push_back(d.name);

            jp.setDeviceList(EmuJackPanel::AUDIO_OUT1, outNames,
                             findDropdownIndex(outNames, pcAudio.getCurrentDeviceName()));
            jp.setDeviceList(EmuJackPanel::AUDIO_OUT2, outNames,
                             findDropdownIndex(outNames, pcAudio2.getCurrentDeviceName()));

            auto inDevices = enumerateAudioInputDevices();
            std::vector<std::string> inNames;
            inNames.push_back("(None)");
            for (auto& d : inDevices) inNames.push_back(d.name);

            jp.setDeviceList(EmuJackPanel::AUDIO_IN1, inNames,
                             findDropdownIndex(inNames, pcAudioIn1.getCurrentDeviceName()));
            jp.setDeviceList(EmuJackPanel::AUDIO_IN2, inNames,
                             findDropdownIndex(inNames, pcAudioIn2.getCurrentDeviceName()));
        }
#endif

#ifdef USE_MIDI
        // MIDI jack labels — show currently connected ports
        if (midi.isOutputOpen()) {
            RtMidiOut probe;
            for (unsigned int i = 0; i < probe.getPortCount(); i++) {
                jp.setDeviceName(EmuJackPanel::MIDI_OUT, probe.getPortName(i));
                break;
            }
            jp.setConnected(EmuJackPanel::MIDI_OUT, true);
        }
        if (midi.isInputOpen()) {
            RtMidiIn probe;
            for (unsigned int i = 0; i < probe.getPortCount(); i++) {
                jp.setDeviceName(EmuJackPanel::MIDI_IN, probe.getPortName(i));
                break;
            }
            jp.setConnected(EmuJackPanel::MIDI_IN, true);
        }

        // Populate MIDI port lists
        {
            std::vector<std::string> midiOutPorts;
            midiOutPorts.push_back("(None)");
            for (unsigned int i = 0; i < midi.getOutputPortCount(); i++)
                midiOutPorts.push_back(midi.getOutputPortName(i));
            jp.setDeviceList(EmuJackPanel::MIDI_OUT, midiOutPorts, 0);

            std::vector<std::string> midiInPorts;
            midiInPorts.push_back("(None)");
            for (unsigned int i = 0; i < midi.getInputPortCount(); i++)
                midiInPorts.push_back(midi.getInputPortName(i));
            jp.setDeviceList(EmuJackPanel::MIDI_IN, midiInPorts, 0);
        }
#endif

        // Device selection callback — handles all jack types
        jp.setOnDeviceSelected([](int jackId, unsigned int deviceIndex) {
            auto& jp = stm32Emu.getJackPanel();

            switch (jackId) {
#ifdef USE_AUDIO
            case EmuJackPanel::AUDIO_OUT1:
            case EmuJackPanel::AUDIO_OUT2: {
                auto& output = (jackId == EmuJackPanel::AUDIO_OUT1) ? pcAudio : pcAudio2;
                auto jid = static_cast<EmuJackPanel::JackId>(jackId);

                if (deviceIndex == 0) {
                    output.end();
                    jp.setConnected(jid, false);
                    jp.setDeviceName(jid, "");
                    if (jackId == EmuJackPanel::AUDIO_OUT1) s_devicePrefs.audioOut1.clear();
                    else s_devicePrefs.audioOut2.clear();
                } else {
                    auto outDevices = enumerateAudioOutputDevices();
                    unsigned int realIdx = deviceIndex - 1;
                    if (realIdx < outDevices.size()) {
                        output.switchDevice(outDevices[realIdx].rtAudioId);
                        jp.setConnected(jid, output.isOpen());
                        jp.setDeviceName(jid, output.getCurrentDeviceName());
                        if (jackId == EmuJackPanel::AUDIO_OUT1)
                            s_devicePrefs.audioOut1 = output.getCurrentDeviceName();
                        else
                            s_devicePrefs.audioOut2 = output.getCurrentDeviceName();
                    }
                }
                saveDevicePrefs();
                break;
            }

            case EmuJackPanel::AUDIO_IN1:
            case EmuJackPanel::AUDIO_IN2: {
                auto& input = (jackId == EmuJackPanel::AUDIO_IN1) ? pcAudioIn1 : pcAudioIn2;
                auto jid = static_cast<EmuJackPanel::JackId>(jackId);

                if (deviceIndex == 0) {
                    input.end();
                    jp.setConnected(jid, false);
                    jp.setDeviceName(jid, "");
                    if (jackId == EmuJackPanel::AUDIO_IN1) s_devicePrefs.audioIn1.clear();
                    else s_devicePrefs.audioIn2.clear();
                } else {
                    auto inDevices = enumerateAudioInputDevices();
                    unsigned int realIdx = deviceIndex - 1;
                    if (realIdx < inDevices.size()) {
                        input.switchDevice(inDevices[realIdx].rtAudioId);
                        jp.setConnected(jid, input.isOpen());
                        jp.setDeviceName(jid, input.getCurrentDeviceName());
                        if (jackId == EmuJackPanel::AUDIO_IN1)
                            s_devicePrefs.audioIn1 = input.getCurrentDeviceName();
                        else
                            s_devicePrefs.audioIn2 = input.getCurrentDeviceName();
                    }
                }
                saveDevicePrefs();
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
                    unsigned int portIdx = deviceIndex - 1;
                    midi.end();
                    midi.begin(portIdx, 0);
                    jp.setConnected(EmuJackPanel::MIDI_OUT, midi.isOutputOpen());
                    if (midi.isOutputOpen()) {
                        std::string name = midi.getOutputPortName(portIdx);
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
                    unsigned int portIdx = deviceIndex - 1;
                    midi.end();
                    midi.begin(0, portIdx);
                    jp.setConnected(EmuJackPanel::MIDI_IN, midi.isInputOpen());
                    if (midi.isInputOpen()) {
                        std::string name = midi.getInputPortName(portIdx);
                        jp.setDeviceName(EmuJackPanel::MIDI_IN, name);
                        s_devicePrefs.midiIn = name;
                    }
                }
                saveDevicePrefs();
                break;
            }
#endif

            default:
                break;
            }
        });
    }

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

        // IN1 levels → jack panel
        if (pcAudioIn1.isOpen()) {
            int16_t rawL, rawR;
            pcAudioIn1.getInputLevel(rawL, rawR);
            s_jpIn1.update(rawL, rawR);
            jp.setLevel(EmuJackPanel::AUDIO_IN1, s_jpIn1.left(), s_jpIn1.right());
        }

        // IN2 levels → jack panel
        if (pcAudioIn2.isOpen()) {
            int16_t rawL, rawR;
            pcAudioIn2.getInputLevel(rawL, rawR);
            s_jpIn2.update(rawL, rawR);
            jp.setLevel(EmuJackPanel::AUDIO_IN2, s_jpIn2.left(), s_jpIn2.right());
        }
    }, 16, nullptr);
#endif

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

AudioMixerEngine& getMixerEngine()
{
    return s_mixerEngine;
}

void pc_platform_save_mixer_state()
{
    s_mixerEngine.saveState(getMixerStatePath());
}
#else
PcAudioOutput* pc_platform_get_audio_output(int /*index*/) { return nullptr; }
void pc_platform_save_mixer_state() {}
#endif
