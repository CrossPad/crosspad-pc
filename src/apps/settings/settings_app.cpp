#if USE_LVGL

#include "settings_app.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include <cstdio>

lv_obj_t * lv_CreateSettings(lv_obj_t * parent, App * a) {
    (void)a;

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

    // Delegate to shared settings UI in crosspad-gui
    crosspad_gui::settings_ui_create(content, nullptr);

    return container;
}

void lv_DestroySettings(lv_obj_t * obj) {
    (void)obj;
    crosspad_gui::settings_ui_destroy();
}

void _register_Settings_app() {
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sgear.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());
    static const crosspad::AppEntry entry = {
        "Settings", icon_path,
        lv_CreateSettings, lv_DestroySettings,
        nullptr, nullptr, nullptr, nullptr, 10
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}

#endif // USE_LVGL
