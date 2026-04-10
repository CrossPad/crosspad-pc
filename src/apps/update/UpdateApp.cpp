/**
 * @file UpdateApp.cpp
 * @brief Update app — version check, download, release notes, version cache & rollback
 */

#if USE_LVGL

#include "UpdateApp.hpp"
#include "pc_stubs/PcApp.hpp"
#include "pc_stubs/pc_platform.h"
#include "updater/PcUpdater.hpp"

#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/platform/PlatformCapabilities.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/components/markdown_view.h"

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
#include <vector>

/* ── State ───────────────────────────────────────────────────────────── */

static App* s_app = nullptr;

// Widgets
static lv_obj_t* s_versionLabel      = nullptr;
static lv_obj_t* s_latestLabel       = nullptr;
static lv_obj_t* s_checkBtn          = nullptr;
static lv_obj_t* s_downloadBtn       = nullptr;
static lv_obj_t* s_progressBar       = nullptr;
static lv_obj_t* s_updateStatusLabel = nullptr;
static lv_obj_t* s_releaseNotesBox   = nullptr;
static lv_obj_t* s_versionListBox    = nullptr;
static lv_timer_t* s_updateTimer     = nullptr;

// Update state (shared with background thread)
static std::atomic<UpdateState> s_updateState{UpdateState::Idle};
static std::atomic<int>         s_updateProgress{0};
static std::mutex               s_updateMsgMutex;
static std::string              s_updateMsg;
static UpdateInfo               s_updateInfo;
static PcUpdater                s_updater;

// Releases list (shared with background thread)
static std::mutex               s_releasesMutex;
static std::vector<ReleaseInfo> s_releases;
static std::atomic<bool>        s_releasesFetched{false};
static std::atomic<bool>        s_notesNeedRender{false};
static std::string              s_notesToRender;

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

static void set_notes_to_render(const std::string& notes)
{
    std::lock_guard<std::mutex> lock(s_releasesMutex);
    s_notesToRender = notes;
    s_notesNeedRender.store(true);
}

/* ── Version list UI builder ─────────────────────────────────────────── */

static void build_version_list()
{
    if (!s_versionListBox) return;
    lv_obj_clean(s_versionListBox);

    std::lock_guard<std::mutex> lock(s_releasesMutex);

    for (size_t i = 0; i < s_releases.size(); i++) {
        auto& rel = s_releases[i];

        lv_obj_t* row = lv_obj_create(s_versionListBox);
        lv_obj_set_size(row, lv_pct(100), 28);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_pad_column(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Version label
        lv_obj_t* verLbl = lv_label_create(row);
        static char verBuf[32];
        snprintf(verBuf, sizeof(verBuf), "v%s", rel.version.c_str());
        lv_label_set_text(verLbl, verBuf);
        lv_obj_set_style_text_font(verLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_flex_grow(verLbl, 1);

        if (rel.isCurrent) {
            lv_obj_set_style_text_color(verLbl, lv_color_hex(0x44DD44), 0);
            lv_obj_t* tag = lv_label_create(row);
            lv_label_set_text(tag, "Current");
            lv_obj_set_style_text_font(tag, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(tag, lv_color_hex(0x44DD44), 0);
        } else {
            lv_obj_set_style_text_color(verLbl, lv_color_hex(0xBBBBBB), 0);

            // Action button
            lv_obj_t* btn = lv_button_create(row);
            lv_obj_set_size(btn, 60, 22);
            lv_obj_set_style_radius(btn, 3, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);

            lv_obj_t* btnLbl = lv_label_create(btn);
            lv_obj_set_style_text_font(btnLbl, &lv_font_montserrat_10, 0);
            lv_obj_center(btnLbl);

            if (rel.isCached) {
                lv_label_set_text(btnLbl, "Install");
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x225588), 0);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x3377AA), LV_STATE_PRESSED);
            } else {
                lv_label_set_text(btnLbl, "Download");
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x666666), LV_STATE_PRESSED);
            }

            // Store index in user data for click handler
            lv_obj_add_event_cb(btn, [](lv_event_t* e) {
                int idx = (int)(uintptr_t)lv_event_get_user_data(e);
                ReleaseInfo rel;
                {
                    std::lock_guard<std::mutex> lk(s_releasesMutex);
                    if (idx < 0 || idx >= (int)s_releases.size()) return;
                    rel = s_releases[idx];
                }

                if (rel.isCached) {
                    s_updater.installCachedAndRestart(rel.version);
                } else {
                    // Download in background, then install from cache
                    s_updateState.store(UpdateState::Downloading);
                    set_update_msg("Downloading v" + rel.version + "...");
                    std::string ver = rel.version;
                    std::thread([rel, ver]() {
                        bool ok = s_updater.downloadAndCache(rel,
                            [](UpdateState state, int pct, const std::string& msg) {
                                s_updateState.store(state);
                                s_updateProgress.store(pct);
                                set_update_msg(msg);
                            });
                        if (ok) {
                            // Cached now — install directly instead of going through timer
                            s_updateState.store(UpdateState::Idle);
                            s_updater.installCachedAndRestart(ver);
                        } else if (s_updateState.load() != UpdateState::Error) {
                            s_updateState.store(UpdateState::Error);
                            set_update_msg("Download failed");
                        }
                    }).detach();
                }
            }, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        }

        // Click row to show release notes
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(uintptr_t)lv_event_get_user_data(e);
            std::lock_guard<std::mutex> lk(s_releasesMutex);
            if (idx >= 0 && idx < (int)s_releases.size()) {
                set_notes_to_render(s_releases[idx].releaseNotes);
            }
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ── Timer callback ──────────────────────────────────────────────────── */

static void update_timer_cb(lv_timer_t*)
{
    UpdateState state = s_updateState.load();
    int progress = s_updateProgress.load();
    std::string msg = get_update_msg();

    // Status label
    if (s_updateStatusLabel && !msg.empty()) {
        lv_label_set_text(s_updateStatusLabel, msg.c_str());
        lv_color_t color = lv_color_hex(0xCCCCCC);
        if (state == UpdateState::Error) color = lv_color_hex(0xFF4444);
        else if (state == UpdateState::Checking || state == UpdateState::Downloading || state == UpdateState::Extracting)
            color = lv_color_hex(0xAAAA44);
        else if (state == UpdateState::ReadyToInstall) color = lv_color_hex(0x44AA44);
        lv_obj_set_style_text_color(s_updateStatusLabel, color, 0);
    }

    // Progress bar
    if (s_progressBar) {
        if (state == UpdateState::Downloading) {
            lv_obj_remove_flag(s_progressBar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(s_progressBar, progress, LV_ANIM_ON);
        } else {
            lv_obj_add_flag(s_progressBar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Latest version label
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

    // Show/hide buttons
    if (s_downloadBtn) {
        if (state == UpdateState::UpdateAvailable)
            lv_obj_remove_flag(s_downloadBtn, LV_OBJ_FLAG_HIDDEN);
        else if (state == UpdateState::Downloading || state == UpdateState::Extracting)
            lv_obj_add_flag(s_downloadBtn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_checkBtn) {
        if (state == UpdateState::Checking || state == UpdateState::Downloading || state == UpdateState::Extracting)
            lv_obj_add_flag(s_checkBtn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(s_checkBtn, LV_OBJ_FLAG_HIDDEN);
    }

    // Render release notes (from background thread data)
    if (s_notesNeedRender.load() && s_releaseNotesBox) {
        s_notesNeedRender.store(false);
        std::string notes;
        {
            std::lock_guard<std::mutex> lk(s_releasesMutex);
            notes = s_notesToRender;
        }
        if (!notes.empty()) {
            crosspad_gui::markdown_render(s_releaseNotesBox, notes);
        } else {
            lv_obj_clean(s_releaseNotesBox);
            lv_obj_t* lbl = lv_label_create(s_releaseNotesBox);
            lv_label_set_text(lbl, "(no release notes)");
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        }
    }

    // Build version list when fetched
    if (s_releasesFetched.load()) {
        s_releasesFetched.store(false);
        build_version_list();

        // If no notes shown yet, show notes for the current version
        if (!s_notesNeedRender.load() && s_releaseNotesBox &&
            lv_obj_get_child_count(s_releaseNotesBox) == 0) {
            std::lock_guard<std::mutex> lk(s_releasesMutex);
            for (auto& r : s_releases) {
                if (r.isCurrent && !r.releaseNotes.empty()) {
                    set_notes_to_render(r.releaseNotes);
                    break;
                }
            }
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
    UpdateState cur = s_updateState.load();
    if (cur != UpdateState::Idle && cur != UpdateState::Error &&
        cur != UpdateState::UpdateAvailable)
        return;

    s_updateState.store(UpdateState::Checking);
    set_update_msg("Checking for updates...");

    std::thread([]() {
        // Check latest (include prereleases if setting is on)
        bool showPre = pc_platform_get_show_prereleases();
        s_updateInfo = s_updater.checkForUpdate(showPre);

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
            // Show release notes for latest
            set_notes_to_render(s_updateInfo.releaseNotes);
        } else {
            s_updateState.store(UpdateState::Idle);
            set_update_msg("Up to date (v" + s_updateInfo.currentVersion + ")");
            // Show release notes for current version
            if (!s_updateInfo.releaseNotes.empty())
                set_notes_to_render(s_updateInfo.releaseNotes);
        }

        // Also fetch releases list
        {
            auto releases = s_updater.listReleases();

            // Insert current version if not in the list (e.g. local/dev build)
            bool foundCurrent = false;
            for (auto& r : releases) {
                if (r.isCurrent) { foundCurrent = true; break; }
            }
            if (!foundCurrent) {
                ReleaseInfo cur;
                cur.version = CROSSPAD_PC_VERSION;
                cur.tagName = "v" + cur.version;
                cur.releaseName = "CrossPad PC v" + cur.version;
                cur.isCurrent = true;
                cur.isCached = s_updater.isCached(cur.version);
                // Insert sorted by version (descending)
                auto it = releases.begin();
                while (it != releases.end() && compareSemver(it->version, cur.version) > 0)
                    ++it;
                releases.insert(it, cur);
            }

            std::lock_guard<std::mutex> lk(s_releasesMutex);
            s_releases = std::move(releases);
        }
        s_releasesFetched.store(true);
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

lv_obj_t* Update_create(lv_obj_t* parent, App* a)
{
    s_app = a;

    /* Root container */
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_style_pad_row(cont, 3, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    /* ── Title bar ───────────────────────────────────────────── */
    lv_obj_t* titleBar = lv_obj_create(cont);
    lv_obj_set_size(titleBar, lv_pct(100), 24);
    lv_obj_set_style_bg_opa(titleBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(titleBar, 0, 0);
    lv_obj_set_style_pad_all(titleBar, 0, 0);
    lv_obj_remove_flag(titleBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = lv_label_create(titleBar);
    lv_label_set_text(titleLabel, "Update");
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

    /* ── Version + auto-check row ────────────────────────────── */
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

    // Auto-check toggle row
    lv_obj_t* autoRow = lv_obj_create(cont);
    lv_obj_set_size(autoRow, lv_pct(100), 24);
    lv_obj_set_style_bg_opa(autoRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(autoRow, 0, 0);
    lv_obj_set_style_pad_all(autoRow, 0, 0);
    lv_obj_remove_flag(autoRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* autoLbl = lv_label_create(autoRow);
    lv_label_set_text(autoLbl, "Auto-check on startup");
    lv_obj_set_style_text_font(autoLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(autoLbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(autoLbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* autoSwitch = lv_switch_create(autoRow);
    lv_obj_set_size(autoSwitch, 36, 18);
    lv_obj_align(autoSwitch, LV_ALIGN_RIGHT_MID, -2, 0);
    if (pc_platform_get_auto_check_updates()) {
        lv_obj_add_state(autoSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(autoSwitch, [](lv_event_t* e) {
        bool checked = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
        pc_platform_set_auto_check_updates(checked);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Show pre-releases toggle row
    lv_obj_t* preRow = lv_obj_create(cont);
    lv_obj_set_size(preRow, lv_pct(100), 24);
    lv_obj_set_style_bg_opa(preRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(preRow, 0, 0);
    lv_obj_set_style_pad_all(preRow, 0, 0);
    lv_obj_remove_flag(preRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* preLbl = lv_label_create(preRow);
    lv_label_set_text(preLbl, "Show pre-releases");
    lv_obj_set_style_text_font(preLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(preLbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(preLbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* preSwitch = lv_switch_create(preRow);
    lv_obj_set_size(preSwitch, 36, 18);
    lv_obj_align(preSwitch, LV_ALIGN_RIGHT_MID, -2, 0);
    if (pc_platform_get_show_prereleases()) {
        lv_obj_add_state(preSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(preSwitch, [](lv_event_t* e) {
        bool checked = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
        pc_platform_set_show_prereleases(checked);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    /* ── Latest version + buttons ────────────────────────────── */
    s_latestLabel = lv_label_create(cont);
    lv_obj_set_style_text_font(s_latestLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_latestLabel, lv_color_hex(0x888888), 0);
    lv_obj_set_width(s_latestLabel, lv_pct(100));
    lv_label_set_text(s_latestLabel, "Latest: (not checked)");

    // Button row
    lv_obj_t* btnRow = lv_obj_create(cont);
    lv_obj_set_size(btnRow, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 6, 0);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);

    s_checkBtn = lv_button_create(btnRow);
    lv_obj_set_size(s_checkBtn, 130, 26);
    lv_obj_set_style_bg_color(s_checkBtn, lv_color_hex(0x225588), 0);
    lv_obj_set_style_bg_color(s_checkBtn, lv_color_hex(0x3377AA), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_checkBtn, 4, 0);
    lv_obj_set_style_shadow_width(s_checkBtn, 0, 0);
    lv_obj_t* checkLbl = lv_label_create(s_checkBtn);
    lv_label_set_text(checkLbl, "Check for Updates");
    lv_obj_set_style_text_font(checkLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(checkLbl);
    lv_obj_add_event_cb(s_checkBtn, on_check_update_clicked, LV_EVENT_CLICKED, nullptr);

    s_downloadBtn = lv_button_create(btnRow);
    lv_obj_set_size(s_downloadBtn, 130, 26);
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

    // Progress bar
    s_progressBar = lv_bar_create(cont);
    lv_obj_set_size(s_progressBar, lv_pct(100), 10);
    lv_bar_set_range(s_progressBar, 0, 100);
    lv_bar_set_value(s_progressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progressBar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(s_progressBar, lv_color_hex(0x2288AA), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progressBar, 3, 0);
    lv_obj_set_style_radius(s_progressBar, 3, LV_PART_INDICATOR);
    lv_obj_add_flag(s_progressBar, LV_OBJ_FLAG_HIDDEN);

    // Status label
    s_updateStatusLabel = lv_label_create(cont);
    lv_obj_set_style_text_font(s_updateStatusLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_updateStatusLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_updateStatusLabel, lv_pct(100));
    lv_label_set_long_mode(s_updateStatusLabel, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_updateStatusLabel, "");

    /* ── Release Notes ───────────────────────────────────────── */
    create_section_header(cont, "Release Notes");

    s_releaseNotesBox = lv_obj_create(cont);
    lv_obj_set_size(s_releaseNotesBox, lv_pct(100), 80);
    lv_obj_set_style_bg_color(s_releaseNotesBox, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_releaseNotesBox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_releaseNotesBox, 1, 0);
    lv_obj_set_style_border_color(s_releaseNotesBox, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(s_releaseNotesBox, 4, 0);
    lv_obj_set_style_pad_row(s_releaseNotesBox, 2, 0);
    lv_obj_set_style_radius(s_releaseNotesBox, 4, 0);
    lv_obj_set_flex_flow(s_releaseNotesBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_releaseNotesBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_releaseNotesBox, LV_SCROLLBAR_MODE_AUTO);

    /* ── Available Versions ──────────────────────────────────── */
    create_section_header(cont, "Available Versions");

    s_versionListBox = lv_obj_create(cont);
    lv_obj_set_size(s_versionListBox, lv_pct(100), 80);
    lv_obj_set_style_bg_color(s_versionListBox, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_versionListBox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_versionListBox, 1, 0);
    lv_obj_set_style_border_color(s_versionListBox, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(s_versionListBox, 2, 0);
    lv_obj_set_style_pad_row(s_versionListBox, 1, 0);
    lv_obj_set_style_radius(s_versionListBox, 4, 0);
    lv_obj_set_flex_flow(s_versionListBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_versionListBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_versionListBox, LV_SCROLLBAR_MODE_AUTO);

#ifndef CROSSPAD_DEV_BUILD
    /* ── Apps (release only) ─────────────────────────────────── */
    create_section_header(cont, "Apps");

    lv_obj_t* appsInfoLabel = lv_label_create(cont);
    lv_label_set_text(appsInfoLabel,
        LV_SYMBOL_WARNING " App management is available in the\n"
        "developer build (BUILD_TESTING=ON).");
    lv_obj_set_style_text_font(appsInfoLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(appsInfoLabel, lv_color_hex(0xFFAA33), 0);
    lv_obj_set_width(appsInfoLabel, lv_pct(100));
    lv_label_set_long_mode(appsInfoLabel, LV_LABEL_LONG_WRAP);

    // List registered apps
    lv_obj_t* appsInfoLabel2 = lv_label_create(cont);
    lv_label_set_text(appsInfoLabel2, "Installed apps:");
    lv_obj_set_style_text_font(appsInfoLabel2, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(appsInfoLabel2, lv_color_hex(0x888888), 0);
    lv_obj_set_width(appsInfoLabel2, lv_pct(100));
    lv_obj_set_style_pad_top(appsInfoLabel2, 4, 0);

    auto& registry = crosspad::AppRegistry::getInstance();
    for (int i = 0; i < registry.getAppCount(); i++) {
        const auto* entry = registry.getApp(i);
        if (!entry) continue;
        lv_obj_t* appRow = lv_obj_create(cont);
        lv_obj_set_size(appRow, lv_pct(100), 20);
        lv_obj_set_style_bg_opa(appRow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(appRow, 0, 0);
        lv_obj_set_style_pad_all(appRow, 0, 0);
        lv_obj_set_style_pad_left(appRow, 8, 0);
        lv_obj_remove_flag(appRow, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* appLbl = lv_label_create(appRow);
        static char appBuf[64];
        snprintf(appBuf, sizeof(appBuf), LV_SYMBOL_OK " %s", entry->name);
        lv_label_set_text(appLbl, appBuf);
        lv_obj_set_style_text_font(appLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(appLbl, lv_color_hex(0xBBBBBB), 0);
        lv_obj_align(appLbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
#endif

    /* ── Timer ───────────────────────────────────────────────── */
    s_updateTimer = lv_timer_create(update_timer_cb, 500, nullptr);

    /* ── Load cached auto-check result ───────────────────────── */
    if (pc_updater_has_cached_result()) {
        s_updateInfo = pc_updater_get_cached_check_result();
        if (s_updateInfo.updateAvailable) {
            s_updateState.store(UpdateState::UpdateAvailable);
            char buf[128];
            snprintf(buf, sizeof(buf), "v%s available (%.1f MB)",
                     s_updateInfo.latestVersion.c_str(),
                     s_updateInfo.assetSize / 1048576.0);
            set_update_msg(buf);
            set_notes_to_render(s_updateInfo.releaseNotes);
        } else if (!s_updateInfo.errorMessage.empty()) {
            s_updateState.store(UpdateState::Error);
            set_update_msg(s_updateInfo.errorMessage);
        } else if (!s_updateInfo.latestVersion.empty()) {
            set_update_msg("Up to date (v" + s_updateInfo.currentVersion + ")");
        }
    }

    printf("[Update] App created\n");
    return cont;
}

void Update_destroy(lv_obj_t* app_obj)
{
    if (s_updateTimer) {
        lv_timer_delete(s_updateTimer);
        s_updateTimer = nullptr;
    }

    s_app = nullptr;
    s_versionLabel = nullptr;
    s_latestLabel = nullptr;
    s_checkBtn = nullptr;
    s_downloadBtn = nullptr;
    s_progressBar = nullptr;
    s_updateStatusLabel = nullptr;
    s_releaseNotesBox = nullptr;
    s_versionListBox = nullptr;

    lv_obj_delete_async(app_obj);
    printf("[Update] App destroyed\n");
}

/* ── App registration ────────────────────────────────────────────────── */

void _register_Update_app()
{
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sCrossPad_Logo_110w.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());

    static const crosspad::AppEntry entry = {
        "Update", icon_path,
        Update_create, Update_destroy,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}

#endif // USE_LVGL
