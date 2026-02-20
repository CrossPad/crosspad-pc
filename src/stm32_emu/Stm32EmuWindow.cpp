/**
 * @file Stm32EmuWindow.cpp
 * @brief CrossPad device body with embedded LCD — single LVGL window
 *
 * Builds the full device visualization (body, pads, encoder) on the active
 * LVGL screen and provides a 320x240 container where the LCD GUI renders.
 * Pad clicks feed into PadManager; LED colors are polled at ~30fps.
 */

#include "Stm32EmuWindow.hpp"

#include <cstdio>
#include <cstdint>

#include "crosspad/pad/PadManager.hpp"
#include "crosspad/led/RgbColor.hpp"
#include "pc_stubs/pc_platform.h"

/* ── Layout constants ────────────────────────────────────────────────── */

// LCD container (top-left, actual 320x240 LVGL content)
static constexpr int32_t LCD_X = 20;
static constexpr int32_t LCD_Y = 20;
static constexpr int32_t LCD_W = 320;
static constexpr int32_t LCD_H = 240;

// Encoder (top-right, vertically centered next to LCD)
static constexpr int32_t ENC_SIZE = 60;
static constexpr int32_t ENC_X   = LCD_X + LCD_W + 20;
static constexpr int32_t ENC_Y   = LCD_Y + (LCD_H - ENC_SIZE) / 2;

// Pad grid (centered below LCD area)
static constexpr int32_t PAD_SIZE = 80;
static constexpr int32_t PAD_GAP  = 8;
static constexpr int32_t GRID_W   = 4 * PAD_SIZE + 3 * PAD_GAP;
static constexpr int32_t GRID_X   = (Stm32EmuWindow::WIN_W - GRID_W) / 2;
static constexpr int32_t GRID_Y   = LCD_Y + LCD_H + 20;

/* ── init ─────────────────────────────────────────────────────────────── */

lv_obj_t* Stm32EmuWindow::init()
{
    screen_ = lv_screen_active();

    // Set window title
    lv_display_t* disp = lv_display_get_default();
    lv_sdl_window_set_title(disp, "CrossPad");

    buildLayout();

    // LED color polling timer (~30 fps)
    ledTimer_ = lv_timer_create(onLedUpdateTimer, 33, this);

    printf("[STM32 EMU] Device body initialized (%dx%d), LCD container 320x240\n",
           WIN_W, WIN_H);

    return lcdContainer_;
}

/* ── Layout ───────────────────────────────────────────────────────────── */

void Stm32EmuWindow::buildLayout()
{
    // Dark device body background (zero padding so child coordinates are absolute)
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    lcdContainer_ = buildLcdContainer(screen_);
    buildEncoder(screen_);
    buildPadGrid(screen_);

    // Device label at bottom
    lv_obj_t* label = lv_label_create(screen_);
    lv_label_set_text(label, "CrossPad");
    lv_obj_set_style_text_color(label, lv_color_hex(0x555555), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* ── LCD container (320x240 — hosts the actual GUI) ──────────────────── */

lv_obj_t* Stm32EmuWindow::buildLcdContainer(lv_obj_t* parent)
{
    lv_obj_t* lcd = lv_obj_create(parent);
    lv_obj_set_pos(lcd, LCD_X, LCD_Y);
    lv_obj_set_size(lcd, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(lcd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(lcd, 4, 0);
    lv_obj_set_style_pad_all(lcd, 0, 0);
    lv_obj_set_style_clip_corner(lcd, true, 0);
    lv_obj_remove_flag(lcd, LV_OBJ_FLAG_SCROLLABLE);

    return lcd;
}

/* ── Encoder ──────────────────────────────────────────────────────────── */

void Stm32EmuWindow::buildEncoder(lv_obj_t* parent)
{
    lv_obj_t* enc = lv_obj_create(parent);
    lv_obj_set_pos(enc, ENC_X, ENC_Y);
    lv_obj_set_size(enc, ENC_SIZE, ENC_SIZE);
    lv_obj_set_style_radius(enc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(enc, lv_color_hex(0x606060), 0);
    lv_obj_set_style_bg_opa(enc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(enc, 3, 0);
    lv_obj_set_style_border_color(enc, lv_color_hex(0x888888), 0);
    lv_obj_set_style_shadow_width(enc, 8, 0);
    lv_obj_set_style_shadow_color(enc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(enc, LV_OPA_50, 0);
    lv_obj_add_flag(enc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(enc, LV_OBJ_FLAG_SCROLLABLE);

    // Pressed style
    lv_obj_set_style_bg_color(enc, lv_color_hex(0x505050), LV_STATE_PRESSED);

    // Knob indicator dot (top of encoder)
    lv_obj_t* dot = lv_obj_create(enc);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_add_event_cb(enc, onEncoderClick, LV_EVENT_CLICKED, this);

    encoderObj_ = enc;
}

/* ── Pad grid ─────────────────────────────────────────────────────────── */

void Stm32EmuWindow::buildPadGrid(lv_obj_t* parent)
{
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int padIdx = row * 4 + col;
            int32_t x = GRID_X + col * (PAD_SIZE + PAD_GAP);
            int32_t y = GRID_Y + row * (PAD_SIZE + PAD_GAP);

            lv_obj_t* pad = lv_obj_create(parent);
            lv_obj_set_pos(pad, x, y);
            lv_obj_set_size(pad, PAD_SIZE, PAD_SIZE);

            // Default dark pad look
            lv_obj_set_style_radius(pad, 8, 0);
            lv_obj_set_style_bg_color(pad, lv_color_hex(0x0A1A08), 0);
            lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(pad, 2, 0);
            lv_obj_set_style_border_color(pad, lv_color_hex(0x333333), 0);

            // LED glow via shadow
            lv_obj_set_style_shadow_width(pad, 20, 0);
            lv_obj_set_style_shadow_spread(pad, 2, 0);
            lv_obj_set_style_shadow_color(pad, lv_color_hex(0x0A1A08), 0);
            lv_obj_set_style_shadow_opa(pad, LV_OPA_70, 0);

            // Pressed state
            lv_obj_set_style_bg_opa(pad, LV_OPA_80, LV_STATE_PRESSED);
            lv_obj_set_style_transform_scale_x(pad, 240, LV_STATE_PRESSED);
            lv_obj_set_style_transform_scale_y(pad, 240, LV_STATE_PRESSED);

            lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

            // Store pad index
            lv_obj_set_user_data(pad, (void*)(intptr_t)padIdx);

            // Events
            lv_obj_add_event_cb(pad, onPadPressed, LV_EVENT_PRESSED, this);
            lv_obj_add_event_cb(pad, onPadReleased, LV_EVENT_RELEASED, this);

            // Pad number label (subtle)
            lv_obj_t* label = lv_label_create(pad);
            lv_label_set_text_fmt(label, "%d", padIdx);
            lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
            lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
            lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);

            padButtons_[padIdx] = pad;
        }
    }
}

/* ── Pad event callbacks ──────────────────────────────────────────────── */

void Stm32EmuWindow::onPadPressed(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    int padIdx = (int)(intptr_t)lv_obj_get_user_data(target);

    if (padIdx >= 0 && padIdx < 16) {
        crosspad::getPadManager().handlePadPress((uint8_t)padIdx, 127);
        printf("[STM32 EMU] Pad %d pressed\n", padIdx);
    }
}

void Stm32EmuWindow::onPadReleased(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    int padIdx = (int)(intptr_t)lv_obj_get_user_data(target);

    if (padIdx >= 0 && padIdx < 16) {
        crosspad::getPadManager().handlePadRelease((uint8_t)padIdx);
        printf("[STM32 EMU] Pad %d released\n", padIdx);
    }
}

/* ── Encoder event callbacks ──────────────────────────────────────────── */

void Stm32EmuWindow::onEncoderClick(lv_event_t* e)
{
    (void)e;
    printf("[STM32 EMU] Encoder clicked\n");
}

void Stm32EmuWindow::onEncoderScroll(lv_event_t* e)
{
    (void)e;
    printf("[STM32 EMU] Encoder scroll\n");
}

/* ── LED update timer ─────────────────────────────────────────────────── */

void Stm32EmuWindow::onLedUpdateTimer(lv_timer_t* t)
{
    auto* self = (Stm32EmuWindow*)lv_timer_get_user_data(t);

    for (int i = 0; i < 16; i++) {
        crosspad::RgbColor c = pc_get_led_color((uint16_t)i);
        lv_color_t lc = lv_color_make(c.R, c.G, c.B);

        lv_obj_set_style_bg_color(self->padButtons_[i], lc, 0);
        lv_obj_set_style_shadow_color(self->padButtons_[i], lc, 0);
    }
}
