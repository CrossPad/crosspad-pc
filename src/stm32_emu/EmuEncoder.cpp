/**
 * @file EmuEncoder.cpp
 * @brief Visual rotary encoder for the CrossPad PC emulator.
 */

#include "EmuEncoder.hpp"

#include <cstdio>
#include <SDL2/SDL.h>

/* ── SDL event watcher ───────────────────────────────────────────────── */

static int encoderSdlWatcher(void* userdata, SDL_Event* event)
{
    auto* self = static_cast<EmuEncoder*>(userdata);
    switch (event->type) {
        case SDL_MOUSEWHEEL:
            self->handleWheelDelta(event->wheel.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event->button.button == SDL_BUTTON_MIDDLE)
                self->handleMiddleButton(true);
            break;
        case SDL_MOUSEBUTTONUP:
            if (event->button.button == SDL_BUTTON_MIDDLE)
                self->handleMiddleButton(false);
            break;
    }
    return 1;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

EmuEncoder::~EmuEncoder()
{
    SDL_DelEventWatch(encoderSdlWatcher, this);
}

void EmuEncoder::create(lv_obj_t* parent, int32_t x, int32_t y, int32_t size)
{
    size_ = size;
    const int32_t border = 3;
    const int32_t half   = size / 2;

    /* ── Outer knob ──────────────────────────────────────────────────── */
    lv_obj_t* enc = lv_obj_create(parent);
    lv_obj_set_pos(enc, x, y);
    lv_obj_set_size(enc, size, size);
    lv_obj_set_style_radius(enc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(enc, lv_color_hex(0x606060), 0);
    lv_obj_set_style_bg_opa(enc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(enc, border, 0);
    lv_obj_set_style_border_color(enc, lv_color_hex(0x888888), 0);
    lv_obj_set_style_shadow_width(enc, 8, 0);
    lv_obj_set_style_shadow_color(enc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(enc, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(enc, 0, 0);
    lv_obj_add_flag(enc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(enc, LV_OBJ_FLAG_SCROLLABLE);

    // Pressed style (middle-click or left-click)
    lv_obj_set_style_bg_color(enc, lv_color_hex(0x505050), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_x(enc, 240, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(enc, 240, LV_STATE_PRESSED);

    /* ── Radial indicator line ───────────────────────────────────────── */
    constexpr int32_t IND_W = 4;
    constexpr int32_t IND_H = 12;
    constexpr int32_t IND_MARGIN = 3;  // from top of content area

    lv_obj_t* indicator = lv_obj_create(enc);
    lv_obj_set_size(indicator, IND_W, IND_H);
    lv_obj_set_style_radius(indicator, 2, 0);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(indicator, 0, 0);
    lv_obj_align(indicator, LV_ALIGN_TOP_MID, 0, IND_MARGIN);
    lv_obj_remove_flag(indicator, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Pivot at encoder center, relative to indicator's top-left corner.
    // Indicator top-left in encoder coords: (half - IND_W/2, border + IND_MARGIN)
    // Encoder center: (half, half)
    lv_obj_set_style_transform_pivot_x(indicator, IND_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(indicator, half - border - IND_MARGIN, 0);

    lv_obj_add_event_cb(enc, onClicked, LV_EVENT_CLICKED, this);

    obj_       = enc;
    indicator_ = indicator;

    SDL_AddEventWatch(encoderSdlWatcher, this);
}

/* ── Periodic visual refresh ─────────────────────────────────────────── */

void EmuEncoder::update()
{
    if (!indicator_ || !obj_) return;

    lv_obj_set_style_transform_rotation(indicator_, angle_, 0);

    if (pressed_) {
        lv_obj_add_state(obj_, LV_STATE_PRESSED);
    } else {
        lv_obj_remove_state(obj_, LV_STATE_PRESSED);
    }
}

/* ── SDL-driven state changes ────────────────────────────────────────── */

void EmuEncoder::handleWheelDelta(int dy)
{
    angle_ += dy * 150;   // 15 degrees per scroll tick (0.1° units)
}

void EmuEncoder::handleMiddleButton(bool pressed)
{
    pressed_ = pressed;
}

/* ── LVGL click callback ─────────────────────────────────────────────── */

void EmuEncoder::onClicked(lv_event_t* e)
{
    (void)e;
    printf("[STM32 EMU] Encoder clicked\n");
}
