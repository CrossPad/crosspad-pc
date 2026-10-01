/**
 * @file    wasm_bridge.cpp
 * @brief   The browser twin's side door: what the page feeds in and what it
 *          reads out (the LCD, as RGBA).
 *
 * Two ways in:
 *
 *  · Lockstep (the live twin of a real CrossPad). The board's firmware sends
 *    what its LVGL read, where it read it, stamped with its lv_tick
 *    (platform-idf twin_mirror.h, SysEx F0 7D 1F 09): the touch panel and the
 *    encoder, one frame per LVGL read, the pads as they enter the PadManager,
 *    the power button's resolved gesture, and clock frames. Here LVGL's time
 *    is the board's: it never runs past the last tick the board vouched for,
 *    the indevs are read only when a frame says the board read them — at that
 *    frame's tick, with that frame's data — and every timer due before it runs
 *    first. Same firmware, same inputs, same times: the same screen. Nothing
 *    of the screen itself travels. TWIN_STATE (twin_state.h, the same code on
 *    both sides) is the safety net: the page asks the board every second and
 *    hands the answer to wasm_twin_check(), which puts the app, the focus and
 *    the scroll back when they stayed apart on a screen at rest.
 *
 *  · Loose (the standalone emulator, or a board without twin_mirror): pads,
 *    encoder, touches and the power button as the page saw them, applied
 *    through the same read callbacks on LVGL's own read timer.
 *
 * Replaces RemoteControl in the Emscripten build (no TCP in a browser); the
 * LVGL task still calls remote::process_pending() every loop, which is where
 * the queued input is applied — on the LVGL task, never from a JS call, so a
 * pad press that wakes another task can switch fibers safely.
 */
#include <emscripten.h>
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <strings.h>

#include "lvgl/lvgl.h"
#include "hal/hal.h"
#include "twin_state.h"
#include "board/twin_board.h"
#include "remote/RemoteControl.hpp"
#include "stm32_emu/Stm32EmuWindow.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/components/touch_gesture.h"
#include "crosspad-gui/platform/touchpad_interaction.h"
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/styles/styles.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad/settings/ISettingsUI.hpp"
#include <vector>

extern crosspad::CrosspadStatus status;

#include "crosspad_app.hpp"

// The asset loader resolves its folder in a static constructor, long before
// main(): point it at the preloaded /assets (CrossPad.data) ahead of those.
__attribute__((constructor(101))) static void wasm_env() { setenv("CROSSPAD_ASSETS", "/assets", 1); }

extern "C" void wasm_io_service(void);

/* The board's navigation encoder, as the apps name it (display.h there), and
 * the group it rests on when no screen has handed it one. */
extern "C" { lv_indev_t* indev_encoderTop = nullptr; }
lv_group_t* encoderTop_group = nullptr;

namespace {

/* Where the LCD sits on the display: at the origin with --lcd (the twin),
 * inside the device body otherwise (crosspad.html). */
int32_t lcd_x() { return crosspad_app_lcd_only() ? 0 : Stm32EmuWindow::LCD_X; }
int32_t lcd_y() { return crosspad_app_lcd_only() ? 0 : Stm32EmuWindow::LCD_Y; }

lv_display_t* s_disp = nullptr;
lv_indev_t* s_ptr = nullptr;     // the touch panel (SDL's mouse indev)
lv_indev_t* s_enc = nullptr;     // the navigation encoder (SDL's wheel indev)
uint32_t s_frame = 0;
uint8_t s_rgba[Stm32EmuWindow::LCD_W * Stm32EmuWindow::LCD_H * 4];

void on_refr_ready(lv_event_t*) { ++s_frame; }

/* ── time ───────────────────────────────────────────────────────────────
 * lv_tick is ours. In lockstep it is the board's tick (plus s_k, which only
 * ever moves to keep it from going backwards: a board rebooted under a page
 * that stayed open); otherwise it follows the page's clock. */
uint32_t s_now = 0;
bool     s_lock = false;               // the board's frames drive time and input
uint32_t s_k = 0;                      // twin tick = board tick + s_k
uint32_t s_vouched = 0;                // latest board tick nothing may come before (board's)
double   s_lastFrameAt = -1e9;         // page ms of the last 09 frame
double   s_freeBase = 0;               // free run: page ms ...
uint32_t s_freeTick = 0;               // ... at this tick
constexpr double kLockTimeoutMs = 1000;

void free_run() {
    const uint32_t t = s_freeTick + (uint32_t)(emscripten_get_now() - s_freeBase);
    if ((int32_t)(t - s_now) > 0) s_now = t;
}
/* Something waiting for time to pass by reading the tick in a loop would wait
 * forever for a clock that only moves between loops: past this many reads
 * without a loop the tick creeps on by itself (and says who spun). */
uint32_t s_reads = 0;
uint32_t s_spins = 0;
uint32_t twin_tick(void) {
    if (!s_lock) { free_run(); return s_now; }
    if (++s_reads > 100000) {
        if (s_reads == 100001) {
            ++s_spins;
            emscripten_log(EM_LOG_CONSOLE | EM_LOG_WARN | EM_LOG_C_STACK, "[twin] spinning on lv_tick in lockstep");
        }
        if ((s_reads & 63) == 0) ++s_now;
    }
    return s_now;
}

/* ── the indevs ─────────────────────────────────────────────────────────
 * The pointer's chain, outermost first: +LCD offset → the board's swipe
 * recogniser (touch_gesture, which judges in LCD pixels: its top band is an
 * absolute y) → ptr_base (the board's panel in LCD pixels, or SDL's mouse). */
lv_indev_read_cb_t s_ptrSdl = nullptr, s_ptrGesture = nullptr, s_encSdl = nullptr;

struct Touch { bool down; int32_t x, y; };
Touch s_touch{false, 0, 0};            // lockstep: the board's sample being read
int32_t s_encDiff = 0;                 // lockstep: the board's sample being read
bool s_encDown = false;

std::deque<Touch> s_looseTouch;        // loose: one state per read, the last one held
Touch s_looseCur{false, 0, 0};
bool s_looseHeld = false;              // a loose contact is down (SDL's mouse is ignored)
int32_t s_looseDiff = 0;
std::deque<bool> s_looseBtn;
bool s_looseBtnCur = false;

void ptr_base(lv_indev_t* indev, lv_indev_data_t* d) {
    if (s_lock) {
        d->state = s_touch.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        d->point.x = s_touch.x; d->point.y = s_touch.y;
        return;
    }
    if (!s_looseTouch.empty()) { s_looseCur = s_looseTouch.front(); s_looseTouch.pop_front(); s_looseHeld = true; }
    if (s_looseHeld) {
        d->state = s_looseCur.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        d->point.x = s_looseCur.x; d->point.y = s_looseCur.y;
        if (!s_looseCur.down && s_looseTouch.empty()) s_looseHeld = false;
        return;
    }
    if (s_ptrSdl) s_ptrSdl(indev, d);
    d->point.x -= lcd_x(); d->point.y -= lcd_y();
}
void ptr_outer(lv_indev_t* indev, lv_indev_data_t* d) {
    if (s_ptrGesture) s_ptrGesture(indev, d); else ptr_base(indev, d);
    d->point.x += lcd_x(); d->point.y += lcd_y();
}
void enc_read(lv_indev_t* indev, lv_indev_data_t* d) {
    if (s_lock) {
        d->enc_diff = (int16_t)s_encDiff; s_encDiff = 0;
        d->state = s_encDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    if (s_encSdl) s_encSdl(indev, d);
    d->enc_diff = (int16_t)(d->enc_diff + s_looseDiff); s_looseDiff = 0;
    if (!s_looseBtn.empty()) { s_looseBtnCur = s_looseBtn.front(); s_looseBtn.pop_front(); }
    if (s_looseBtnCur) d->state = LV_INDEV_STATE_PRESSED;
}

void read_timers(bool run) {
    for (lv_indev_t* i : {s_ptr, s_enc}) {
        lv_timer_t* t = i ? lv_indev_get_read_timer(i) : nullptr;
        if (!t) continue;
        if (run) lv_timer_resume(t); else lv_timer_pause(t);
    }
}

/* ── lockstep frames ──────────────────────────────────────────────────── */
enum : uint8_t { F_TOUCH = 0, F_ENC = 1, F_PAD = 2, F_POWER = 3, F_HOST = 4, F_CLOCK = 5, F_LOST = 6, F_STATUS = 8 };
struct Frame { uint8_t kind; uint32_t tick; uint8_t p[5]; std::string host; };
std::deque<Frame> s_frames;

struct Stats {
    uint32_t frames = 0, touch = 0, enc = 0, pad = 0, power = 0, host = 0, clock = 0, lost = 0;
    uint32_t lateMax = 0;              // ms a frame's tick was already behind the twin (must stay 0)
    uint32_t checks = 0, same = 0, late = 0;
    uint32_t dApp = 0, dCfg = 0, dHash = 0, dFocus = 0, dScroll = 0, dGeo = 0, dOvl = 0;
    uint32_t fixApp = 0, fixCfg = 0, fixReopen = 0, fixFocus = 0, fixScroll = 0, gaveUp = 0;
    uint32_t inputs = 0;               // frames applied (touch, encoder, pad, power)
    std::string board, twin;           // the last pair compared
} s_st;
bool s_resync = false;
/* The board's wall clock (kind 8): time() here is that, moved on by lv_tick. */
time_t s_epoch = 0;
uint32_t s_epochTick = 0;
bool s_epochKnown = false;

/* From here on the board's tick is the clock, shifted only as far as it takes
 * never to go back. */
void anchor(uint32_t boardTick) { s_k = (int32_t)(s_now - boardTick) > 0 ? s_now - boardTick : 0; }

void enter_lock() {
    s_lock = true;
    anchor(s_frames.empty() ? s_vouched : s_frames.front().tick);
    s_touch = Touch{false, 0, 0}; s_encDiff = 0; s_encDown = false;
    s_looseTouch.clear(); s_looseHeld = false; s_looseBtn.clear(); s_looseBtnCur = false; s_looseDiff = 0;
    read_timers(false);
    s_resync = true;
    printf("[twin] lockstep with the board (tick %u, k %u)\n", (unsigned)s_vouched, (unsigned)s_k);
}
void leave_lock() {
    s_lock = false;
    s_freeBase = emscripten_get_now(); s_freeTick = s_now;
    read_timers(true);
    printf("[twin] the board went quiet: running free\n");
}

void apply_frame(const Frame& f) {
    switch (f.kind) {
    case F_TOUCH:
        s_touch = Touch{f.p[0] != 0, (int32_t)((f.p[1] << 7) | f.p[2]), (int32_t)((f.p[3] << 7) | f.p[4])};
        ++s_st.touch; ++s_st.inputs;
        lv_indev_read(s_ptr);
        break;
    case F_ENC:
        s_encDown = f.p[0] != 0; s_encDiff = (int32_t)f.p[1] - 64;
        ++s_st.enc; ++s_st.inputs;
        lv_indev_read(s_enc);
        break;
    case F_PAD:
        ++s_st.pad; ++s_st.inputs;
        if (f.p[0] < 16) {
            if (f.p[1]) crosspad::getPadManager().handlePadPress(f.p[0], f.p[1]);
            else        crosspad::getPadManager().handlePadRelease(f.p[0]);
        }
        break;
    case F_POWER:
        ++s_st.power; ++s_st.inputs;
        if (f.p[0]) crosspad_gui::powerToggleQuickSettings(); else crosspad_gui::powerBack();
        break;
    case F_HOST: {
        /* A USB host's message, dispatched where the board's MIDI task does
         * (platform-idf midiCallbacks.cpp). */
        ++s_st.host; ++s_st.inputs;
        const auto* m = reinterpret_cast<const uint8_t*>(f.host.data());
        const size_t n = f.host.size();
        if (n < 2) break;
        const uint8_t ch = m[0] & 0x0F;
        auto& in = crosspad::getMidiInputHandler();
        switch (m[0] & 0xF0) {
        case 0x90: if (n >= 3) in.handleNoteOn(ch, m[1], m[2]); break;
        case 0x80: if (n >= 3) in.handleNoteOff(ch, m[1], m[2]); break;
        case 0xB0: if (n >= 3) in.handleSampleControlChange(ch + 1, m[1], m[2]); break;
        case 0xA0: if (n >= 3) in.handleAftertouch(ch, m[1], m[2]); break;
        case 0xE0: if (n >= 3) in.handleSamplePitchBend(ch + 1, (int16_t)((m[1] | (m[2] << 7)) - 8192)); break;
        case 0xC0: in.handleSampleProgramChange(ch + 1, m[1]); break;
        case 0xF0:
            if (auto* h = crosspad::getPlatformServices().stm32MessageHandler) h->handleMessage(m, (unsigned)n);
            break;
        }
        break;
    }
    case F_STATUS: {
        /* The board's telemetry, at the tick it moved (twin_mirror.h kind 8). */
        const auto* p = reinterpret_cast<const uint8_t*>(f.host.data());
        if (f.host.size() < 16) break;
        auto get7 = [p](int at, int n) { uint64_t v = 0; for (int i = 0; i < n; ++i) v = (v << 7) | p[at + i]; return v; };
        status.batteryPct = p[0] == 127 ? (int8_t)-1 : (int8_t)p[0];
        status.stm32.chargingStatus = p[1] & 1; status.stm32.sdDetected = (p[1] >> 1) & 1;
        status.hostSeenUs = (p[1] & 4) ? (crosspad::getClock().getTimeUs() | 1) : 0;
        status.fwUpdateAvailable = (p[1] >> 3) & 1;
        status.bluetoothActive = (p[1] >> 4) & 1; status.bluetoothConnected = (p[1] >> 5) & 1;
        status.wifiConnected = (p[1] >> 6) & 1;
        status.hostCpuPct = p[2];
        status.hostRamUsedMb = (uint32_t)get7(3, 3); status.hostRamTotalMb = (uint32_t)get7(6, 3);
        status.hostDawsInstalled = p[9]; status.hostDawsRunning = p[10];
        s_epoch = (time_t)get7(11, 5); s_epochTick = f.tick + s_k; s_epochKnown = true;
        break;
    }
    case F_LOST:
        ++s_st.lost;
        s_resync = true;
        break;
    default: break;
    }
}

/* ── the safety net ─────────────────────────────────────────────────────── */
struct Desc {
    std::string app, cfg, focus, scroll;
    unsigned long hash = 0, geo = 0, ovl = 0;
    bool ok = false;
};

std::string field(const char* s, const char* key) {
    const char* p = strstr(s, key);
    if (!p) return "";
    p += strlen(key);
    const char* e = p;
    while (*e && *e != ' ' && *e != '\r' && *e != '\n') ++e;
    return std::string(p, e - p);
}
Desc parse(const char* s) {
    Desc d;
    d.app = field(s, " app="); d.cfg = field(s, " cfg="); d.focus = field(s, " focus="); d.scroll = field(s, " scroll=");
    d.hash = strtoul(field(s, " hash=").c_str(), nullptr, 16);
    d.geo = strtoul(field(s, " geo=").c_str(), nullptr, 16);
    d.ovl = strtoul(field(s, " ovl=").c_str(), nullptr, 16);
    d.ok = !d.app.empty() && !d.focus.empty();
    return d;
}
lv_group_t* nav_group() { return s_enc ? lv_indev_get_group(s_enc) : nullptr; }
lv_obj_t* lcd_root() { lv_obj_t* a = crosspad_app_container(); return a ? lv_obj_get_parent(a) : nullptr; }

std::string running_name() {
    auto* running = crosspad_gui::AppOrchestrator::getInstance().getRunningApp();
    return running && running->isStarted() ? running->getName() : "-";
}
/* The board's telemetry (TWIN_STATE st=, lvl=): what the status bar and the
 * drawer show that no input decides. Taken as it comes, every check. */
bool s_heapKnown = false;
uint8_t s_ramPct = 0, s_psramPct = 0;
bool s_arKnown = false, s_stmConfig = false;
crosspad_gui::AudioRouteState s_ar{};
std::string s_boardSong;                // the project open on the board's card ("" unknown)
bool s_songPending = false;             // asked the twin's engine to open it
/* The settings: when the board's set= hash stays apart from the twin's, the
 * page fetches them (TWIN_SETTINGS) and they are loaded here. */
int s_setApart = 0;
bool s_wantSettings = false;
std::string s_settingsIn;                // fetched, applied on the LVGL task


std::string unescape(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) { o += (char)strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += v[i];
    }
    return o;
}
std::string escape(const std::string& v) {
    std::string o;
    char b[4];
    for (char c : v) {
        if (c == ' ' || c == '%') { snprintf(b, sizeof b, "%%%02X", (unsigned)(uint8_t)c); o += b; }
        else o += c;
    }
    return o.empty() ? "-" : o;
}
std::string twin_song() {
    auto* se = crosspad::getPlatformServices().songEngine;
    if (!se) return "";
    std::lock_guard<std::recursive_mutex> g(se->songMutex());
    return se->song().name;
}
void sync_to(const std::string& app, int launcher);

void take_telemetry(const char* reply) {
    unsigned V = 0, R = 0, B = 0;
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    if (cfg && sscanf(field(reply, " lvl=").c_str(), "V%uR%uB%u", &V, &R, &B) == 3) {
        cfg->masterFX.outVolumeCodec[0][0] = (uint8_t)V; cfg->RGBbrightness = (uint8_t)R; cfg->LCDbrightness = (uint8_t)B;
    }
    int b = 0, c = 0, sd = 0, h = 0, u = 0, a = 0, n = 0, w = 0;
    unsigned m = 0, p = 0;
    if (sscanf(field(reply, " st=").c_str(), "b%dc%ds%dh%du%da%dn%dw%dm%up%u", &b, &c, &sd, &h, &u, &a, &n, &w, &m, &p) == 10) {
        s_ramPct = (uint8_t)m; s_psramPct = (uint8_t)p; s_heapKnown = true;   // the rest rides the stream (kind 8)
    }
    unsigned r8[8];
    int have = 0, sc = 0;
    if (sscanf(field(reply, " ar=").c_str(), "%d,%u,%u,%u,%u,%u,%u,%u,%u", &have, &r8[0], &r8[1], &r8[2], &r8[3], &r8[4],
               &r8[5], &r8[6], &r8[7]) == 9) {
        s_arKnown = have != 0;
        s_ar = crosspad_gui::AudioRouteState{(uint8_t)r8[0], {(uint8_t)r8[1], (uint8_t)r8[2]}, {(uint8_t)r8[3], (uint8_t)r8[4]},
                                             {(uint8_t)r8[5], (uint8_t)r8[6]}, (uint8_t)r8[7]};
    }
    if (sscanf(field(reply, " sc=").c_str(), "%d", &sc) == 1) s_stmConfig = sc != 0;
    const std::string ble = field(reply, " ble=");
    if (!ble.empty()) {
        const size_t c = ble.find(',');
        twin_board_set_ble(ble.substr(0, c).c_str(), c == std::string::npos ? "" : ble.substr(c + 1).c_str());
    }
    const std::string imu = field(reply, " imu=");
    if (!imu.empty()) twin_board_set_imu((uint32_t)strtoul(imu.c_str(), nullptr, 10));
    const std::string fwl = field(reply, " fwl=");
    if (!fwl.empty()) snprintf(status.fwLatest, sizeof status.fwLatest, "%s", fwl == "-" ? "" : fwl.c_str());
    auto* cfgs = crosspad::CrosspadSettings::getInstance();
    const std::string set = field(reply, " set=");
    if (cfgs && !set.empty()) {
        char mine[12];
        snprintf(mine, sizeof mine, "%08lx", (unsigned long)twin_state::settings_hash(*cfgs));
        s_setApart = set == mine ? 0 : s_setApart + 1;
        if (s_setApart >= 2 && s_settingsIn.empty()) s_wantSettings = true;
    }
    // The song project: opened here too, and a screen showing it rebuilt once it is.
    const std::string song = field(reply, " song=");
    if (!song.empty() && song != "-") {
        s_boardSong = unescape(song);
        auto* se = crosspad::getPlatformServices().songEngine;
        const std::string cur = twin_song();
        if (se && cur != s_boardSong && !s_songPending) {
            printf("[twin] opening the board's song '%s' (here '%s')\n", s_boardSong.c_str(), cur.c_str());
            s_songPending = se->requestOpen(s_boardSong);
        } else if (se && s_songPending && cur == s_boardSong && se->openState() == crosspad::SongOpState::Ready) {
            s_songPending = false;
            const std::string app = running_name();
            if (app != "-") { crosspad_app_go_home(); sync_to(app, -1); }
        }
    }
}

std::string own_state() {
    static char buf[640];
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    int n = snprintf(buf, sizeof buf, "TWIN_STATE tick=%lu app=%s cfg=L%uT%uQ%uP%u ", (unsigned long)(s_now - s_k),
                     running_name().c_str(), cfg ? (unsigned)cfg->launcherStyle : 0u,
                     cfg ? (unsigned)cfg->themeColorIndex : 0u, cfg && cfg->quickSettingsEnabled ? 1u : 0u,
                     cfg ? (unsigned)cfg->perfStatsFlags : 0u);
    n += (int)twin_state::describe(buf + n, sizeof buf - n, crosspad_app_container(), crosspad_gui::getOverlayParent(), nav_group());
    snprintf(buf + n, sizeof buf - n, " lvl=V%uR%uB%u st=b%dc%ds%dh%du%da%dn%dw%dm%up%u",
             cfg ? (unsigned)cfg->masterFX.outVolumeCodec[0][0] : 0u, cfg ? (unsigned)cfg->RGBbrightness : 0u,
             cfg ? (unsigned)cfg->LCDbrightness : 0u, (int)status.batteryPct, status.stm32.chargingStatus ? 1 : 0,
             status.stm32.sdDetected ? 1 : 0, status.hostAlive(crosspad::getClock().getTimeUs()) ? 1 : 0,
             status.fwUpdateAvailable ? 1 : 0, status.bluetoothActive ? 1 : 0, status.bluetoothConnected ? 1 : 0,
             status.wifiConnected ? 1 : 0, (unsigned)s_ramPct, (unsigned)s_psramPct);
    n = (int)strlen(buf);
    crosspad_gui::AudioRouteState ar{};
    const bool haveAr = crosspad_gui::getGuiPlatform().getAudioRoute(ar);
    snprintf(buf + n, sizeof buf - n, " ar=%d,%u,%u,%u,%u,%u,%u,%u,%u sc=%d host=%u,%lu,%lu,%u,%u fwl=%s song=%s",
             haveAr ? 1 : 0, ar.captureCodec, ar.adcInput[0], ar.adcInput[1], ar.dacOutput[0], ar.dacOutput[1],
             ar.mute[0], ar.mute[1], ar.preset, crosspad_gui::getGuiPlatform().hasStmConfigWrite() ? 1 : 0,
             (unsigned)status.hostCpuPct, (unsigned long)status.hostRamUsedMb, (unsigned long)status.hostRamTotalMb,
             (unsigned)status.hostDawsInstalled, (unsigned)status.hostDawsRunning,
             status.fwLatest[0] ? status.fwLatest : "-", escape(twin_song()).c_str());
    n = (int)strlen(buf);
    if (cfg) snprintf(buf + n, sizeof buf - n, " set=%08lx ble=%s,%s imu=%lu", (unsigned long)twin_state::settings_hash(*cfg),
                      twin_ble_state(), *twin_ble_peer() ? twin_ble_peer() : "-", (unsigned long)twin_imu_sent());
    return buf;
}


void fix_scroll(const std::string& board, const std::string& twin) {
    lv_obj_t* root = lcd_root();
    lv_obj_t* ovl = crosspad_gui::getOverlayParent();
    // the board's offsets, and back to 0 for what only the twin has scrolled
    auto each = [](const std::string& list, auto fn) {
        if (list == "-") return;
        size_t at = 0;
        while (at < list.size()) {
            size_t e = list.find(',', at);
            if (e == std::string::npos) e = list.size();
            const std::string it = list.substr(at, e - at);
            long x = 0, y = 0;
            const size_t c1 = it.find(':'), c2 = it.find(':', c1 + 1);
            if (c1 != std::string::npos && c2 != std::string::npos) {
                x = strtol(it.c_str() + c1 + 1, nullptr, 10); y = strtol(it.c_str() + c2 + 1, nullptr, 10);
                fn(it.substr(0, c1), x, y);
            }
            at = e + 1;
        }
    };
    each(twin, [&](const std::string& path, long, long) {
        if (board.find(path + ":") == std::string::npos)
            if (lv_obj_t* o = twin_state::obj_at(root, ovl, path.c_str())) lv_obj_scroll_to(o, 0, 0, LV_ANIM_OFF);
    });
    each(board, [&](const std::string& path, long x, long y) {
        if (lv_obj_t* o = twin_state::obj_at(root, ovl, path.c_str())) lv_obj_scroll_to(o, x, y, LV_ANIM_OFF);
    });
}

/* A difference is only acted on when it held across two checks with the board
 * standing still in between and no input applied here: mid-animation, or a
 * check answered a moment before a frame the twin has already applied, is
 * not a desync. A lost frame or a fresh lockstep acts at once. */
Desc s_prevBoard;
bool s_prevDiff = false;
std::string s_fixKey;
int s_fixTries = 0;
uint32_t s_prevInputs = 0;

void check(const Desc& b, uint32_t boardTick) {
    const Desc t = parse(own_state().c_str());
    ++s_st.checks;
    if ((int32_t)(s_now - (boardTick + s_k)) > 0) ++s_st.late;
    const bool dApp = strcasecmp(b.app.c_str(), t.app.c_str()) != 0;
    const bool dCfg = !b.cfg.empty() && b.cfg != t.cfg;
    const bool dHash = b.hash != t.hash, dFocus = b.focus != t.focus, dScroll = b.scroll != t.scroll;
    const bool dGeo = b.geo != t.geo, dOvl = b.ovl != t.ovl;
    s_st.dApp += dApp; s_st.dCfg += dCfg; s_st.dHash += dHash; s_st.dFocus += dFocus; s_st.dScroll += dScroll;
    s_st.dGeo += dGeo; s_st.dOvl += dOvl;
    const bool diff = dApp || dCfg || dHash || dFocus || dScroll;
    if (!diff) ++s_st.same;

    const bool still = s_prevBoard.ok && s_prevBoard.app == b.app && s_prevBoard.cfg == b.cfg && s_prevBoard.hash == b.hash &&
                       s_prevBoard.focus == b.focus && s_prevBoard.scroll == b.scroll;
    const bool act = diff && (s_resync || (s_prevDiff && still && s_st.inputs == s_prevInputs));
    s_prevBoard = b; s_prevDiff = diff; s_prevInputs = s_st.inputs;
    if (!act) { if (!diff) s_resync = false; return; }
    s_resync = false;

    /* The same fix for the same board screen, made twice already and still
     * apart: the twin cannot get there this way; counted, not repeated. */
    const std::string key = b.app + b.cfg + std::to_string(b.hash) + b.focus + b.scroll + "|" + t.app + t.cfg +
                            std::to_string(t.hash) + t.focus + t.scroll;
    if (key == s_fixKey && ++s_fixTries > 2) { if (s_fixTries == 3) ++s_st.gaveUp; return; }
    if (key != s_fixKey) { s_fixKey = key; s_fixTries = 1; }

    if (dCfg) {
        unsigned L = 0, T = 0, Q = 0, P = 0;
        if (sscanf(b.cfg.c_str(), "L%uT%uQ%uP%u", &L, &T, &Q, &P) == 4) {
            auto* cfg = crosspad::CrosspadSettings::getInstance();
            printf("[twin] correct: settings %s -> %s\n", t.cfg.c_str(), b.cfg.c_str());
            ++s_st.fixCfg;
            const bool theme = cfg->themeColorIndex != T;
            cfg->launcherStyle = (crosspad::LauncherStyle)L; cfg->themeColorIndex = (uint8_t)T;
            cfg->quickSettingsEnabled = Q != 0; cfg->perfStatsFlags = (uint8_t)P;
            if (theme) reinitStyles();
            crosspad_app_go_home();                    // rebuilt from them
            if (b.app != "-") sync_to(b.app, -1);
            return;
        }
    }
    if (dApp) {
        printf("[twin] correct: app %s -> %s\n", t.app.c_str(), b.app.c_str());
        ++s_st.fixApp;
        sync_to(b.app, -1);
        return;                                        // the rest is checked on the new screen
    }
    if (dFocus) {
        lv_group_t* g = nav_group();
        std::string path = b.focus;
        const bool edit = !path.empty() && path.back() == 'e';
        if (edit) path.pop_back();
        lv_obj_t* o = twin_state::obj_at(lcd_root(), crosspad_gui::getOverlayParent(), path.c_str());
        printf("[twin] correct: focus %s -> %s%s\n", t.focus.c_str(), b.focus.c_str(), o && g ? "" : " (not found)");
        if (o && g) {
            ++s_st.fixFocus;
            lv_group_focus_obj(o);
            lv_group_set_editing(g, edit);
        }
    }
    if (dScroll || dFocus) {
        printf("[twin] correct: scroll %s -> %s\n", t.scroll.c_str(), b.scroll.c_str());
        ++s_st.fixScroll;
        fix_scroll(b.scroll, t.scroll);
    }
    if (dHash && !dFocus && !dScroll) {
        printf("[twin] correct: %s rebuilt (tree %08lx, board %08lx)\n", b.app.c_str(), t.hash, b.hash);
        ++s_st.fixReopen;
        crosspad_app_go_home();
        if (b.app != "-") sync_to(b.app, -1);
        return;
    }
}

struct Check { Desc d; uint32_t tick; std::string raw; };
std::deque<Check> s_checks;

/* ── loose input (the page's own) ─────────────────────────────────────── */
enum Kind { PAD, ENC, ENC_PRESS, TOUCH, POWER, SYNC };
struct Input { Kind kind; int a, b, c; std::string s; };
std::deque<Input> s_queue;

/* Put the twin where the board is: the same launcher look and the same app
 * (by name, as the board's UI_STATE reports it; "-" is the launcher). The
 * board's own APP_START path: the orchestrator's onAppSelected. */
void sync_to(const std::string& app, int launcher) {
    auto* cfg = crosspad::CrosspadSettings::getInstance();
    bool rebuild = false;
    if (cfg && launcher >= 0 && (int)cfg->launcherStyle != launcher) {
        cfg->launcherStyle = (crosspad::LauncherStyle)launcher;
        rebuild = true;
    }
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    crosspad_gui::ILvglApp* running = orch.getRunningApp();
    if (running && !running->isStarted()) running = nullptr;
    const std::string cur = running ? running->getName() : "-";
    if (app == "-" || app.empty()) {
        if (running || rebuild) crosspad_app_go_home();
        return;
    }
    if (!strcasecmp(cur.c_str(), app.c_str()) && !rebuild) return;
    crosspad_gui::ILvglApp* want = nullptr;
    for (const auto& a : orch.getApps())
        if (a && !strcasecmp(a->getName(), app.c_str())) want = a.get();
    if (!want) { printf("[twin] no app '%s' here\n", app.c_str()); return; }
    if (running || rebuild) crosspad_app_go_home();
    printf("[twin] following the board into %s\n", want->getName());
    crosspad_gui::AppOrchestrator::onAppSelected(want, crosspad_app_container());
}

void apply_loose(const Input& in) {
    switch (in.kind) {
    case PAD:
        if (in.b > 0) crosspad::getPadManager().handlePadPress(in.a, in.b);
        else          crosspad::getPadManager().handlePadRelease(in.a);
        break;
    case ENC:       s_looseDiff += in.a; break;
    case ENC_PRESS: s_looseBtn.push_back(in.a != 0); break;
    case TOUCH:     // st 1 press, 2 drag, 0 release — LCD pixels
        s_looseTouch.push_back(Touch{in.a != 0, in.b, in.c});
        break;
    case POWER:
        if (in.a) crosspad_gui::powerButtonPress();
        else      crosspad_gui::powerButtonRelease();
        break;
    case SYNC:
        sync_to(in.s, in.a);
        break;
    }
}

} // namespace

namespace remote {
void start(lv_display_t* disp) {
    s_disp = disp;
    lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, nullptr);

    s_now = lv_tick_get();
    s_freeBase = emscripten_get_now(); s_freeTick = s_now;
    lv_tick_set_cb(twin_tick);

    for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) { s_ptr = i; break; }
    s_enc = sdl_hal_get_encoder();
    indev_encoderTop = s_enc;
    encoderTop_group = lv_group_create();
    twin_board_attach();
    if (s_ptr) {
        s_ptrSdl = lv_indev_get_read_cb(s_ptr);
        lv_indev_set_read_cb(s_ptr, ptr_base);
        // as display.h wires indev_touchpad on the board
        lv_indev_add_event_cb(s_ptr, crosspad_gui::gui_touchpad_event_cb, LV_EVENT_ALL, nullptr);
        crosspad_gui::touch_gesture_attach(s_ptr);
        s_ptrGesture = lv_indev_get_read_cb(s_ptr);
        lv_indev_set_read_cb(s_ptr, ptr_outer);
    }
    if (s_enc) {
        s_encSdl = lv_indev_get_read_cb(s_enc);
        lv_indev_set_read_cb(s_enc, enc_read);
    }
}
void stop() {}
void process_pending() {
    s_reads = 0;
    wasm_io_service();   // MIDI in, helper services, the audio clock (web_io.cpp)

    const bool fresh = emscripten_get_now() - s_lastFrameAt <= kLockTimeoutMs;
    if (s_lock && !fresh) leave_lock();
    if (!s_lock && fresh) enter_lock();
    // A board that rebooted under an open page starts its ticks again.
    if (s_lock && (int32_t)(s_now - (s_vouched + s_k)) > 10000) {
        anchor(s_frames.empty() ? s_vouched : s_frames.front().tick);
        s_resync = true;
    }

    if (s_lock) {
        // Everything the board has vouched for, in its order, each at its tick.
        while (!s_frames.empty()) {
            const Frame f = s_frames.front();
            s_frames.pop_front();
            const uint32_t t = f.tick + s_k;
            if ((int32_t)(t - s_now) > 0) {
                s_now = t;
                lv_timer_handler();                     // what was due before this read
            } else if ((int32_t)(s_now - t) > 0 && (uint32_t)(s_now - t) > s_st.lateMax) {
                s_st.lateMax = s_now - t;
            }
            apply_frame(f);
        }
        if ((int32_t)(s_vouched + s_k - s_now) > 0) s_now = s_vouched + s_k;
    } else {
        while (!s_queue.empty()) {
            const Input in = s_queue.front();
            s_queue.pop_front();
            apply_loose(in);
        }
    }
    if (!s_settingsIn.empty()) {
        auto* cfg = crosspad::CrosspadSettings::getInstance();
        const uint8_t theme = cfg ? cfg->themeColorIndex : 0;
        if (cfg && twin_state::settings_apply(*cfg, s_settingsIn)) {
            printf("[twin] took the board's settings (%u bytes)\n", (unsigned)s_settingsIn.size());
            if (cfg->themeColorIndex != theme) reinitStyles();
            const std::string app = running_name();
            crosspad_app_go_home();                    // rebuilt from them
            if (app != "-") sync_to(app, -1);
        }
        s_settingsIn.clear();
        s_wantSettings = false;
        s_setApart = 0;
    }
    while (!s_checks.empty()) {
        const Check c = s_checks.front();
        s_checks.pop_front();
        if (s_lock) { take_telemetry(c.raw.c_str()); check(c.d, c.tick); }
    }
}
void set_kit_loader(void (*)(int), bool (*)()) {}
void set_audio_device_ctl(const AudioDeviceCtl&) {}
} // namespace remote


/** The board's Stack/PSRAM use for the drawer (the PC stub asks, PcPlatformStubs.cpp). */
extern "C" time_t __real_time(time_t* t);
extern "C" time_t __wrap_time(time_t* t) {
    const time_t v = s_epochKnown ? s_epoch + (time_t)((int32_t)(s_now - s_epochTick) / 1000) : __real_time(nullptr);
    if (t) *t = v;
    return v;
}

extern "C" bool wasm_twin_audio_route(crosspad_gui::AudioRouteState* out) {
    if (!s_arKnown || !out) return false;
    *out = s_ar;
    return true;
}
extern "C" bool wasm_twin_stm_config(void) { return s_stmConfig; }

extern "C" bool wasm_twin_heap(uint8_t* ramPct, uint8_t* psramPct) {
    if (!s_heapKnown) return false;
    *ramPct = s_ramPct; *psramPct = s_psramPct;
    return true;
}

extern "C" {

/** The pad LEDs, 16 × RGB and then the strip's brightness (0..255), as the firmware's
 *  PadLedController drives them -- what the board's LED_STATE reports and its pads show
 *  (not PadManager's colour, which a pad keeps while it is dark); valid until the next call. */
EMSCRIPTEN_KEEPALIVE const uint8_t* wasm_pad_colors() {
    static uint8_t rgb[16 * 3 + 1];
    auto& leds = crosspad::getPadLedController();
    for (uint8_t i = 0; i < 16; ++i) {
        const crosspad::RgbColor c = leds.getPadColor(i);
        rgb[i * 3] = c.R; rgb[i * 3 + 1] = c.G; rgb[i * 3 + 2] = c.B;
    }
    rgb[48] = (uint8_t)leds.getBrightness();
    return rgb;
}

/** Frames LVGL has finished so far — the page re-uploads the LCD when it moves. */
EMSCRIPTEN_KEEPALIVE uint32_t wasm_lcd_frame() { return s_frame; }

/** The LCD as 320×240 RGBA, top row first; valid until the next call. */
EMSCRIPTEN_KEEPALIVE uint8_t* wasm_lcd_rgba() {
    if (!s_disp) return nullptr;
    lv_draw_buf_t* buf = lv_display_get_buf_active(s_disp);
    if (!buf || !buf->data) return nullptr;
    const uint32_t stride = buf->header.stride;
    uint8_t* out = s_rgba;
    for (int y = 0; y < Stm32EmuWindow::LCD_H; ++y) {
        const uint8_t* row = buf->data + (size_t)(y + lcd_y()) * stride + lcd_x() * 4;
        for (int x = 0; x < Stm32EmuWindow::LCD_W; ++x, row += 4, out += 4) {
            out[0] = row[2]; out[1] = row[1]; out[2] = row[0]; out[3] = 255;   // BGRA → RGBA
        }
    }
    return s_rgba;
}

/** One board frame, F0 7D 1F 09 <kind> <tick 4x7> <payload> F7, at page time `at` (ms). */
EMSCRIPTEN_KEEPALIVE void wasm_twin_frame(const uint8_t* d, int len, double at) {
    if (len < 10 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x1F || d[3] != 0x09) return;
    Frame f{};
    f.kind = d[4];
    f.tick = ((uint32_t)d[5] << 21) | ((uint32_t)d[6] << 14) | ((uint32_t)d[7] << 7) | d[8];
    for (int i = 0; i < 5 && 9 + i < len - 1; ++i) f.p[i] = d[9 + i];
    if (f.kind == F_HOST) {                                   // 7-bit groups back to bytes
        for (int i = 9; i < len - 1;) {
            const uint8_t hi = d[i++];
            for (int k = 0; k < 7 && i < len - 1; ++k) f.host += (char)(d[i++] | (((hi >> k) & 1) << 7));
        }
    }
    (void)at;
    s_lastFrameAt = emscripten_get_now();
    ++s_st.frames;
    // Frames arrive in the board's tick order: each one vouches for its tick.
    if (s_st.frames == 1 || (int32_t)(f.tick - s_vouched) > 0 || (int32_t)(s_vouched - f.tick) > 10000) s_vouched = f.tick;
    if (f.kind == F_CLOCK) { ++s_st.clock; return; }
    if (f.kind == F_STATUS) f.host.assign(reinterpret_cast<const char*>(d + 9), (size_t)(len - 10));
    s_frames.push_back(f);
}
/** The board's TWIN_STATE reply, compared on the LVGL task. */
EMSCRIPTEN_KEEPALIVE void wasm_twin_check(const char* reply) {
    if (!reply) return;
    const Desc d = parse(reply);
    if (!d.ok) return;
    s_checks.push_back({d, (uint32_t)strtoul(field(reply, "tick=").c_str(), nullptr, 10), reply});
}
/** The twin's settings stayed apart from the board's: the page should fetch them. */
EMSCRIPTEN_KEEPALIVE int wasm_twin_want_settings() { return s_wantSettings ? 1 : 0; }
/** The board's settings (TWIN_SETTINGS, assembled), loaded on the LVGL task. */
EMSCRIPTEN_KEEPALIVE void wasm_twin_settings(const char* text) {
    s_wantSettings = false;
    if (text && *text) s_settingsIn = text;
}

/** Lockstep on (the board's frames are arriving). */
EMSCRIPTEN_KEEPALIVE int wasm_twin_locked() { return s_lock ? 1 : 0; }
/** The twin's own TWIN_STATE line. */
EMSCRIPTEN_KEEPALIVE const char* wasm_twin_state() {
    static std::string s;
    s = own_state();
    return s.c_str();
}
/** Counters, as JSON. */
EMSCRIPTEN_KEEPALIVE const char* wasm_twin_stats() {
    static char buf[1400];
    snprintf(buf, sizeof buf,
             "{\"lock\":%d,\"now\":%u,\"k\":%u,\"frames\":%u,\"touch\":%u,\"enc\":%u,\"pad\":%u,\"power\":%u,\"host\":%u,"
             "\"clock\":%u,\"lost\":%u,\"lateMax\":%u,\"checks\":%u,\"same\":%u,\"late\":%u,"
             "\"dApp\":%u,\"dCfg\":%u,\"dHash\":%u,\"dFocus\":%u,\"dScroll\":%u,\"dGeo\":%u,\"dOvl\":%u,"
             "\"spins\":%u,\"fixApp\":%u,\"fixCfg\":%u,\"fixReopen\":%u,\"fixFocus\":%u,\"fixScroll\":%u,\"gaveUp\":%u,\"queued\":%u}",
             s_lock ? 1 : 0, (unsigned)s_now, (unsigned)s_k, s_st.frames, s_st.touch, s_st.enc, s_st.pad, s_st.power, s_st.host,
             s_st.clock, s_st.lost, s_st.lateMax, s_st.checks, s_st.same, s_st.late,
             s_st.dApp, s_st.dCfg, s_st.dHash, s_st.dFocus, s_st.dScroll, s_st.dGeo, s_st.dOvl,
             s_spins, s_st.fixApp, s_st.fixCfg, s_st.fixReopen, s_st.fixFocus, s_st.fixScroll, s_st.gaveUp, (unsigned)s_frames.size());
    return buf;
}

/** A pad: velocity > 0 presses, 0 releases. */
EMSCRIPTEN_KEEPALIVE void wasm_pad(int pad, int velocity) {
    if (pad >= 0 && pad < 16) s_queue.push_back({PAD, pad, velocity, 0});
}
/** Encoder detents, in LVGL's enc_diff sense (the firmware's encoder indev). */
EMSCRIPTEN_KEEPALIVE void wasm_encoder(int diff) { if (diff) s_queue.push_back({ENC, diff, 0, 0}); }
EMSCRIPTEN_KEEPALIVE void wasm_encoder_press(int down) { s_queue.push_back({ENC_PRESS, down ? 1 : 0, 0, 0}); }
/** The touch panel, in LCD pixels: st 1 press, 2 drag, 0 release. */
EMSCRIPTEN_KEEPALIVE void wasm_touch(int st, int x, int y) { s_queue.push_back({TOUCH, st, x, y}); }
EMSCRIPTEN_KEEPALIVE void wasm_power(int down) { s_queue.push_back({POWER, down ? 1 : 0, 0, 0}); }
/** Follow the board: its running app ("-" = launcher) and launcher style (-1 = unknown). */
EMSCRIPTEN_KEEPALIVE void wasm_ui_sync(const char* app, int launcher) {
    s_queue.push_back({SYNC, launcher, 0, 0, app ? app : "-"});
}
/** The twin's running app ("-" on the launcher). */
EMSCRIPTEN_KEEPALIVE const char* wasm_ui_app() {
    static std::string name;
    name = running_name();
    return name.c_str();
}

} // extern "C"
