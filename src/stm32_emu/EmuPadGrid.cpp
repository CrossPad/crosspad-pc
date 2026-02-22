/**
 * @file EmuPadGrid.cpp
 * @brief 4x4 pad grid for the CrossPad PC emulator.
 */

#include "EmuPadGrid.hpp"

#include <cstdio>
#include <cstdint>

#include "crosspad/pad/PadManager.hpp"
#include "crosspad/led/RgbColor.hpp"
#include "pc_stubs/pc_platform.h"

/* ── Appearance ──────────────────────────────────────────────────────── */

static const lv_color_t PAD_OFF_COLOR = lv_color_hex(0xEDE8DE);   // cream-white

/* ── Build ───────────────────────────────────────────────────────────── */

void EmuPadGrid::create(lv_obj_t* parent, int32_t x, int32_t y,
                         int32_t padSize, int32_t padGap)
{
    const int32_t colPitch = padSize + padGap;
    const int32_t rowPitch = padSize + LABEL_MARGIN + LABEL_H + padGap;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            const int padIdx     = (3 - row) * 4 + col;       // bottom-left = 0
            const int displayNum = padIdx + 1;                 // bottom-left = 1
            const int32_t px = x + col * colPitch;
            const int32_t py = y + row * rowPitch;

            /* ── Pad button ──────────────────────────────────────────── */
            lv_obj_t* pad = lv_obj_create(parent);
            lv_obj_set_pos(pad, px, py);
            lv_obj_set_size(pad, padSize, padSize);

            lv_obj_set_style_radius(pad, 8, 0);
            lv_obj_set_style_bg_color(pad, PAD_OFF_COLOR, 0);
            lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(pad, 2, 0);
            lv_obj_set_style_border_color(pad, lv_color_hex(0xAAAAAA), 0);

            // LED glow via shadow
            lv_obj_set_style_shadow_width(pad, 20, 0);
            lv_obj_set_style_shadow_spread(pad, 2, 0);
            lv_obj_set_style_shadow_color(pad, PAD_OFF_COLOR, 0);
            lv_obj_set_style_shadow_opa(pad, LV_OPA_70, 0);

            // Pressed state
            lv_obj_set_style_bg_opa(pad, LV_OPA_80, LV_STATE_PRESSED);
            lv_obj_set_style_transform_scale_x(pad, 240, LV_STATE_PRESSED);
            lv_obj_set_style_transform_scale_y(pad, 240, LV_STATE_PRESSED);

            lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_set_user_data(pad, (void*)(intptr_t)padIdx);
            lv_obj_add_event_cb(pad, onPressed,  LV_EVENT_PRESSED,  nullptr);
            lv_obj_add_event_cb(pad, onReleased, LV_EVENT_RELEASED, nullptr);

            pads_[padIdx] = pad;

            /* ── Label below pad ─────────────────────────────────────── */
            lv_obj_t* lbl = lv_label_create(parent);
            lv_label_set_text_fmt(lbl, "PAD %d", displayNum);
            lv_obj_set_pos(lbl, px, py + padSize + LABEL_MARGIN);
            lv_obj_set_height(lbl, LABEL_H);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x1A1A1A), 0);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_hor(lbl, 2, 0);
            lv_obj_set_style_bg_color(lbl, lv_color_hex(0xF0EDE6), 0);
            lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_top(lbl, (LABEL_H - 10) / 2, 0);  // vertically center text
            lv_obj_set_style_radius(lbl, 2, 0);
            lv_obj_remove_flag(lbl, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        }
    }
}

/* ── LED update ──────────────────────────────────────────────────────── */

void EmuPadGrid::updateLeds()
{
    for (int i = 0; i < 16; i++) {
        crosspad::RgbColor c = pc_get_led_color((uint16_t)i);

        if (c.R <= 2 && c.G <= 2 && c.B <= 2) {
            // LED off — cream-white default look
            lv_obj_set_style_bg_color(pads_[i], PAD_OFF_COLOR, 0);
            lv_obj_set_style_shadow_color(pads_[i], PAD_OFF_COLOR, 0);
        } else {
            lv_color_t lc = lv_color_make(c.R, c.G, c.B);
            lv_obj_set_style_bg_color(pads_[i], lc, 0);
            lv_obj_set_style_shadow_color(pads_[i], lc, 0);
        }
    }
}

/* ── Pad event callbacks ─────────────────────────────────────────────── */

void EmuPadGrid::onPressed(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    int padIdx = (int)(intptr_t)lv_obj_get_user_data(target);

    if (padIdx >= 0 && padIdx < 16) {
        crosspad::getPadManager().handlePadPress((uint8_t)padIdx, 127);
        printf("[STM32 EMU] Pad %d pressed\n", padIdx);
    }
}

void EmuPadGrid::onReleased(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    int padIdx = (int)(intptr_t)lv_obj_get_user_data(target);

    if (padIdx >= 0 && padIdx < 16) {
        crosspad::getPadManager().handlePadRelease((uint8_t)padIdx);
        printf("[STM32 EMU] Pad %d released\n", padIdx);
    }
}
