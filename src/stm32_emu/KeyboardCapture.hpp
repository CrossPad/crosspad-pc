#pragma once

/**
 * @file KeyboardCapture.hpp
 * @brief Maps PC keyboard keys to the 4x4 pad grid for playing pads.
 *
 * Keyboard layout (matches physical pad grid, bottom-left = pad 0):
 *
 *   1  2  3  4   → pads 12, 13, 14, 15  (top row)
 *   Q  W  E  R   → pads  8,  9, 10, 11
 *   A  S  D  F   → pads  4,  5,  6,  7
 *   Z  X  C  V   → pads  0,  1,  2,  3  (bottom row)
 *
 * Three capture modes:
 *   OFF    – keyboard input disabled
 *   FOCUS  – capture only when the SDL window is focused
 *   GLOBAL – capture always (Windows: low-level keyboard hook; others: always-on SDL)
 */

#include <cstdint>

class KeyboardCapture {
public:
    using ActionCallback = void(*)();

    enum class Mode : uint8_t {
        Off    = 0,
        Focus  = 1,
        Global = 2
    };

    KeyboardCapture() = default;
    ~KeyboardCapture();

    /// Initialize (call once after SDL is up).
    void init();

    /// Cycle to next mode: Off → Focus → Global → Off.
    void cycleMode();

    /// Set mode directly.
    void setMode(Mode m);

    /// Current mode.
    Mode mode() const { return mode_; }

    /// Handle a keyboard key event.
    /// @param keycode  SDL_Keycode value
    /// @param pressed  true = key down, false = key up
    /// @param isRepeat true if this is a key-repeat event
    /// @return true if the event was consumed (mapped to a pad)
    bool handleKey(int keycode, bool pressed, bool isRepeat);

    /// Set callback for Escape key (go home / close app).
    void setEscapeCallback(ActionCallback cb) { onEscape_ = cb; }

    /// Set callback for power button (Ctrl key → volume overlay toggle).
    void setPowerCallback(ActionCallback cb) { onPower_ = cb; }

    /// Process Windows messages for global hotkeys (call from a timer or message pump).
    /// Only relevant in Global mode on Windows.
    void processGlobalHotkeys();

private:
    Mode mode_ = Mode::Off;
    ActionCallback onEscape_ = nullptr;
    ActionCallback onPower_  = nullptr;

    /// Map SDL keycode to pad index (0-15), or -1 if unmapped.
    static int keyToPad(int keycode);

    /// Track which pads are currently held by keyboard to avoid double-press.
    bool padHeld_[16] = {};

    // pressPad/releasePad are public so the low-level hook callback can call them.
    friend struct LLKeyboardHookAccess;
public:
    void pressPad(int padIdx);
    void releasePad(int padIdx);
private:
    void releaseAllPads();

#ifdef _WIN32
    /// Low-level keyboard hook (WH_KEYBOARD_LL) for Global mode.
    void* kbHook_ = nullptr;   // HHOOK, stored as void* to avoid windows.h in header
    void hookKeyboard();
    void unhookKeyboard();
#endif
};
