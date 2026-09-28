#pragma once

/**
 * @file crosspad_app.hpp
 * @brief Shared CrossPad application init — used by both standard and FreeRTOS main.
 *
 * Initializes platform stubs, STM32 emulator window, MIDI, audio,
 * styles, app registry, and loads the launcher screen.
 * Call after lv_init() + sdl_hal_init().
 */

#include "lvgl/lvgl.h"

class PcUart;

void crosspad_app_init();

/**
 * @brief The display is the board's LCD alone (320x240, the GUI on the active
 *        screen as on the board), with no device body around it. Set before
 *        the display is made (freertos_main.cpp: --lcd). The browser twin of a
 *        board runs this way, so LVGL sees exactly the board's display: the
 *        default theme's sizes, scroll timings and dropdown placement all
 *        follow the display's resolution.
 */
void crosspad_app_set_lcd_only(bool on);
bool crosspad_app_lcd_only();

/** The container apps are started in (the launcher's app area). */
lv_obj_t* crosspad_app_container();

/// Release platform resources that must be cleaned up before exit
/// (virtual audio sinks, RtAudio streams, etc.). Safe to call from
/// signal handlers and atexit; idempotent.
void crosspad_app_shutdown();

/// Return to the launcher main screen (destroys running app, reloads launcher)
void crosspad_app_go_home();

/// Update the status bar icon showing which pad logic is currently active
void crosspad_app_update_pad_icon();

/// Access the global PcUart instance (virtual USB/UART)
PcUart& pc_platform_get_uart();
