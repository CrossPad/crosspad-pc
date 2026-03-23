#if USE_LVGL

#include "settings_app.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "pc_stubs/pc_platform.h"
#include <cstdio>

/* ── PC-specific settings: USB/UART ──────────────────────────────────── */

static bool s_usbAutoconnect = true;

static void onUsbAutoconnectChanged(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    s_usbAutoconnect = lv_obj_has_state(sw, LV_STATE_CHECKED);
    pc_platform_set_usb_autoconnect(s_usbAutoconnect);
}

static void cat_build_usb(lv_obj_t* parent, lv_group_t* group) {
    (void)group;
    s_usbAutoconnect = pc_platform_get_usb_autoconnect();

    crosspad_gui::settings_section_header(parent, "USB / UART");

    // Auto-connect switch
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Auto-connect CrossPad");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 40, 20);
    if (s_usbAutoconnect) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, onUsbAutoconnectChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    // Info label
    lv_obj_t* info = lv_label_create(parent);
    lv_label_set_text(info, "Detect CrossPad by USB VID:PID\n(0x303A:0x1001) and auto-connect");
    lv_obj_set_style_text_font(info, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0x888888), 0);
    lv_obj_set_width(info, LV_PCT(100));
}

/* ── App create / destroy ────────────────────────────────────────────── */

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

    // Register PC-specific USB settings category
    crosspad_gui::settings_ui_clear_extra_categories();
    crosspad_gui::settings_ui_add_category({"USB / UART", LV_SYMBOL_USB, cat_build_usb});

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
