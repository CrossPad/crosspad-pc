/**
 * @file Stm32EmuWindow.cpp
 * @brief CrossPad device body with embedded LCD — single LVGL window.
 *
 * Assembles the full device visualization (LCD, encoder, pad grid, label)
 * on the active LVGL screen. Component details live in EmuEncoder and
 * EmuPadGrid.
 */

#include "Stm32EmuWindow.hpp"

#include <cstdio>
#include <cstdint>

/* ── Layout constants ────────────────────────────────────────────────── */

// LCD container
static constexpr int32_t LCD_W = 320;
static constexpr int32_t LCD_H = 240;

// Pad grid
static constexpr int32_t PAD_SIZE = 64;
static constexpr int32_t PAD_GAP  = 12;
static constexpr int32_t GRID_W   = 4 * PAD_SIZE + 3 * PAD_GAP;
static constexpr int32_t GRID_H   = EmuPadGrid::gridHeight(PAD_SIZE, PAD_GAP);

// LCD centered horizontally in window
static constexpr int32_t LCD_X = (Stm32EmuWindow::WIN_W - LCD_W) / 2;
static constexpr int32_t LCD_Y = 20;

// Encoder to the right of LCD
static constexpr int32_t ENC_SIZE = 60;
static constexpr int32_t ENC_X   = LCD_X + LCD_W + 12;
static constexpr int32_t ENC_Y   = LCD_Y + (LCD_H - ENC_SIZE) / 2;

// Label gap between LCD and pad grid (for "CrossPad" logo text)
static constexpr int32_t LABEL_GAP = 30;

// Pad grid centered horizontally
static constexpr int32_t GRID_X = (Stm32EmuWindow::WIN_W - GRID_W) / 2;
static constexpr int32_t GRID_Y = LCD_Y + LCD_H + LABEL_GAP;

/* ── init ─────────────────────────────────────────────────────────────── */

lv_obj_t* Stm32EmuWindow::init()
{
    screen_ = lv_screen_active();

    lv_display_t* disp = lv_display_get_default();
    lv_sdl_window_set_title(disp, "CrossPad");

    buildLayout();

    // LED + encoder visual polling timer (~30 fps)
    updateTimer_ = lv_timer_create(onUpdateTimer, 33, this);

    printf("[STM32 EMU] Device body initialized (%dx%d), LCD container %dx%d\n",
           WIN_W, WIN_H, LCD_W, LCD_H);

    return lcdContainer_;
}

/* ── Layout ───────────────────────────────────────────────────────────── */

void Stm32EmuWindow::buildLayout()
{
    // Dark device body background
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    lcdContainer_ = buildLcdContainer(screen_);
    encoder_.create(screen_, ENC_X, ENC_Y, ENC_SIZE);
    padGrid_.create(screen_, GRID_X, GRID_Y, PAD_SIZE, PAD_GAP);

    // Device label centered between LCD and pad grid
    lv_obj_t* label = lv_label_create(screen_);
    lv_label_set_text(label, "CrossPad");
    lv_obj_set_style_text_color(label, lv_color_hex(0x555555), 0);
    lv_obj_set_pos(label, 0, LCD_Y + LCD_H);
    lv_obj_set_size(label, WIN_W, LABEL_GAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t vpad = (LABEL_GAP - lv_font_get_line_height(font)) / 2;
    lv_obj_set_style_pad_top(label, vpad, LV_PART_MAIN);
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

/* ── Periodic update timer ───────────────────────────────────────────── */

void Stm32EmuWindow::onUpdateTimer(lv_timer_t* t)
{
    auto* self = (Stm32EmuWindow*)lv_timer_get_user_data(t);

    self->padGrid_.updateLeds();
    self->encoder_.update();
}
