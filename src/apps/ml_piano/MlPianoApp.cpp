/**
 * @file MlPianoApp.cpp
 * @brief ML Piano app — LVGL GUI with synth parameter sliders
 *
 * GUI layout (320x240):
 *   - Title bar with octave info + close (X) button
 *   - Preset dropdown
 *   - Synth parameter sliders (Attack, Decay, Sustain, Release, Feedback)
 *   - Octave +/- buttons at bottom
 */

#include "MlPianoApp.hpp"
#include "PianoPadLogic.hpp"
#include "synth/MlPianoSynth.hpp"
#include "pc_stubs/PcApp.hpp"
#include "pc_stubs/pc_platform.h"

#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/pad/PadManager.hpp>
#include "crosspad-gui/components/app_lifecycle.h"
#include "crosspad-gui/platform/IGuiPlatform.h"

#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <memory>

/* ── Preset names (matching FmSynth_Init channel presets) ────────────── */

static const char* const PRESET_NAMES[] = {
    "E-Piano",       // ch 0
    "E-Piano Slow",  // ch 1
    "Guitar",        // ch 2
    "Hard Voice",    // ch 3
    "Bassy Base",    // ch 4
    "Organ",         // ch 5
    "Some Bass",     // ch 6
    "Schnatter",     // ch 7
    "Harpsichord",   // ch 8
    "Harsh",         // ch 9
    "String Bass",   // ch 10
    "Kick Bass",     // ch 11
    "Wood Bass",     // ch 12
    "Saw Bass",      // ch 13
    "Plug Sound",    // ch 14
    "Fady Stuff",    // ch 15
};

/* ── Static state ────────────────────────────────────────────────────── */

static App* thisApp = nullptr;
static lv_obj_t* s_octaveLabel = nullptr;
static lv_obj_t* s_presetDropdown = nullptr;
static PianoPadLogic* s_padLogic = nullptr;
static std::shared_ptr<PianoPadLogic> s_padLogicShared;

/* ── Note name helper ────────────────────────────────────────────────── */

static const char* noteNameForMidi(uint8_t note)
{
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return names[note % 12];
}

static int octaveForMidi(uint8_t note)
{
    return (note / 12) - 1;
}

static void updateOctaveLabel()
{
    if (!s_octaveLabel || !s_padLogic) return;
    uint8_t base = s_padLogic->getBaseNote();
    lv_label_set_text_fmt(s_octaveLabel, "%s%d - %s%d",
        noteNameForMidi(base), octaveForMidi(base),
        noteNameForMidi(base + 15), octaveForMidi(base + 15));
}

/* ── Slider helper ───────────────────────────────────────────────────── */

static lv_obj_t* make_slider_row(lv_obj_t* parent, const char* name,
                                  int32_t min, int32_t max, int32_t init,
                                  lv_event_cb_t cb)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_width(lbl, 52);

    lv_obj_t* slider = lv_slider_create(row);
    lv_obj_set_size(slider, 200, 12);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 56, 0);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x222244), 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x6644AA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x9966FF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Value label
    lv_obj_t* valLbl = lv_label_create(row);
    lv_label_set_text_fmt(valLbl, "%d", init);
    lv_obj_set_style_text_color(valLbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(valLbl, &lv_font_montserrat_10, 0);
    lv_obj_align(valLbl, LV_ALIGN_RIGHT_MID, -2, 0);
    // Store value label pointer in slider user data
    lv_obj_set_user_data(slider, valLbl);

    return slider;
}

/* ── Callbacks ───────────────────────────────────────────────────────── */

static void on_close(lv_event_t* e)
{
    (void)e;
    if (thisApp) thisApp->destroyApp();
}

static void on_octave_up(lv_event_t* e)
{
    (void)e;
    if (!s_padLogic) return;
    s_padLogic->octaveUp();
    s_padLogic->colorPads(crosspad::getPadManager());
    updateOctaveLabel();
}

static void on_octave_down(lv_event_t* e)
{
    (void)e;
    if (!s_padLogic) return;
    s_padLogic->octaveDown();
    s_padLogic->colorPads(crosspad::getPadManager());
    updateOctaveLabel();
}

static void on_preset_changed(lv_event_t* e)
{
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dropdown);
    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth && sel < 16) {
        synth->setMidiChannel(static_cast<uint8_t>(sel));
        printf("[MlPiano] Preset: %s (ch %u)\n", PRESET_NAMES[sel], sel);
    }
}

// Helper to update the value label next to a slider
static void update_slider_val_label(lv_obj_t* slider)
{
    lv_obj_t* valLbl = (lv_obj_t*)lv_obj_get_user_data(slider);
    if (valLbl) {
        lv_label_set_text_fmt(valLbl, "%d", (int)lv_slider_get_value(slider));
    }
}

static void on_attack(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    float val = lv_slider_get_value(slider) / 100.0f;
    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth) synth->setAttack(val);
    update_slider_val_label(slider);
}

static void on_decay(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    float val = lv_slider_get_value(slider) / 100.0f;
    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth) synth->setDecay(val);
    update_slider_val_label(slider);
}

static void on_sustain(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    uint8_t val = static_cast<uint8_t>(lv_slider_get_value(slider));
    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth) synth->setSustain(val);
    update_slider_val_label(slider);
}

static void on_release(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    float val = lv_slider_get_value(slider) / 100.0f;
    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth) synth->setRelease(val);
    update_slider_val_label(slider);
}

static void on_feedback(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    float val = lv_slider_get_value(slider) / 100.0f;
    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth) synth->setFeedback(val);
    update_slider_val_label(slider);
}

/* ── App create / destroy ────────────────────────────────────────────── */

lv_obj_t* MlPiano_create(lv_obj_t* parent, App* a)
{
    thisApp = a;
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 2, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Title bar with close button ─────────────────────────── */
    lv_obj_t* titleBar = lv_obj_create(cont);
    lv_obj_set_size(titleBar, lv_pct(100), 24);
    lv_obj_set_style_bg_opa(titleBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(titleBar, 0, 0);
    lv_obj_set_style_pad_all(titleBar, 0, 0);
    lv_obj_remove_flag(titleBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = lv_label_create(titleBar);
    lv_label_set_text(titleLabel, "ML Piano");
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(titleLabel, LV_ALIGN_LEFT_MID, 4, 0);

    // Close (X) button
    lv_obj_t* closeBtn = lv_button_create(titleBar);
    lv_obj_set_size(closeBtn, 28, 20);
    lv_obj_align(closeBtn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x662222), 0);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0xAA3333), LV_STATE_PRESSED);
    lv_obj_set_style_radius(closeBtn, 4, 0);
    lv_obj_set_style_shadow_width(closeBtn, 0, 0);
    lv_obj_t* closeLbl = lv_label_create(closeBtn);
    lv_label_set_text(closeLbl, "X");
    lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(closeLbl);
    lv_obj_add_event_cb(closeBtn, on_close, LV_EVENT_CLICKED, nullptr);

    /* ── Preset dropdown ─────────────────────────────────────── */
    lv_obj_t* presetRow = lv_obj_create(cont);
    lv_obj_set_size(presetRow, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(presetRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(presetRow, 0, 0);
    lv_obj_set_style_pad_all(presetRow, 0, 0);
    lv_obj_remove_flag(presetRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* presetLbl = lv_label_create(presetRow);
    lv_label_set_text(presetLbl, "Preset");
    lv_obj_set_style_text_color(presetLbl, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(presetLbl, &lv_font_montserrat_10, 0);
    lv_obj_align(presetLbl, LV_ALIGN_LEFT_MID, 2, 0);

    s_presetDropdown = lv_dropdown_create(presetRow);
    static char presetOptions[512];
    presetOptions[0] = '\0';
    for (int i = 0; i < 16; i++) {
        if (i > 0) strcat(presetOptions, "\n");
        strcat(presetOptions, PRESET_NAMES[i]);
    }
    lv_dropdown_set_options(s_presetDropdown, presetOptions);
    lv_obj_set_size(s_presetDropdown, 200, 24);
    lv_obj_align(s_presetDropdown, LV_ALIGN_LEFT_MID, 56, 0);
    lv_obj_set_style_text_font(s_presetDropdown, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(s_presetDropdown, lv_color_hex(0x222244), 0);
    lv_obj_set_style_text_color(s_presetDropdown, lv_color_white(), 0);
    lv_obj_add_event_cb(s_presetDropdown, on_preset_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    auto* synth = static_cast<MlPianoSynth*>(pc_platform_get_synth_engine());
    if (synth) {
        lv_dropdown_set_selected(s_presetDropdown, synth->getMidiChannel());
    }

    /* ── Synth parameter sliders ─────────────────────────────── */
    make_slider_row(cont, "Attack",   0, 100, 50, on_attack);
    make_slider_row(cont, "Decay",    0, 100, 50, on_decay);
    make_slider_row(cont, "Sustain",  0, 127, 80, on_sustain);
    make_slider_row(cont, "Release",  0, 100, 30, on_release);
    make_slider_row(cont, "Feedback", 0, 100, 10, on_feedback);

    /* ── Octave row ──────────────────────────────────────────── */
    lv_obj_t* octRow = lv_obj_create(cont);
    lv_obj_set_size(octRow, lv_pct(100), 26);
    lv_obj_set_style_bg_opa(octRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(octRow, 0, 0);
    lv_obj_set_style_pad_all(octRow, 0, 0);
    lv_obj_remove_flag(octRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* btnDown = lv_button_create(octRow);
    lv_obj_set_size(btnDown, 44, 22);
    lv_obj_align(btnDown, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(btnDown, lv_color_hex(0x333355), 0);
    lv_obj_set_style_radius(btnDown, 4, 0);
    lv_obj_set_style_shadow_width(btnDown, 0, 0);
    lv_obj_t* lblDown = lv_label_create(btnDown);
    lv_label_set_text(lblDown, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lblDown, &lv_font_montserrat_12, 0);
    lv_obj_center(lblDown);
    lv_obj_add_event_cb(btnDown, on_octave_down, LV_EVENT_CLICKED, nullptr);

    s_octaveLabel = lv_label_create(octRow);
    lv_obj_set_style_text_color(s_octaveLabel, lv_color_hex(0x78C8FF), 0);
    lv_obj_set_style_text_font(s_octaveLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(s_octaveLabel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* btnUp = lv_button_create(octRow);
    lv_obj_set_size(btnUp, 44, 22);
    lv_obj_align(btnUp, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(btnUp, lv_color_hex(0x333355), 0);
    lv_obj_set_style_radius(btnUp, 4, 0);
    lv_obj_set_style_shadow_width(btnUp, 0, 0);
    lv_obj_t* lblUp = lv_label_create(btnUp);
    lv_label_set_text(lblUp, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lblUp, &lv_font_montserrat_12, 0);
    lv_obj_center(lblUp);
    lv_obj_add_event_cb(btnUp, on_octave_up, LV_EVENT_CLICKED, nullptr);

    /* ── Register pad logic ──────────────────────────────────── */
    if (synth) {
        s_padLogicShared = std::make_shared<PianoPadLogic>(*synth);
        s_padLogic = s_padLogicShared.get();

        crosspad::getPadManager().registerPadLogic("MLPiano", s_padLogicShared);
    }

    updateOctaveLabel();

    /* ── Lifecycle callbacks ─────────────────────────────────── */
    if (a) {
        a->setOnShow([](lv_obj_t*) {
            crosspad::getPadManager().setActivePadLogic("MLPiano");
        });
        a->setOnHide([](lv_obj_t*) {
            crosspad::getPadManager().setActivePadLogic("");
        });
    }

    printf("[MlPiano] App created\n");
    return cont;
}

void MlPiano_destroy(lv_obj_t* app_obj)
{
    crosspad::getPadManager().setActivePadLogic("");
    crosspad::getPadManager().unregisterPadLogic("MLPiano");
    s_padLogic = nullptr;
    s_padLogicShared.reset();
    s_octaveLabel = nullptr;
    s_presetDropdown = nullptr;
    thisApp = nullptr;

    lv_obj_delete_async(app_obj);
    printf("[MlPiano] App destroyed\n");
}

/* ── App registration ────────────────────────────────────────────────── */

void _register_MLPiano_app() {
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sCrossPad_Logo_110w.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());

    static const crosspad::AppEntry entry = {
        "MLPiano", icon_path, MlPiano_create, MlPiano_destroy,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}
