#pragma once

#ifdef USE_LVGL
#include "lvgl.h"

namespace crosspad { class CrosspadSettings; }
using crosspad::CrosspadSettings;

// ---- Category system ----

struct SettingsCategory {
    const char * name;
    const char * icon;  // LV_SYMBOL_*
    void (*build)(lv_obj_t * parent, lv_group_t * group);
};

extern const SettingsCategory settings_categories[];
extern const int settings_category_count;

// ---- Navigation API ----

void settings_nav_init(lv_obj_t * content_area, lv_group_t * group);
void settings_nav_show_categories();
void settings_nav_show_detail(int category_index);
bool settings_nav_go_back();
void settings_nav_cleanup();

// ---- Widget helpers ----

lv_obj_t * settings_section_header(lv_obj_t * parent, const char * text);

lv_obj_t * settings_row_switch(lv_obj_t * parent, lv_group_t * group,
                                const char * label, bool * setting,
                                const char * nvs_ns, const char * nvs_key,
                                lv_event_cb_t extra_cb = NULL);

lv_obj_t * settings_row_slider(lv_obj_t * parent, lv_group_t * group,
                                const char * label, int min, int max,
                                uint8_t * setting,
                                const char * nvs_ns, const char * nvs_key,
                                lv_event_cb_t extra_cb = NULL);

lv_obj_t * settings_row_info(lv_obj_t * parent, const char * label, const char * value);

lv_obj_t * settings_row_action(lv_obj_t * parent, lv_group_t * group,
                                const char * label, const char * btn_text,
                                lv_event_cb_t callback, void * user_data);

#endif // USE_LVGL
