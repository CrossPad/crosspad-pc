#pragma once

#include "lvgl/lvgl.h"
#include "EmuEncoder.hpp"
#include "EmuPadGrid.hpp"
#include <cstdint>

/**
 * @brief CrossPad device body rendered in LVGL with embedded LCD area.
 *
 * Assembles the full device visualization (LCD, encoder, pad grid, label)
 * on the active LVGL screen and returns a 320x240 container where the
 * LCD GUI should be rendered.
 */
class Stm32EmuWindow {
public:
    Stm32EmuWindow() = default;

    static constexpr int32_t WIN_W = 490;
    static constexpr int32_t WIN_H = 660;

    /// Build device body on the active screen. Returns the 320x240 LCD container.
    /// Call after sdl_hal_init(WIN_W, WIN_H) and pc_platform_init().
    lv_obj_t* init();

private:
    lv_obj_t* screen_       = nullptr;
    lv_obj_t* lcdContainer_ = nullptr;

    EmuEncoder encoder_;
    EmuPadGrid padGrid_;

    lv_timer_t* updateTimer_ = nullptr;

    void buildLayout();
    lv_obj_t* buildLcdContainer(lv_obj_t* parent);

    static void onUpdateTimer(lv_timer_t* t);
};
