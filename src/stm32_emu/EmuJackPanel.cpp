/**
 * @file EmuJackPanel.cpp
 * @brief Visual audio/MIDI jack connectors for the CrossPad PC emulator.
 *
 * Jack bars act as live VU meters when connected: the bar fills with a
 * green-yellow-red gradient proportional to audio level. When disconnected,
 * the bar is a solid gray rectangle. All jacks are clickable for device
 * selection via dropdown.
 */

#include "EmuJackPanel.hpp"
#include <cstdio>
#include <cmath>

/* ── Layout constants ────────────────────────────────────────────────── */

// Audio OUT jacks — vertical bars on LEFT edge, at LCD height
static constexpr int32_t AOUT1_BAR_X = 2,   AOUT1_BAR_Y = 110;
static constexpr int32_t AOUT2_BAR_X = 2,   AOUT2_BAR_Y = 210;
static constexpr int32_t AOUT_BAR_W  = 12,  AOUT_BAR_H  = 50;
static constexpr int32_t AOUT_LBL_X  = 26;  // label to the right of bar
static constexpr int32_t AOUT_LBL_W  = 64;  // width becomes height when rotated 90°

// Audio IN jacks — horizontal bars on TOP edge, left side
static constexpr int32_t AIN1_BAR_X = 50,   AIN1_BAR_Y = 2;
static constexpr int32_t AIN2_BAR_X = 120,  AIN2_BAR_Y = 2;
static constexpr int32_t AIN_BAR_W  = 50,   AIN_BAR_H  = 12;
static constexpr int32_t AIN_LBL_Y  = 16;
static constexpr int32_t AIN_LBL_W  = 52;

// MIDI jacks — horizontal bars on TOP edge, right side
static constexpr int32_t MIDI_OUT_BAR_X = 320, MIDI_OUT_BAR_Y = 2;
static constexpr int32_t MIDI_IN_BAR_X  = 385, MIDI_IN_BAR_Y  = 2;
static constexpr int32_t MIDI_BAR_W     = 50,  MIDI_BAR_H     = 8;
static constexpr int32_t MIDI_LBL_Y     = 10;
static constexpr int32_t MIDI_LBL_W     = 60;

// Colors
static constexpr uint32_t COLOR_DISCONNECTED = 0x555555;
static constexpr uint32_t COLOR_CONNECTED    = 0x00CC66;
static constexpr uint32_t COLOR_MIDI_CONN    = 0x3399FF;
static constexpr uint32_t COLOR_VU_BG        = 0x1A1A1A;

/* ── VU helpers ──────────────────────────────────────────────────────── */

static uint16_t levelToPercent(int16_t val, int base) {
    const int16_t NOISE_GATE = 800;
    if (val < NOISE_GATE) return 0;

    float normalized = (float)val / 32767.0f;
    const float GAIN_MULTIPLIER = 3.5f;
    normalized *= GAIN_MULTIPLIER;
    if (normalized > 1.0f) normalized = 1.0f;

    float db_normalized;
    if (normalized < 0.018f) {
        db_normalized = 0.0f;
    } else {
        float db = 20.0f * log10f(normalized);
        db_normalized = (db + 35.0f) / 35.0f;
        if (db_normalized < 0.0f) db_normalized = 0.0f;
        if (db_normalized > 1.0f) db_normalized = 1.0f;
    }

    uint32_t scaled = (uint32_t)(db_normalized * (float)base);
    return (uint16_t)(scaled > (uint32_t)base ? base : scaled);
}

static lv_color_t vuColor(float percent) {
    if (percent < 0.60f) {
        return lv_color_hex(0x00FF00);
    } else if (percent < 0.85f) {
        float t = (percent - 0.60f) / 0.25f;
        uint8_t red = (uint8_t)(t * 255);
        return lv_color_make(red, 255, 0);
    } else {
        float t = (percent - 0.85f) / 0.15f;
        if (t > 1.0f) t = 1.0f;
        uint8_t green = (uint8_t)((1.0f - t) * 255);
        return lv_color_make(255, green, 0);
    }
}

/* ── create ──────────────────────────────────────────────────────────── */

void EmuJackPanel::create(lv_obj_t* parent)
{
    parent_ = parent;

    // Audio OUT1 (clickable)
    createJack(parent, AUDIO_OUT1,
               AOUT1_BAR_X, AOUT1_BAR_Y, AOUT_BAR_W, AOUT_BAR_H,
               AOUT_LBL_X, AOUT1_BAR_Y + 2, AOUT_LBL_W,
               "OUT 1", true, LV_DIR_RIGHT);

    // Audio OUT2 (clickable)
    createJack(parent, AUDIO_OUT2,
               AOUT2_BAR_X, AOUT2_BAR_Y, AOUT_BAR_W, AOUT_BAR_H,
               AOUT_LBL_X, AOUT2_BAR_Y + 2, AOUT_LBL_W,
               "OUT 2", true, LV_DIR_RIGHT);

    // Audio IN1 (clickable)
    createJack(parent, AUDIO_IN1,
               AIN1_BAR_X, AIN1_BAR_Y, AIN_BAR_W, AIN_BAR_H,
               AIN1_BAR_X, AIN_LBL_Y, AIN_LBL_W,
               "IN 1", true, LV_DIR_BOTTOM);

    // Audio IN2 (clickable)
    createJack(parent, AUDIO_IN2,
               AIN2_BAR_X, AIN2_BAR_Y, AIN_BAR_W, AIN_BAR_H,
               AIN2_BAR_X, AIN_LBL_Y, AIN_LBL_W,
               "IN 2", true, LV_DIR_BOTTOM);

    // MIDI OUT (clickable)
    createJack(parent, MIDI_OUT,
               MIDI_OUT_BAR_X, MIDI_OUT_BAR_Y, MIDI_BAR_W, MIDI_BAR_H,
               MIDI_OUT_BAR_X, MIDI_LBL_Y, MIDI_LBL_W,
               "MIDI OUT", true, LV_DIR_BOTTOM);

    // MIDI IN (clickable)
    createJack(parent, MIDI_IN,
               MIDI_IN_BAR_X, MIDI_IN_BAR_Y, MIDI_BAR_W, MIDI_BAR_H,
               MIDI_IN_BAR_X, MIDI_LBL_Y, MIDI_LBL_W,
               "MIDI IN", true, LV_DIR_BOTTOM);

    // Click anywhere on screen closes all open dropdowns
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(parent, this);
    lv_obj_add_event_cb(parent, onScreenClicked, LV_EVENT_CLICKED, this);
}

/* ── Single jack creation ────────────────────────────────────────────── */

void EmuJackPanel::createJack(lv_obj_t* parent, JackId id,
                               int32_t barX, int32_t barY, int32_t barW, int32_t barH,
                               int32_t lblX, int32_t lblY, int32_t lblW,
                               const char* defaultLabel, bool clickable,
                               lv_dir_t dropdownDir)
{
    Jack& jack = jacks_[id];
    jack.id = id;
    jack.vertical = (id == AUDIO_OUT1 || id == AUDIO_OUT2);
    bool rotatedLabel = jack.vertical;
    bool isAudio = (id <= AUDIO_IN2);

    // --- Bar (thick line on device edge) ---
    jack.bar = lv_obj_create(parent);
    lv_obj_set_pos(jack.bar, barX, barY);
    lv_obj_set_size(jack.bar, barW, barH);
    lv_obj_set_style_radius(jack.bar, 3, 0);
    lv_obj_set_style_border_width(jack.bar, 0, 0);
    lv_obj_set_style_pad_all(jack.bar, 0, 0);
    lv_obj_remove_flag(jack.bar, LV_OBJ_FLAG_SCROLLABLE);

    applyBarStyle(jack);

    // All jacks are clickable
    lv_obj_add_flag(jack.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(jack.bar, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(jack.bar);
    lv_obj_set_user_data(jack.bar, this);
    lv_obj_add_event_cb(jack.bar, onBarClicked, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    lv_obj_set_style_bg_opa(jack.bar, LV_OPA_80, LV_STATE_PRESSED);

    // --- VU meter bars inside the jack bar (only for audio jacks) ---
    if (isAudio) {
        if (jack.vertical) {
            // Vertical bar: two VU channels side-by-side, growing upward
            int32_t chW = (barW - 2) / 2;

            jack.vuBarL = lv_obj_create(jack.bar);
            lv_obj_set_style_radius(jack.vuBarL, 0, 0);
            lv_obj_set_style_border_width(jack.vuBarL, 0, 0);
            lv_obj_set_style_bg_color(jack.vuBarL, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(jack.vuBarL, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(jack.vuBarL, 0, 0);
            lv_obj_set_size(jack.vuBarL, chW, 0);
            lv_obj_set_pos(jack.vuBarL, 1, barH - 1);
            lv_obj_remove_flag(jack.vuBarL, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(jack.vuBarL, LV_OBJ_FLAG_HIDDEN);

            jack.vuBarR = lv_obj_create(jack.bar);
            lv_obj_set_style_radius(jack.vuBarR, 0, 0);
            lv_obj_set_style_border_width(jack.vuBarR, 0, 0);
            lv_obj_set_style_bg_color(jack.vuBarR, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(jack.vuBarR, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(jack.vuBarR, 0, 0);
            lv_obj_set_size(jack.vuBarR, chW, 0);
            lv_obj_set_pos(jack.vuBarR, 1 + chW, barH - 1);
            lv_obj_remove_flag(jack.vuBarR, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(jack.vuBarR, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Horizontal bar: two VU channels stacked (top=L, bottom=R), growing rightward
            int32_t chH = (barH - 2) / 2;

            jack.vuBarL = lv_obj_create(jack.bar);
            lv_obj_set_style_radius(jack.vuBarL, 0, 0);
            lv_obj_set_style_border_width(jack.vuBarL, 0, 0);
            lv_obj_set_style_bg_color(jack.vuBarL, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(jack.vuBarL, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(jack.vuBarL, 0, 0);
            lv_obj_set_size(jack.vuBarL, 0, chH);
            lv_obj_set_pos(jack.vuBarL, 1, 1);
            lv_obj_remove_flag(jack.vuBarL, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(jack.vuBarL, LV_OBJ_FLAG_HIDDEN);

            jack.vuBarR = lv_obj_create(jack.bar);
            lv_obj_set_style_radius(jack.vuBarR, 0, 0);
            lv_obj_set_style_border_width(jack.vuBarR, 0, 0);
            lv_obj_set_style_bg_color(jack.vuBarR, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(jack.vuBarR, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(jack.vuBarR, 0, 0);
            lv_obj_set_size(jack.vuBarR, 0, chH);
            lv_obj_set_pos(jack.vuBarR, 1, 1 + chH);
            lv_obj_remove_flag(jack.vuBarR, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(jack.vuBarR, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- Label ---
    jack.label = lv_label_create(parent);
    lv_label_set_text(jack.label, defaultLabel);
    lv_obj_set_pos(jack.label, lblX, lblY);
    lv_obj_set_width(jack.label, lblW);
    lv_obj_set_style_text_font(jack.label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(jack.label, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_opa(jack.label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(jack.label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(jack.label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_remove_flag(jack.label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    if (rotatedLabel) {
        lv_obj_set_style_transform_rotation(jack.label, 900, 0);
        lv_obj_set_style_transform_pivot_x(jack.label, 0, 0);
        lv_obj_set_style_transform_pivot_y(jack.label, 0, 0);
    }

    // --- Dropdown (all jacks are clickable) ---
    jack.dropdown = lv_dropdown_create(lv_layer_top());
    lv_dropdown_set_text(jack.dropdown, nullptr);
    lv_dropdown_set_options(jack.dropdown, "(None)");
    lv_obj_set_style_text_font(jack.dropdown, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_font(jack.dropdown, &lv_font_montserrat_10, LV_PART_INDICATOR);
    lv_obj_set_size(jack.dropdown, 200, 24);
    lv_obj_set_style_pad_all(jack.dropdown, 4, 0);
    lv_obj_set_style_bg_color(jack.dropdown, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(jack.dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(jack.dropdown, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_border_color(jack.dropdown, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(jack.dropdown, 1, 0);
    lv_obj_set_style_radius(jack.dropdown, 4, 0);

    lv_obj_remove_flag(jack.dropdown, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(jack.dropdown);

    lv_dropdown_set_dir(jack.dropdown, dropdownDir);

    // Position dropdown near the bar
    switch (dropdownDir) {
    case LV_DIR_BOTTOM:
        lv_obj_set_pos(jack.dropdown, barX, barY + barH + 2);
        break;
    case LV_DIR_RIGHT:
        lv_obj_set_pos(jack.dropdown, barX + barW + 4, barY);
        break;
    case LV_DIR_LEFT:
        lv_obj_set_pos(jack.dropdown, barX - 204, barY);
        break;
    default:
        lv_obj_set_pos(jack.dropdown, barX, barY + barH + 2);
        break;
    }

    // Hidden by default
    lv_obj_add_flag(jack.dropdown, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_user_data(jack.dropdown, this);
    lv_obj_add_event_cb(jack.dropdown, onDropdownChanged,
                        LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)id);
    lv_obj_add_event_cb(jack.dropdown, onDropdownListCreated,
                        LV_EVENT_READY, (void*)(intptr_t)id);
}

/* ── Bar styling ─────────────────────────────────────────────────────── */

void EmuJackPanel::applyBarStyle(Jack& jack)
{
    bool isAudio = (jack.id <= AUDIO_IN2);

    if (jack.connected && isAudio) {
        // Connected audio jack: dark background, VU bars will render the level
        lv_obj_set_style_bg_color(jack.bar, lv_color_hex(COLOR_VU_BG), 0);
        lv_obj_set_style_bg_opa(jack.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(jack.bar, 1, 0);
        lv_obj_set_style_border_color(jack.bar, lv_color_hex(0x333333), 0);
        lv_obj_set_style_shadow_width(jack.bar, 8, 0);
        lv_obj_set_style_shadow_spread(jack.bar, 1, 0);
        lv_obj_set_style_shadow_color(jack.bar, lv_color_hex(COLOR_CONNECTED), 0);
        lv_obj_set_style_shadow_opa(jack.bar, LV_OPA_40, 0);

        // Show VU bars
        if (jack.vuBarL) lv_obj_clear_flag(jack.vuBarL, LV_OBJ_FLAG_HIDDEN);
        if (jack.vuBarR) lv_obj_clear_flag(jack.vuBarR, LV_OBJ_FLAG_HIDDEN);
    } else {
        uint32_t color;
        if (jack.connected) {
            color = (jack.id == MIDI_IN || jack.id == MIDI_OUT) ? COLOR_MIDI_CONN : COLOR_CONNECTED;
        } else {
            color = COLOR_DISCONNECTED;
        }

        lv_obj_set_style_bg_color(jack.bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(jack.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(jack.bar, 0, 0);

        if (jack.connected) {
            lv_obj_set_style_shadow_width(jack.bar, 12, 0);
            lv_obj_set_style_shadow_spread(jack.bar, 2, 0);
            lv_obj_set_style_shadow_color(jack.bar, lv_color_hex(color), 0);
            lv_obj_set_style_shadow_opa(jack.bar, LV_OPA_60, 0);
        } else {
            lv_obj_set_style_shadow_width(jack.bar, 0, 0);
            lv_obj_set_style_shadow_opa(jack.bar, LV_OPA_TRANSP, 0);
        }

        // Hide VU bars for disconnected or MIDI jacks
        if (jack.vuBarL) lv_obj_add_flag(jack.vuBarL, LV_OBJ_FLAG_HIDDEN);
        if (jack.vuBarR) lv_obj_add_flag(jack.vuBarR, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── VU meter bar update ─────────────────────────────────────────────── */

void EmuJackPanel::updateVuBars(Jack& jack)
{
    if (!jack.connected || !jack.vuBarL || !jack.vuBarR) return;

    int32_t barW = lv_obj_get_width(jack.bar);
    int32_t barH = lv_obj_get_height(jack.bar);

    if (jack.vertical) {
        // Vertical: bars grow upward from bottom
        int32_t maxH = barH - 2;
        int32_t hL = levelToPercent(jack.levelL, maxH);
        int32_t hR = levelToPercent(jack.levelR, maxH);

        float pctL = (maxH > 0) ? (float)hL / (float)maxH : 0;
        float pctR = (maxH > 0) ? (float)hR / (float)maxH : 0;

        lv_obj_set_height(jack.vuBarL, hL);
        lv_obj_set_y(jack.vuBarL, 1 + (maxH - hL));
        lv_obj_set_style_bg_color(jack.vuBarL, vuColor(pctL), 0);

        lv_obj_set_height(jack.vuBarR, hR);
        lv_obj_set_y(jack.vuBarR, 1 + (maxH - hR));
        lv_obj_set_style_bg_color(jack.vuBarR, vuColor(pctR), 0);
    } else {
        // Horizontal: bars grow rightward from left
        int32_t maxW = barW - 2;
        int32_t wL = levelToPercent(jack.levelL, maxW);
        int32_t wR = levelToPercent(jack.levelR, maxW);

        float pctL = (maxW > 0) ? (float)wL / (float)maxW : 0;
        float pctR = (maxW > 0) ? (float)wR / (float)maxW : 0;

        lv_obj_set_width(jack.vuBarL, wL);
        lv_obj_set_style_bg_color(jack.vuBarL, vuColor(pctL), 0);

        lv_obj_set_width(jack.vuBarR, wR);
        lv_obj_set_style_bg_color(jack.vuBarR, vuColor(pctR), 0);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void EmuJackPanel::setDeviceName(JackId id, const std::string& name)
{
    if (id < 0 || id >= JACK_COUNT) return;
    Jack& jack = jacks_[id];
    if (jack.label) {
        if (name.empty()) {
            static const char* defaults[] = {
                "OUT 1", "OUT 2", "IN 1", "IN 2", "MIDI OUT", "MIDI IN"
            };
            lv_label_set_text(jack.label, defaults[id]);
            lv_obj_set_style_text_color(jack.label, lv_color_hex(0x999999), 0);
        } else {
            lv_label_set_text(jack.label, name.c_str());
            lv_obj_set_style_text_color(jack.label, lv_color_hex(0xCCCCCC), 0);
        }
    }
}

void EmuJackPanel::setConnected(JackId id, bool connected)
{
    if (id < 0 || id >= JACK_COUNT) return;
    jacks_[id].connected = connected;
    applyBarStyle(jacks_[id]);
}

void EmuJackPanel::setLevel(JackId id, int16_t left, int16_t right)
{
    if (id < 0 || id >= JACK_COUNT) return;
    jacks_[id].levelL = left;
    jacks_[id].levelR = right;
}

void EmuJackPanel::setDeviceList(JackId id,
                                  const std::vector<std::string>& devices,
                                  int currentIndex)
{
    if (id < 0 || id >= JACK_COUNT) return;
    Jack& jack = jacks_[id];
    if (!jack.dropdown) return;

    // Build newline-separated options string for LVGL dropdown
    std::string opts;
    for (size_t i = 0; i < devices.size(); i++) {
        if (i > 0) opts += '\n';
        opts += devices[i];
    }

    lv_dropdown_set_options(jack.dropdown, opts.c_str());
    if (currentIndex >= 0 && currentIndex < (int)devices.size()) {
        lv_dropdown_set_selected(jack.dropdown, (uint32_t)currentIndex);
    }
}

void EmuJackPanel::setOnDeviceSelected(DeviceSelectedCb cb)
{
    deviceSelectedCb_ = std::move(cb);
}

void EmuJackPanel::update()
{
    // Update VU bars for connected audio jacks
    for (int i = 0; i <= AUDIO_IN2; i++) {
        if (jacks_[i].connected) {
            updateVuBars(jacks_[i]);
        }
    }
}

/* ── Close all dropdowns ─────────────────────────────────────────────── */

void EmuJackPanel::closeAllDropdowns(int exceptJackId)
{
    for (int i = 0; i < JACK_COUNT; i++) {
        if (i == exceptJackId) continue;
        if (jacks_[i].dropdown && !lv_obj_has_flag(jacks_[i].dropdown, LV_OBJ_FLAG_HIDDEN)) {
            lv_dropdown_close(jacks_[i].dropdown);
            lv_obj_add_flag(jacks_[i].dropdown, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── Event callbacks ─────────────────────────────────────────────────── */

void EmuJackPanel::onScreenClicked(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    auto* self = static_cast<EmuJackPanel*>(lv_event_get_user_data(e));
    if (!self) return;

    // Only close if the click target is the screen itself (not a jack bar)
    if (target == self->parent_) {
        self->closeAllDropdowns();
    }
}

void EmuJackPanel::onBarClicked(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    auto* self = static_cast<EmuJackPanel*>(lv_obj_get_user_data(target));
    int jackId = (int)(intptr_t)lv_event_get_user_data(e);

    if (!self || jackId < 0 || jackId >= JACK_COUNT) return;

    Jack& jack = self->jacks_[jackId];
    if (!jack.dropdown) return;

    // Toggle dropdown visibility
    if (lv_obj_has_flag(jack.dropdown, LV_OBJ_FLAG_HIDDEN)) {
        // Close all other dropdowns first
        self->closeAllDropdowns(jackId);
        lv_obj_remove_flag(jack.dropdown, LV_OBJ_FLAG_HIDDEN);
        lv_dropdown_open(jack.dropdown);
    } else {
        lv_dropdown_close(jack.dropdown);
        lv_obj_add_flag(jack.dropdown, LV_OBJ_FLAG_HIDDEN);
    }
}

void EmuJackPanel::onDropdownChanged(lv_event_t* e)
{
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    auto* self = static_cast<EmuJackPanel*>(lv_obj_get_user_data(target));
    int jackId = (int)(intptr_t)lv_event_get_user_data(e);

    if (!self || jackId < 0 || jackId >= JACK_COUNT) return;

    uint32_t selected = lv_dropdown_get_selected(target);

    // Close and hide dropdown after selection
    lv_dropdown_close(target);
    lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);

    printf("[JackPanel] Jack %d: selected device index %u\n", jackId, selected);

    if (self->deviceSelectedCb_) {
        self->deviceSelectedCb_(jackId, selected);
    }
}

void EmuJackPanel::onDropdownListCreated(lv_event_t* e)
{
    // When a dropdown list opens, close all other dropdowns
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    auto* self = static_cast<EmuJackPanel*>(lv_obj_get_user_data(dropdown));
    if (!self) return;

    // Find which jack this dropdown belongs to
    for (int i = 0; i < JACK_COUNT; i++) {
        if (self->jacks_[i].dropdown == dropdown) {
            self->closeAllDropdowns(i);
            break;
        }
    }
}
