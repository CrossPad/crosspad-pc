#if USE_LVGL

#include "settings_app.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/components/app_icon.h"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "pc_stubs/pc_platform.h"
#include <cstdio>

#ifdef USE_BLE
#include "midi/PcBleMidi.hpp"
#include "crosspad-gui/components/midi_ble_panel.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
extern PcBleMidi bleMidi;
#endif

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

/* ── PC-specific settings: Bluetooth MIDI ───────────────────────────── */

/* crosspad-gui's page (midi_ble_panel.h) over SimpleBLE + RtMidi. A connect
 * blocks until the link is up, so it runs on a thread of its own and the
 * State row says "connecting" meanwhile. The stack runs from start-up
 * whatever wireless.enableBLE says, so the page has no Enabled switch here;
 * PcBleMidi does not apply the send transpose, so no Send transpose either. */

#ifdef USE_BLE

static std::atomic<bool> s_bleConnecting{false};

// Last raw MIDI messages (written from the BLE/RtMidi threads, read by the page)
static char s_lastMidiIn[48]  = "---";
static char s_lastMidiOut[48] = "---";
static std::mutex s_midiRawMutex;

/// Called from BLE MIDI input callbacks to record last raw message
void ble_settings_log_midi_in(uint8_t status, uint8_t d1, uint8_t d2) {
    std::lock_guard<std::mutex> lock(s_midiRawMutex);
    snprintf(s_lastMidiIn, sizeof(s_lastMidiIn), "%02X %02X %02X", status, d1, d2);
}

/// Called from BLE MIDI output to record last raw message
void ble_settings_log_midi_out(uint8_t status, uint8_t d1, uint8_t d2) {
    std::lock_guard<std::mutex> lock(s_midiRawMutex);
    snprintf(s_lastMidiOut, sizeof(s_lastMidiOut), "%02X %02X %02X", status, d1, d2);
}

static const char* ble_state_name() {
    if (s_bleConnecting.load()) return "connecting";
    if (bleMidi.isConnected())  return "connected";
    if (bleMidi.isScanning())   return "scanning";
    if (bleMidi.isAdvertising()) return "advertising";
    return "idle";
}

static void ble_set_mode(uint8_t mode) {
    bleMidi.end();
    bleMidi.begin(mode == 0 ? crosspad::BleMidiMode::Host : crosspad::BleMidiMode::Server);
}

static void ble_connect(const std::string& address) {
    s_bleConnecting.store(true);
    std::thread([address]() {
        bleMidi.connectToDevice(address);
        s_bleConnecting.store(false);
    }).detach();
}

static void ble_last_messages(char* in, size_t inLen, char* out, size_t outLen) {
    std::lock_guard<std::mutex> lock(s_midiRawMutex);
    snprintf(in, inLen, "%s", s_lastMidiIn);
    snprintf(out, outLen, "%s", s_lastMidiOut);
}

static void register_ble_category() {
    crosspad_gui::MidiBleCallbacks cb;
    cb.setMode         = ble_set_mode;
    cb.applyRxOffset   = [] {
        bleMidi.setNoteOffset(crosspad::CrosspadSettings::getInstance()->wireless.bleMidiNoteOffset);
    };
    cb.stateName       = ble_state_name;
    cb.connectedDevice = [] { return bleMidi.getConnectedDevice(); };
    cb.scanResults     = [] { return bleMidi.getScanResults(); };
    cb.lastMessages    = ble_last_messages;
    cb.startScan       = [] { bleMidi.startScan(5000); };
    cb.connect         = ble_connect;
    cb.disconnect      = [] { bleMidi.disconnect(); };
    crosspad_gui::midi_ble_set_callbacks(cb);
    crosspad_gui::settings_ui_add_category({"BLE MIDI", LV_SYMBOL_BLUETOOTH, crosspad_gui::midi_ble_settings_build});
}

#endif // USE_BLE

/* ── App create / destroy ────────────────────────────────────────────── */

lv_obj_t * lv_CreateSettings(lv_obj_t * parent, App * a) {
    /* Power-button back: one level up inside Settings, and only when there is
     * a level to pop — from the category list the framework closes the app. */
    if (a) a->setOnBack([](lv_obj_t *) { return crosspad_gui::settings_ui_back(); });

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

    // Register PC-specific settings categories
    crosspad_gui::settings_ui_clear_extra_categories();
    crosspad_gui::settings_ui_add_category({"USB / UART", LV_SYMBOL_USB, cat_build_usb});
#ifdef USE_BLE
    register_ble_category();
#endif

    // Delegate to shared settings UI in crosspad-gui
    crosspad_gui::settings_ui_create(content, nullptr);

    return container;
}

void lv_DestroySettings(lv_obj_t * obj) {
    crosspad_gui::settings_ui_destroy();

    /* The root is this function's to delete — the framework only calls it.
     * Without this the settings tree stayed on screen after any close that is
     * not the Exit button (which rebuilds the main screen and hid the leak):
     * the app was gone, its widgets were not. */
    lv_obj_delete_async(obj);
}

// The shared asset set ships settings.png; "gear.png" never existed, which
// is why the Settings tile came up blank.
REGISTER_APP(Settings, nullptr, crosspad_gui::resolveAppIcon("settings.png"),
             lv_CreateSettings, lv_DestroySettings,
             nullptr, nullptr, nullptr, 10)

#endif // USE_LVGL
