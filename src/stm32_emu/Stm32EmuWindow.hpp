#pragma once

#include "lvgl/lvgl.h"
#include <cstdint>

/**
 * @brief CrossPad device body rendered in LVGL with embedded LCD area.
 *
 * Builds the full device visualization (pads, encoder, body) on the
 * active LVGL screen and returns a 320x240 container where the LCD
 * GUI should be rendered. Pad clicks route through PadManager.
 */
class Stm32EmuWindow {
public:
    Stm32EmuWindow() = default;
    ~Stm32EmuWindow() = default;

    // Window dimensions (call sdl_hal_init with these)
    static constexpr int32_t WIN_W = 440;
    static constexpr int32_t WIN_H = 650;

    /// Build device body on the active screen. Returns the 320x240 LCD container.
    /// Call after sdl_hal_init(WIN_W, WIN_H) and pc_platform_init().
    lv_obj_t* init();

private:
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* lcdContainer_ = nullptr;

    // Pad grid
    lv_obj_t* padButtons_[16] = {};

    // Encoder
    lv_obj_t* encoderObj_ = nullptr;

    // LED update timer
    lv_timer_t* ledTimer_ = nullptr;

    // Layout builders
    void buildLayout();
    lv_obj_t* buildLcdContainer(lv_obj_t* parent);
    void buildEncoder(lv_obj_t* parent);
    void buildPadGrid(lv_obj_t* parent);

    // Event callbacks
    static void onPadPressed(lv_event_t* e);
    static void onPadReleased(lv_event_t* e);
    static void onEncoderScroll(lv_event_t* e);
    static void onEncoderClick(lv_event_t* e);
    static void onLedUpdateTimer(lv_timer_t* t);
};
