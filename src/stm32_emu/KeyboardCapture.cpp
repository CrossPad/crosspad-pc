/**
 * @file KeyboardCapture.cpp
 * @brief PC keyboard → pad grid mapping with 3 capture modes.
 */

#include "KeyboardCapture.hpp"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h defines ERROR as 0, which clashes with enum members in crosspad-core
#undef ERROR
#endif

#include <SDL2/SDL.h>
#include <crosspad/pad/PadManager.hpp>

/* ── Key → pad mapping ────────────────────────────────────────────────── */

// Layout mirrors the physical 4x4 grid:
//   1 2 3 4   → row 3 (top):    pads 12-15
//   Q W E R   → row 2:          pads  8-11
//   A S D F   → row 1:          pads  4-7
//   Z X C V   → row 0 (bottom): pads  0-3

int KeyboardCapture::keyToPad(int keycode)
{
    switch (keycode) {
        // Bottom row (pads 0-3)
        case SDLK_z: return 0;
        case SDLK_x: return 1;
        case SDLK_c: return 2;
        case SDLK_v: return 3;
        // Row 1 (pads 4-7)
        case SDLK_a: return 4;
        case SDLK_s: return 5;
        case SDLK_d: return 6;
        case SDLK_f: return 7;
        // Row 2 (pads 8-11)
        case SDLK_q: return 8;
        case SDLK_w: return 9;
        case SDLK_e: return 10;
        case SDLK_r: return 11;
        // Top row (pads 12-15)
        case SDLK_1: return 12;
        case SDLK_2: return 13;
        case SDLK_3: return 14;
        case SDLK_4: return 15;
        default:     return -1;
    }
}

/* ── Pad press/release ────────────────────────────────────────────────── */

void KeyboardCapture::pressPad(int padIdx)
{
    if (padIdx < 0 || padIdx >= 16) return;
    if (padHeld_[padIdx]) return;  // already held

    padHeld_[padIdx] = true;
    crosspad::getPadManager().handlePadPress((uint8_t)padIdx, 127);
}

void KeyboardCapture::releasePad(int padIdx)
{
    if (padIdx < 0 || padIdx >= 16) return;
    if (!padHeld_[padIdx]) return;

    padHeld_[padIdx] = false;
    crosspad::getPadManager().handlePadRelease((uint8_t)padIdx);
}

void KeyboardCapture::releaseAllPads()
{
    for (int i = 0; i < 16; i++) {
        if (padHeld_[i]) {
            padHeld_[i] = false;
            crosspad::getPadManager().handlePadRelease((uint8_t)i);
        }
    }
}

/* ── Init / Destroy ───────────────────────────────────────────────────── */

void KeyboardCapture::init()
{
    mode_ = Mode::Off;
}

KeyboardCapture::~KeyboardCapture()
{
    releaseAllPads();
#ifdef _WIN32
    unhookKeyboard();
#endif
}

/* ── Mode switching ───────────────────────────────────────────────────── */

void KeyboardCapture::cycleMode()
{
    switch (mode_) {
        case Mode::Off:    setMode(Mode::Focus);  break;
        case Mode::Focus:  setMode(Mode::Global); break;
        case Mode::Global: setMode(Mode::Off);    break;
    }
}

void KeyboardCapture::setMode(Mode m)
{
    if (m == mode_) return;

    // Release any held pads when switching modes
    releaseAllPads();

#ifdef _WIN32
    if (mode_ == Mode::Global)
        unhookKeyboard();

    if (m == Mode::Global)
        hookKeyboard();
#endif

    mode_ = m;

    const char* names[] = { "OFF", "FOCUS", "GLOBAL" };
    printf("[KeyboardCapture] Mode: %s\n", names[(int)m]);
}

/* ── Key event handling ──────────────────────────────────────────────── */

bool KeyboardCapture::handleKey(int keycode, bool pressed, bool isRepeat)
{
    if (mode_ == Mode::Off) return false;

    // Ignore key repeats
    if (pressed && isRepeat) return false;

    int padIdx = keyToPad(keycode);
    if (padIdx < 0) return false;

    if (pressed)
        pressPad(padIdx);
    else
        releasePad(padIdx);

    return true;  // consumed
}

/* ── Windows low-level keyboard hook (Global mode) ───────────────────── */

#ifdef _WIN32

// Map Windows VK code to pad index (0-15), or -1 if unmapped.
static int vkToPad(DWORD vk)
{
    switch (vk) {
        case 'Z': return 0;   case 'X': return 1;
        case 'C': return 2;   case 'V': return 3;
        case 'A': return 4;   case 'S': return 5;
        case 'D': return 6;   case 'F': return 7;
        case 'Q': return 8;   case 'W': return 9;
        case 'E': return 10;  case 'R': return 11;
        case '1': return 12;  case '2': return 13;
        case '3': return 14;  case '4': return 15;
        default:  return -1;
    }
}

// Static instance pointer for the hook callback (only one KeyboardCapture exists).
static KeyboardCapture* s_hookInstance = nullptr;

static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_hookInstance) {
        auto* kb = (KBDLLHOOKSTRUCT*)lParam;

        // Ignore injected events (avoid feedback loops)
        if (!(kb->flags & LLKHF_INJECTED)) {
            int padIdx = vkToPad(kb->vkCode);
            if (padIdx >= 0) {
                if (wParam == WM_KEYDOWN) {
                    // LLKHF_UP is not set for key-down; check repeat via padHeld_
                    s_hookInstance->pressPad(padIdx);
                } else if (wParam == WM_KEYUP) {
                    s_hookInstance->releasePad(padIdx);
                }
                // Swallow the key so other apps don't see it
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void KeyboardCapture::hookKeyboard()
{
    if (kbHook_) return;

    s_hookInstance = this;
    kbHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc, NULL, 0);

    if (kbHook_) {
        printf("[KeyboardCapture] Global keyboard hook installed\n");
    } else {
        printf("[KeyboardCapture] Failed to install keyboard hook (err=%lu)\n", GetLastError());
    }
}

void KeyboardCapture::unhookKeyboard()
{
    if (!kbHook_) return;

    UnhookWindowsHookEx((HHOOK)kbHook_);
    kbHook_ = nullptr;
    s_hookInstance = nullptr;
    printf("[KeyboardCapture] Global keyboard hook removed\n");
}

void KeyboardCapture::processGlobalHotkeys()
{
    // Low-level keyboard hook is callback-driven, but Windows requires
    // the installing thread to pump messages for the hook to fire.
    // SDL already does this via its event loop, so nothing extra needed here.
    // This method is kept for API compatibility.
}

#else

void KeyboardCapture::processGlobalHotkeys()
{
    // No-op on non-Windows platforms
}

#endif
