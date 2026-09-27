#pragma once

/**
 * @file RemoteControl.hpp
 * @brief Lightweight TCP server for external control of the CrossPad simulator.
 *
 * Listens on localhost:19840, accepts JSON commands over TCP.
 * Runs in a background thread, dispatches commands to LVGL/SDL on the main thread.
 *
 * Protocol: newline-delimited JSON.
 * Request:  {"cmd":"screenshot"}\n
 * Response: {"ok":true,"data":"base64..."}\n
 *
 * Commands:
 *   screenshot {file?,region?} — PNG of the window (or region "lcd"); the
 *                             reply carries lcd_origin/lcd_size/scale so a
 *                             pixel in the image maps to an LCD coordinate
 *   click {x,y,space?,hold_ms?} — mouse click; space "lcd" (panel coords, as
 *                             screenshots of the panel) or "window" (default);
 *                             the button is held hold_ms (default 120) so the
 *                             30 ms indev poll sees it; reply reports the
 *                             object LVGL will deliver the press to ("hit")
 *   pad_press {pad,vel}     — press pad (0-15), velocity 0-127
 *   pad_release {pad}       — release pad
 *   encoder_rotate {delta}  — rotate encoder by delta steps
 *   encoder_press            — press encoder button
 *   encoder_release          — release encoder button
 *   key {keycode}           — inject SDL keypress
 *   kit_list                — kits the kit manager found
 *   kit_load {kit}          — load one by index, the way KIT_LOAD does on the device
 *   kit_status              — current kit, whether a load is in flight
 *   audio_out_list          — OUT1/OUT2 dropdown entries (index 0 = "(None)")
 *   audio_in_list           — IN1/IN2 dropdown entries (index 0 = "(None)")
 *   audio_out_set {slot,index} — pick OUT device by dropdown index (slot 0/1)
 *   audio_in_set {slot,index}  — pick IN device by dropdown index (slot 0/1)
 *   mix_lvl                 — per-channel/per-output mixer peaks + routing
 *                             (parity with the board's MIX_LVL CDC verb)
 *   pitched_status          — pitched engine: zones/roots, voices, steals
 *                             (parity with the board's PITCHED_STATUS verb)
 *   wave_status             — waveform loader counters (requested/loaded/…)
 *                             (parity with the board's WAVE_STATUS verb)
 *   app_list                — registered apps + the running one
 *                             (parity with the board's APP_LIST verb)
 *   smpl_peak               — sample engine peak, free WAV slots, load
 *                             (parity with the board's SMPL_PEAK verb)
 *   led_state               — pad LED brightness, anim flags, 16 colours
 *                             (parity with the board's LED_STATE verb)
 *   ping                    — health check
 */

#include "lvgl/lvgl.h"

#include <string>
#include <vector>

namespace remote {

/// Start the remote control TCP server on a background thread.
/// @param disp  The main LVGL display (for screenshot capture).
/// Call once after sdl_hal_init().
void start(lv_display_t* disp);

/// Stop the server and join the background thread.
void stop();

/// Must be called periodically from the LVGL task to process queued commands.
/// Safe to call from lv_timer callback.
void process_pending();

/// Register what `kit_load` should call. The kit *manager* is a core service,
/// but starting a load is the sampler's business — it owns the worker and the
/// audio-engine reload — so the platform hands the entry point in rather than
/// this file including an app that may not be installed.
/// @param loader  runs on the LVGL thread; takes a kit index.
/// @param busy    true while a load is in flight.
void set_kit_loader(void (*loader)(int), bool (*busy)());

/// Audio-device control the platform hands in — same reason as the kit loader:
/// the device objects live in crosspad_app.cpp, not here. list() returns the
/// dropdown labels ("(None)" at index 0); select(slot, index, &label) opens the
/// device at that index and reports whether it connected, writing the shown
/// label back. select() blocks (ALSA/PA open) and runs on the server thread.
struct AudioDeviceCtl {
    std::vector<std::string> (*outList)() = nullptr;
    std::vector<std::string> (*inList)()  = nullptr;
    bool (*outSelect)(int slot, int index, std::string& label) = nullptr;
    bool (*inSelect)(int slot, int index, std::string& label)  = nullptr;
};

void set_audio_device_ctl(const AudioDeviceCtl& ctl);

} // namespace remote
