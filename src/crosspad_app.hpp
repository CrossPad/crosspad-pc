#pragma once

/**
 * @file crosspad_app.hpp
 * @brief Shared CrossPad application init — used by both standard and FreeRTOS main.
 *
 * Initializes platform stubs, STM32 emulator window, MIDI, audio,
 * styles, app registry, and loads the launcher screen.
 */

class PcUart;

/// Start the boot on the LVGL task, after lv_init() + sdl_hal_init(): the
/// device body and the boot splash at once, the devices on a worker task
/// behind it. Returns without waiting for them; crosspad_app_tick() does
/// the rest.
void crosspad_app_init();

/// One pass of the LVGL task's loop: lv_timer_handler(), and the boot's
/// remaining steps as they come due (the launcher once the devices are up,
/// the splash's exit once it has played). True on the one pass that built
/// the launcher -- from then on the simulator is up.
bool crosspad_app_tick();

/// Release platform resources that must be cleaned up before exit
/// (virtual audio sinks, RtAudio streams, etc.). Safe to call from
/// signal handlers and atexit; idempotent.
void crosspad_app_shutdown();

/// Return to the launcher main screen (destroys running app, reloads launcher)
void crosspad_app_go_home();

/// Open the app with this launcher name (case-insensitive) as a tap on its
/// tile does. False when no app has that name. LVGL thread.
bool crosspad_app_start(const char* name);

/// Update the status bar icon showing which pad logic is currently active
void crosspad_app_update_pad_icon();

/// Access the global PcUart instance (virtual USB/UART)
PcUart& pc_platform_get_uart();
