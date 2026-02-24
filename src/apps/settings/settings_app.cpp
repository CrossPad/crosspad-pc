#if USE_LVGL

#include "settings_app.h"
#include "settings_nav.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "pc_stubs/pc_platform.h"

extern CrosspadSettings * settings;

static App * s_app = NULL;

lv_obj_t * lv_CreateSettings(lv_obj_t * parent, App * a) {
    s_app = a;

    // Main container
    lv_obj_t * container = lv_obj_create(parent);
    lv_obj_set_size(container, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_center(container);

    // Scrollable content area
    lv_obj_t * content = lv_obj_create(container);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    settings_nav_init(content, nullptr);
    settings_nav_show_categories();

    return container;
}

void lv_DestroySettings(lv_obj_t * obj) {
    pc_platform_save_settings();
    settings_nav_cleanup();
    s_app = NULL;
}

REGISTER_APP(Settings, nullptr, "gear.png",
             lv_CreateSettings, lv_DestroySettings, nullptr, nullptr, nullptr, 10);

#endif // USE_LVGL
