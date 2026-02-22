#pragma once

#include "lvgl/lvgl.h"
#include <cstdint>

/**
 * @brief Visual encoder knob for the STM32 emulator window.
 *
 * Renders a rotary encoder circle with a radial indicator line.
 * Mouse scroll-wheel maps to rotation, middle-click maps to press.
 * An SDL event watcher is registered on create() and removed on destroy.
 */
class EmuEncoder {
public:
    EmuEncoder() = default;
    ~EmuEncoder();

    /// Build the encoder on @p parent at the given position / size.
    void create(lv_obj_t* parent, int32_t x, int32_t y, int32_t size);

    /// Apply current angle + pressed state to the LVGL objects.
    /// Call from a periodic timer (~30 fps).
    void update();

    /// Called by SDL event watcher
    void handleWheelDelta(int dy);
    void handleMiddleButton(bool pressed);

    /// Set encoder angle from an incoming CC value using shortest-path delta.
    /// @param value       Current CC value
    /// @param ccRange     Total number of distinct CC values before wrap (e.g. 31 for 0-30)
    /// @param stepsPerRev Physical encoder detents per full revolution (default 18)
    void setFromCC(uint8_t value, uint8_t ccRange = 31, uint8_t stepsPerRev = 18);

private:
    lv_obj_t* obj_       = nullptr;   // outer knob circle
    lv_obj_t* indicator_ = nullptr;   // radial mark
    int32_t   angle_     = 0;         // accumulated rotation (0.1-degree units)
    bool      pressed_   = false;
    int32_t   size_      = 0;
    int8_t    lastCC_    = -1;        // previous CC value for delta tracking

    static void onClicked(lv_event_t* e);
};
