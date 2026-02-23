/**
 * @file EmuPowerButton.cpp
 * @brief Emulated power button for the CrossPad PC emulator.
 *
 * Click toggles the volume overlay, matching real hardware behavior
 * where a short press on the power button cycles through
 * speakers → headphones → dismiss.
 */

#include "EmuPowerButton.hpp"

#include <cstdio>

#include "crosspad-gui/components/volume_overlay.h"

/* ── Lifecycle ───────────────────────────────────────────────────────── */

void EmuPowerButton::create(lv_obj_t* parent, int32_t x, int32_t y, int32_t size)
{
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_shadow_width(btn, 6, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    // Pressed style
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_x(btn, 240, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(btn, 240, LV_STATE_PRESSED);

    // Power icon (Unicode U+23FB POWER SYMBOL) — use a simple "I" line as fallback
    // since LVGL built-in symbols don't include a power icon.
    // We draw a small vertical line + arc using an LVGL label with LV_SYMBOL_POWER
    // if available, otherwise use a simple circle+line visual.
    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_add_event_cb(btn, onPressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(btn, onReleased, LV_EVENT_RELEASED, this);

    obj_ = btn;
}

/* ── Periodic visual refresh ─────────────────────────────────────────── */

void EmuPowerButton::update()
{
    // Currently no periodic state to update (LED glow could be added later)
}

/* ── LVGL event callbacks ────────────────────────────────────────────── */

void EmuPowerButton::onPressed(lv_event_t* e)
{
    (void)e;
    printf("[STM32 EMU] Power button pressed\n");
    crosspad_gui::volume_overlay_toggle();
}

void EmuPowerButton::onReleased(lv_event_t* e)
{
    (void)e;
    printf("[STM32 EMU] Power button released\n");
}
