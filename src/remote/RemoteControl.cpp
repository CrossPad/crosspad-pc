/**
 * @file RemoteControl.cpp
 * @brief TCP server for external simulator control (screenshot, input injection).
 */

#include "RemoteControl.hpp"
#include "ControlJson.hpp"
#include "JsonText.hpp"
#include "crosspad/control/PadInput.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

// SDL2 for framebuffer capture and event injection
#include <SDL2/SDL.h>

// stb_image_write for PNG encoding (single-header, public domain)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

// LVGL SDL driver internals — window/renderer access; object class names for
// the click hit report
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"
#include "lvgl/src/core/lv_obj_class_private.h"

// Where the LCD sits inside the window — one copy, shared with the layout
#include "stm32_emu/Stm32EmuWindow.hpp"

// crosspad-core
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"   // getPadLedController() — led_state
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/audio/IAudioModule.hpp"
#include "crosspad/synth/ISynthEngine.hpp"
#include "apps/citest/CITestApi.hpp"
#if __has_include("crosspad-mixer/AudioMixerEngine.hpp")
#include "crosspad-mixer/AudioMixerEngine.hpp"   // getMixerEngine() — mixer app present
#define REMOTE_HAS_MIXER 1
#endif
#if __has_include("audio/sampler/PcPitchedPort.hpp")
#include "audio/sampler/PcPitchedPort.hpp"
#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/instrument/SampleBank.hpp>
#define REMOTE_HAS_PITCHED 1
#endif
#if __has_include(<crosspad-sampler/waveform/waveform_loader.hpp>)
#include <crosspad-sampler/waveform/waveform_loader.hpp>
#define REMOTE_HAS_WAVE 1
#endif
#if __has_include("audio/sampler/SampleStreamPlayer.hpp")
#include "audio/sampler/SampleStreamPlayer.hpp"
#define REMOTE_HAS_SMPL 1
#endif
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/components/touch_gesture.h"
#include "crosspad-gui/components/transport_bar.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/components/app_orchestrator.h"   // AppOrchestrator — app_list
#include "crosspad_app.hpp"                              // crosspad_app_start() — app_start

// PC platform
#include "pc_stubs/pc_platform.h"
#include "crosspad-gui/platform/IGuiPlatform.h"

// Winsock for TCP server
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET socket_t;
#  define CLOSE_SOCKET closesocket
#  define SOCKET_INVALID INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
   typedef int socket_t;
#  define CLOSE_SOCKET close
#  define SOCKET_INVALID (-1)
#endif

using namespace remote::json;

/* ── Base64 encoder ──────────────────────────────────────────────────── */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out.push_back(b64_table[(n >> 18) & 0x3F]);
        out.push_back(b64_table[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? b64_table[n & 0x3F] : '=');
    }
    return out;
}

/* ── Simple JSON readers (the writers are in JsonText.hpp) ──────────── */

/// Extract a string value for a key from a JSON-like string (very simple parser)
static std::string json_get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

/// Extract an int value for a key from a JSON-like string
static int json_get_int(const std::string& json, const std::string& key, int dflt = 0) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return dflt;
    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    try { return std::stoi(json.substr(pos)); } catch (...) { return dflt; }
}

/* ── PNG writer (via stb_image_write) ─────────────────────────────────── */

static void stbi_write_cb(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// Convert BGRA pixels (SDL_PIXELFORMAT_ARGB8888 on little-endian) to PNG.
static std::vector<uint8_t> pixels_to_png(const uint8_t* bgra, int w, int h) {
    // Convert BGRA → RGB for stb_image_write (which expects RGB)
    std::vector<uint8_t> rgb(w * h * 3);
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2]; // R
        rgb[i * 3 + 1] = bgra[i * 4 + 1]; // G
        rgb[i * 3 + 2] = bgra[i * 4 + 0]; // B
    }

    std::vector<uint8_t> png;
    png.reserve(w * h); // rough estimate
    stbi_write_png_to_func(stbi_write_cb, &png, w, h, 3, rgb.data(), w * 3);
    return png;
}

/* ── State ───────────────────────────────────────────────────────────── */

static constexpr uint16_t PORT = 19840;

static lv_display_t* s_disp = nullptr;
static std::thread s_serverThread;
static std::atomic<bool> s_running{false};
static socket_t s_listenSocket = SOCKET_INVALID;

// Command queue: commands are received on TCP thread, executed on LVGL thread
struct PendingCommand {
    std::string request;
    std::function<void(const std::string&)> respond;
};

static std::mutex s_queueMutex;
static std::queue<PendingCommand> s_commandQueue;

// Response storage for synchronous command processing
static std::mutex s_responseMutex;
static std::string s_pendingResponse;
static bool s_responseReady = false;

/* ── Command handlers (run on LVGL thread) ───────────────────────────── */

static std::string handle_screenshot(const std::string& json = "") {
    if (!s_disp) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "no display") + "}";
    }

    SDL_Window* window = lv_sdl_window_get_window(s_disp);
    SDL_Renderer* renderer = (SDL_Renderer*)lv_sdl_window_get_renderer(s_disp);
    if (!window || !renderer) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "no SDL window/renderer") + "}";
    }

    // Check if "lcd_only" region requested
    std::string region = json_get_string(json, "region");
    bool lcdOnly = (region == "lcd");

    int w, h;
    SDL_Rect* captureRect = nullptr;
    SDL_Rect lcdRect;

    // The capture is in window pixels; the layout is in LVGL units. They differ
    // by the SDL zoom (HiDPI), so the panel rectangle is scaled, and the reply
    // says where the panel is in the returned image so a caller can convert a
    // pixel it sees into an LCD coordinate without knowing the layout.
    const float zoom = lv_sdl_window_get_zoom(s_disp);
    lcdRect = { (int)(Stm32EmuWindow::LCD_X * zoom), (int)(Stm32EmuWindow::LCD_Y * zoom),
                (int)(Stm32EmuWindow::LCD_W * zoom), (int)(Stm32EmuWindow::LCD_H * zoom) };

    if (lcdOnly) {
        captureRect = &lcdRect;
        w = lcdRect.w;
        h = lcdRect.h;
    } else {
        SDL_GetWindowSize(window, &w, &h);
    }

    const std::string geometry =
        "\"lcd_origin\":[" + std::to_string(lcdOnly ? 0 : lcdRect.x) + "," +
                            std::to_string(lcdOnly ? 0 : lcdRect.y) + "]," +
        "\"lcd_size\":[" + std::to_string(lcdRect.w) + "," + std::to_string(lcdRect.h) + "]," +
        json_float("scale", zoom);

    // ARGB8888: on little-endian memory layout is [B, G, R, A] per pixel.
    std::vector<uint8_t> pixels(w * h * 4);
    if (SDL_RenderReadPixels(renderer, captureRect, SDL_PIXELFORMAT_ARGB8888,
                             pixels.data(), w * 4) != 0) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", SDL_GetError()) + "}";
    }

    // Encode to PNG
    auto png = pixels_to_png(pixels.data(), w, h);

    // Check if caller wants to save to file
    std::string filePath = json_get_string(json, "file");
    if (!filePath.empty()) {
        FILE* f = fopen(filePath.c_str(), "wb");
        if (f) {
            fwrite(png.data(), 1, png.size(), f);
            fclose(f);
            return "{" + json_bool("ok", true) + "," +
                   json_int("width", w) + "," +
                   json_int("height", h) + "," +
                   json_string("format", "png") + "," +
                   json_string("file", filePath) + "," +
                   json_int("size", (int)png.size()) + "," +
                   geometry + "}";
        } else {
            return "{" + json_bool("ok", false) + "," +
                   json_string("error", "cannot write file: " + filePath) + "}";
        }
    }

    // Return inline base64 PNG (much smaller than BMP)
    std::string b64 = base64_encode(png.data(), png.size());

    return "{" + json_bool("ok", true) + "," +
           json_int("width", w) + "," +
           json_int("height", h) + "," +
           json_string("format", "png") + "," +
           json_string("encoding", "base64") + "," +
           geometry + "," +
           json_string("data", b64) + "}";
}

/// Deferred half of a click: the button-up pushed once hold_ms have passed.
/// The mouse indev is polled every ~30 ms and only samples the button state,
/// so a down and an up pushed back to back are never seen as a press at all.
static void click_release_cb(lv_timer_t* t) {
    auto* ev = static_cast<SDL_Event*>(lv_timer_get_user_data(t));
    SDL_PushEvent(ev);
    delete ev;
}

static std::string handle_click(const std::string& json) {
    int x = json_get_int(json, "x", -1);
    int y = json_get_int(json, "y", -1);
    if (x < 0 || y < 0) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "missing x/y") + "}";
    }

    // Which space the caller measured in. "lcd" is what screenshots of the
    // panel, ENC_GROUP labels and the UI code use; "window" is the raw SDL
    // window (the old behaviour, and the default on the wire so older
    // clients keep clicking where they always did).
    std::string space = json_get_string(json, "space");
    if (space.empty()) space = "window";
    if (space != "lcd" && space != "window") {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "space must be lcd or window") + "}";
    }

    int holdMs = json_get_int(json, "hold_ms", 120);
    if (holdMs < 0) holdMs = 0;
    if (holdMs > 5000) holdMs = 5000;

    // LVGL coordinates are the layout's; SDL event coordinates are those times
    // the window zoom, which is what the mouse driver in hal.c divides by.
    const float zoom = lv_sdl_window_get_zoom(s_disp);
    int lx, ly;
    if (space == "lcd") {
        lx = x + Stm32EmuWindow::LCD_X;
        ly = y + Stm32EmuWindow::LCD_Y;
    } else {
        lx = (int)(x / zoom);
        ly = (int)(y / zoom);
    }
    const int wx = (int)(lx * zoom);
    const int wy = (int)(ly * zoom);
    const int lcdX = lx - Stm32EmuWindow::LCD_X;
    const int lcdY = ly - Stm32EmuWindow::LCD_Y;
    const bool inLcd = lcdX >= 0 && lcdX < Stm32EmuWindow::LCD_W &&
                       lcdY >= 0 && lcdY < Stm32EmuWindow::LCD_H;

    // What LVGL will deliver the press to — the same search the indev does —
    // so a click on empty background is distinguishable from one on a widget.
    lv_point_t p = { (lv_coord_t)lx, (lv_coord_t)ly };
    lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &p);
    std::string hitJson = "null";
    if (hit) {
        const lv_obj_class_t* cls = lv_obj_get_class(hit);
        lv_area_t a;
        lv_obj_get_coords(hit, &a);
        hitJson = "{" + json_string("class", (cls && cls->name) ? cls->name : "?") + "," +
                  json_int("x", a.x1 - Stm32EmuWindow::LCD_X) + "," +
                  json_int("y", a.y1 - Stm32EmuWindow::LCD_Y) + "," +
                  json_int("w", lv_area_get_width(&a)) + "," +
                  json_int("h", lv_area_get_height(&a)) + "}";
    }

    SDL_Window* window = lv_sdl_window_get_window(s_disp);
    Uint32 windowId = SDL_GetWindowID(window);

    SDL_Event ev = {};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = windowId;
    ev.motion.x = wx;
    ev.motion.y = wy;
    SDL_PushEvent(&ev);

    ev = {};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_PRESSED;
    ev.button.x = wx;
    ev.button.y = wy;
    ev.button.clicks = 1;
    SDL_PushEvent(&ev);

    auto* up = new SDL_Event{};
    up->type = SDL_MOUSEBUTTONUP;
    up->button.windowID = windowId;
    up->button.button = SDL_BUTTON_LEFT;
    up->button.state = SDL_RELEASED;
    up->button.x = wx;
    up->button.y = wy;
    up->button.clicks = 1;
    if (holdMs == 0) {
        SDL_PushEvent(up);
        delete up;
    } else {
        // We are on the LVGL thread (process_pending), so a timer is safe here.
        lv_timer_t* t = lv_timer_create(click_release_cb, (uint32_t)holdMs, up);
        lv_timer_set_repeat_count(t, 1);
    }

    return "{" + json_bool("ok", true) + "," +
           json_int("x", x) + "," + json_int("y", y) + "," +
           json_string("space", space) + "," +
           "\"window\":{" + json_int("x", wx) + "," + json_int("y", wy) + "}," +
           "\"lcd\":{" + json_int("x", lcdX) + "," + json_int("y", lcdY) + "}," +
           json_bool("in_lcd", inLcd) + "," +
           json_int("hold_ms", holdMs) + "," +
           "\"hit\":" + hitJson + "}";
}

/* A drag with the mouse, the board's finger: press at (x1,y1), move to
 * (x2,y2) in `steps` even steps over `ms`, rest there `rest_ms`, release.
 * The same SDL events a real mouse makes, so LVGL's scrolling, its momentum
 * and the swipe recogniser on the indev all see an ordinary drag. A rest
 * longer than LVGL's momentum window lets go without a flick. The reply comes
 * at once; the drag takes ms + rest_ms after it. */
struct DragJob {
    Uint32 windowId;
    int x1, y1, x2, y2;   // window pixels
    int steps, step = 0;
    uint32_t restMs;
};

static void drag_push(Uint32 type, Uint32 windowId, int x, int y) {
    SDL_Event ev = {};
    ev.type = type;
    if (type == SDL_MOUSEMOTION) {
        ev.motion.windowID = windowId;
        ev.motion.state = SDL_BUTTON_LMASK;
        ev.motion.x = x;
        ev.motion.y = y;
    } else {
        ev.button.windowID = windowId;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
        ev.button.x = x;
        ev.button.y = y;
        ev.button.clicks = 1;
    }
    SDL_PushEvent(&ev);
}

static void drag_release_cb(lv_timer_t* t) {
    auto* job = static_cast<DragJob*>(lv_timer_get_user_data(t));
    drag_push(SDL_MOUSEBUTTONUP, job->windowId, job->x2, job->y2);
    delete job;
}

static void drag_step_cb(lv_timer_t* t) {
    auto* job = static_cast<DragJob*>(lv_timer_get_user_data(t));
    job->step++;
    const int x = job->x1 + (job->x2 - job->x1) * job->step / job->steps;
    const int y = job->y1 + (job->y2 - job->y1) * job->step / job->steps;
    drag_push(SDL_MOUSEMOTION, job->windowId, x, y);
    if (job->step < job->steps) return;
    lv_timer_delete(t);
    if (job->restMs == 0) {
        drag_push(SDL_MOUSEBUTTONUP, job->windowId, job->x2, job->y2);
        delete job;
        return;
    }
    lv_timer_t* up = lv_timer_create(drag_release_cb, job->restMs, job);
    lv_timer_set_repeat_count(up, 1);
}

static std::string handle_drag(const std::string& json) {
    const int x1 = json_get_int(json, "x1", -1), y1 = json_get_int(json, "y1", -1);
    const int x2 = json_get_int(json, "x2", -1), y2 = json_get_int(json, "y2", -1);
    if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "missing x1/y1/x2/y2") + "}";
    }
    std::string space = json_get_string(json, "space");
    if (space.empty()) space = "lcd";
    if (space != "lcd" && space != "window") {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "space must be lcd or window") + "}";
    }
    int ms = json_get_int(json, "ms", 200);
    if (ms < 10) ms = 10;
    if (ms > 5000) ms = 5000;
    int steps = json_get_int(json, "steps", 0);
    if (steps <= 0) steps = ms / 15;   // the board's panel reports every ~13 ms
    if (steps < 1) steps = 1;
    if (steps > 200) steps = 200;
    int restMs = json_get_int(json, "rest_ms", 0);
    if (restMs < 0) restMs = 0;
    if (restMs > 5000) restMs = 5000;

    const float zoom = lv_sdl_window_get_zoom(s_disp);
    auto toWindow = [&](int x, int y, int& wx, int& wy) {
        if (space == "lcd") {
            wx = (int)((x + Stm32EmuWindow::LCD_X) * zoom);
            wy = (int)((y + Stm32EmuWindow::LCD_Y) * zoom);
        } else {
            wx = x;
            wy = y;
        }
    };
    auto* job = new DragJob{};
    job->windowId = SDL_GetWindowID(lv_sdl_window_get_window(s_disp));
    toWindow(x1, y1, job->x1, job->y1);
    toWindow(x2, y2, job->x2, job->y2);
    job->steps = steps;
    job->restMs = (uint32_t)restMs;

    drag_push(SDL_MOUSEMOTION, job->windowId, job->x1, job->y1);
    drag_push(SDL_MOUSEBUTTONDOWN, job->windowId, job->x1, job->y1);
    lv_timer_create(drag_step_cb, (uint32_t)(ms / steps > 0 ? ms / steps : 1), job);

    return "{" + json_bool("ok", true) + "," +
           json_string("space", space) + "," +
           json_int("ms", ms) + "," + json_int("steps", steps) + "," +
           json_int("rest_ms", restMs) + "}";
}

/* The top row: which of the two bars is the page the encoder works on, where
 * the row rests, the drawer, the encoder's focus and the last swipe -- what a
 * drag test checks instead of reading pixels. */
static std::string handle_top_bar() {
    lv_indev_t* nav = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    lv_group_t* g = nav ? lv_indev_get_group(nav) : nullptr;
    lv_obj_t* focused = g ? lv_group_get_focused(g) : nullptr;
    const char* focus = !focused ? "none"
                      : focused == crosspad_gui::statusbar_get_arrow() ? "arrow"
                      : crosspad_gui::transportbar_has_focus() ? "transport"
                      : "screen";
    lv_area_t fa = {};
    if (focused) lv_obj_get_coords(focused, &fa);
    return "{" + json_bool("ok", true) + "," +
           "\"focus_at\":[" + std::to_string(fa.x1 - Stm32EmuWindow::LCD_X) + "," +
           std::to_string(fa.y1 - Stm32EmuWindow::LCD_Y) + "]," +
           json_bool("transport_shown", crosspad_gui::transportbar_shown()) + "," +
           json_int("row_scroll", crosspad_gui::transportbar_row_scroll()) + "," +
           json_bool("drawer_open", crosspad_gui::statusbar_drawer_is_open()) + "," +
           json_string("focus", focus) + "," +
           json_string("swipe_last", crosspad_gui::swipeDirName(crosspad_gui::touch_gesture_last())) + "," +
           json_int("swipe_count", (int)crosspad_gui::touch_gesture_count()) + "}";
}

/* The power button. The firmware resolves both gestures in crosspad-gui, so
 * these verbs exercise exactly the code the hardware button reaches — the
 * device-side equivalent is the CDC verb PWR_GESTURE CLICK|HOLD. */
static std::string handle_power_click() {
    const crosspad_gui::BackResult r = crosspad_gui::powerBack();
    return "{" + json_bool("ok", true) + "," +
           json_string("gesture", "click") + "," +
           json_string("context", crosspad_gui::backResultName(r)) + "}";
}

static std::string handle_power_hold() {
    const bool acted = crosspad_gui::powerToggleQuickSettings();
    return "{" + json_bool("ok", true) + "," +
           json_string("gesture", "hold") + "," +
           json_bool("toggled", acted) + "," +
           json_bool("drawer_open", crosspad_gui::statusbar_drawer_is_open()) + "}";
}

/* Press / release halves, for a test that wants the real 0.5 s timing rather
 * than the resolved action. */
static std::string handle_power_press(bool down) {
    if (down) crosspad_gui::powerButtonPress();
    else      crosspad_gui::powerButtonRelease();
    return "{" + json_bool("ok", true) + "," +
           json_string("gesture", down ? "press" : "release") + "}";
}

static std::string handle_pad_press(const std::string& json) {
    int pad = json_get_int(json, "pad", -1);
    int vel = json_get_int(json, "velocity", 127);
    if (pad < 0 || pad > 15) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pad (0-15)") + "}";
    }
    crosspad::control::applyPadInput(crosspad::getPadManager(), crosspad::control::PadInput::Press,
                                     (uint8_t)pad, (uint8_t)vel);
    return "{" + json_bool("ok", true) + "," + json_int("pad", pad) + "," + json_int("velocity", vel) + "}";
}

static std::string handle_pad_release(const std::string& json) {
    int pad = json_get_int(json, "pad", -1);
    if (pad < 0 || pad > 15) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pad (0-15)") + "}";
    }
    crosspad::control::applyPadInput(crosspad::getPadManager(), crosspad::control::PadInput::Release,
                                     (uint8_t)pad, 0);
    return "{" + json_bool("ok", true) + "," + json_int("pad", pad) + "}";
}

// The board's PAD_PRESSURE: pressure on a pad, as the STM streams it, to the
// active pad logic (or the LEDs' aftertouch without one).
static std::string handle_pad_pressure(const std::string& json) {
    int pad = json_get_int(json, "pad", -1);
    int pressure = json_get_int(json, "pressure", -1);
    if (pad < 0 || pad > 15) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pad (0-15)") + "}";
    }
    if (pressure < 0 || pressure > 255) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pressure (0-255)") + "}";
    }
    crosspad::control::applyPadInput(crosspad::getPadManager(), crosspad::control::PadInput::Pressure,
                                     (uint8_t)pad, (uint8_t)pressure);
    return "{" + json_bool("ok", true) + "," + json_int("pad", pad) + "," + json_int("pressure", pressure) + "}";
}

// The board's PAD_PAN: where on a held pad the press sits, as the STM's 7-bit
// value (0 left .. 64 centred .. 127 right), to the active pad logic's
// onPadPan() as value - 64. "pan" in the reply is what the logic got.
static std::string handle_pad_pan(const std::string& json) {
    int pad = json_get_int(json, "pad", -1);
    int value = json_get_int(json, "value", -1);
    if (pad < 0 || pad > 15) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pad (0-15)") + "}";
    }
    if (value < 0 || value > 127) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid value (0-127)") + "}";
    }
    const int8_t pan = crosspad::IPadLogicHandler::panFromWire((uint8_t)value);
    crosspad::control::applyPadInput(crosspad::getPadManager(), crosspad::control::PadInput::Pan,
                                     (uint8_t)pad, (uint8_t)value);
    return "{" + json_bool("ok", true) + "," + json_int("pad", pad) + "," + json_int("value", value) +
           "," + json_int("pan", pan) + "}";
}

// The board's APP_START: open an app by its launcher name, case-insensitive,
// as a tap on its tile would (the kit-selector gate included).
static std::string handle_app_start(const std::string& json) {
    const std::string name = json_get_string(json, "name");
    if (name.empty()) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "missing name") + "}";
    }
    if (!crosspad_app_start(name.c_str())) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "unknown app") + "}";
    }
    return "{" + json_bool("ok", true) + "," + json_string("name", name) + "}";
}

static std::string handle_encoder_rotate(const std::string& json) {
    int delta = json_get_int(json, "delta", 0);
    SDL_Window* window = lv_sdl_window_get_window(s_disp);
    Uint32 windowId = SDL_GetWindowID(window);

    SDL_Event ev = {};
    ev.type = SDL_MOUSEWHEEL;
    ev.wheel.windowID = windowId;
    ev.wheel.y = delta;
    SDL_PushEvent(&ev);

    return "{" + json_bool("ok", true) + "," + json_int("delta", delta) + "}";
}

static std::string handle_encoder_press(bool pressed) {
    SDL_Window* window = lv_sdl_window_get_window(s_disp);
    Uint32 windowId = SDL_GetWindowID(window);

    SDL_Event ev = {};
    ev.type = pressed ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_MIDDLE;
    ev.button.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    ev.button.x = 0;
    ev.button.y = 0;
    SDL_PushEvent(&ev);

    return "{" + json_bool("ok", true) + "}";
}

static std::string handle_key(const std::string& json) {
    int keycode = json_get_int(json, "keycode", 0);
    if (keycode == 0) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "missing keycode") + "}";
    }

    SDL_Window* window = lv_sdl_window_get_window(s_disp);
    Uint32 windowId = SDL_GetWindowID(window);

    SDL_Event ev = {};
    ev.type = SDL_KEYDOWN;
    ev.key.windowID = windowId;
    ev.key.keysym.sym = (SDL_Keycode)keycode;
    ev.key.state = SDL_PRESSED;
    SDL_PushEvent(&ev);

    ev.type = SDL_KEYUP;
    ev.key.state = SDL_RELEASED;
    SDL_PushEvent(&ev);

    return "{" + json_bool("ok", true) + "," + json_int("keycode", keycode) + "}";
}

/* ── Stats handler ────────────────────────────────────────────────────── */

static std::string handle_stats() {
    auto& pm = crosspad::getPadManager();
    auto* settings = crosspad::CrosspadSettings::getInstance();
    auto& guiPlatform = crosspad_gui::getGuiPlatform();

    std::string out = "{" + json_bool("ok", true);

    // Platform capabilities
    {
        uint32_t caps = (uint32_t)crosspad::getPlatformCapabilities();
        out += "," + json_int("capabilities_raw", (int)caps);

        // Readable list
        std::string capList = "[";
        const char* names[] = {"Midi","AudioOut","AudioIn","Synth","Pads","Leds",
                               "Encoder","Display","Persistence","Vibration","WiFi",
                               "Bluetooth","Usb","Imu","Stm32","Sequencer"};
        bool first = true;
        for (int i = 0; i < 16; i++) {
            if (caps & (1u << i)) {
                if (!first) capList += ",";
                capList += "\""; capList += names[i]; capList += "\"";
                first = false;
            }
        }
        capList += "]";
        out += ",\"capabilities\":" + capList;
    }

    // Pad state
    {
        std::string pads = "[";
        for (int i = 0; i < 16; i++) {
            if (i > 0) pads += ",";
            auto color = pm.getPadColor(i);
            pads += "{";
            pads += json_bool("pressed", pm.isPadPressed(i)) + ",";
            pads += json_bool("playing", pm.isPadPlaying(i)) + ",";
            pads += json_int("note", pm.getPadNote(i)) + ",";
            pads += json_int("channel", pm.getPadChannel(i)) + ",";
            pads += json_int("r", color.R) + ",";
            pads += json_int("g", color.G) + ",";
            pads += json_int("b", color.B);
            pads += "}";
        }
        pads += "]";
        out += ",\"pads\":" + pads;
    }

    // Active pad logic
    {
        std::string logic = pm.getActivePadLogic();
        out += "," + json_string("active_pad_logic", logic.empty() ? "none" : logic);

        auto registered = pm.getRegisteredPadLogics();
        std::string regList = "[";
        for (size_t i = 0; i < registered.size(); i++) {
            if (i > 0) regList += ",";
            regList += "\"" + registered[i] + "\"";
        }
        regList += "]";
        out += ",\"registered_pad_logics\":" + regList;
    }

    // Apps
    {
        auto& reg = crosspad::AppRegistry::getInstance();
        out += "," + json_int("app_count", (int)reg.getAppCount());

        std::string appList = "[";
        const auto* apps = reg.getApps();
        for (size_t i = 0; i < reg.getAppCount(); i++) {
            if (i > 0) appList += ",";
            appList += "\""; appList += apps[i].name; appList += "\"";
        }
        appList += "]";
        out += ",\"apps\":" + appList;
    }

    // Heap stats
    {
        auto heap = guiPlatform.getHeapStats();
        out += ",\"heap\":{";
        out += json_int("sram_free", (int)heap.sram_free) + ",";
        out += json_int("sram_total", (int)heap.sram_total) + ",";
        out += json_int("psram_free", (int)heap.psram_free) + ",";
        out += json_int("psram_total", (int)heap.psram_total);
        out += "}";
    }

    // Settings summary
    if (settings) {
        out += ",\"settings\":{";
        out += json_int("lcd_brightness", settings->LCDbrightness) + ",";
        out += json_int("rgb_brightness", settings->RGBbrightness) + ",";
        out += json_int("theme_color", settings->themeColorIndex) + ",";
        out += json_bool("audio_engine", settings->AudioEngineEnabled) + ",";
        out += json_int("kit", settings->Kit) + ",";
        out += json_int("perf_stats_flags", settings->perfStatsFlags);
        out += "}";
    }

    out += "}";
    return out;
}

/* ── Settings read handler ───────────────────────────────────────────── */

static std::string handle_settings_get(const std::string& json) {
    auto* settings = crosspad::CrosspadSettings::getInstance();
    if (!settings) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "settings not initialized") + "}";
    }

    std::string category = json_get_string(json, "category");

    std::string out = "{" + json_bool("ok", true);

    if (category.empty() || category == "all" || category == "display") {
        out += ",\"display\":{";
        out += json_int("lcd_brightness", settings->LCDbrightness) + ",";
        out += json_int("theme_color", settings->themeColorIndex) + ",";
        out += json_int("rgb_brightness", settings->RGBbrightness) + ",";
        out += json_int("perf_stats_flags", settings->perfStatsFlags);
        out += "}";
    }

    if (category.empty() || category == "all" || category == "keypad") {
        auto& kp = settings->keypad;
        out += ",\"keypad\":{";
        out += json_bool("enable", kp.enableKeypad) + ",";
        out += json_bool("note_off_release", kp.noteOffOnRelease) + ",";
        out += json_bool("inactive_lights", kp.inactiveLights) + ",";
        out += json_bool("lights_on_note_on", kp.lightsOnNoteOn) + ",";
        out += json_bool("lights_on_note_off", kp.lightsOnNoteOff) + ",";
        out += json_bool("upper_fn", kp.upperRowFunctions) + ",";
        out += json_bool("eco_mode", kp.ecoMode) + ",";
        out += json_bool("send_stm", kp.send2STM) + ",";
        out += json_bool("send_ble", kp.send2BLE) + ",";
        out += json_bool("send_usb", kp.send2USB) + ",";
        out += json_bool("send_cc", kp.sendCC);
        out += "}";
    }

    if (category.empty() || category == "all" || category == "vibration") {
        auto& vib = settings->vibration;
        out += ",\"vibration\":{";
        out += json_bool("enable", vib.enable) + ",";
        out += json_bool("on_touch", vib.enableVibrationOnTouch) + ",";
        out += json_bool("on_error", vib.enableVibrationOnError) + ",";
        out += json_bool("audio_reactive", vib.enableAudioToVibe) + ",";
        out += json_int("in_min", vib.inputMin) + ",";
        out += json_int("in_max", vib.inputMax) + ",";
        out += json_int("out_min", vib.outputMin) + ",";
        out += json_int("out_max", vib.outputMax);
        out += "}";
    }

    if (category.empty() || category == "all" || category == "wireless") {
        auto& w = settings->wireless;
        out += ",\"wireless\":{";
        out += json_bool("wifi", w.enableWiFi) + ",";
        out += json_bool("ble", w.enableBLE) + ",";
        out += json_bool("osc", w.enableOSC) + ",";
        out += json_bool("osc_server", w.enableOSCServer) + ",";
        out += json_bool("udp", w.enableUDP) + ",";
        out += json_bool("tcp", w.enableTCP) + ",";
        out += json_bool("web_server", w.enableWebServer);
        out += "}";
    }

    if (category.empty() || category == "all" || category == "audio") {
        auto& mfx = settings->masterFX;
        out += ",\"master_fx\":{";
        out += json_bool("mute", mfx.mute) + ",";
        out += json_int("in_volume", mfx.inVolume) + ",";
        out += json_int("out_volume", mfx.outVolume) + ",";
        out += json_bool("delay_bypass", mfx.delay.bypass) + ",";
        out += json_bool("reverb_bypass", mfx.reverb.bypass) + ",";
        out += json_bool("distortion_bypass", mfx.distortion.bypass) + ",";
        out += json_bool("chorus_bypass", mfx.chorus.bypass) + ",";
        out += json_bool("flanger_bypass", mfx.flanger.bypass);
        out += "}";
    }

    if (category.empty() || category == "all" || category == "system") {
        out += ",\"system\":{";
        out += json_int("kit", settings->Kit) + ",";
        out += json_bool("audio_engine", settings->AudioEngineEnabled);
        out += "}";
    }

    out += "}";
    return out;
}

/* ── Settings write handler ──────────────────────────────────────────── */

static std::string handle_settings_set(const std::string& json) {
    auto* settings = crosspad::CrosspadSettings::getInstance();
    if (!settings) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "settings not initialized") + "}";
    }

    std::string key = json_get_string(json, "key");
    int value = json_get_int(json, "value", -9999);

    if (key.empty()) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "missing key") + "}";
    }

    // Map key names to settings fields
    bool found = false;

    // Display
    if (key == "lcd_brightness") { settings->LCDbrightness = (uint8_t)value; found = true; }
    else if (key == "rgb_brightness") { settings->RGBbrightness = (uint8_t)value; found = true; }
    else if (key == "theme_color") { settings->themeColorIndex = (uint8_t)value; found = true; }
    else if (key == "perf_stats_flags") { settings->perfStatsFlags = (uint8_t)value; found = true; }
    // System
    else if (key == "kit") { settings->Kit = (uint8_t)value; found = true; }
    else if (key == "audio_engine") { settings->AudioEngineEnabled = (value != 0); found = true; }
    else if (key == "launcher_style") {   // 0 icon grid, 1 list; shown on the next launcher build
        settings->launcherStyle = value ? crosspad::LauncherStyle::List : crosspad::LauncherStyle::Grid;
        found = true;
    }
    // Keypad
    else if (key == "keypad.enable") { settings->keypad.enableKeypad = (value != 0); found = true; }
    else if (key == "keypad.inactive_lights") { settings->keypad.inactiveLights = (value != 0); found = true; }
    else if (key == "keypad.eco_mode") { settings->keypad.ecoMode = (value != 0); found = true; }
    else if (key == "keypad.send_stm") { settings->keypad.send2STM = (value != 0); found = true; }
    else if (key == "keypad.send_ble") { settings->keypad.send2BLE = (value != 0); found = true; }
    else if (key == "keypad.send_usb") { settings->keypad.send2USB = (value != 0); found = true; }
    else if (key == "keypad.send_cc") { settings->keypad.sendCC = (value != 0); found = true; }
    // Vibration
    else if (key == "vibration.enable") { settings->vibration.enable = (value != 0); found = true; }
    else if (key == "vibration.on_touch") { settings->vibration.enableVibrationOnTouch = (value != 0); found = true; }
    else if (key == "vibration.in_min") { settings->vibration.inputMin = (uint8_t)value; found = true; }
    else if (key == "vibration.in_max") { settings->vibration.inputMax = (uint8_t)value; found = true; }
    else if (key == "vibration.out_min") { settings->vibration.outputMin = (uint8_t)value; found = true; }
    else if (key == "vibration.out_max") { settings->vibration.outputMax = (uint8_t)value; found = true; }
    // Audio master FX
    else if (key == "master_fx.mute") { settings->masterFX.mute = (value != 0); found = true; }
    else if (key == "master_fx.in_volume") { settings->masterFX.inVolume = (uint8_t)value; found = true; }
    else if (key == "master_fx.out_volume") { settings->masterFX.outVolume = (uint8_t)value; found = true; }

    if (!found) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "unknown key: " + key) + "}";
    }

    // Auto-save after setting change
    pc_platform_save_settings();

    return "{" + json_bool("ok", true) + "," + json_string("key", key) + "," + json_int("value", value) + "}";
}

// ── Audio integration helpers (Tier 3 in-app driver) ──────────────────────

static std::string handle_midi_note_on(const std::string& json) {
    (void)json_get_int(json, "channel", 0);
    int note     = json_get_int(json, "note",    -1);
    int velocity = json_get_int(json, "velocity", 100);
    if (note < 0 || note > 127) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid note (0-127)") + "}";
    }
    // Drive the synth engine directly — the event bus path requires an active
    // app handler to route NoteOn → synth, which the launcher doesn't have.
    // Bypassing that gives a deterministic audio-pipeline smoke test.
    crosspad::ISynthEngine* synth = pc_platform_get_synth_engine();
    if (!synth) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "no synth engine") + "}";
    }
    synth->noteOn(static_cast<uint8_t>(note), static_cast<uint8_t>(velocity));
    return "{" + json_bool("ok", true) + "," + json_int("note", note) + "}";
}

static std::string handle_midi_note_off(const std::string& json) {
    (void)json_get_int(json, "channel", 0);
    int note = json_get_int(json, "note", -1);
    if (note < 0 || note > 127) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid note (0-127)") + "}";
    }
    crosspad::ISynthEngine* synth = pc_platform_get_synth_engine();
    if (!synth) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "no synth engine") + "}";
    }
    synth->noteOff(static_cast<uint8_t>(note));
    return "{" + json_bool("ok", true) + "," + json_int("note", note) + "}";
}

static std::string handle_citest_run(const std::string& /*json*/) {
    if (citest::isRunning()) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "already running") + "}";
    }
    if (!citest::start()) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "failed to start") + "}";
    }
    return "{" + json_bool("ok", true) + "}";
}

static std::string handle_citest_status(const std::string& /*json*/) {
    const size_t n = citest::stageCount();
    std::string body;
    body.reserve(512);
    body += "{";
    body += json_bool("ok", true);
    body += ",";
    body += json_bool("running", citest::isRunning());
    body += ",\"stages\":[";

    int passCount = 0, failCount = 0;
    for (size_t i = 0; i < n; ++i) {
        const char* name = "";
        const char* detail = "";
        citest::Result r = citest::Result::Pending;
        if (!citest::stageAt(i, name, r, detail)) continue;
        const char* rs = "pending";
        switch (r) {
            case citest::Result::Pending: rs = "pending"; break;
            case citest::Result::Running: rs = "running"; break;
            case citest::Result::Pass:    rs = "pass"; ++passCount; break;
            case citest::Result::Fail:    rs = "fail"; ++failCount; break;
        }
        if (i > 0) body += ",";
        body += "{";
        body += json_string("name",   name);
        body += ",";
        body += json_string("result", rs);
        body += ",";
        body += json_string("detail", detail);
        body += "}";
    }
    body += "],";
    body += json_int("pass", passCount);
    body += ",";
    body += json_int("fail", failCount);
    body += "}";
    return body;
}

static std::string handle_audio_level(const std::string& json) {
    int stream = json_get_int(json, "stream", 0);
    return remote::controlJsonAudioLevel(stream, crosspad::control::readAudioLevel(static_cast<uint8_t>(stream)));
}

/* ── Kit handlers ────────────────────────────────────────────────────── */
//
// The device answers KIT_LIST / KIT_LOAD / KIT_STATUS over CDC and the
// simulator answered nothing, so every test that needed a kit had to click its
// way through the browser — which is neither deterministic nor what the
// hardware tests do.

static void (*s_kitLoader)(int) = nullptr;
static bool (*s_kitBusy)()      = nullptr;
static remote::AudioDeviceCtl s_audioCtl;

static std::string audio_list_json(std::vector<std::string> (*fn)()) {
    if (!fn) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "no audio device control") + "}";
    }
    auto names = fn();
    std::string out = "{" + json_bool("ok", true) + "," +
                      json_int("count", (int)names.size()) + ",\"devices\":[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ",";
        out += json_quote(names[i]);
    }
    return out + "]}";
}

static std::string audio_set_json(const std::string& json,
                                  bool (*fn)(int, int, std::string&)) {
    if (!fn) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "no audio device control") + "}";
    }
    const int slot  = json_get_int(json, "slot", 0);
    const int index = json_get_int(json, "index", -1);
    if (slot < 0 || slot > 1 || index < 0) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "slot 0/1 and index >= 0 required") + "}";
    }
    std::string label;
    bool connected = fn(slot, index, label);
    // ok = the command was applied; connected = a device is live on the slot.
    // index 0 is "(None)", a deliberate disconnect, so ok stays true.
    return "{" + json_bool("ok", true) + "," + json_int("slot", slot) + "," +
           json_int("index", index) + "," + json_bool("connected", connected) +
           "," + json_string("name", label) + "}";
}

static std::string handle_audio_out_list() { return audio_list_json(s_audioCtl.outList); }
static std::string handle_audio_in_list()  { return audio_list_json(s_audioCtl.inList); }
static std::string handle_audio_out_set(const std::string& j) {
    return audio_set_json(j, s_audioCtl.outSelect);
}
static std::string handle_audio_in_set(const std::string& j) {
    return audio_set_json(j, s_audioCtl.inSelect);
}

// Per-channel and per-output mixer peaks + routing — the sim's parallel to the
// board's MIX_LVL CDC verb (shared crosspad-mixer AudioMixerEngine), so HIL can
// read the mixer the same way on both.
static std::string handle_mix_lvl() {
#ifdef REMOTE_HAS_MIXER
    auto& mx = getMixerEngine();
    std::string chans, outs;
    crosspad::control::MixerTapLevel tap;
    crosspad::control::readMixerLevels(mx,
        [&](const crosspad::control::MixerChannelLevel& row) {
            if (!chans.empty()) chans += ",";
            chans += remote::controlJsonMixChannel(row);
        },
        [&](const crosspad::control::MixerOutputLevel& row) {
            if (!outs.empty()) outs += ",";
            outs += remote::controlJsonMixOutput(row);
        },
        [&](const crosspad::control::MixerTapLevel& row) { tap = row; });
    return remote::controlJsonMixLevels(mx.numOutputs(), chans, outs, tap);
#else
    return "{" + json_bool("ok", false) + "," +
           json_string("error", "mixer not built") + "}";
#endif
}

// Pitched-engine status — the sim's parallel to the board's PITCHED_STATUS
// (shared crosspad-core PitchedInstrument): zones/roots, live voices, steals.
static std::string handle_pitched_status() {
#ifdef REMOTE_HAS_PITCHED
    auto& inst = crosspad_pc::pitched_port_instrument();
    crosspad::control::PitchedStatus st;
    st.active = crosspad_pc::pitched_port_active();
    crosspad::control::readPitchedBank(inst.bank(), st);
    crosspad::control::readPitchedEngine(inst, st);
    return remote::controlJsonPitchedStatus(st);
#else
    return "{" + json_bool("ok", false) + "," +
           json_string("error", "pitched not built") + "}";
#endif
}

// Waveform-cache loader counters — the sim's parallel to the board's
// WAVE_STATUS (shared crosspad-sampler waveform loader): how many waveform
// reads were requested, cached, retried, dropped or failed.
static std::string handle_wave_status() {
#ifdef REMOTE_HAS_WAVE
    crosspad_sampler::WaveformLoaderStats loader;
    crosspad_sampler::waveform_loader_get_stats(&loader);
    crosspad::control::WaveStatus st;
    crosspad::control::setWaveLoader(st, loader, crosspad_sampler::waveform_loader_is_running());
    return remote::controlJsonWaveStatus(st);
#else
    return "{" + json_bool("ok", false) + "," +
           json_string("error", "sampler not built") + "}";
#endif
}

// Registered apps + the one currently running — the sim's parallel to the
// board's APP_LIST (shared crosspad-gui AppOrchestrator).
static std::string handle_app_list() {
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    std::string out = "{" + json_bool("ok", true) + ",\"apps\":[";
    bool first = true;
    for (const auto& app : orch.getApps()) {
        if (!app) continue;
        const char* nm = app->getName();
        if (!first) out += ",";
        first = false;
        out += json_quote(nm ? nm : "");
    }
    auto* running = orch.getRunningApp();
    const char* rn = running ? running->getName() : nullptr;
    out += "]," + json_string("running", rn ? rn : "-") + "}";
    return out;
}

// Pad LED state — the sim's parallel to the board's LED_STATE (shared core
// PadLedController): brightness, animation flags and the 16 pad colours.
static std::string handle_led_state() {
    return remote::controlJsonLedState(crosspad::control::readLedState(crosspad::getPadLedController()));
}

// Sample engine peak + free WAV slots and load — the sim's parallel to the
// board's SMPL_PEAK (shared streaming SampleStreamPlayer).
static std::string handle_smpl_peak() {
#ifdef REMOTE_HAS_SMPL
    return "{" + json_bool("ok", true) + "," +
           json_float("peak", SampleStreamPly_GetPeak()) + "," +
           json_int("free", SampleStreamPlayer_GetFreeWavCnt()) + "," +
           json_int("active_voices", (int)SampleStreamPly_GetActiveVoices()) + "," +
           json_int("underrun", (int)SampleStreamPly_GetUnderrunCount()) + "," +
           json_int("dropped_notes", (int)SampleStreamPly_GetDroppedNoteCount()) + "," +
           json_float("load_main", SampleStreamPly_GetLoadMain()) + "}";
#else
    return "{" + json_bool("ok", false) + "," +
           json_string("error", "sampler not built") + "}";
#endif
}

static std::string handle_kit_list() {
    auto* mgr = crosspad::getKitManager();
    if (!mgr) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "no kit manager") + "}";
    }
    const int count = mgr->getKitCount();
    const int current = mgr->getCurrentKitId();
    std::string kits;
    crosspad::control::readKitList(*mgr, [&kits](const crosspad::control::KitListRow& row) {
        if (row.id) kits += ",";
        kits += remote::controlJsonKitRow(row);
    });
    return remote::controlJsonKitList(count, current, kits);
}

static std::string handle_kit_status() {
    return remote::controlJsonKitStatus(crosspad::control::readKitStatus(s_kitBusy ? s_kitBusy() : false, -1));
}

static std::string handle_kit_load(const std::string& json) {
    if (!s_kitLoader) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "no sampler installed") + "}";
    }
    const int id = json_get_int(json, "kit", -1);
    if (!crosspad::control::kitIdLoadable(id)) {
        return "{" + json_bool("ok", false) + "," +
               json_string("error", "kit index out of range") + "}";
    }
    s_kitLoader(id);
    // "OK" here means the load *started*. It is not an acknowledgement that the
    // kit is playable — ask kit_status for that, exactly as on the device.
    return "{" + json_bool("ok", true) + "," + json_int("kit", id) + "," +
           json_bool("started", true) + "}";
}

static std::string dispatch_command(const std::string& json) {
    std::string cmd = json_get_string(json, "cmd");

    if (cmd == "ping") {
        return "{" + json_bool("ok", true) + "," + json_string("cmd", "ping") + "}";
    }
    if (cmd == "screenshot") {
        return handle_screenshot(json);
    }
    if (cmd == "click") {
        return handle_click(json);
    }
    if (cmd == "drag") {
        return handle_drag(json);
    }
    if (cmd == "top_bar") {
        return handle_top_bar();
    }
    if (cmd == "pad_press") {
        return handle_pad_press(json);
    }
    if (cmd == "pad_release") {
        return handle_pad_release(json);
    }
    if (cmd == "pad_pressure") {
        return handle_pad_pressure(json);
    }
    if (cmd == "pad_pan") {
        return handle_pad_pan(json);
    }
    if (cmd == "app_start") {
        return handle_app_start(json);
    }
    if (cmd == "power_click") {
        return handle_power_click();
    }
    if (cmd == "power_hold") {
        return handle_power_hold();
    }
    if (cmd == "power_press") {
        return handle_power_press(true);
    }
    if (cmd == "power_release") {
        return handle_power_press(false);
    }
    if (cmd == "encoder_rotate") {
        return handle_encoder_rotate(json);
    }
    if (cmd == "encoder_press") {
        return handle_encoder_press(true);
    }
    if (cmd == "encoder_release") {
        return handle_encoder_press(false);
    }
    if (cmd == "key") {
        return handle_key(json);
    }
    if (cmd == "stats") {
        return handle_stats();
    }
    if (cmd == "settings_get") {
        return handle_settings_get(json);
    }
    if (cmd == "settings_set") {
        return handle_settings_set(json);
    }
    if (cmd == "midi_note_on") {
        return handle_midi_note_on(json);
    }
    if (cmd == "midi_note_off") {
        return handle_midi_note_off(json);
    }
    if (cmd == "audio_level") {
        return handle_audio_level(json);
    }
    if (cmd == "kit_list") {
        return handle_kit_list();
    }
    if (cmd == "kit_load") {
        return handle_kit_load(json);
    }
    if (cmd == "kit_status") {
        return handle_kit_status();
    }
    if (cmd == "audio_out_list") {
        return handle_audio_out_list();
    }
    if (cmd == "audio_in_list") {
        return handle_audio_in_list();
    }
    if (cmd == "audio_out_set") {
        return handle_audio_out_set(json);
    }
    if (cmd == "audio_in_set") {
        return handle_audio_in_set(json);
    }
    if (cmd == "mix_lvl") {
        return handle_mix_lvl();
    }
    if (cmd == "pitched_status") {
        return handle_pitched_status();
    }
    if (cmd == "wave_status") {
        return handle_wave_status();
    }
    if (cmd == "app_list") {
        return handle_app_list();
    }
    if (cmd == "smpl_peak") {
        return handle_smpl_peak();
    }
    if (cmd == "led_state") {
        return handle_led_state();
    }
    if (cmd == "citest_run") {
        return handle_citest_run(json);
    }
    if (cmd == "citest_status") {
        return handle_citest_status(json);
    }

    return "{" + json_bool("ok", false) + "," + json_string("error", "unknown command: " + cmd) + "}";
}

/* ── TCP server thread ───────────────────────────────────────────────── */

static void handle_client(socket_t client) {
    // Set receive timeout
    #ifdef _WIN32
    DWORD timeout = 30000; // 30s
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    #endif

    std::string buffer;
    char chunk[4096];

    while (s_running) {
        int n = recv(client, chunk, sizeof(chunk) - 1, 0);
        if (n <= 0) break;
        chunk[n] = '\0';
        buffer.append(chunk, n);

        // Process complete lines (newline-delimited JSON)
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            // Commands that need SDL/LVGL context must run on LVGL thread
            std::string cmd = json_get_string(line, "cmd");
            if (cmd == "ping") {
                // ping can respond immediately
                std::string resp = dispatch_command(line) + "\n";
                send(client, resp.c_str(), (int)resp.size(), 0);
            } else {
                // Queue for LVGL thread, wait for response
                std::unique_lock<std::mutex> lock(s_responseMutex);
                s_responseReady = false;

                {
                    std::lock_guard<std::mutex> qlock(s_queueMutex);
                    s_commandQueue.push({line, nullptr});
                }

                // Busy wait for response (with timeout)
                lock.unlock();
                int waitMs = 0;
                while (!s_responseReady && s_running && waitMs < 10000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    waitMs += 5;
                }

                lock.lock();
                std::string resp = s_responseReady ? s_pendingResponse :
                    ("{" + json_bool("ok", false) + "," + json_string("error", "timeout") + "}");
                lock.unlock();

                resp += "\n";
                send(client, resp.c_str(), (int)resp.size(), 0);
            }
        }
    }

    CLOSE_SOCKET(client);
}

static void server_thread_func() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[Remote] WSAStartup failed\n");
        return;
    }
#endif

    s_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listenSocket == SOCKET_INVALID) {
        printf("[Remote] Failed to create socket\n");
        return;
    }

    // Allow reuse
    int reuse = 1;
    setsockopt(s_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    addr.sin_port = htons(PORT);

    if (bind(s_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[Remote] Failed to bind port %d\n", PORT);
        CLOSE_SOCKET(s_listenSocket);
        s_listenSocket = SOCKET_INVALID;
        return;
    }

    if (listen(s_listenSocket, 2) != 0) {
        printf("[Remote] Failed to listen\n");
        CLOSE_SOCKET(s_listenSocket);
        s_listenSocket = SOCKET_INVALID;
        return;
    }

    printf("[Remote] Listening on 127.0.0.1:%d\n", PORT);

    while (s_running) {
        // Use select() with timeout so we can check s_running
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s_listenSocket, &readfds);
        struct timeval tv = {1, 0}; // 1 second timeout

        int sel = select((int)s_listenSocket + 1, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        socket_t client = accept(s_listenSocket, nullptr, nullptr);
        if (client == SOCKET_INVALID) continue;

        // Handle one client at a time (simple for MCP use case)
        handle_client(client);
    }

    CLOSE_SOCKET(s_listenSocket);
    s_listenSocket = SOCKET_INVALID;

#ifdef _WIN32
    WSACleanup();
#endif

    printf("[Remote] Server stopped\n");
}

/* ── Public API ──────────────────────────────────────────────────────── */

namespace remote {

void start(lv_display_t* disp) {
    s_disp = disp;
    s_running = true;
    s_serverThread = std::thread(server_thread_func);
    printf("[Remote] Control server starting on port %d\n", PORT);
}

void stop() {
    s_running = false;
    if (s_listenSocket != SOCKET_INVALID) {
        CLOSE_SOCKET(s_listenSocket);
        s_listenSocket = SOCKET_INVALID;
    }
    if (s_serverThread.joinable()) {
        s_serverThread.join();
    }
    printf("[Remote] Control server stopped\n");
}

void set_kit_loader(void (*loader)(int), bool (*busy)()) {
    s_kitLoader = loader;
    s_kitBusy   = busy;
}

void set_audio_device_ctl(const AudioDeviceCtl& ctl) {
    s_audioCtl = ctl;
}

void process_pending() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    while (!s_commandQueue.empty()) {
        auto cmd = std::move(s_commandQueue.front());
        s_commandQueue.pop();

        std::string response = dispatch_command(cmd.request);

        // Signal response to waiting TCP thread
        {
            std::lock_guard<std::mutex> rlock(s_responseMutex);
            s_pendingResponse = response;
            s_responseReady = true;
        }
    }
}

} // namespace remote
