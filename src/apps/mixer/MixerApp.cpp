/**
 * @file MixerApp.cpp
 * @brief Audio Mixer/Router LVGL app — GUI + registration.
 *
 * The AudioMixerEngine and MixerPadLogic run globally (started at boot).
 * This app is a pure view/controller — it connects to the global engine
 * for display and control, and disconnects on close without stopping anything.
 *
 * GUI layout (320x240):
 *   - Title bar (22px)
 *   - VU meters section (70px): IN1, IN2, SYN | OUT1, OUT2
 *   - Routing matrix (52px): 3x2 toggle grid
 *   - Channel strips (72px): volume slider + [M][S] per channel
 *   - Output masters (24px): OUT1 + OUT2 sliders
 */

#include "MixerApp.hpp"
#include "AudioMixerEngine.hpp"
#include "MixerPadLogic.hpp"

#include "pc_stubs/PcApp.hpp"
#include "pc_stubs/pc_platform.h"
#include "crosspad_app.hpp"

#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/pad/PadManager.hpp>
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/components/vu_meter.h"

#include "lvgl.h"

#include <cstdio>

/* ── Static state ──────────────────────────────────────────────────────── */

static App* s_thisApp = nullptr;
static lv_timer_t* s_vuTimer = nullptr;

// VU bar objects: 0=IN1, 1=IN2, 2=SYNTH, 3=OUT1, 4=OUT2
static lv_obj_t* s_vuBarsL[5] = {};
static lv_obj_t* s_vuBarsR[5] = {};
static lv_obj_t* s_vuLabels[5] = {};

// Routing matrix buttons [input][output]
static lv_obj_t* s_routeButtons[3][2] = {};

// Channel mute/solo buttons
static lv_obj_t* s_muteButtons[3] = {};
static lv_obj_t* s_soloButtons[3] = {};

// Channel volume sliders
static lv_obj_t* s_channelSliders[3] = {};

// Output controls
static lv_obj_t* s_outSliders[2] = {};
static lv_obj_t* s_outMuteButtons[2] = {};

// VU decay values
static int16_t s_vuDecay[5][2] = {};  // [channel][L/R]

/* ── Color constants ──────────────────────────────────────────────────── */

static const lv_color_t COL_BG        = lv_color_hex(0x000000);
static const lv_color_t COL_SLIDER_BG = lv_color_hex(0x222244);
static const lv_color_t COL_SLIDER_IND= lv_color_hex(0x6644AA);
static const lv_color_t COL_KNOB      = lv_color_hex(0x9966FF);
static const lv_color_t COL_MUTE_ON   = lv_color_hex(0xCC2222);
static const lv_color_t COL_MUTE_OFF  = lv_color_hex(0x333333);
static const lv_color_t COL_SOLO_ON   = lv_color_hex(0xCCAA00);
static const lv_color_t COL_SOLO_OFF  = lv_color_hex(0x333333);
static const lv_color_t COL_ROUTE_ON  = lv_color_hex(0x0099AA);
static const lv_color_t COL_ROUTE_OFF = lv_color_hex(0x222222);
static const lv_color_t COL_LABEL     = lv_color_hex(0x999999);
static const lv_color_t COL_VU_BG     = lv_color_hex(0x111111);

/* ── Forward declarations ─────────────────────────────────────────────── */

static void syncGuiFromEngine();

/* ── Helpers ───────────────────────────────────────────────────────────── */

static lv_color_t vuColor(int16_t level, int16_t max)
{
    if (max <= 0) return lv_color_hex(0x00AA00);
    float pct = (float)level / (float)max;
    if (pct > 0.85f) return lv_color_hex(0xFF2222);  // red
    if (pct > 0.60f) return lv_color_hex(0xDDAA00);  // yellow
    return lv_color_hex(0x00AA00);                    // green
}

/* ── Callbacks ─────────────────────────────────────────────────────────── */

static void on_close(lv_event_t* e)
{
    (void)e;
    if (s_thisApp) s_thisApp->destroyApp();
}

static void on_route_toggle(lv_event_t* e)
{
    auto& engine = getMixerEngine();
    // user_data encodes: input * 2 + output
    intptr_t code = (intptr_t)lv_event_get_user_data(e);
    int in  = (int)(code / 2);
    int out = (int)(code % 2);
    auto mi = static_cast<MixerInput>(in);
    auto mo = static_cast<MixerOutput>(out);

    engine.setRouteEnabled(mi, mo, !engine.isRouteEnabled(mi, mo));
    syncGuiFromEngine();
    pc_platform_save_mixer_state();
}

static void on_channel_mute(lv_event_t* e)
{
    auto& engine = getMixerEngine();
    intptr_t ch = (intptr_t)lv_event_get_user_data(e);
    auto mi = static_cast<MixerInput>(ch);
    engine.setChannelMute(mi, !engine.isChannelMuted(mi));
    syncGuiFromEngine();
    pc_platform_save_mixer_state();
}

static void on_channel_solo(lv_event_t* e)
{
    auto& engine = getMixerEngine();
    intptr_t ch = (intptr_t)lv_event_get_user_data(e);
    auto mi = static_cast<MixerInput>(ch);
    engine.setChannelSolo(mi, !engine.isChannelSoloed(mi));
    syncGuiFromEngine();
    pc_platform_save_mixer_state();
}

static void on_channel_volume(lv_event_t* e)
{
    auto& engine = getMixerEngine();
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    intptr_t ch = (intptr_t)lv_event_get_user_data(e);
    float vol = lv_slider_get_value(slider) / 100.0f;
    engine.setChannelVolume(static_cast<MixerInput>(ch), vol);
    pc_platform_save_mixer_state();
}

static void on_output_volume(lv_event_t* e)
{
    auto& engine = getMixerEngine();
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    intptr_t out = (intptr_t)lv_event_get_user_data(e);
    float vol = lv_slider_get_value(slider) / 100.0f;
    engine.setOutputVolume(static_cast<MixerOutput>(out), vol);
    pc_platform_save_mixer_state();
}

static void on_output_mute(lv_event_t* e)
{
    auto& engine = getMixerEngine();
    intptr_t out = (intptr_t)lv_event_get_user_data(e);
    auto mo = static_cast<MixerOutput>(out);
    engine.setOutputMute(mo, !engine.isOutputMuted(mo));
    syncGuiFromEngine();
    pc_platform_save_mixer_state();
}

/* ── Sync GUI state from engine ────────────────────────────────────────── */

static void syncGuiFromEngine()
{
    auto& engine = getMixerEngine();

    // Route buttons
    for (int in = 0; in < 3; in++) {
        for (int out = 0; out < 2; out++) {
            if (!s_routeButtons[in][out]) continue;
            auto mi = static_cast<MixerInput>(in);
            auto mo = static_cast<MixerOutput>(out);
            bool en = engine.isRouteEnabled(mi, mo);
            lv_obj_set_style_bg_color(s_routeButtons[in][out],
                                      en ? COL_ROUTE_ON : COL_ROUTE_OFF, 0);
        }
    }

    // Mute/Solo buttons
    for (int i = 0; i < 3; i++) {
        if (!s_muteButtons[i] || !s_soloButtons[i]) continue;
        auto ch = static_cast<MixerInput>(i);
        lv_obj_set_style_bg_color(s_muteButtons[i],
                                  engine.isChannelMuted(ch) ? COL_MUTE_ON : COL_MUTE_OFF, 0);
        lv_obj_set_style_bg_color(s_soloButtons[i],
                                  engine.isChannelSoloed(ch) ? COL_SOLO_ON : COL_SOLO_OFF, 0);
    }

    // Output mute buttons
    for (int i = 0; i < 2; i++) {
        if (!s_outMuteButtons[i]) continue;
        auto mo = static_cast<MixerOutput>(i);
        lv_obj_set_style_bg_color(s_outMuteButtons[i],
                                  engine.isOutputMuted(mo) ? COL_MUTE_ON : COL_MUTE_OFF, 0);
    }
}

/* ── VU meter timer ────────────────────────────────────────────────────── */

static void vu_timer_cb(lv_timer_t*)
{
    auto& engine = getMixerEngine();

    // Sync GUI toggles (mute/solo/route buttons) from engine state
    // so pad-driven changes are reflected immediately in the GUI
    syncGuiFromEngine();

    constexpr int DECAY = 230;  // 230/256 ~ 0.90
    constexpr int16_t BAR_MAX = 100;

    // Input channels
    for (int i = 0; i < 3; i++) {
        if (!s_vuBarsL[i] || !s_vuBarsR[i]) continue;
        int16_t rawL, rawR;
        engine.getChannelLevel(static_cast<MixerInput>(i), rawL, rawR);

        int16_t logL = (int16_t)int16toLogScale(rawL, BAR_MAX);
        int16_t logR = (int16_t)int16toLogScale(rawR, BAR_MAX);

        // Apply decay
        s_vuDecay[i][0] = static_cast<int16_t>((s_vuDecay[i][0] * DECAY) / 256);
        s_vuDecay[i][1] = static_cast<int16_t>((s_vuDecay[i][1] * DECAY) / 256);
        if (logL > s_vuDecay[i][0]) s_vuDecay[i][0] = logL;
        if (logR > s_vuDecay[i][1]) s_vuDecay[i][1] = logR;

        lv_bar_set_value(s_vuBarsL[i], s_vuDecay[i][0], LV_ANIM_OFF);
        lv_bar_set_value(s_vuBarsR[i], s_vuDecay[i][1], LV_ANIM_OFF);

        lv_obj_set_style_bg_color(s_vuBarsL[i], vuColor(s_vuDecay[i][0], BAR_MAX), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_vuBarsR[i], vuColor(s_vuDecay[i][1], BAR_MAX), LV_PART_INDICATOR);
    }

    // Output buses
    for (int i = 0; i < 2; i++) {
        int idx = 3 + i;
        if (!s_vuBarsL[idx] || !s_vuBarsR[idx]) continue;
        int16_t rawL, rawR;
        engine.getOutputLevel(static_cast<MixerOutput>(i), rawL, rawR);

        int16_t logL = (int16_t)int16toLogScale(rawL, BAR_MAX);
        int16_t logR = (int16_t)int16toLogScale(rawR, BAR_MAX);

        s_vuDecay[idx][0] = static_cast<int16_t>((s_vuDecay[idx][0] * DECAY) / 256);
        s_vuDecay[idx][1] = static_cast<int16_t>((s_vuDecay[idx][1] * DECAY) / 256);
        if (logL > s_vuDecay[idx][0]) s_vuDecay[idx][0] = logL;
        if (logR > s_vuDecay[idx][1]) s_vuDecay[idx][1] = logR;

        lv_bar_set_value(s_vuBarsL[idx], s_vuDecay[idx][0], LV_ANIM_OFF);
        lv_bar_set_value(s_vuBarsR[idx], s_vuDecay[idx][1], LV_ANIM_OFF);

        lv_obj_set_style_bg_color(s_vuBarsL[idx], vuColor(s_vuDecay[idx][0], BAR_MAX), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_vuBarsR[idx], vuColor(s_vuDecay[idx][1], BAR_MAX), LV_PART_INDICATOR);
    }
}

/* ── GUI builder helpers ───────────────────────────────────────────────── */

static lv_obj_t* make_vu_pair(lv_obj_t* parent, int idx, const char* label,
                               int32_t x, int32_t barH)
{
    // Container for the VU pair + label
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 38, barH + 14);
    lv_obj_set_pos(cont, x, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // Left bar
    lv_obj_t* barL = lv_bar_create(cont);
    lv_obj_set_size(barL, 8, barH);
    lv_obj_set_pos(barL, 8, 0);
    lv_bar_set_range(barL, 0, 100);
    lv_bar_set_value(barL, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(barL, COL_VU_BG, 0);
    lv_obj_set_style_bg_color(barL, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    lv_obj_set_style_radius(barL, 1, 0);
    lv_obj_set_style_radius(barL, 1, LV_PART_INDICATOR);
    s_vuBarsL[idx] = barL;

    // Right bar
    lv_obj_t* barR = lv_bar_create(cont);
    lv_obj_set_size(barR, 8, barH);
    lv_obj_set_pos(barR, 20, 0);
    lv_bar_set_range(barR, 0, 100);
    lv_bar_set_value(barR, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(barR, COL_VU_BG, 0);
    lv_obj_set_style_bg_color(barR, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    lv_obj_set_style_radius(barR, 1, 0);
    lv_obj_set_style_radius(barR, 1, LV_PART_INDICATOR);
    s_vuBarsR[idx] = barR;

    // Label
    lv_obj_t* lbl = lv_label_create(cont);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_vuLabels[idx] = lbl;

    return cont;
}

static lv_obj_t* make_small_button(lv_obj_t* parent, const char* text,
                                    lv_color_t bg, lv_event_cb_t cb,
                                    void* userData)
{
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 22, 18);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 3, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, userData);
    return btn;
}

/* ── App create / destroy ──────────────────────────────────────────────── */

lv_obj_t* Mixer_create(lv_obj_t* parent, App* a)
{
    s_thisApp = a;
    auto& engine = getMixerEngine();

    // Reset VU decay
    for (auto& d : s_vuDecay) { d[0] = 0; d[1] = 0; }

    // ── Root container ──
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cont, COL_BG, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 1, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Title bar (22px) ──────────────────────────────────────── */
    lv_obj_t* titleBar = lv_obj_create(cont);
    lv_obj_set_size(titleBar, lv_pct(100), 22);
    lv_obj_set_style_bg_opa(titleBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(titleBar, 0, 0);
    lv_obj_set_style_pad_all(titleBar, 0, 0);
    lv_obj_remove_flag(titleBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = lv_label_create(titleBar);
    lv_label_set_text(titleLabel, "Mixer");
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(titleLabel, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t* closeBtn = lv_button_create(titleBar);
    lv_obj_set_size(closeBtn, 28, 18);
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

    /* ── VU meters section (70px) ──────────────────────────────── */
    lv_obj_t* vuSection = lv_obj_create(cont);
    lv_obj_set_size(vuSection, lv_pct(100), 70);
    lv_obj_set_style_bg_opa(vuSection, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vuSection, 0, 0);
    lv_obj_set_style_pad_all(vuSection, 0, 0);
    lv_obj_remove_flag(vuSection, LV_OBJ_FLAG_SCROLLABLE);

    static const char* vuNames[] = {"IN1", "IN2", "SYN", "OUT1", "OUT2"};
    int vuX[] = {4, 48, 92, 172, 224};
    for (int i = 0; i < 5; i++) {
        make_vu_pair(vuSection, i, vuNames[i], vuX[i], 52);
    }

    // Separator between inputs and outputs
    lv_obj_t* sep = lv_obj_create(vuSection);
    lv_obj_set_size(sep, 1, 50);
    lv_obj_set_pos(sep, 145, 4);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Routing matrix (52px) ─────────────────────────────────── */
    lv_obj_t* routeSection = lv_obj_create(cont);
    lv_obj_set_size(routeSection, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(routeSection, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(routeSection, 0, 0);
    lv_obj_set_style_pad_all(routeSection, 0, 0);
    lv_obj_remove_flag(routeSection, LV_OBJ_FLAG_SCROLLABLE);

    // Column headers
    lv_obj_t* routeHdrLabel = lv_label_create(routeSection);
    lv_label_set_text(routeHdrLabel, "Routing");
    lv_obj_set_style_text_color(routeHdrLabel, lv_color_hex(0x78C8FF), 0);
    lv_obj_set_style_text_font(routeHdrLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(routeHdrLabel, 4, 0);

    static const char* colHeaders[] = {"OUT1", "OUT2"};
    for (int out = 0; out < 2; out++) {
        lv_obj_t* lbl = lv_label_create(routeSection);
        lv_label_set_text(lbl, colHeaders[out]);
        lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_pos(lbl, 70 + out * 120, 0);
    }

    // Row labels + toggle buttons
    static const char* rowLabels[] = {"IN1", "IN2", "SYN"};
    for (int in = 0; in < 3; in++) {
        int y = 13 + in * 13;

        lv_obj_t* lbl = lv_label_create(routeSection);
        lv_label_set_text(lbl, rowLabels[in]);
        lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_pos(lbl, 4, y + 2);

        for (int out = 0; out < 2; out++) {
            intptr_t code = in * 2 + out;
            lv_obj_t* btn = lv_button_create(routeSection);
            lv_obj_set_size(btn, 80, 12);
            lv_obj_set_pos(btn, 50 + out * 120, y);
            lv_obj_set_style_bg_color(btn, COL_ROUTE_OFF, 0);
            lv_obj_set_style_radius(btn, 2, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);

            lv_obj_t* btnLbl = lv_label_create(btn);
            lv_label_set_text(btnLbl, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_font(btnLbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(btnLbl, lv_color_white(), 0);
            lv_obj_center(btnLbl);

            lv_obj_add_event_cb(btn, on_route_toggle, LV_EVENT_CLICKED,
                                (void*)code);
            s_routeButtons[in][out] = btn;
        }
    }

    /* ── Channel strips (72px = 24px x 3) ──────────────────────── */
    static const char* chNames[] = {"IN1", "IN2", "SYN"};
    for (int ch = 0; ch < 3; ch++) {
        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_size(row, lv_pct(100), 22);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Label
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, chNames[ch]);
        lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_set_width(lbl, 30);

        // Volume slider — initialized from engine state
        lv_obj_t* slider = lv_slider_create(row);
        lv_obj_set_size(slider, 200, 10);
        lv_obj_align(slider, LV_ALIGN_LEFT_MID, 34, 0);
        lv_slider_set_range(slider, 0, 100);
        int curVol = (int)(engine.getChannelVolume(static_cast<MixerInput>(ch)) * 100.0f);
        lv_slider_set_value(slider, curVol, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, COL_SLIDER_BG, 0);
        lv_obj_set_style_bg_color(slider, COL_SLIDER_IND, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, COL_KNOB, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
        lv_obj_add_event_cb(slider, on_channel_volume, LV_EVENT_VALUE_CHANGED,
                            (void*)(intptr_t)ch);
        s_channelSliders[ch] = slider;

        // Mute button
        s_muteButtons[ch] = make_small_button(row, "M", COL_MUTE_OFF,
                                               on_channel_mute,
                                               (void*)(intptr_t)ch);
        lv_obj_align(s_muteButtons[ch], LV_ALIGN_RIGHT_MID, -26, 0);

        // Solo button
        s_soloButtons[ch] = make_small_button(row, "S", COL_SOLO_OFF,
                                               on_channel_solo,
                                               (void*)(intptr_t)ch);
        lv_obj_align(s_soloButtons[ch], LV_ALIGN_RIGHT_MID, -2, 0);
    }

    /* ── Output masters (24px) ─────────────────────────────────── */
    lv_obj_t* outRow = lv_obj_create(cont);
    lv_obj_set_size(outRow, lv_pct(100), 22);
    lv_obj_set_style_bg_opa(outRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outRow, 0, 0);
    lv_obj_set_style_pad_all(outRow, 0, 0);
    lv_obj_remove_flag(outRow, LV_OBJ_FLAG_SCROLLABLE);

    static const char* outNames[] = {"O1", "O2"};
    for (int out = 0; out < 2; out++) {
        int xBase = out * 158;

        lv_obj_t* lbl = lv_label_create(outRow);
        lv_label_set_text(lbl, outNames[out]);
        lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_pos(lbl, xBase + 2, 6);

        lv_obj_t* slider = lv_slider_create(outRow);
        lv_obj_set_size(slider, 100, 10);
        lv_obj_set_pos(slider, xBase + 22, 6);
        lv_slider_set_range(slider, 0, 100);
        int curVol = (int)(engine.getOutputVolume(static_cast<MixerOutput>(out)) * 100.0f);
        lv_slider_set_value(slider, curVol, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, COL_SLIDER_BG, 0);
        lv_obj_set_style_bg_color(slider, COL_SLIDER_IND, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, COL_KNOB, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
        lv_obj_add_event_cb(slider, on_output_volume, LV_EVENT_VALUE_CHANGED,
                            (void*)(intptr_t)out);
        s_outSliders[out] = slider;

        s_outMuteButtons[out] = make_small_button(outRow, "M", COL_MUTE_OFF,
                                                   on_output_mute,
                                                   (void*)(intptr_t)out);
        lv_obj_set_pos(s_outMuteButtons[out], xBase + 128, 2);
    }

    /* ── Lifecycle callbacks ─────────────────────────────────────── */
    if (a) {
        a->setOnShow([](lv_obj_t*) {
            crosspad::getPadManager().setActivePadLogic("Mixer");
            crosspad_app_update_pad_icon();
        });
        a->setOnHide([](lv_obj_t*) {
            crosspad::getPadManager().setActivePadLogic("");
            crosspad_app_update_pad_icon();
        });
    }

    /* ── VU meter timer (60 Hz) ────────────────────────────────── */
    s_vuTimer = lv_timer_create(vu_timer_cb, 16, nullptr);

    /* ── Sync initial GUI state from engine ────────────────────── */
    syncGuiFromEngine();

    printf("[Mixer] App GUI opened\n");
    return cont;
}

void Mixer_destroy(lv_obj_t* app_obj)
{
    // Stop VU timer
    if (s_vuTimer) {
        lv_timer_delete(s_vuTimer);
        s_vuTimer = nullptr;
    }

    // Clear GUI pointers (engine and pad logic keep running)
    s_thisApp = nullptr;
    for (auto& p : s_vuBarsL) p = nullptr;
    for (auto& p : s_vuBarsR) p = nullptr;
    for (auto& p : s_vuLabels) p = nullptr;
    for (auto& row : s_routeButtons) for (auto& p : row) p = nullptr;
    for (auto& p : s_muteButtons) p = nullptr;
    for (auto& p : s_soloButtons) p = nullptr;
    for (auto& p : s_channelSliders) p = nullptr;
    for (auto& p : s_outSliders) p = nullptr;
    for (auto& p : s_outMuteButtons) p = nullptr;

    lv_obj_delete_async(app_obj);
    printf("[Mixer] App GUI closed (engine keeps running)\n");
}

/* ── App registration ──────────────────────────────────────────────────── */

void _register_Mixer_app()
{
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sCrossPad_Logo_110w.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());

    static const crosspad::AppEntry entry = {
        "Mixer", icon_path, Mixer_create, Mixer_destroy,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}
