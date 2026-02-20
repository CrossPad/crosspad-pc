/**
 * @file main.cpp
 * @brief CrossPad PC — desktop simulator with full launcher GUI
 *
 * Initializes LVGL + SDL, platform stubs, styles, status bar and
 * the app launcher grid from crosspad-gui. Reuses the same GUI
 * components as the ESP-IDF firmware.
 */

#ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE /* needed for usleep() */
#endif

#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cstring>
#include <cmath>
#include <thread>
#include <chrono>

#include "lvgl/lvgl.h"
#include <SDL.h>

#include "hal/hal.h"
#include "pc_stubs/pc_platform.h"
#include "pc_stubs/PcApp.hpp"

// crosspad-core
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/pad/PadManager.hpp"

// crosspad-gui
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/components/app_launcher.h"

// Windows.h AFTER crosspad headers (its ERROR macro conflicts with AnimType::ERROR)
#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
  #include <pthread.h>
#endif

#ifdef USE_MIDI
#include "midi/PcMidi.hpp"
#endif

#ifdef USE_AUDIO
#include "audio/PcAudio.hpp"
#include "crosspad-gui/components/vu_meter.h"
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define APP_BUTTON_SIZE 64
#define APP_BUTTON_NAME_VISIBLE true

/* ── State ────────────────────────────────────────────────────────────── */

// Global required by crosspad-gui (declared extern in status_bar.cpp)
extern lv_obj_t* status_c;

static lv_obj_t* app_c = nullptr;
static App* runningApp = nullptr;
static std::vector<App*> app_shortcuts;
static crosspad_gui::LauncherConfig s_launcher_config;

#ifdef USE_MIDI
static PcMidi midi;
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

    // Destroy currently running app
    if (runningApp != nullptr && runningApp->isStarted()) {
        runningApp->destroyApp();
        runningApp = nullptr;
    }

    // Setup parent container
    lv_obj_clean(parent);
    lv_obj_set_size(parent, LV_HOR_RES, LV_VER_RES);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);
    lv_obj_set_style_pad_column(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);

    // Status bar placeholder (height reservation)
    lv_obj_t* fill_cont = lv_obj_create(parent);
    lv_obj_set_size(fill_cont, lv_obj_get_content_width(parent), LV_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_opa(fill_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fill_cont, 0, 0);
    lv_obj_set_style_pad_all(fill_cont, 0, 0);
    status_c = lv_CreateStatusBar();

    // App container
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

    // Build LauncherAppInfo array from app_shortcuts
    std::vector<crosspad_gui::LauncherAppInfo> infos;
    for (App* a : app_shortcuts) {
        if (a) infos.push_back({a->getName(), a->getIcon(), a});
    }

    // Configure and create the shared button grid
    s_launcher_config = {};
    s_launcher_config.encoder_group = nullptr; // no encoder on PC
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
    // Register Power OFF in AppRegistry
    crosspad_gui::launcher_register_power_off();

    // Create App instances from registry
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

/* ── Main ─────────────────────────────────────────────────────────────── */

#if LV_USE_OS != LV_OS_FREERTOS

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    /* Initialize LVGL */
    lv_init();

    /* Initialize the HAL (display, input devices, tick) for LVGL */
    sdl_hal_init(320, 480);

    /* Initialize CrossPad platform stubs (event bus, settings, pad manager, GUI) */
    pc_platform_init();

#ifdef USE_MIDI
    midi.begin();
    pc_platform_set_midi_output(&midi);

    midi.setHandleNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOn  ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
    });

    midi.setHandleNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
        printf("[MIDI IN] NoteOff ch=%u note=%u vel=%u\n", channel + 1, note, velocity);
    });

    midi.setHandleControlChange([](uint8_t channel, uint8_t cc, uint8_t value) {
        printf("[MIDI IN] CC      ch=%u cc=%u  val=%u\n", channel + 1, cc, value);
    });
#endif

#ifdef USE_AUDIO
    pcAudio.begin();
    pc_platform_set_audio_output(&pcAudio);

    // Sine wave test tone — 440 Hz, ~25% volume, runs in background thread
    std::thread([]() {
        const double freq = 440.0;
        const double amp  = 8192.0;  // ~25% of INT16_MAX
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
                buf[i * 2]     = s;  // left
                buf[i * 2 + 1] = s;  // right
                phase += inc;
                if (phase >= twoPi) phase -= twoPi;
            }
            // Write all frames — retry remainder so no samples are lost
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

    /* Initialize styles and apps */
    initStyles();
    InitializeApps();

    /* Load the main screen (status bar + launcher) */
    LoadMainScreen(lv_screen_active());

#ifdef USE_AUDIO
    /* VU meter update timer (~60 Hz) */
    static int16_t s_vuDecayL = 0, s_vuDecayR = 0;
    lv_timer_create([](lv_timer_t* t) {
        auto* a = static_cast<PcAudioOutput*>(lv_timer_get_user_data(t));
        int16_t rawL, rawR;
        a->getOutputLevel(rawL, rawR);
        // Decay (same constant as ESP32: 238/256)
        s_vuDecayL = static_cast<int16_t>((s_vuDecayL * 238) / 256);
        s_vuDecayR = static_cast<int16_t>((s_vuDecayR * 238) / 256);
        if (rawL > s_vuDecayL) s_vuDecayL = rawL;
        if (rawR > s_vuDecayR) s_vuDecayR = rawR;
        crosspad_gui::vu_set_levels(s_vuDecayL, s_vuDecayR);
    }, 16, &pcAudio);
#endif

    /* Main loop */
    while (1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
#ifdef _MSC_VER
        Sleep(sleep_time_ms);
#else
        usleep(sleep_time_ms * 1000);
#endif
    }

    return 0;
}

#endif
