#pragma once

#include "lvgl/lvgl.h"
#include <cstdint>

/**
 * @brief 4x4 pad grid for the STM32 emulator window.
 *
 * Creates 16 clickable pads that route press/release through PadManager.
 * Each pad has a "PAD N" label below it, numbered from bottom-left.
 * LED colours are polled externally via updateLeds().
 */
class EmuPadGrid {
public:
    /// Space reserved below each pad for the label.
    static constexpr int32_t LABEL_MARGIN = 2;   // gap from pad bottom to label
    static constexpr int32_t LABEL_H      = 14;  // label object height

    /// Total grid height for a given pad size / gap.
    static constexpr int32_t gridHeight(int32_t padSize, int32_t padGap)
    {
        return 4 * (padSize + LABEL_MARGIN + LABEL_H) + 3 * padGap;
    }

    EmuPadGrid() = default;

    void create(lv_obj_t* parent, int32_t x, int32_t y,
                int32_t padSize, int32_t padGap);

    void updateLeds();

private:
    lv_obj_t* pads_[16] = {};

    static void onPressed(lv_event_t* e);
    static void onReleased(lv_event_t* e);
};
