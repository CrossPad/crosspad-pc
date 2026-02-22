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

#ifdef USE_MIDI
#include "midi/PcMidi.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#endif

#ifdef USE_AUDIO
#include "audio/PcAudio.hpp"
#include "crosspad-gui/components/vu_meter.h"
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define APP_BUTTON_SIZE 64
#define APP_BUTTON_NAME_VISIBLE true

/* ── State ────────────────────────────────────────────────────────────── */

extern lv_obj_t* status_c;

static lv_obj_t* app_c = nullptr;
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
static PcAudioOutput pcAudio;
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

    crosspad::getPadManager().setActivePadLogic("");

    if (runningApp != nullptr && runningApp->isStarted()) {
        runningApp->destroyApp();
        runningApp = nullptr;
    }

    lv_obj_clean(parent);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);
    lv_obj_set_style_pad_column(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* Status bar is now a normal flex child (overlay parent = lcdContainer).
       Reset alignment from TOP_MID (set inside lv_CreateStatusBar) to DEFAULT
       so the flex column layout controls its position instead of ignoring it. */
    status_c = lv_CreateStatusBar();
    lv_obj_set_style_align(status_c, LV_ALIGN_DEFAULT, 0);

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

#ifdef USE_MIDI
    // Initialize STM32 message handler — routes real CrossPad SysEx to PadManager
    stm32Handler.init(crosspad::getPadManager(), status);

    // Auto-connect: scans MIDI port names for "CrossPad", falls back to port 0
    midi.beginAutoConnect("CrossPad");
    pc_platform_set_midi_output(&midi);

    midi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        crosspad::getPadManager().handleMidiNoteOn(channel, note, velocity);
    });
    midi.setHandleNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOff ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
        crosspad::getPadManager().handleMidiNoteOff(channel, note);
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
#endif

#ifdef USE_AUDIO
    pcAudio.begin();
    pc_platform_set_audio_output(&pcAudio);

    std::thread([]() {
        const double freq = 440.0;
        const double amp  = 8192.0;
        const uint32_t sr = pcAudio.getSampleRate();
        const uint32_t chunk = 256;
        std::vector<int16_t> buf(chunk * 2);
        double phase = 0.0;
        const double twoPi = 6.283185307179586;
        const double inc   = twoPi * freq / sr;

        printf("[Audio] Sine test tone: %.0f Hz, sr=%u\n", freq, sr);
        fflush(stdout);

        while (true) {
            for (uint32_t i = 0; i < chunk; i++) {
                auto s = static_cast<int16_t>(amp * std::sin(phase));
                buf[i * 2]     = s;
                buf[i * 2 + 1] = s;
                phase += inc;
                if (phase >= twoPi) phase -= twoPi;
            }
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

    /* Use the LCD container as overlay parent so the status bar renders
       on the normal working layer instead of lv_layer_top() (system layer),
       which has different dimensions on the PC build. */
    crosspad_gui::setOverlayParent(lcdContainer);

    /* Styles, apps, main screen */
    initStyles();
    InitializeApps();
    LoadMainScreen(lcdContainer);

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
