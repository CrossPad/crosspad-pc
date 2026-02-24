#ifdef USE_LVGL

#include "settings_nav.h"
#include "pc_stubs/pc_platform.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad_app.hpp"

#include <cstdio>

// ---- Navigation state ----

static lv_obj_t * s_content = NULL;
static lv_group_t * s_group = NULL;
static int s_current_category = -1;

extern CrosspadSettings * settings;

// ---- Category declarations ----

extern void cat_build_display(lv_obj_t * parent, lv_group_t * group);
extern void cat_build_audio(lv_obj_t * parent, lv_group_t * group);
extern void cat_build_pads(lv_obj_t * parent, lv_group_t * group);
extern void cat_build_midi(lv_obj_t * parent, lv_group_t * group);
extern void cat_build_system(lv_obj_t * parent, lv_group_t * group);

const SettingsCategory settings_categories[] = {
    { "Display",       LV_SYMBOL_EYE_OPEN,   cat_build_display },
    { "Audio",         LV_SYMBOL_AUDIO,       cat_build_audio },
    { "Pads & LEDs",   LV_SYMBOL_KEYBOARD,    cat_build_pads },
    { "MIDI",          LV_SYMBOL_SHUFFLE,     cat_build_midi },
    { "System",        LV_SYMBOL_SETTINGS,    cat_build_system },
};
const int settings_category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);

// ---- Helper: clear content area ----

static void clear_content() {
    if (!s_content) return;
    lv_obj_clean(s_content);
    if (s_group) {
        lv_group_remove_all_objs(s_group);
    }
}

// ---- Click handlers ----

static void category_row_cb(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_nav_show_detail(idx);
}

static void back_row_cb(lv_event_t * e) {
    (void)e;
    settings_nav_show_categories();
}

static void exit_row_cb(lv_event_t * e) {
    (void)e;
    pc_platform_save_settings();
    crosspad_app_go_home();
}

// ---- Navigation API ----

void settings_nav_init(lv_obj_t * content_area, lv_group_t * group) {
    s_content = content_area;
    s_group = group;
    s_current_category = -1;
}

void settings_nav_show_categories() {
    clear_content();
    s_current_category = -1;

    // Exit row at top
    lv_obj_t * exit_btn = lv_button_create(s_content);
    lv_obj_set_size(exit_btn, LV_PCT(100), 32);
    lv_obj_add_style(exit_btn, &styleSettingsHeader, 0);
    lv_obj_set_style_bg_color(exit_btn, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_radius(exit_btn, 8, 0);
    lv_obj_set_style_outline_width(exit_btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(exit_btn, lv_color_white(), LV_STATE_FOCUSED);

    lv_obj_t * exit_row = lv_obj_create(exit_btn);
    lv_obj_set_size(exit_row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(exit_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(exit_row, 0, 0);
    lv_obj_set_style_pad_all(exit_row, 0, 0);
    lv_obj_set_flex_flow(exit_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(exit_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(exit_row, 6, 0);
    lv_obj_remove_flag(exit_row, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(exit_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t * exit_icon = lv_label_create(exit_row);
    lv_label_set_text(exit_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(exit_icon, lv_color_black(), 0);

    lv_obj_t * exit_label = lv_label_create(exit_row);
    lv_label_set_text(exit_label, "Exit Settings");
    lv_obj_set_style_text_color(exit_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(exit_label, &lv_font_montserrat_14, 0);

    lv_obj_add_event_cb(exit_btn, exit_row_cb, LV_EVENT_CLICKED, NULL);
    if (s_group) lv_group_add_obj(s_group, exit_btn);

    // Category rows
    for (int i = 0; i < settings_category_count; i++) {
        lv_obj_t * row = lv_button_create(s_content);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_add_style(row, &styleSettingsRow, 0);
        lv_obj_add_style(row, &styleSettingsRowFocused, LV_STATE_FOCUSED);

        lv_obj_t * inner = lv_obj_create(row);
        lv_obj_set_size(inner, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(inner, 0, 0);
        lv_obj_set_style_pad_all(inner, 0, 0);
        lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(inner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(inner, 8, 0);
        lv_obj_remove_flag(inner, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(inner, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t * icon = lv_label_create(inner);
        lv_label_set_text(icon, settings_categories[i].icon);
        lv_obj_set_style_text_color(icon, CROSSPAD_THEME_PRIMARY, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);

        lv_obj_t * name = lv_label_create(inner);
        lv_label_set_text(name, settings_categories[i].name);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);

        lv_obj_t * chev = lv_label_create(inner);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_make(100, 100, 100), 0);

        lv_obj_add_event_cb(row, category_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (s_group) lv_group_add_obj(s_group, row);
    }

    printf("[Settings] Category list shown\n");
}

void settings_nav_show_detail(int category_index) {
    if (category_index < 0 || category_index >= settings_category_count) return;

    clear_content();
    s_current_category = category_index;

    const SettingsCategory * cat = &settings_categories[category_index];

    // Back row
    lv_obj_t * back_btn = lv_button_create(s_content);
    lv_obj_set_size(back_btn, LV_PCT(100), 30);
    lv_obj_set_style_bg_color(back_btn, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_set_style_outline_width(back_btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(back_btn, lv_color_white(), LV_STATE_FOCUSED);

    lv_obj_t * back_inner = lv_obj_create(back_btn);
    lv_obj_set_size(back_inner, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(back_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back_inner, 0, 0);
    lv_obj_set_style_pad_all(back_inner, 0, 0);
    lv_obj_set_flex_flow(back_inner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back_inner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(back_inner, 6, 0);
    lv_obj_remove_flag(back_inner, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(back_inner, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t * back_icon = lv_label_create(back_inner);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_black(), 0);

    lv_obj_t * back_label = lv_label_create(back_inner);
    lv_label_set_text_fmt(back_label, "%s  %s", cat->icon, cat->name);
    lv_obj_set_style_text_color(back_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_14, 0);

    lv_obj_add_event_cb(back_btn, back_row_cb, LV_EVENT_CLICKED, NULL);
    if (s_group) lv_group_add_obj(s_group, back_btn);

    // Build category content
    if (cat->build) {
        cat->build(s_content, s_group);
    }

    printf("[Settings] Detail page: %s\n", cat->name);
}

bool settings_nav_go_back() {
    if (s_current_category >= 0) {
        settings_nav_show_categories();
        return true;
    }
    return false;
}

void settings_nav_cleanup() {
    s_content = NULL;
    s_group = NULL;
    s_current_category = -1;
}

// ==== Widget helpers ====

lv_obj_t * settings_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_add_style(lbl, &styleSettingsSection, 0);
    return lbl;
}

// ---- Switch row ----

struct SwitchRowCtx {
    bool * setting;
    const char * nvs_ns;
    const char * nvs_key;
    lv_event_cb_t extra_cb;
};

static void switch_value_changed_cb(lv_event_t * e) {
    SwitchRowCtx * ctx = (SwitchRowCtx *)lv_event_get_user_data(e);
    lv_obj_t * sw = (lv_obj_t *)lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    *(ctx->setting) = checked;
    pc_platform_save_settings();

    if (ctx->extra_cb) {
        ctx->extra_cb(e);
    }
}

lv_obj_t * settings_row_switch(lv_obj_t * parent, lv_group_t * group,
                                const char * label, bool * setting,
                                const char * nvs_ns, const char * nvs_key,
                                lv_event_cb_t extra_cb) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    lv_obj_t * sw = lv_switch_create(row);
    if (setting && *setting) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    SwitchRowCtx * ctx = new SwitchRowCtx{setting, nvs_ns, nvs_key, extra_cb};
    lv_obj_add_event_cb(sw, switch_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);

    if (group) lv_group_add_obj(group, sw);

    return row;
}

// ---- Slider row ----

struct SliderRowCtx {
    uint8_t * setting;
    const char * nvs_ns;
    const char * nvs_key;
    lv_event_cb_t extra_cb;
    lv_obj_t * value_label;
};

static void slider_value_changed_cb(lv_event_t * e) {
    SliderRowCtx * ctx = (SliderRowCtx *)lv_event_get_user_data(e);
    lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);

    *(ctx->setting) = (uint8_t)val;

    if (ctx->value_label) {
        lv_label_set_text_fmt(ctx->value_label, "%d", (int)val);
    }

    pc_platform_save_settings();

    if (ctx->extra_cb) {
        ctx->extra_cb(e);
    }
}

lv_obj_t * settings_row_slider(lv_obj_t * parent, lv_group_t * group,
                                const char * label, int min, int max,
                                uint8_t * setting,
                                const char * nvs_ns, const char * nvs_key,
                                lv_event_cb_t extra_cb) {
    lv_obj_t * label_row = lv_obj_create(parent);
    lv_obj_set_size(label_row, LV_PCT(100), 18);
    lv_obj_set_style_bg_opa(label_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label_row, 0, 0);
    lv_obj_set_style_pad_all(label_row, 0, 0);
    lv_obj_set_style_pad_hor(label_row, 4, 0);
    lv_obj_set_flex_flow(label_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(label_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(label_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(label_row);
    lv_label_set_text(lbl, label);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    lv_obj_t * val_lbl = lv_label_create(label_row);
    lv_label_set_text_fmt(val_lbl, "%d", setting ? (int)*setting : 0);
    lv_obj_set_style_text_color(val_lbl, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t * slider = lv_slider_create(parent);
    lv_obj_set_size(slider, LV_PCT(95), 10);
    lv_slider_set_range(slider, min, max);
    if (setting) lv_slider_set_value(slider, *setting, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, CROSSPAD_THEME_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 3, LV_PART_KNOB);

    SliderRowCtx * ctx = new SliderRowCtx{setting, nvs_ns, nvs_key, extra_cb, val_lbl};
    lv_obj_add_event_cb(slider, slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);

    if (group) lv_group_add_obj(group, slider);

    return label_row;
}

// ---- Info row ----

lv_obj_t * settings_row_info(lv_obj_t * parent, const char * label, const char * value) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_color(lbl, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    lv_obj_t * val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, lv_color_white(), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);

    return row;
}

// ---- Action button row ----

lv_obj_t * settings_row_action(lv_obj_t * parent, lv_group_t * group,
                                const char * label, const char * btn_text,
                                lv_event_cb_t callback, void * user_data) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (label) {
        lv_obj_t * lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        lv_obj_set_flex_grow(lbl, 1);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }

    lv_obj_t * btn = lv_button_create(row);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_bg_color(btn, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn, CROSSPAD_THEME_PRIMARY, LV_STATE_FOCUSED);

    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, btn_text);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);

    if (callback) {
        lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, user_data);
    }
    if (group) lv_group_add_obj(group, btn);

    return row;
}

#endif // USE_LVGL
