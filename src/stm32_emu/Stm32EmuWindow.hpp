#pragma once

#include "lvgl/lvgl.h"
#include "crosspad-gui/virtual_device/VirtualEncoder.h"
#include "crosspad-gui/virtual_device/VirtualPadGrid.h"
#include "crosspad-gui/virtual_device/VirtualPowerButton.h"
#include "EmuJackPanel.hpp"
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
    ~Stm32EmuWindow();

    static constexpr int32_t WIN_W = 490;
    static constexpr int32_t WIN_H = 680;

    /// Build device body on the active screen. Returns the 320x240 LCD container.
    /// Call after sdl_hal_init(WIN_W, WIN_H) and pc_platform_init().
    lv_obj_t* init();

    /// Forward a MIDI CC value to the virtual encoder rotation.
    /// @param value       Current CC value
    /// @param ccRange     Total distinct CC values before wrap (default 31 for range 0-30)
    /// @param stepsPerRev Physical detents per revolution (default 18)
    void handleEncoderCC(uint8_t value, uint8_t ccRange = 31, uint8_t stepsPerRev = 18);

    /// Forward encoder button press/release state.
    void handleEncoderPress(bool pressed);

    /// Forward mouse wheel delta to the virtual encoder rotation.
    void handleEncoderWheel(int dy);

    /// Access the jack panel for wiring device selection callbacks.
    EmuJackPanel& getJackPanel() { return jackPanel_; }

private:
    lv_obj_t* screen_       = nullptr;
    lv_obj_t* lcdContainer_ = nullptr;

    crosspad_gui::VirtualEncoder     encoder_;
    crosspad_gui::VirtualPadGrid     padGrid_;
    crosspad_gui::VirtualPowerButton powerBtn_;
    EmuJackPanel   jackPanel_;

    lv_timer_t* updateTimer_ = nullptr;

    void buildLayout();
    lv_obj_t* buildLcdContainer(lv_obj_t* parent);

    static void onUpdateTimer(lv_timer_t* t);
};
