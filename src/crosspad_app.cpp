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

// crosspad-gui
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/components/app_launcher.h"
#include "crosspad-gui/components/main_screen.h"
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/components/volume_overlay.h"

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
#include "crosspad-gui/components/vu_meter.h"
#include "crosspad/audio/PeakMeter.hpp"
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
#ifdef HAS_MIXER
static AudioMixerEngine s_mixerEngine;
static std::shared_ptr<MixerPadLogic> s_mixerPadLogic;
#endif
#endif

/* ── Virtual USB/UART ─────────────────────────────────────────────────── */

static PcUart pcUart;

/* ── Device Preferences ──────────────────────────────────────────────── */

struct DevicePreferences {
    std::string audioOut1;
    std::string audioOut2;
    std::string audioIn1;
    std::string audioIn2;
    std::string midiOut;
    std::string midiIn;
    std::string sdcardPath;
    std::string uartPort;
    uint32_t    uartBaud = 115200;
    std::string bleDevice;    // Last connected BLE MIDI device address
    uint8_t     bleMode = 0;  // 0=Host, 1=Server
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
    if (doc["sdcard_path"].is<const char*>()) s_devicePrefs.sdcardPath = doc["sdcard_path"].as<const char*>();
    if (doc["uart_port"].is<const char*>())  s_devicePrefs.uartPort   = doc["uart_port"].as<const char*>();
    if (doc["uart_baud"].is<uint32_t>())     s_devicePrefs.uartBaud   = doc["uart_baud"].as<uint32_t>();
    if (doc["ble_device"].is<const char*>()) s_devicePrefs.bleDevice  = doc["ble_device"].as<const char*>();
    if (doc["ble_mode"].is<uint8_t>())       s_devicePrefs.bleMode    = doc["ble_mode"].as<uint8_t>();

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
    doc["audio_in1"]  = s_devicePrefs.audioIn1;
    doc["audio_in2"]  = s_devicePrefs.audioIn2;
    doc["midi_out"]    = s_devicePrefs.midiOut;
    doc["midi_in"]     = s_devicePrefs.midiIn;
    doc["sdcard_path"] = s_devicePrefs.sdcardPath;
    doc["uart_port"]   = s_devicePrefs.uartPort;
    doc["uart_baud"]   = s_devicePrefs.uartBaud;
    doc["ble_device"]  = s_devicePrefs.bleDevice;
    doc["ble_mode"]    = s_devicePrefs.bleMode;

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

/* ── App Orchestrator setup ───────────────────────────────────────────── */

static void LoadMainScreen(lv_obj_t* parent);

/// PC app factory — creates App instances for the orchestrator
static crosspad_gui::ILvglApp* pc_app_factory(
    lv_obj_t* container, const char* name, const char* icon,
    lv_obj_t* (*createLVGL)(lv_obj_t*, App*),
    void (*destroyLVGL)(lv_obj_t*))
{
    return new App(container, name, icon, createLVGL, destroyLVGL);
}

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
    config.app_factory = pc_app_factory;
    config.icon_resolver = pc_icon_resolver;
    crosspad_gui::AppOrchestrator::getInstance().init(config);
}

static void LoadMainScreen(lv_obj_t* parent) {
    if (parent == nullptr) parent = lv_screen_active();

    crosspad::getPadManager().setActivePadLogic("Mixer");

    auto& orch = crosspad_gui::AppOrchestrator::getInstance();

    crosspad_gui::MainScreenConfig config;
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
    /* Platform stubs (event bus, settings, pad manager, GUI) */
    pc_platform_init();

    /* Register go-home callback for shared Settings UI */
    pc_platform_set_go_home([]() { LoadMainScreen(s_lcdContainer); });

    /* Load saved device preferences */
    loadDevicePrefs();

    /* STM32 emulator window — returns 320x240 LCD container */
    lv_obj_t* lcdContainer = stm32Emu.init();
    s_lcdContainer = lcdContainer;

    /* Wire keyboard shortcuts: Escape→go home, Space/Ctrl→volume overlay */
    stm32Emu.getKeyboardCapture().setEscapeCallback(crosspad_app_go_home);
    stm32Emu.getKeyboardCapture().setPowerCallback(crosspad_gui::volume_overlay_toggle);

    /* Virtual SD card slot — auto-mount from saved preferences */
    if (!s_devicePrefs.sdcardPath.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::exists(s_devicePrefs.sdcardPath, ec)) {
            pc_platform_set_sdcard_path(s_devicePrefs.sdcardPath);
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
        s_devicePrefs.sdcardPath = path;
        saveDevicePrefs();
    });
    stm32Emu.getSdCardSlot().setOnUnmount([]() {
        pc_platform_set_sdcard_path("");
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
    // ── Audio OUT1: auto-connect saved device or default ──
    {
        auto outDevices = enumerateAudioOutputDevices();
        unsigned int dev1 = findDeviceByName(outDevices, s_devicePrefs.audioOut1);
        pcAudio.begin(dev1);
        crosspad::getPlatformServices().setAudioOutput(&pcAudio);

        // Save actual device name if we connected
        if (pcAudio.isOpen()) {
            s_devicePrefs.audioOut1 = pcAudio.getCurrentDeviceName();
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
    // Use output sample rate so mixer doesn't need sample rate conversion
    uint32_t outSampleRate = pcAudio.isOpen() ? pcAudio.getSampleRate() : 44100;
    pc_platform_set_audio_input(0, &pcAudioIn1);
    pc_platform_set_audio_input(1, &pcAudioIn2);

    {
        auto inDevices = enumerateAudioInputDevices();
        if (!s_devicePrefs.audioIn1.empty()) {
            unsigned int devId = findDeviceByName(inDevices, s_devicePrefs.audioIn1);
            if (devId != 0) {
                pcAudioIn1.begin(devId, outSampleRate);
                if (pcAudioIn1.isOpen()) {
                    printf("[Audio] IN1 auto-connected: %s @ %u Hz\n",
                           pcAudioIn1.getCurrentDeviceName().c_str(), pcAudioIn1.getSampleRate());
                    crosspad::getPlatformServices().setAudioInput(&pcAudioIn1);
                }
            }
        }
        if (!s_devicePrefs.audioIn2.empty()) {
            unsigned int devId = findDeviceByName(inDevices, s_devicePrefs.audioIn2);
            if (devId != 0) {
                pcAudioIn2.begin(devId, outSampleRate);
                if (pcAudioIn2.isOpen()) {
                    printf("[Audio] IN2 auto-connected: %s @ %u Hz\n",
                           pcAudioIn2.getCurrentDeviceName().c_str(), pcAudioIn2.getSampleRate());
                }
            }
        }
    }

    // Initialize FM synth engine at the audio device's actual sample rate
    fmSynth.setSampleRate(pcAudio.getSampleRate());
    fmSynth.init();
    crosspad::getPlatformServices().setSynthEngine(&fmSynth);

#ifdef HAS_MIXER
    // Load mixer state from preferences (or set defaults)
    s_mixerEngine.loadState(getMixerStatePath());

    // Register mixer pad logic globally (always available)
    s_mixerPadLogic = std::make_shared<MixerPadLogic>(s_mixerEngine);
    s_mixerPadLogic->setOnStateChanged([]() {
        s_mixerEngine.saveState(getMixerStatePath());
    });
    crosspad::getPadManager().registerPadLogic("Mixer", s_mixerPadLogic);
    crosspad::getPadManager().setActivePadLogic("Mixer");

    // Start the mixer engine
    s_mixerEngine.start();
#endif

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
