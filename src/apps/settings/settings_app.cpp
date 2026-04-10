#if USE_LVGL

#include "settings_app.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "pc_stubs/pc_platform.h"
#include <cstdio>

#ifdef USE_BLE
#include "midi/PcBleMidi.hpp"
#include "crosspad/settings/ISettingsUI.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
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

#ifdef USE_BLE

static uint8_t s_bleOffsetIdx = 4;

static const int8_t s_bleOffsets[] = {-48, -36, -24, -12, 0, 12, 24, 36, 48};
static constexpr size_t s_bleOffsetCount = sizeof(s_bleOffsets) / sizeof(s_bleOffsets[0]);

// UI state
static lv_obj_t* s_bleContent = nullptr;       // our wrapper (rebuilt, not the back button parent)
static lv_group_t* s_bleGroup = nullptr;
static lv_timer_t* s_bleRefreshTimer = nullptr;
static std::vector<std::string> s_bleScanAddresses;  // owns addresses for Connect button user_data

// Live-updated labels (no rebuild needed for these)
static lv_obj_t* s_bleStatusLabel = nullptr;    // "Disconnected" / "Connecting..." / "Connected"
static lv_obj_t* s_bleMidiInLabel = nullptr;    // last raw MIDI IN
static lv_obj_t* s_bleMidiOutLabel = nullptr;   // last raw MIDI OUT

// Tracked state for rebuild triggers
static bool s_bleLastConnected = false;
static bool s_bleLastScanning = false;
static size_t s_bleLastResultCount = 0;
static std::atomic<bool> s_bleConnecting{false};
static int s_bleDotCount = 0;

// Last raw MIDI messages (updated from callbacks, read by timer)
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

static uint8_t offsetToIndex(int8_t offset) {
    for (size_t i = 0; i < s_bleOffsetCount; i++) {
        if (s_bleOffsets[i] == offset) return static_cast<uint8_t>(i);
    }
    return 4;
}

// Forward declarations
static void cat_build_bluetooth(lv_obj_t* parent, lv_group_t* group);
static void ble_build_content(lv_obj_t* parent, lv_group_t* group);

static void bleRebuildPanel() {
    if (!s_bleContent || !lv_obj_is_valid(s_bleContent)) return;
    lv_obj_clean(s_bleContent);
    ble_build_content(s_bleContent, s_bleGroup);
}

/// Polling timer — updates live labels + triggers rebuild on state changes
static void bleRefreshTimerCb(lv_timer_t*) {
    bool connected = bleMidi.isConnected();
    bool scanning = bleMidi.isScanning();
    size_t resultCount = bleMidi.getScanResults().size();
    bool connecting = s_bleConnecting.load();

    // Update status label in-place (no rebuild)
    if (s_bleStatusLabel && lv_obj_is_valid(s_bleStatusLabel)) {
        if (connected) {
            lv_label_set_text(s_bleStatusLabel, "Connected");
        } else if (connecting) {
            s_bleDotCount = (s_bleDotCount + 1) % 4;
            const char* dots[] = {"Connecting", "Connecting.", "Connecting..", "Connecting..."};
            lv_label_set_text(s_bleStatusLabel, dots[s_bleDotCount]);
        } else if (scanning) {
            s_bleDotCount = (s_bleDotCount + 1) % 4;
            const char* dots[] = {"Scanning", "Scanning.", "Scanning..", "Scanning..."};
            lv_label_set_text(s_bleStatusLabel, dots[s_bleDotCount]);
        } else {
            lv_label_set_text(s_bleStatusLabel, "Disconnected");
        }
    }

    // Update raw MIDI labels in-place
    {
        std::lock_guard<std::mutex> lock(s_midiRawMutex);
        if (s_bleMidiInLabel && lv_obj_is_valid(s_bleMidiInLabel))
            lv_label_set_text(s_bleMidiInLabel, s_lastMidiIn);
        if (s_bleMidiOutLabel && lv_obj_is_valid(s_bleMidiOutLabel))
            lv_label_set_text(s_bleMidiOutLabel, s_lastMidiOut);
    }

    // Rebuild only on major state transitions
    if (connected != s_bleLastConnected ||
        scanning != s_bleLastScanning ||
        resultCount != s_bleLastResultCount) {
        s_bleLastConnected = connected;
        s_bleLastScanning = scanning;
        s_bleLastResultCount = resultCount;
        bleRebuildPanel();
    }
}

static void onBleModeChanged(lv_event_t* e) {
    (void)e;
    auto* s = crosspad::CrosspadSettings::getInstance();
    auto mode = s->wireless.bleMidiMode == 0
        ? crosspad::BleMidiMode::Host : crosspad::BleMidiMode::Server;
    bleMidi.end();
    bleMidi.begin(mode);
    bleRebuildPanel();
    auto* ui = crosspad::getSettingsUI();
    if (ui) ui->saveSettings();
}

static void onBleNoteOffsetChanged(lv_event_t* e) {
    (void)e;
    auto* s = crosspad::CrosspadSettings::getInstance();
    int8_t offset = s_bleOffsets[s_bleOffsetIdx];
    s->wireless.bleMidiNoteOffset = offset;
    bleMidi.setNoteOffset(offset);
    auto* ui = crosspad::getSettingsUI();
    if (ui) ui->saveSettings();
}

static void onBleScan(lv_event_t* e) {
    (void)e;
    bleMidi.startScan(5000);
}

static void onBleDisconnect(lv_event_t* e) {
    (void)e;
    bleMidi.disconnect();
}

static void onBleConnect(lv_event_t* ev) {
    auto* addr = static_cast<std::string*>(lv_event_get_user_data(ev));
    if (addr) {
        std::string a = *addr;
        s_bleConnecting.store(true);
        std::thread([a]() {
            bleMidi.connectToDevice(a);
            s_bleConnecting.store(false);
        }).detach();
    }
}

/// Build the BLE settings content widgets
static void ble_build_content(lv_obj_t* parent, lv_group_t* group) {
    auto* s = crosspad::CrosspadSettings::getInstance();

    // Reset live label pointers (old objects were cleaned)
    s_bleStatusLabel = nullptr;
    s_bleMidiInLabel = nullptr;
    s_bleMidiOutLabel = nullptr;

    // ── BLE Mode ──
    crosspad_gui::settings_section_header(parent, "BLE Mode");
    crosspad_gui::settings_row_dropdown(parent, group,
        "Mode", "Host (Central)\nServer (Peripheral)",
        &s->wireless.bleMidiMode, onBleModeChanged);

    // ── Connection Status ──
    crosspad_gui::settings_section_header(parent, "Connection");

    // Status row with live-updated label
    {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), 28);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Status");
        lv_obj_set_flex_grow(lbl, 1);
        lv_obj_set_style_text_color(lbl, lv_color_make(160, 160, 160), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

        s_bleStatusLabel = lv_label_create(row);
        // Left-align status text, fixed width so dots don't shift layout
        lv_obj_set_width(s_bleStatusLabel, 110);
        lv_obj_set_style_text_align(s_bleStatusLabel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(s_bleStatusLabel, &lv_font_montserrat_14, 0);

        bool connecting = s_bleConnecting.load();
        if (bleMidi.isConnected()) {
            lv_label_set_text(s_bleStatusLabel, "Connected");
            lv_obj_set_style_text_color(s_bleStatusLabel, lv_color_make(0, 200, 80), 0);
        } else if (connecting) {
            lv_label_set_text(s_bleStatusLabel, "Connecting...");
            lv_obj_set_style_text_color(s_bleStatusLabel, lv_color_make(255, 200, 0), 0);
        } else if (bleMidi.isScanning()) {
            lv_label_set_text(s_bleStatusLabel, "Scanning...");
            lv_obj_set_style_text_color(s_bleStatusLabel, lv_color_make(255, 200, 0), 0);
        } else {
            lv_label_set_text(s_bleStatusLabel, "Disconnected");
            lv_obj_set_style_text_color(s_bleStatusLabel, lv_color_white(), 0);
        }
    }

    if (bleMidi.isConnected()) {
        auto dev = bleMidi.getConnectedDevice();
        crosspad_gui::settings_row_info(parent, "Device", dev.name.c_str());
        crosspad_gui::settings_row_info(parent, "Address", dev.address.c_str());
        crosspad_gui::settings_row_action(parent, group,
            "Disconnect", "Disconnect", onBleDisconnect);
    }

    // ── Host mode: Scan + Device List ──
    if (bleMidi.getMode() == crosspad::BleMidiMode::Host && !bleMidi.isConnected()) {
        bool busy = bleMidi.isScanning() || s_bleConnecting.load();
        crosspad_gui::settings_row_action(parent, group,
            "Scan for devices", busy ? "..." : "Scan",
            onBleScan);

        auto results = bleMidi.getScanResults();
        if (!results.empty()) {
            crosspad_gui::settings_section_header(parent, "Devices Found");
            s_bleScanAddresses.clear();
            s_bleScanAddresses.reserve(results.size());
            for (auto& dev : results) {
                s_bleScanAddresses.push_back(dev.address);
            }
            for (size_t i = 0; i < results.size(); i++) {
                char label[96];
                snprintf(label, sizeof(label), "%s (%ddBm)",
                         results[i].name.c_str(), results[i].rssi);

                crosspad_gui::settings_row_action(parent, group,
                    label, "Connect", onBleConnect,
                    &s_bleScanAddresses[i]);
            }
        }
    }

    // ── Note Offset ──
    crosspad_gui::settings_section_header(parent, "MIDI");
    s_bleOffsetIdx = offsetToIndex(s->wireless.bleMidiNoteOffset);
    crosspad_gui::settings_row_dropdown(parent, group,
        "Note Offset", "-48\n-36\n-24\n-12\n0\n+12\n+24\n+36\n+48",
        &s_bleOffsetIdx, onBleNoteOffsetChanged);

    // ── Raw MIDI Monitor ──
    crosspad_gui::settings_section_header(parent, "MIDI Monitor");
    {
        // MIDI IN row
        lv_obj_t* rowIn = lv_obj_create(parent);
        lv_obj_set_size(rowIn, LV_PCT(100), 24);
        lv_obj_set_style_bg_opa(rowIn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rowIn, 0, 0);
        lv_obj_set_style_pad_hor(rowIn, 4, 0);
        lv_obj_set_style_pad_ver(rowIn, 0, 0);
        lv_obj_set_flex_flow(rowIn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rowIn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(rowIn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lblIn = lv_label_create(rowIn);
        lv_label_set_text(lblIn, "IN");
        lv_obj_set_width(lblIn, 30);
        lv_obj_set_style_text_color(lblIn, lv_color_make(100, 180, 255), 0);
        lv_obj_set_style_text_font(lblIn, &lv_font_montserrat_12, 0);

        s_bleMidiInLabel = lv_label_create(rowIn);
        lv_obj_set_style_text_font(s_bleMidiInLabel, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_bleMidiInLabel, lv_color_make(180, 180, 180), 0);
        {
            std::lock_guard<std::mutex> lock(s_midiRawMutex);
            lv_label_set_text(s_bleMidiInLabel, s_lastMidiIn);
        }

        // MIDI OUT row
        lv_obj_t* rowOut = lv_obj_create(parent);
        lv_obj_set_size(rowOut, LV_PCT(100), 24);
        lv_obj_set_style_bg_opa(rowOut, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rowOut, 0, 0);
        lv_obj_set_style_pad_hor(rowOut, 4, 0);
        lv_obj_set_style_pad_ver(rowOut, 0, 0);
        lv_obj_set_flex_flow(rowOut, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rowOut, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(rowOut, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lblOut = lv_label_create(rowOut);
        lv_label_set_text(lblOut, "OUT");
        lv_obj_set_width(lblOut, 30);
        lv_obj_set_style_text_color(lblOut, lv_color_make(100, 255, 140), 0);
        lv_obj_set_style_text_font(lblOut, &lv_font_montserrat_12, 0);

        s_bleMidiOutLabel = lv_label_create(rowOut);
        lv_obj_set_style_text_font(s_bleMidiOutLabel, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_bleMidiOutLabel, lv_color_make(180, 180, 180), 0);
        {
            std::lock_guard<std::mutex> lock(s_midiRawMutex);
            lv_label_set_text(s_bleMidiOutLabel, s_lastMidiOut);
        }
    }
}

/// Category builder — creates wrapper so rebuilds don't destroy back button
static void cat_build_bluetooth(lv_obj_t* parent, lv_group_t* group) {
    s_bleGroup = group;

    // Wrapper container (we rebuild only this, not the back button above)
    s_bleContent = lv_obj_create(parent);
    lv_obj_set_size(s_bleContent, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_bleContent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bleContent, 0, 0);
    lv_obj_set_style_pad_all(s_bleContent, 0, 0);
    lv_obj_set_style_pad_row(s_bleContent, 4, 0);
    lv_obj_set_flex_flow(s_bleContent, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_bleContent, LV_OBJ_FLAG_SCROLLABLE);

    ble_build_content(s_bleContent, group);

    // Polling timer (300ms for smooth dot animation)
    if (!s_bleRefreshTimer) {
        s_bleLastConnected = bleMidi.isConnected();
        s_bleLastScanning = bleMidi.isScanning();
        s_bleLastResultCount = bleMidi.getScanResults().size();
        s_bleRefreshTimer = lv_timer_create(bleRefreshTimerCb, 300, nullptr);
    }
}

#endif // USE_BLE

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

    // Register PC-specific settings categories
    crosspad_gui::settings_ui_clear_extra_categories();
    crosspad_gui::settings_ui_add_category({"USB / UART", LV_SYMBOL_USB, cat_build_usb});
#ifdef USE_BLE
    crosspad_gui::settings_ui_add_category({"Bluetooth MIDI", LV_SYMBOL_BLUETOOTH, cat_build_bluetooth});
#endif

    // Delegate to shared settings UI in crosspad-gui
    crosspad_gui::settings_ui_create(content, nullptr);

    return container;
}

void lv_DestroySettings(lv_obj_t * obj) {
    (void)obj;
#ifdef USE_BLE
    if (s_bleRefreshTimer) {
        lv_timer_delete(s_bleRefreshTimer);
        s_bleRefreshTimer = nullptr;
    }
    s_bleContent = nullptr;
    s_bleStatusLabel = nullptr;
    s_bleMidiInLabel = nullptr;
    s_bleMidiOutLabel = nullptr;
#endif
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
