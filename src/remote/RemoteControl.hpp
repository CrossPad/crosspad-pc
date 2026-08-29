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
 *   screenshot              — capture SDL framebuffer, return base64 BMP
 *   click {x,y}             — inject mouse click at (x,y) in window coords
 *   pad_press {pad,vel}     — press pad (0-15), velocity 0-127
 *   pad_release {pad}       — release pad
 *   encoder_rotate {delta}  — rotate encoder by delta steps
 *   encoder_press            — press encoder button
 *   encoder_release          — release encoder button
 *   key {keycode}           — inject SDL keypress
 *   kit_list                — kits the kit manager found
 *   kit_load {kit}          — load one by index, the way KIT_LOAD does on the device
 *   kit_status              — current kit, whether a load is in flight
 *   ping                    — health check
 */

#include "lvgl/lvgl.h"

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

} // namespace remote
