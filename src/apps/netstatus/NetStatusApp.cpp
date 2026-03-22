/**
 * @file NetStatusApp.cpp
 * @brief Net Status app — network connectivity status + auto-update checker
 *
 * Shows HTTP capability status and checks for CrossPad PC updates
 * from GitHub Releases. Downloads and auto-replaces the exe via batch script.
 */

#if USE_LVGL

#include "NetStatusApp.hpp"
#include "pc_stubs/PcApp.hpp"
#include "updater/PcUpdater.hpp"

#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/net/IHttpClient.hpp>
#include <crosspad/platform/PlatformCapabilities.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/settings/CrosspadSettings.hpp>
#include "crosspad-gui/platform/IGuiPlatform.h"

#include "crosspad_pc_version.h"
#include <crosspad/CrosspadCoreVersion.hpp>
#include <crosspad-gui/CrosspadGuiVersion.hpp>

#include "lvgl.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

/* ── State ───────────────────────────────────────────────────────────── */

static App* s_app = nullptr;

static lv_obj_t* s_statusLabel  = nullptr;

// Update widgets
static lv_obj_t* s_versionLabel      = nullptr;
static lv_obj_t* s_latestLabel       = nullptr;
static lv_obj_t* s_checkBtn          = nullptr;
static lv_obj_t* s_downloadBtn       = nullptr;
static lv_obj_t* s_progressBar       = nullptr;
static lv_obj_t* s_updateStatusLabel = nullptr;
static lv_timer_t* s_updateTimer     = nullptr;

// Update state (shared with background thread)
static std::atomic<UpdateState> s_updateState{UpdateState::Idle};
static std::atomic<int>         s_updateProgress{0};
static std::mutex               s_updateMsgMutex;
static std::string              s_updateMsg;
static UpdateInfo               s_updateInfo;
static PcUpdater                s_updater;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void update_status_label()
{
    if (!s_statusLabel) return;

    bool httpCap  = crosspad::hasCapability(crosspad::Capability::Http);
    bool wifiCap  = crosspad::hasCapability(crosspad::Capability::WiFi);

    auto* settings = crosspad::getPlatformServices().settings;
    bool wifiEnabled = settings && settings->wireless.enableWiFi;

    static char buf[128];
    const char* wifiStr = wifiCap ? "Connected" : (wifiEnabled ? "Enabled" : "OFF");
    const char* httpStr = httpCap ? "Ready" : "N/A";
    snprintf(buf, sizeof(buf), "WiFi: %s  |  HTTP: %s", wifiStr, httpStr);
    lv_label_set_text(s_statusLabel, buf);

    lv_color_t color;
    if (httpCap)          color = lv_color_hex(0x44AA44);
    else if (wifiEnabled) color = lv_color_hex(0xAAAA44);
    else                  color = lv_color_hex(0xAA4444);
    lv_obj_set_style_text_color(s_statusLabel, color, 0);
}

/* ── Update UI helpers ──────────────────────────────────────────────── */

static void set_update_msg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(s_updateMsgMutex);
    s_updateMsg = msg;
}

static std::string get_update_msg()
{
    std::lock_guard<std::mutex> lock(s_updateMsgMutex);
    return s_updateMsg;
}

static void update_timer_cb(lv_timer_t*)
{
    UpdateState state = s_updateState.load();
    int progress = s_updateProgress.load();
    std::string msg = get_update_msg();

    // Update status label
    if (s_updateStatusLabel && !msg.empty()) {
        lv_label_set_text(s_updateStatusLabel, msg.c_str());

        lv_color_t color = lv_color_hex(0xCCCCCC);
        if (state == UpdateState::Error)          color = lv_color_hex(0xFF4444);
        else if (state == UpdateState::Checking || state == UpdateState::Downloading || state == UpdateState::Extracting)
                                                   color = lv_color_hex(0xAAAA44);
        else if (state == UpdateState::ReadyToInstall)
                                                   color = lv_color_hex(0x44AA44);
        lv_obj_set_style_text_color(s_updateStatusLabel, color, 0);
    }

    // Update progress bar
    if (s_progressBar) {
        if (state == UpdateState::Downloading) {
            lv_obj_remove_flag(s_progressBar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(s_progressBar, progress, LV_ANIM_ON);
        } else {
            lv_obj_add_flag(s_progressBar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Update latest version label after check completes
    if (s_latestLabel) {
        if (state == UpdateState::UpdateAvailable) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "Latest: v%s - Update available!",
                     s_updateInfo.latestVersion.c_str());
            lv_label_set_text(s_latestLabel, buf);
            lv_obj_set_style_text_color(s_latestLabel, lv_color_hex(0x44DD44), 0);
        } else if (state == UpdateState::Idle && !s_updateInfo.latestVersion.empty()
                   && !s_updateInfo.updateAvailable) {
            lv_label_set_text(s_latestLabel, "Up to date");
            lv_obj_set_style_text_color(s_latestLabel, lv_color_hex(0x888888), 0);
        }
    }

    // Show/hide download button
    if (s_downloadBtn) {
        if (state == UpdateState::UpdateAvailable) {
            lv_obj_remove_flag(s_downloadBtn, LV_OBJ_FLAG_HIDDEN);
        } else if (state == UpdateState::Downloading || state == UpdateState::Extracting) {
            lv_obj_add_flag(s_downloadBtn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Show/hide check button (hide during operations)
    if (s_checkBtn) {
        if (state == UpdateState::Checking || state == UpdateState::Downloading
            || state == UpdateState::Extracting) {
            lv_obj_add_flag(s_checkBtn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_checkBtn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Ready to install — trigger install
    if (state == UpdateState::ReadyToInstall) {
        s_updateState.store(UpdateState::Idle);
        s_updater.installAndRestart();
    }
}

/* ── Event callbacks ─────────────────────────────────────────────────── */

static void on_check_update_clicked(lv_event_t*)
{
    if (s_updateState.load() != UpdateState::Idle &&
        s_updateState.load() != UpdateState::Error &&
        s_updateState.load() != UpdateState::UpdateAvailable) {
        return; // already in progress
    }

    s_updateState.store(UpdateState::Checking);
    set_update_msg("Checking for updates...");

    std::thread([]() {
        s_updateInfo = s_updater.checkForUpdate();

        if (!s_updateInfo.errorMessage.empty()) {
            s_updateState.store(UpdateState::Error);
            set_update_msg(s_updateInfo.errorMessage);
        } else if (s_updateInfo.updateAvailable) {
            s_updateState.store(UpdateState::UpdateAvailable);
            char buf[128];
            snprintf(buf, sizeof(buf), "v%s available (%.1f MB)",
                     s_updateInfo.latestVersion.c_str(),
                     s_updateInfo.assetSize / 1048576.0);
            set_update_msg(buf);
        } else {
            s_updateState.store(UpdateState::Idle);
            set_update_msg("Up to date (v" + s_updateInfo.currentVersion + ")");
        }
    }).detach();
}

static void on_download_clicked(lv_event_t*)
{
    if (s_updateState.load() != UpdateState::UpdateAvailable) return;

    s_updateState.store(UpdateState::Downloading);
    set_update_msg("Starting download...");

    std::thread([]() {
        bool ok = s_updater.downloadUpdate(s_updateInfo,
            [](UpdateState state, int pct, const std::string& msg) {
                s_updateState.store(state);
                s_updateProgress.store(pct);
                set_update_msg(msg);
            });

        if (!ok && s_updateState.load() != UpdateState::Error) {
            s_updateState.store(UpdateState::Error);
            set_update_msg("Download failed");
        }
    }).detach();
}

/* ── Section header helper ──────────────────────────────────────────── */

static lv_obj_t* create_section_header(lv_obj_t* parent, const char* title)
{
    lv_obj_t* lbl = lv_label_create(parent);
    static char buf[64];
    snprintf(buf, sizeof(buf), "-- %s --", title);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(lbl, lv_pct(100));
    return lbl;
}

/* ── App create / destroy ────────────────────────────────────────────── */

lv_obj_t* NetStatus_create(lv_obj_t* parent, App* a)
{
    s_app = a;

    /* Root container */
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_style_pad_row(cont, 4, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    /* ── Title bar ───────────────────────────────────────────── */
    lv_obj_t* titleBar = lv_obj_create(cont);
    lv_obj_set_size(titleBar, lv_pct(100), 26);
    lv_obj_set_style_bg_opa(titleBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(titleBar, 0, 0);
    lv_obj_set_style_pad_all(titleBar, 0, 0);
    lv_obj_remove_flag(titleBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = lv_label_create(titleBar);
    lv_label_set_text(titleLabel, "Net Status");
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(titleLabel, LV_ALIGN_LEFT_MID, 4, 0);

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
    lv_obj_add_event_cb(closeBtn, [](lv_event_t*) {
        if (s_app) s_app->destroyApp();
    }, LV_EVENT_CLICKED, nullptr);

    /* ── Status row ──────────────────────────────────────────── */
    s_statusLabel = lv_label_create(cont);
    lv_obj_set_style_text_font(s_statusLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_statusLabel, lv_pct(100));
    update_status_label();

    /* ── Updates section ─────────────────────────────────────── */

    create_section_header(cont, "Updates");

    // Version info — all three repos
    s_versionLabel = lv_label_create(cont);
    lv_obj_set_style_text_font(s_versionLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_versionLabel, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_width(s_versionLabel, lv_pct(100));
    {
        static char verBuf[128];
        snprintf(verBuf, sizeof(verBuf), "PC: v%s  Core: v%s  GUI: v%s",
                 CROSSPAD_PC_VERSION, CROSSPAD_CORE_VERSION, CROSSPAD_GUI_VERSION);
        lv_label_set_text(s_versionLabel, verBuf);
    }

    // Latest version label
    s_latestLabel = lv_label_create(cont);
    lv_obj_set_style_text_font(s_latestLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_latestLabel, lv_color_hex(0x888888), 0);
    lv_obj_set_width(s_latestLabel, lv_pct(100));
    lv_label_set_text(s_latestLabel, "Latest: (not checked)");

    // Update button row
    lv_obj_t* updateBtnRow = lv_obj_create(cont);
    lv_obj_set_size(updateBtnRow, lv_pct(100), 32);
    lv_obj_set_style_bg_opa(updateBtnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(updateBtnRow, 0, 0);
    lv_obj_set_style_pad_all(updateBtnRow, 0, 0);
    lv_obj_set_style_pad_column(updateBtnRow, 6, 0);
    lv_obj_set_flex_flow(updateBtnRow, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(updateBtnRow, LV_OBJ_FLAG_SCROLLABLE);

    // Check for Updates button
    s_checkBtn = lv_button_create(updateBtnRow);
    lv_obj_set_size(s_checkBtn, 140, 28);
    lv_obj_set_style_bg_color(s_checkBtn, lv_color_hex(0x225588), 0);
    lv_obj_set_style_bg_color(s_checkBtn, lv_color_hex(0x3377AA), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_checkBtn, 4, 0);
    lv_obj_set_style_shadow_width(s_checkBtn, 0, 0);
    lv_obj_t* checkLbl = lv_label_create(s_checkBtn);
    lv_label_set_text(checkLbl, "Check for Updates");
    lv_obj_set_style_text_font(checkLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(checkLbl);
    lv_obj_add_event_cb(s_checkBtn, on_check_update_clicked, LV_EVENT_CLICKED, nullptr);

    // Download & Install button (hidden until update is found)
    s_downloadBtn = lv_button_create(updateBtnRow);
    lv_obj_set_size(s_downloadBtn, 140, 28);
    lv_obj_set_style_bg_color(s_downloadBtn, lv_color_hex(0x226622), 0);
    lv_obj_set_style_bg_color(s_downloadBtn, lv_color_hex(0x338833), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_downloadBtn, 4, 0);
    lv_obj_set_style_shadow_width(s_downloadBtn, 0, 0);
    lv_obj_t* dlLbl = lv_label_create(s_downloadBtn);
    lv_label_set_text(dlLbl, "Download & Install");
    lv_obj_set_style_text_font(dlLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(dlLbl);
    lv_obj_add_event_cb(s_downloadBtn, on_download_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_downloadBtn, LV_OBJ_FLAG_HIDDEN);

    // Progress bar (hidden until download starts)
    s_progressBar = lv_bar_create(cont);
    lv_obj_set_size(s_progressBar, lv_pct(100), 12);
    lv_bar_set_range(s_progressBar, 0, 100);
    lv_bar_set_value(s_progressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progressBar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(s_progressBar, lv_color_hex(0x2288AA), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progressBar, 3, 0);
    lv_obj_set_style_radius(s_progressBar, 3, LV_PART_INDICATOR);
    lv_obj_add_flag(s_progressBar, LV_OBJ_FLAG_HIDDEN);

    // Update status label
    s_updateStatusLabel = lv_label_create(cont);
    lv_obj_set_style_text_font(s_updateStatusLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_updateStatusLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_updateStatusLabel, lv_pct(100));
    lv_label_set_long_mode(s_updateStatusLabel, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_updateStatusLabel, "");

    // Start LVGL timer to poll update state from background thread
    s_updateTimer = lv_timer_create(update_timer_cb, 500, nullptr);

    printf("[NetStatus] App created\n");
    return cont;
}

void NetStatus_destroy(lv_obj_t* app_obj)
{
    if (s_updateTimer) {
        lv_timer_delete(s_updateTimer);
        s_updateTimer = nullptr;
    }

    s_app = nullptr;
    s_statusLabel = nullptr;
    s_versionLabel = nullptr;
    s_latestLabel = nullptr;
    s_checkBtn = nullptr;
    s_downloadBtn = nullptr;
    s_progressBar = nullptr;
    s_updateStatusLabel = nullptr;

    lv_obj_delete_async(app_obj);
    printf("[NetStatus] App destroyed\n");
}

/* ── App registration ────────────────────────────────────────────────── */

void _register_NetStatus_app()
{
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sCrossPad_Logo_110w.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());

    static const crosspad::AppEntry entry = {
        "Net Status", icon_path,
        NetStatus_create, NetStatus_destroy,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}

#endif // USE_LVGL
