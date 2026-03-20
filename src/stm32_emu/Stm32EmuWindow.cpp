/**
 * @file Stm32EmuWindow.cpp
 * @brief CrossPad device body with embedded LCD — single LVGL window.
 *
 * Assembles the full device visualization (LCD, encoder, pad grid, label)
 * on the active LVGL screen. Uses shared VirtualEncoder, VirtualPadGrid,
 * and VirtualPowerButton from crosspad-gui. SDL input wiring lives here.
 */

#include "Stm32EmuWindow.hpp"

#include <cstdio>
#include <cstdint>

#include <SDL2/SDL.h>
#include "crosspad-gui/platform/IGuiPlatform.h"

/* ── SDL event watcher (encoder + keyboard capture) ──────────────────── */

static int sdlEventWatcher(void* userdata, SDL_Event* event)
{
    auto* self = static_cast<Stm32EmuWindow*>(userdata);

    // Keyboard capture (pad playing via PC keyboard)
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        bool pressed = (event->type == SDL_KEYDOWN);
        bool isRepeat = (event->key.repeat != 0);
        if (self->getKeyboardCapture().handleKey(event->key.keysym.sym, pressed, isRepeat))
            return 0;  // consumed — don't pass to LVGL
    }

    // Encoder: mouse wheel + middle click
    switch (event->type) {
        case SDL_MOUSEWHEEL:
            self->handleEncoderWheel(event->wheel.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event->button.button == SDL_BUTTON_MIDDLE)
                self->handleEncoderPress(true);
            break;
        case SDL_MOUSEBUTTONUP:
            if (event->button.button == SDL_BUTTON_MIDDLE)
                self->handleEncoderPress(false);
            break;
    }
    return 1;
}

Stm32EmuWindow::~Stm32EmuWindow()
{
    SDL_DelEventWatch(sdlEventWatcher, this);
}

/* ── Layout constants ────────────────────────────────────────────────── */

// LCD container
static constexpr int32_t LCD_W = 320;
static constexpr int32_t LCD_H = 240;

// Pad grid
static constexpr int32_t PAD_SIZE_SCALE = 5;  // overall size multiplier for emulator UI
static constexpr int32_t PAD_SIZE = 12 * PAD_SIZE_SCALE;
static constexpr int32_t PAD_GAP  = 4 * PAD_SIZE_SCALE;
static constexpr int32_t GRID_W   = 4 * PAD_SIZE + 3 * PAD_GAP;
static constexpr int32_t GRID_H   = crosspad_gui::VirtualPadGrid::gridHeight(PAD_SIZE, PAD_GAP*2/3);

// LCD centered horizontally in window
static constexpr int32_t LCD_X = (Stm32EmuWindow::WIN_W - LCD_W) / 2;
static constexpr int32_t LCD_Y = 40;

// Encoder to the right of LCD
static constexpr int32_t ENC_SIZE = 60;
static constexpr int32_t ENC_X   = LCD_X + LCD_W + 12;
static constexpr int32_t ENC_Y   = LCD_Y + (LCD_H - ENC_SIZE) / 2;

// Power button — to the right of encoder, slightly lower
static constexpr int32_t PWR_SIZE = 30;
static constexpr int32_t PWR_X   = ENC_X + (ENC_SIZE - PWR_SIZE) / 2;
static constexpr int32_t PWR_Y   = ENC_Y + ENC_SIZE + 16;

// Label gap between LCD and pad grid (for "CrossPad" logo text)
static constexpr int32_t LABEL_GAP = 30;

// Pad grid centered horizontally
static constexpr int32_t GRID_X = (Stm32EmuWindow::WIN_W - GRID_W) / 2;
static constexpr int32_t GRID_Y = LCD_Y + LCD_H + LABEL_GAP;

// Screw dimensions
static constexpr int32_t SCREW_DIA = 16;

/* ── Screw helper ─────────────────────────────────────────────────────── */

static void createScrew(lv_obj_t* parent, int32_t cx, int32_t cy, int32_t dia)
{
    lv_obj_t* screw = lv_obj_create(parent);
    lv_obj_set_size(screw, dia, dia);
    lv_obj_set_pos(screw, cx - dia / 2, cy - dia / 2);
    lv_obj_set_style_radius(screw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(screw, lv_color_hex(0x252525), 0);
    lv_obj_set_style_bg_opa(screw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screw, 1, 0);
    lv_obj_set_style_border_color(screw, lv_color_hex(0x383838), 0);
    lv_obj_set_style_shadow_width(screw, 4, 0);
    lv_obj_set_style_shadow_color(screw, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(screw, 80, 0);
    lv_obj_set_style_shadow_offset_y(screw, 1, 0);
    lv_obj_set_style_pad_all(screw, 0, 0);
    lv_obj_remove_flag(screw, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Inner socket (hex/Allen head recess)
    int32_t inner = dia * 2 / 5;
    lv_obj_t* socket = lv_obj_create(screw);
    lv_obj_set_size(socket, inner, inner);
    lv_obj_center(socket);
    lv_obj_set_style_radius(socket, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(socket, lv_color_hex(0x0E0E0E), 0);
    lv_obj_set_style_bg_opa(socket, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(socket, 0, 0);
    lv_obj_remove_flag(socket, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

/* ── init ─────────────────────────────────────────────────────────────── */

lv_obj_t* Stm32EmuWindow::init()
{
    screen_ = lv_screen_active();

    lv_display_t* disp = lv_display_get_default();
    lv_sdl_window_set_title(disp, "CrossPad");

    buildLayout();

    // Initialize keyboard capture
    kbCapture_.init();

    // Register SDL event watcher for encoder + keyboard input
    SDL_AddEventWatch(sdlEventWatcher, this);

    // Ensure the LCD container (and its children like the embedded status bar)
    // is drawn above other screen children so it isn't occluded.
    lv_obj_move_foreground(lcdContainer_);

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
    powerBtn_.create(screen_, PWR_X, PWR_Y, PWR_SIZE);
    padGrid_.create(screen_, GRID_X, GRID_Y, PAD_SIZE, PAD_GAP);

    // Logo centered between LCD and pad grid
    lv_obj_t* logo = lv_image_create(screen_);
    static char logo_path[256];
    snprintf(logo_path, sizeof(logo_path), "%sCrossPad_Logo_110w.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());
    lv_image_set_src(logo, logo_path);
    lv_obj_set_pos(logo, (WIN_W - 120) / 2, LCD_Y + LCD_H + (LABEL_GAP - 36) / 2);
    lv_obj_set_size(logo, 120, 36);
    
    // White color tint
    lv_obj_set_style_img_recolor(logo, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, 0);
    
    lv_obj_remove_flag(logo, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Keyboard capture toggle button (left of encoder)
    buildKeyboardButton(screen_);

    // Audio/MIDI jack connectors on device edges
    jackPanel_.create(screen_);

    // 6 screws matching the hardware render
    constexpr int32_t M = 22;  // corner margin from window edge
    // 4 corner screws
    createScrew(screen_, M,         M,          SCREW_DIA);  // top-left
    createScrew(screen_, WIN_W - M, M,          SCREW_DIA);  // top-right
    createScrew(screen_, M,         WIN_H - M,  SCREW_DIA);  // bottom-left
    createScrew(screen_, WIN_W - M, WIN_H - M,  SCREW_DIA);  // bottom-right
    // 2 mid-height screws at the panel seam (below LCD, between screen and pad area)
    constexpr int32_t SEAM_Y = LCD_Y + LCD_H + LABEL_GAP / 2;
    createScrew(screen_, M,         SEAM_Y, SCREW_DIA);  // mid-left
    createScrew(screen_, WIN_W - M, SEAM_Y, SCREW_DIA);  // mid-right
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

    // Ustaw flex layout i styl, aby status bar był widoczny
    lv_obj_set_flex_flow(lcd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lcd, 0, 0);
    lv_obj_set_style_pad_column(lcd, 0, 0);
    lv_obj_set_scrollbar_mode(lcd, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);

    return lcd;
}

/* ── Periodic update timer ───────────────────────────────────────────── */

void Stm32EmuWindow::onUpdateTimer(lv_timer_t* t)
{
    auto* self = (Stm32EmuWindow*)lv_timer_get_user_data(t);

    self->padGrid_.updateLeds();
    self->encoder_.update();
    self->jackPanel_.update();

    // Poll Windows global hotkeys (no-op when not in Global mode)
    self->kbCapture_.processGlobalHotkeys();
}

/* ── Keyboard capture toggle button ───────────────────────────────────── */

// Position: left edge, below Audio OUT2 jack (OUT2 is at y=210, h=50)
static constexpr int32_t KB_BTN_W = 40;
static constexpr int32_t KB_BTN_H = 50;
static constexpr int32_t KB_BTN_X = 5;
static constexpr int32_t KB_BTN_Y = 320;

void Stm32EmuWindow::buildKeyboardButton(lv_obj_t* parent)
{
    kbButton_ = lv_obj_create(parent);
    lv_obj_set_pos(kbButton_, KB_BTN_X, KB_BTN_Y);
    lv_obj_set_size(kbButton_, KB_BTN_W, KB_BTN_H);

    lv_obj_set_style_radius(kbButton_, 6, 0);
    lv_obj_set_style_bg_color(kbButton_, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(kbButton_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kbButton_, 1, 0);
    lv_obj_set_style_border_color(kbButton_, lv_color_hex(0x555555), 0);
    lv_obj_set_style_pad_all(kbButton_, 2, 0);

    lv_obj_add_flag(kbButton_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(kbButton_, LV_OBJ_FLAG_SCROLLABLE);

    // Pressed visual feedback
    lv_obj_set_style_bg_color(kbButton_, lv_color_hex(0x444444), LV_STATE_PRESSED);

    // Keyboard icon (using Unicode keyboard symbol ⌨ approximation)
    kbIcon_ = lv_label_create(kbButton_);
    lv_label_set_text(kbIcon_, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_font(kbIcon_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(kbIcon_, lv_color_hex(0x666666), 0);
    lv_obj_align(kbIcon_, LV_ALIGN_TOP_MID, 0, 4);

    // Mode label below icon
    kbModeLabel_ = lv_label_create(kbButton_);
    lv_label_set_text(kbModeLabel_, "OFF");
    lv_obj_set_style_text_font(kbModeLabel_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(kbModeLabel_, lv_color_hex(0x666666), 0);
    lv_obj_align(kbModeLabel_, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Click callback
    lv_obj_add_event_cb(kbButton_, onKbButtonClicked, LV_EVENT_CLICKED, this);

    updateKeyboardButtonVisual();
}

void Stm32EmuWindow::updateKeyboardButtonVisual()
{
    auto mode = kbCapture_.mode();

    switch (mode) {
        case KeyboardCapture::Mode::Off:
            lv_obj_set_style_text_color(kbIcon_, lv_color_hex(0x666666), 0);
            lv_obj_set_style_border_color(kbButton_, lv_color_hex(0x555555), 0);
            lv_label_set_text(kbModeLabel_, "OFF");
            lv_obj_set_style_text_color(kbModeLabel_, lv_color_hex(0x666666), 0);
            break;
        case KeyboardCapture::Mode::Focus:
            lv_obj_set_style_text_color(kbIcon_, lv_color_hex(0x00CC66), 0);
            lv_obj_set_style_border_color(kbButton_, lv_color_hex(0x00CC66), 0);
            lv_label_set_text(kbModeLabel_, "FOCUS");
            lv_obj_set_style_text_color(kbModeLabel_, lv_color_hex(0x00CC66), 0);
            break;
        case KeyboardCapture::Mode::Global:
            lv_obj_set_style_text_color(kbIcon_, lv_color_hex(0xFF6600), 0);
            lv_obj_set_style_border_color(kbButton_, lv_color_hex(0xFF6600), 0);
            lv_label_set_text(kbModeLabel_, "GLOBAL");
            lv_obj_set_style_text_color(kbModeLabel_, lv_color_hex(0xFF6600), 0);
            break;
    }
}

void Stm32EmuWindow::onKbButtonClicked(lv_event_t* e)
{
    auto* self = (Stm32EmuWindow*)lv_event_get_user_data(e);
    self->kbCapture_.cycleMode();
    self->updateKeyboardButtonVisual();
}

/* ── Encoder forwarding ──────────────────────────────────────────────── */

void Stm32EmuWindow::handleEncoderCC(uint8_t value, uint8_t ccRange, uint8_t stepsPerRev)
{
    encoder_.setFromCC(value, ccRange, stepsPerRev);
}

void Stm32EmuWindow::handleEncoderPress(bool pressed)
{
    encoder_.handleButtonPress(pressed);
}

void Stm32EmuWindow::handleEncoderWheel(int dy)
{
    encoder_.handleWheelDelta(dy);
}
