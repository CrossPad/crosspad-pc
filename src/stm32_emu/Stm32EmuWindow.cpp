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

    // Logo centered between LCD and pad grid
    lv_obj_t* logo = lv_image_create(screen_);
    lv_image_set_src(logo, "C:/crosspad-gui/assets/CrossPad_Logo_110w.png");
    lv_obj_set_pos(logo, (WIN_W - 120) / 2, LCD_Y + LCD_H + (LABEL_GAP - 36) / 2);
    lv_obj_set_size(logo, 120, 36);
    
    // White color tint
    lv_obj_set_style_img_recolor(logo, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, 0);
    
    lv_obj_remove_flag(logo, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
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

void Stm32EmuWindow::handleEncoderCC(uint8_t value, uint8_t ccRange, uint8_t stepsPerRev)
{
    encoder_.setFromCC(value, ccRange, stepsPerRev);
}

void Stm32EmuWindow::handleEncoderPress(bool pressed)
{
    encoder_.handleMiddleButton(pressed);
}
