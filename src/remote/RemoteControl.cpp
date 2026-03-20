/**
 * @file RemoteControl.cpp
 * @brief TCP server for external simulator control (screenshot, input injection).
 */

#include "RemoteControl.hpp"

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

// LVGL SDL driver internals — window/renderer access
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"

// crosspad-core
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"
#include "crosspad/app/AppRegistry.hpp"

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

/* ── Simple JSON helpers (no dependency) ─────────────────────────────── */

static std::string json_string(const std::string& key, const std::string& val) {
    return "\"" + key + "\":\"" + val + "\"";
}

static std::string json_bool(const std::string& key, bool val) {
    return "\"" + key + "\":" + (val ? "true" : "false");
}

static std::string json_int(const std::string& key, int val) {
    return "\"" + key + "\":" + std::to_string(val);
}

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

    if (lcdOnly) {
        // LCD position within the window (must match Stm32EmuWindow layout)
        static constexpr int LCD_W = 320;
        static constexpr int LCD_H = 240;
        static constexpr int WIN_W = 490;
        static constexpr int LCD_X = (WIN_W - LCD_W) / 2; // 85
        static constexpr int LCD_Y = 40;

        lcdRect = { LCD_X, LCD_Y, LCD_W, LCD_H };
        captureRect = &lcdRect;
        w = LCD_W;
        h = LCD_H;
    } else {
        SDL_GetWindowSize(window, &w, &h);
    }

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
                   json_int("size", (int)png.size()) + "}";
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
           json_string("data", b64) + "}";
}

static std::string handle_click(const std::string& json) {
    int x = json_get_int(json, "x", -1);
    int y = json_get_int(json, "y", -1);
    if (x < 0 || y < 0) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "missing x/y") + "}";
    }

    // Get window ID
    SDL_Window* window = lv_sdl_window_get_window(s_disp);
    Uint32 windowId = SDL_GetWindowID(window);

    // Inject mouse move + button down + button up
    SDL_Event ev = {};

    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = windowId;
    ev.motion.x = x;
    ev.motion.y = y;
    SDL_PushEvent(&ev);

    ev = {};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_PRESSED;
    ev.button.x = x;
    ev.button.y = y;
    ev.button.clicks = 1;
    SDL_PushEvent(&ev);

    ev = {};
    ev.type = SDL_MOUSEBUTTONUP;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_RELEASED;
    ev.button.x = x;
    ev.button.y = y;
    ev.button.clicks = 1;
    SDL_PushEvent(&ev);

    return "{" + json_bool("ok", true) + "," + json_int("x", x) + "," + json_int("y", y) + "}";
}

static std::string handle_pad_press(const std::string& json) {
    int pad = json_get_int(json, "pad", -1);
    int vel = json_get_int(json, "velocity", 127);
    if (pad < 0 || pad > 15) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pad (0-15)") + "}";
    }
    crosspad::getPadManager().handlePadPress(pad, vel);
    return "{" + json_bool("ok", true) + "," + json_int("pad", pad) + "," + json_int("velocity", vel) + "}";
}

static std::string handle_pad_release(const std::string& json) {
    int pad = json_get_int(json, "pad", -1);
    if (pad < 0 || pad > 15) {
        return "{" + json_bool("ok", false) + "," + json_string("error", "invalid pad (0-15)") + "}";
    }
    crosspad::getPadManager().handlePadRelease(pad);
    return "{" + json_bool("ok", true) + "," + json_int("pad", pad) + "}";
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
    if (cmd == "pad_press") {
        return handle_pad_press(json);
    }
    if (cmd == "pad_release") {
        return handle_pad_release(json);
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
