#if USE_LVGL

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad_app.hpp"
#include "uart/PcUart.hpp"
#include "lvgl.h"

#include <cstdio>
#include <string>
#include <vector>
#include <deque>

/* ── Layout constants ────────────────────────────────────────────────── */

static constexpr int32_t MAX_DISPLAY_LINES = 200;
static constexpr uint32_t POLL_INTERVAL_MS = 50;

/* ── Per-instance state ──────────────────────────────────────────────── */

struct SerialMonitorState {
    lv_obj_t* container     = nullptr;
    lv_obj_t* outputLabel   = nullptr;
    lv_obj_t* inputTA       = nullptr;
    lv_obj_t* statusLabel   = nullptr;
    lv_obj_t* baudDropdown  = nullptr;
    lv_obj_t* clearBtn      = nullptr;
    lv_obj_t* autoScrollBtn = nullptr;
    lv_obj_t* scrollArea    = nullptr;
    lv_timer_t* pollTimer   = nullptr;

    std::deque<std::string> lines;
    bool autoScroll = true;
    bool dirty      = false;   // text needs refresh
};

/* ── Baud rate options ───────────────────────────────────────────────── */

struct BaudOption {
    const char* label;
    PcUart::BaudRate rate;
};

static const BaudOption BAUD_OPTIONS[] = {
    {"9600",   PcUart::BaudRate::B9600},
    {"19200",  PcUart::BaudRate::B19200},
    {"38400",  PcUart::BaudRate::B38400},
    {"57600",  PcUart::BaudRate::B57600},
    {"115200", PcUart::BaudRate::B115200},
    {"230400", PcUart::BaudRate::B230400},
    {"460800", PcUart::BaudRate::B460800},
    {"921600", PcUart::BaudRate::B921600},
};
static constexpr int BAUD_COUNT = sizeof(BAUD_OPTIONS) / sizeof(BAUD_OPTIONS[0]);

static int findBaudIndex(PcUart::BaudRate rate) {
    for (int i = 0; i < BAUD_COUNT; i++) {
        if (BAUD_OPTIONS[i].rate == rate) return i;
    }
    return 4; // default 115200
}

/* ── Rebuild the displayed text ──────────────────────────────────────── */

static void rebuildOutputText(SerialMonitorState* st)
{
    if (!st->outputLabel) return;

    std::string text;
    for (auto& line : st->lines) {
        text += line;
        text += '\n';
    }

    lv_label_set_text(st->outputLabel, text.c_str());
    st->dirty = false;

    if (st->autoScroll && st->scrollArea) {
        lv_obj_scroll_to_y(st->scrollArea, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/* ── Poll timer ──────────────────────────────────────────────────────── */

static void onPollTimer(lv_timer_t* t)
{
    auto* st = static_cast<SerialMonitorState*>(lv_timer_get_user_data(t));
    auto& uart = pc_platform_get_uart();

    // Update status
    if (st->statusLabel) {
        if (uart.isOpen()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s @ %u",
                     uart.getPortName().c_str(),
                     static_cast<uint32_t>(uart.getBaudRate()));
            lv_label_set_text(st->statusLabel, buf);
            lv_obj_set_style_text_color(st->statusLabel, lv_color_hex(0xFF9900), 0);
        } else {
            lv_label_set_text(st->statusLabel, "Not connected");
            lv_obj_set_style_text_color(st->statusLabel, lv_color_hex(0x666666), 0);
        }
    }

    // Read new lines
    auto newLines = uart.readLines();
    if (newLines.empty()) return;

    for (auto& line : newLines) {
        st->lines.push_back(std::move(line));
    }

    // Trim to max
    while (st->lines.size() > MAX_DISPLAY_LINES) {
        st->lines.pop_front();
    }

    rebuildOutputText(st);
}

/* ── Input send callback ─────────────────────────────────────────────── */

static void onInputSend(lv_event_t* e)
{
    auto* st = static_cast<SerialMonitorState*>(lv_event_get_user_data(e));
    if (!st->inputTA) return;

    const char* text = lv_textarea_get_text(st->inputTA);
    if (!text || text[0] == '\0') return;

    auto& uart = pc_platform_get_uart();
    if (uart.isOpen()) {
        std::string msg(text);
        msg += "\r\n";
        uart.write(msg);

        // Echo to output
        st->lines.push_back(std::string("> ") + text);
        while (st->lines.size() > MAX_DISPLAY_LINES)
            st->lines.pop_front();
        rebuildOutputText(st);
    }

    lv_textarea_set_text(st->inputTA, "");
}

/* ── Clear button ────────────────────────────────────────────────────── */

static void onClearClicked(lv_event_t* e)
{
    auto* st = static_cast<SerialMonitorState*>(lv_event_get_user_data(e));
    st->lines.clear();
    if (st->outputLabel) {
        lv_label_set_text(st->outputLabel, "");
    }
}

/* ── Auto-scroll toggle ─────────────────────────────────────────────── */

static void onAutoScrollClicked(lv_event_t* e)
{
    auto* st = static_cast<SerialMonitorState*>(lv_event_get_user_data(e));
    st->autoScroll = !st->autoScroll;

    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    if (st->autoScroll) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF9900), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_white(), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_hex(0x888888), 0);
    }
}

/* ── Baud rate change ────────────────────────────────────────────────── */

static void onBaudChanged(lv_event_t* e)
{
    lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel >= (uint32_t)BAUD_COUNT) return;

    auto& uart = pc_platform_get_uart();
    if (uart.isOpen()) {
        std::string port = uart.getPortName();
        uart.close();
        uart.open(port, BAUD_OPTIONS[sel].rate);
    }
}

/* ── App create / destroy ────────────────────────────────────────────── */

static lv_obj_t* lv_CreateSerialMonitor(lv_obj_t* parent, App* a)
{
    (void)a;

    auto* st = new SerialMonitorState();

    // Root container
    st->container = lv_obj_create(parent);
    lv_obj_set_size(st->container, lv_obj_get_content_width(parent),
                    lv_obj_get_content_height(parent));
    lv_obj_set_style_bg_color(st->container, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(st->container, 0, 0);
    lv_obj_set_style_pad_all(st->container, 4, 0);
    lv_obj_set_style_radius(st->container, 0, 0);
    lv_obj_center(st->container);
    lv_obj_set_flex_flow(st->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(st->container, 3, 0);
    lv_obj_remove_flag(st->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(st->container, st);

    // ── Top toolbar ──
    lv_obj_t* toolbar = lv_obj_create(st->container);
    lv_obj_set_size(toolbar, LV_PCT(100), 22);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_pad_all(toolbar, 0, 0);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(toolbar, 4, 0);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Status label (port + baud)
    st->statusLabel = lv_label_create(toolbar);
    lv_label_set_text(st->statusLabel, "Not connected");
    lv_obj_set_style_text_font(st->statusLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(st->statusLabel, lv_color_hex(0x666666), 0);
    lv_obj_set_flex_grow(st->statusLabel, 1);

    // Baud rate dropdown
    st->baudDropdown = lv_dropdown_create(toolbar);
    std::string baudOpts;
    for (int i = 0; i < BAUD_COUNT; i++) {
        if (i > 0) baudOpts += '\n';
        baudOpts += BAUD_OPTIONS[i].label;
    }
    lv_dropdown_set_options(st->baudDropdown, baudOpts.c_str());
    lv_dropdown_set_selected(st->baudDropdown,
                             findBaudIndex(pc_platform_get_uart().getBaudRate()));
    lv_obj_set_size(st->baudDropdown, 70, 20);
    lv_obj_set_style_text_font(st->baudDropdown, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_all(st->baudDropdown, 2, 0);
    lv_obj_set_style_bg_color(st->baudDropdown, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(st->baudDropdown, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_color(st->baudDropdown, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(st->baudDropdown, 1, 0);
    lv_obj_set_style_radius(st->baudDropdown, 3, 0);
    lv_obj_add_event_cb(st->baudDropdown, onBaudChanged, LV_EVENT_VALUE_CHANGED, st);

    // Clear button
    st->clearBtn = lv_button_create(toolbar);
    lv_obj_set_size(st->clearBtn, 40, 20);
    lv_obj_set_style_bg_color(st->clearBtn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(st->clearBtn, 3, 0);
    lv_obj_set_style_pad_all(st->clearBtn, 0, 0);
    lv_obj_t* clrLbl = lv_label_create(st->clearBtn);
    lv_label_set_text(clrLbl, "CLR");
    lv_obj_set_style_text_font(clrLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(clrLbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_center(clrLbl);
    lv_obj_add_event_cb(st->clearBtn, onClearClicked, LV_EVENT_CLICKED, st);

    // Auto-scroll toggle
    st->autoScrollBtn = lv_button_create(toolbar);
    lv_obj_set_size(st->autoScrollBtn, 20, 20);
    lv_obj_set_style_bg_color(st->autoScrollBtn, lv_color_hex(0xFF9900), 0);
    lv_obj_set_style_radius(st->autoScrollBtn, 3, 0);
    lv_obj_set_style_pad_all(st->autoScrollBtn, 0, 0);
    lv_obj_t* scrollLbl = lv_label_create(st->autoScrollBtn);
    lv_label_set_text(scrollLbl, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(scrollLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(scrollLbl, lv_color_white(), 0);
    lv_obj_center(scrollLbl);
    lv_obj_add_event_cb(st->autoScrollBtn, onAutoScrollClicked, LV_EVENT_CLICKED, st);

    // ── Scroll area for output ──
    st->scrollArea = lv_obj_create(st->container);
    lv_obj_set_size(st->scrollArea, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(st->scrollArea, 1);
    lv_obj_set_style_bg_color(st->scrollArea, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(st->scrollArea, 1, 0);
    lv_obj_set_style_border_color(st->scrollArea, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(st->scrollArea, 4, 0);
    lv_obj_set_style_radius(st->scrollArea, 2, 0);
    lv_obj_add_flag(st->scrollArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(st->scrollArea, LV_SCROLLBAR_MODE_AUTO);

    // Output label (monospace-style, wrapping)
    st->outputLabel = lv_label_create(st->scrollArea);
    lv_label_set_text(st->outputLabel, "");
    lv_obj_set_width(st->outputLabel, LV_PCT(100));
    lv_obj_set_style_text_font(st->outputLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(st->outputLabel, lv_color_hex(0x00FF66), 0);
    lv_label_set_long_mode(st->outputLabel, LV_LABEL_LONG_WRAP);

    // ── Input row (text area + send button) ──
    lv_obj_t* inputRow = lv_obj_create(st->container);
    lv_obj_set_size(inputRow, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(inputRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(inputRow, 0, 0);
    lv_obj_set_style_pad_all(inputRow, 0, 0);
    lv_obj_remove_flag(inputRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(inputRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(inputRow, 4, 0);

    // Text input
    st->inputTA = lv_textarea_create(inputRow);
    lv_textarea_set_one_line(st->inputTA, true);
    lv_textarea_set_placeholder_text(st->inputTA, "Send...");
    lv_obj_set_flex_grow(st->inputTA, 1);
    lv_obj_set_height(st->inputTA, 26);
    lv_obj_set_style_text_font(st->inputTA, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(st->inputTA, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_text_color(st->inputTA, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_border_color(st->inputTA, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(st->inputTA, 1, 0);
    lv_obj_set_style_radius(st->inputTA, 3, 0);
    lv_obj_set_style_pad_all(st->inputTA, 4, 0);

    // Send button
    lv_obj_t* sendBtn = lv_button_create(inputRow);
    lv_obj_set_size(sendBtn, 44, 26);
    lv_obj_set_style_bg_color(sendBtn, lv_color_hex(0xFF9900), 0);
    lv_obj_set_style_radius(sendBtn, 3, 0);
    lv_obj_set_style_pad_all(sendBtn, 0, 0);
    lv_obj_t* sendLbl = lv_label_create(sendBtn);
    lv_label_set_text(sendLbl, "Send");
    lv_obj_set_style_text_font(sendLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(sendLbl, lv_color_white(), 0);
    lv_obj_center(sendLbl);
    lv_obj_add_event_cb(sendBtn, onInputSend, LV_EVENT_CLICKED, st);

    // Also send on Enter in text area
    lv_obj_add_event_cb(st->inputTA, onInputSend, LV_EVENT_READY, st);

    // ── Poll timer ──
    st->pollTimer = lv_timer_create(onPollTimer, POLL_INTERVAL_MS, st);

    printf("[SerialMonitor] App created\n");
    return st->container;
}

static void lv_DestroySerialMonitor(lv_obj_t* obj)
{
    auto* st = static_cast<SerialMonitorState*>(lv_obj_get_user_data(obj));
    if (st) {
        if (st->pollTimer) {
            lv_timer_delete(st->pollTimer);
        }
        delete st;
    }
    printf("[SerialMonitor] App destroyed\n");
}

/* ── Registration ────────────────────────────────────────────────────── */

void _register_SerialMonitor_app() {
    static const crosspad::AppEntry entry = {
        "Serial", LV_SYMBOL_USB,
        lv_CreateSerialMonitor, lv_DestroySerialMonitor,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}

#endif // USE_LVGL
