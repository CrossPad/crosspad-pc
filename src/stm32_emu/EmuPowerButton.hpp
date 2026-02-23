#pragma once

#include "lvgl/lvgl.h"
#include <cstdint>

/**
 * @brief Emulated power button for the CrossPad PC emulator.
 *
 * Renders a small circular button with a power icon.
 * Click toggles the volume overlay (same as real hardware).
 */
class EmuPowerButton {
public:
    EmuPowerButton() = default;

    /// Build the button on @p parent at the given position / size.
    void create(lv_obj_t* parent, int32_t x, int32_t y, int32_t size);

    /// Apply current pressed state to LVGL objects.
    /// Call from a periodic timer (~30 fps).
    void update();

private:
    lv_obj_t* obj_ = nullptr;

    static void onPressed(lv_event_t* e);
    static void onReleased(lv_event_t* e);
};
