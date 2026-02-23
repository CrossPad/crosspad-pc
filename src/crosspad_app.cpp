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

#include "lvgl/lvgl.h"

#include "pc_stubs/pc_platform.h"
#include "pc_stubs/PcApp.hpp"

// crosspad-core
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/pad/PadManager.hpp"

// STM32 hardware emulator window
#include "stm32_emu/Stm32EmuWindow.hpp"

// crosspad-gui
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/components/app_launcher.h"
#include "crosspad-gui/theme/crosspad_theme.h"

#ifdef USE_MIDI
#include "midi/PcMidi.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#endif

#ifdef USE_AUDIO
#include "audio/PcAudio.hpp"
#include "audio/PcAudioInput.hpp"
#include "crosspad-gui/components/vu_meter.h"
#include "synth/MlPianoSynth.hpp"
#include <RtAudio.h>
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define APP_BUTTON_SIZE 64
#define APP_BUTTON_NAME_VISIBLE true

/* ── State ────────────────────────────────────────────────────────────── */

extern lv_obj_t* status_c;

static lv_obj_t* app_c = nullptr;
static lv_obj_t* s_lcdContainer = nullptr;
static App* runningApp = nullptr;
static std::vector<App*> app_shortcuts;
static crosspad_gui::LauncherConfig s_launcher_config;
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

    lv_obj_clean(parent);
    crosspad::getPadManager().setActivePadLogic("");
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    status_c = lv_CreateStatusBar(parent);
    if (!status_c) {
        printf("[ERROR] Status bar creation failed!\n");
    }

    app_c = lv_obj_create(parent);
    lv_obj_add_style(app_c, &styleAppContainer, 0);
    lv_obj_set_flex_grow(app_c, 1);
    lv_obj_update_layout(parent);
    lv_obj_set_width(app_c, lv_obj_get_content_width(parent));
    lv_obj_set_style_bg_color(app_c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(app_c, LV_OPA_COVER, 0);
    lv_obj_remove_flag(app_c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(app_c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_clip_corner(app_c, true, 0);
    lv_obj_update_layout(app_c);

    std::vector<crosspad_gui::LauncherAppInfo> infos;
    for (App* a : app_shortcuts) {
        if (a) infos.push_back({a->getName(), a->getIcon(), a});
    }

    s_launcher_config = {};
    s_launcher_config.encoder_group = nullptr;
    s_launcher_config.on_select = on_app_selected;
    s_launcher_config.on_close = on_popup_close;
    s_launcher_config.button_size = APP_BUTTON_SIZE;
    s_launcher_config.button_spacing = 10;
    s_launcher_config.show_names = APP_BUTTON_NAME_VISIBLE;

    crosspad_gui::launcher_create_button_grid(
        app_c, infos.data(), infos.size(), &s_launcher_config);

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

    /* STM32 emulator window — returns 320x240 LCD container */
    lv_obj_t* lcdContainer = stm32Emu.init();
    s_lcdContainer = lcdContainer;

    /* Overlay layer on lv_layer_top(), positioned over the LCD area.
     * Using layer_top avoids the LCD container's flex layout that breaks
     * LV_ALIGN_CENTER for overlays. */
    lv_obj_t* overlayLayer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlayLayer);
    lv_obj_set_pos(overlayLayer, (Stm32EmuWindow::WIN_W - 320) / 2, 20);
    lv_obj_set_size(overlayLayer, 320, 240);
    lv_obj_remove_flag(overlayLayer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    crosspad_gui::setOverlayParent(overlayLayer);

#ifdef USE_MIDI
    // Initialize STM32 message handler — routes real CrossPad SysEx to PadManager
    stm32Handler.init(crosspad::getPadManager(), status);

    // Auto-connect: scans MIDI port names for "CrossPad", falls back to port 0
    midi.beginAutoConnect("CrossPad");
    pc_platform_set_midi_output(&midi);

    midi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        auto& pm = crosspad::getPadManager();
        uint8_t padIdx = pm.getPadForMidiNote(note);
        if (padIdx < 16) {
            // Route through handlePadPress so active PadLogic (e.g. PianoPadLogic) processes it
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
        // CC1 on ch1 = physical encoder position (0-30)
        if (channel == 0 && cc == 1) {
            stm32Emu.handleEncoderCC(value, 31, 18);  // CC range 0-30 (31 vals), 18 detents/rev
        } else if (channel == 0 && cc == 64) {
            stm32Emu.handleEncoderPress(value >= 64);  // CC64 = encoder button
        }
    });
    // Route SysEx from real CrossPad hardware → Stm32MessageHandler → PadManager
    midi.setHandleSystemExclusive([](uint8_t* data, unsigned size) {
        stm32Handler.handleMessage(data, size);
    });

    // Periodic MIDI reconnect timer: every 3 s tries to re-find "CrossPad" port.
    // Also reconnects when output port is down (e.g. after a send error) even if
    // the input port is still technically open (WinMM doesn't report disconnection).
    lv_timer_create([](lv_timer_t*) {
        bool connected = midi.isKeywordConnected() && midi.isOutputOpen();
        if (!connected) {
            printf("[MIDI] CrossPad not found or output down — attempting reconnect...\n");
            midi.reconnect();
        }
    }, 3000, nullptr);
#endif

#ifdef USE_AUDIO
    pcAudio.begin();
    pc_platform_set_audio_output(&pcAudio);

    // OUT2 — second independent output (starts on default device, no routing yet)
    pcAudio2.begin();
    pc_platform_set_audio_output_2(&pcAudio2);

    // IN1 + IN2 — audio inputs (not started yet; user selects device via jack panel)
    pc_platform_set_audio_input(0, &pcAudioIn1);
    pc_platform_set_audio_input(1, &pcAudioIn2);

    // Initialize FM synth engine at the audio device's actual sample rate
    fmSynth.setSampleRate(pcAudio.getSampleRate());
    fmSynth.init();
    pc_platform_set_synth_engine(&fmSynth);

    // Audio thread: FM synth -> ring buffer -> RtAudio WASAPI
    std::thread([]() {
        const uint32_t chunk = 256;
        std::vector<int16_t> buf(chunk * 2);

        printf("[Audio] FM synth audio thread started (sr=%u)\n", pcAudio.getSampleRate());
        fflush(stdout);

        while (true) {
            fmSynth.process(buf.data(), chunk);

            uint32_t offset = 0;
            uint32_t remaining = chunk;
            while (remaining > 0) {
                uint32_t written = pcAudio.write(buf.data() + offset * 2, remaining);
                offset += written;
                remaining -= written;
                if (remaining > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    }).detach();
#endif



    /* Register ML Piano app */
#ifdef USE_AUDIO
    {
        extern void _register_MLPiano_app();
        _register_MLPiano_app();
    }
#endif

    /* Styles, apps, main screen */
    initStyles();
    InitializeApps();
    LoadMainScreen(lcdContainer);

    /* ── Jack panel wiring ────────────────────────────────────────────── */
    {
        auto& jp = stm32Emu.getJackPanel();

#ifdef USE_AUDIO
        // Audio OUT labels (read-only)
        jp.setDeviceName(EmuJackPanel::AUDIO_OUT1, pcAudio.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_OUT1, pcAudio.isOpen());

        jp.setDeviceName(EmuJackPanel::AUDIO_OUT2, pcAudio2.getCurrentDeviceName());
        jp.setConnected(EmuJackPanel::AUDIO_OUT2, pcAudio2.isOpen());

        // Enumerate audio input devices for IN1/IN2 dropdowns
        {
            RtAudio probe;
            std::vector<std::string> inputDevices;
            inputDevices.push_back("(None)");
            for (unsigned int id : probe.getDeviceIds()) {
                RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
                if (info.inputChannels >= 2) {
                    inputDevices.push_back(info.name);
                }
            }
            jp.setDeviceList(EmuJackPanel::AUDIO_IN1, inputDevices, 0);
            jp.setDeviceList(EmuJackPanel::AUDIO_IN2, inputDevices, 0);
        }
#endif

#ifdef USE_MIDI
        // MIDI jack labels
        if (midi.isOutputOpen()) {
            // Find the currently connected output port name
            RtMidiOut probe;
            for (unsigned int i = 0; i < probe.getPortCount(); i++) {
                // Use the first port that was opened (port 0 or auto-connected)
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

        // Device selection callback
        jp.setOnDeviceSelected([](int jackId, unsigned int deviceIndex) {
            auto& jp = stm32Emu.getJackPanel();

            switch (jackId) {
#ifdef USE_AUDIO
            case EmuJackPanel::AUDIO_IN1:
            case EmuJackPanel::AUDIO_IN2: {
                auto& input = (jackId == EmuJackPanel::AUDIO_IN1) ? pcAudioIn1 : pcAudioIn2;
                auto jid = static_cast<EmuJackPanel::JackId>(jackId);

                if (deviceIndex == 0) {
                    // "(None)" — disconnect
                    input.end();
                    jp.setConnected(jid, false);
                    jp.setDeviceName(jid, "");
                } else {
                    // Map dropdown index to actual RtAudio device ID
                    RtAudio probe;
                    std::vector<unsigned int> inputIds;
                    for (unsigned int id : probe.getDeviceIds()) {
                        RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
                        if (info.inputChannels >= 2)
                            inputIds.push_back(id);
                    }
                    unsigned int realIdx = deviceIndex - 1;
                    if (realIdx < inputIds.size()) {
                        input.switchDevice(inputIds[realIdx]);
                        jp.setConnected(jid, input.isOpen());
                        jp.setDeviceName(jid, input.getCurrentDeviceName());
                    }
                }
                break;
            }
#endif

#ifdef USE_MIDI
            case EmuJackPanel::MIDI_OUT: {
                if (deviceIndex == 0) {
                    // Disconnect output (keep input as-is via reconnect with invalid keyword)
                    printf("[JackPanel] MIDI OUT disconnected\n");
                    jp.setConnected(EmuJackPanel::MIDI_OUT, false);
                    jp.setDeviceName(EmuJackPanel::MIDI_OUT, "");
                } else {
                    unsigned int portIdx = deviceIndex - 1;
                    midi.end();
                    midi.begin(portIdx, 0);  // reconnect output on new port, keep input port 0
                    jp.setConnected(EmuJackPanel::MIDI_OUT, midi.isOutputOpen());
                    if (midi.isOutputOpen())
                        jp.setDeviceName(EmuJackPanel::MIDI_OUT, midi.getOutputPortName(portIdx));
                }
                break;
            }
            case EmuJackPanel::MIDI_IN: {
                if (deviceIndex == 0) {
                    printf("[JackPanel] MIDI IN disconnected\n");
                    jp.setConnected(EmuJackPanel::MIDI_IN, false);
                    jp.setDeviceName(EmuJackPanel::MIDI_IN, "");
                } else {
                    unsigned int portIdx = deviceIndex - 1;
                    midi.end();
                    midi.begin(0, portIdx);  // keep output port 0, reconnect input on new port
                    jp.setConnected(EmuJackPanel::MIDI_IN, midi.isInputOpen());
                    if (midi.isInputOpen())
                        jp.setDeviceName(EmuJackPanel::MIDI_IN, midi.getInputPortName(portIdx));
                }
                break;
            }
#endif

            default:
                break;
            }
        });
    }

#ifdef USE_AUDIO
    static int16_t s_vuDecayL = 0, s_vuDecayR = 0;
    lv_timer_create([](lv_timer_t* t) {
        auto* a = static_cast<PcAudioOutput*>(lv_timer_get_user_data(t));
        int16_t rawL, rawR;
        a->getOutputLevel(rawL, rawR);
        s_vuDecayL = static_cast<int16_t>((s_vuDecayL * 238) / 256);
        s_vuDecayR = static_cast<int16_t>((s_vuDecayR * 238) / 256);
        if (rawL > s_vuDecayL) s_vuDecayL = rawL;
        if (rawR > s_vuDecayR) s_vuDecayR = rawR;
        crosspad_gui::vu_set_levels(s_vuDecayL, s_vuDecayR);
    }, 16, &pcAudio);
#endif

    printf("[CrossPad] App init complete\n");
}

void crosspad_app_go_home()
{
    LoadMainScreen(s_lcdContainer);
}
