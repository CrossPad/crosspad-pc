/**
 * @file EmuJackPanel.cpp
 * @brief Visual audio/MIDI jack connectors for the CrossPad PC emulator.
 */

#include "EmuJackPanel.hpp"
#include <cstdio>

/* ── Layout constants ────────────────────────────────────────────────── */

// Audio OUT jacks — vertical bars on LEFT edge, at LCD height
static constexpr int32_t AOUT1_BAR_X = 8,   AOUT1_BAR_Y = 70;
static constexpr int32_t AOUT2_BAR_X = 8,   AOUT2_BAR_Y = 180;
static constexpr int32_t AOUT_BAR_W  = 8,   AOUT_BAR_H  = 55;
static constexpr int32_t AOUT_LBL_X  = 14;  // label to the right of bar
static constexpr int32_t AOUT_LBL_W  = 68;  // width becomes height when rotated 90°

// Audio IN jacks — horizontal bars on TOP edge, left side (closer together)
static constexpr int32_t AIN1_BAR_X = 50,   AIN1_BAR_Y = 2;
static constexpr int32_t AIN2_BAR_X = 120,  AIN2_BAR_Y = 2;
static constexpr int32_t AIN_BAR_W  = 50,   AIN_BAR_H  = 8;
static constexpr int32_t AIN_LBL_Y  = 13;   // label below bar
static constexpr int32_t AIN_LBL_W  = 60;

// MIDI jacks — horizontal bars on TOP edge, right side (closer together)
static constexpr int32_t MIDI_OUT_BAR_X = 330, MIDI_OUT_BAR_Y = 2;
static constexpr int32_t MIDI_IN_BAR_X  = 385, MIDI_IN_BAR_Y  = 2;
static constexpr int32_t MIDI_BAR_W     = 50,  MIDI_BAR_H     = 8;
static constexpr int32_t MIDI_LBL_Y     = 13;  // label below bar
static constexpr int32_t MIDI_LBL_W     = 60;

// Colors
static constexpr uint32_t COLOR_DISCONNECTED = 0x555555;
static constexpr uint32_t COLOR_CONNECTED    = 0x00CC66;
static constexpr uint32_t COLOR_MIDI_CONN    = 0x3399FF;

/* ── create ──────────────────────────────────────────────────────────── */

void EmuJackPanel::create(lv_obj_t* parent)
{
    parent_ = parent;

    // Audio OUT1 (read-only)
    createJack(parent, AUDIO_OUT1,
               AOUT1_BAR_X, AOUT1_BAR_Y, AOUT_BAR_W, AOUT_BAR_H,
               AOUT_LBL_X, AOUT1_BAR_Y + 2, AOUT_LBL_W,
               "OUT 1", false, LV_DIR_RIGHT);

    // Audio OUT2 (read-only)
    createJack(parent, AUDIO_OUT2,
               AOUT2_BAR_X, AOUT2_BAR_Y, AOUT_BAR_W, AOUT_BAR_H,
               AOUT_LBL_X, AOUT2_BAR_Y + 2, AOUT_LBL_W,
               "OUT 2", false, LV_DIR_RIGHT);

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

    // MIDI OUT (clickable) — top edge, right side
    createJack(parent, MIDI_OUT,
               MIDI_OUT_BAR_X, MIDI_OUT_BAR_Y, MIDI_BAR_W, MIDI_BAR_H,
               MIDI_OUT_BAR_X, MIDI_LBL_Y, MIDI_LBL_W,
               "MIDI OUT", true, LV_DIR_BOTTOM);

    // MIDI IN (clickable) — top edge, right side
    createJack(parent, MIDI_IN,
               MIDI_IN_BAR_X, MIDI_IN_BAR_Y, MIDI_BAR_W, MIDI_BAR_H,
               MIDI_IN_BAR_X, MIDI_LBL_Y, MIDI_LBL_W,
               "MIDI IN", true, LV_DIR_BOTTOM);
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
    jack.clickable = clickable;
    bool rotatedLabel = (id == AUDIO_OUT1 || id == AUDIO_OUT2);

    // --- Bar (thick line on device edge) ---
    jack.bar = lv_obj_create(parent);
    lv_obj_set_pos(jack.bar, barX, barY);
    lv_obj_set_size(jack.bar, barW, barH);
    lv_obj_set_style_radius(jack.bar, 3, 0);
    lv_obj_set_style_border_width(jack.bar, 0, 0);
    lv_obj_set_style_pad_all(jack.bar, 0, 0);
    lv_obj_remove_flag(jack.bar, LV_OBJ_FLAG_SCROLLABLE);

    applyBarStyle(jack);

    if (clickable) {
        lv_obj_add_flag(jack.bar, LV_OBJ_FLAG_CLICKABLE);
        // Remove from encoder/keyboard group — mouse-only interaction
        lv_obj_remove_flag(jack.bar, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_group_remove_obj(jack.bar);

        lv_obj_set_user_data(jack.bar, this);
        lv_obj_add_event_cb(jack.bar, onBarClicked, LV_EVENT_CLICKED, (void*)(intptr_t)id);

        // Hover effect
        lv_obj_set_style_bg_opa(jack.bar, LV_OPA_80, LV_STATE_PRESSED);
    } else {
        lv_obj_remove_flag(jack.bar, LV_OBJ_FLAG_CLICKABLE);
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
        // Rotate text 90° clockwise (reads top-to-bottom), pivot at top-left
        lv_obj_set_style_transform_rotation(jack.label, 900, 0);
        lv_obj_set_style_transform_pivot_x(jack.label, 0, 0);
        lv_obj_set_style_transform_pivot_y(jack.label, 0, 0);
    }

    // --- Dropdown (only for clickable jacks) ---
    if (clickable) {
        // Create on lv_layer_top() so it draws above the LCD container
        jack.dropdown = lv_dropdown_create(lv_layer_top());
        lv_dropdown_set_text(jack.dropdown, nullptr);
        lv_dropdown_set_options(jack.dropdown, "(None)");
        lv_obj_set_style_text_font(jack.dropdown, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_font(jack.dropdown, &lv_font_montserrat_10, LV_PART_INDICATOR);
        lv_obj_set_size(jack.dropdown, 180, 24);
        lv_obj_set_style_pad_all(jack.dropdown, 4, 0);
        lv_obj_set_style_bg_color(jack.dropdown, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_opa(jack.dropdown, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(jack.dropdown, lv_color_hex(0xDDDDDD), 0);
        lv_obj_set_style_border_color(jack.dropdown, lv_color_hex(0x555555), 0);
        lv_obj_set_style_border_width(jack.dropdown, 1, 0);
        lv_obj_set_style_radius(jack.dropdown, 4, 0);

        // Remove from encoder/keyboard group — mouse-only
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
            lv_obj_set_pos(jack.dropdown, barX - 184, barY);
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
    }
}

/* ── Bar styling ─────────────────────────────────────────────────────── */

void EmuJackPanel::applyBarStyle(Jack& jack)
{
    uint32_t color;
    if (jack.connected) {
        color = (jack.id == MIDI_IN || jack.id == MIDI_OUT) ? COLOR_MIDI_CONN : COLOR_CONNECTED;
    } else {
        color = COLOR_DISCONNECTED;
    }

    lv_obj_set_style_bg_color(jack.bar, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(jack.bar, LV_OPA_COVER, 0);

    if (jack.connected) {
        lv_obj_set_style_shadow_width(jack.bar, 12, 0);
        lv_obj_set_style_shadow_spread(jack.bar, 2, 0);
        lv_obj_set_style_shadow_color(jack.bar, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_opa(jack.bar, LV_OPA_60, 0);
    } else {
        lv_obj_set_style_shadow_width(jack.bar, 0, 0);
        lv_obj_set_style_shadow_opa(jack.bar, LV_OPA_TRANSP, 0);
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
    // Currently no periodic updates needed — labels and colors are set
    // reactively via setDeviceName/setConnected. Could add level metering here.
}

/* ── Event callbacks ─────────────────────────────────────────────────── */

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
        // Hide all other dropdowns first
        for (int i = 0; i < JACK_COUNT; i++) {
            if (self->jacks_[i].dropdown && i != jackId) {
                lv_obj_add_flag(self->jacks_[i].dropdown, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_remove_flag(jack.dropdown, LV_OBJ_FLAG_HIDDEN);
        lv_dropdown_open(jack.dropdown);
    } else {
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

    // Hide dropdown after selection
    lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);

    printf("[JackPanel] Jack %d: selected device index %u\n", jackId, selected);

    if (self->deviceSelectedCb_) {
        self->deviceSelectedCb_(jackId, selected);
    }
}
