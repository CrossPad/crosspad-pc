#pragma once

/**
 * @file crosspad_app.hpp
 * @brief Shared CrossPad application init — used by both standard and FreeRTOS main.
 *
 * Initializes platform stubs, STM32 emulator window, MIDI, audio,
 * styles, app registry, and loads the launcher screen.
 * Call after lv_init() + sdl_hal_init().
 */

void crosspad_app_init();
