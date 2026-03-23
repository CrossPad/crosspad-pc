/**
 * @file EmuSdCardSlot.cpp
 * @brief Virtual micro SD card slot for the CrossPad PC emulator.
 *
 * Renders a clickable SD card slot on the right edge of the device body.
 * Clicking it opens a native Win32 folder picker dialog to select a
 * working directory that simulates the physical SD card.
 */

#include "EmuSdCardSlot.hpp"
#include <cstdio>

// Windows COM headers for folder picker dialog (after LVGL to avoid macro conflicts)
#ifdef _WIN32
#include <Windows.h>
#include <shobjidl.h>
#endif

/* ── Layout constants ────────────────────────────────────────────────── */

// SD card slot — bottom-left corner, positioned relative to window bottom.
// Sits between bottom-left screw and PAD 1 row.
#include "Stm32EmuWindow.hpp"

static constexpr int32_t SLOT_X = 36;
static constexpr int32_t SLOT_Y = Stm32EmuWindow::WIN_H - 40;
static constexpr int32_t SLOT_W = 50;
static constexpr int32_t SLOT_H = 10;

// "SD" label — above slot bar
static constexpr int32_t ICON_LBL_X = 38;
static constexpr int32_t ICON_LBL_Y = SLOT_Y - 12;

// Path label — to the right of slot bar
static constexpr int32_t PATH_LBL_X = 36;
static constexpr int32_t PATH_LBL_Y = SLOT_Y + SLOT_H + 2;
static constexpr int32_t PATH_LBL_W = 55;

// Colors (matching EmuJackPanel style)
static constexpr uint32_t COLOR_UNMOUNTED = 0x555555;
static constexpr uint32_t COLOR_MOUNTED   = 0x00CC66;
static constexpr uint32_t COLOR_LBL_OFF   = 0x999999;
static constexpr uint32_t COLOR_LBL_ON    = 0xCCCCCC;

/* ── create ──────────────────────────────────────────────────────────── */

void EmuSdCardSlot::create(lv_obj_t* parent)
{
    // --- Slot bar (clickable rectangle on device edge) ---
    slot_ = lv_obj_create(parent);
    lv_obj_set_pos(slot_, SLOT_X, SLOT_Y);
    lv_obj_set_size(slot_, SLOT_W, SLOT_H);
    lv_obj_set_style_radius(slot_, 3, 0);
    lv_obj_set_style_border_width(slot_, 0, 0);
    lv_obj_set_style_pad_all(slot_, 0, 0);
    lv_obj_remove_flag(slot_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(slot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(slot_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(slot_);
    lv_obj_add_event_cb(slot_, onSlotClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_style_bg_opa(slot_, LV_OPA_80, LV_STATE_PRESSED);

    // --- "SD" icon label (above the slot bar) ---
    iconLabel_ = lv_label_create(parent);
    lv_label_set_text(iconLabel_, LV_SYMBOL_SD_CARD " SD");
    lv_obj_set_pos(iconLabel_, ICON_LBL_X, ICON_LBL_Y);
    lv_obj_set_style_text_font(iconLabel_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(iconLabel_, lv_color_hex(COLOR_LBL_OFF), 0);
    lv_obj_set_style_text_opa(iconLabel_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(iconLabel_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // --- Path label (below the slot bar, shows folder name when mounted) ---
    pathLabel_ = lv_label_create(parent);
    lv_label_set_text(pathLabel_, "");
    lv_obj_set_pos(pathLabel_, PATH_LBL_X, PATH_LBL_Y);
    lv_obj_set_width(pathLabel_, PATH_LBL_W);
    lv_obj_set_style_text_font(pathLabel_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(pathLabel_, lv_color_hex(COLOR_LBL_OFF), 0);
    lv_obj_set_style_text_opa(pathLabel_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(pathLabel_, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(pathLabel_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_remove_flag(pathLabel_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    updateVisual();
}

/* ── Visual state ────────────────────────────────────────────────────── */

void EmuSdCardSlot::setMounted(bool mounted, const std::string& path)
{
    mounted_   = mounted;
    mountPath_ = path;
    updateVisual();
}

void EmuSdCardSlot::updateVisual()
{
    if (!slot_) return;

    if (mounted_) {
        // Connected look (green with glow shadow, like connected jack bars)
        lv_obj_set_style_bg_color(slot_, lv_color_hex(COLOR_MOUNTED), 0);
        lv_obj_set_style_bg_opa(slot_, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(slot_, 12, 0);
        lv_obj_set_style_shadow_color(slot_, lv_color_hex(COLOR_MOUNTED), 0);
        lv_obj_set_style_shadow_opa(slot_, 60, 0);
        lv_obj_set_style_shadow_spread(slot_, 2, 0);

        lv_obj_set_style_text_color(iconLabel_, lv_color_hex(COLOR_LBL_ON), 0);
        lv_obj_set_style_text_color(pathLabel_, lv_color_hex(COLOR_LBL_ON), 0);

        // Show just the folder name (last component of path)
        std::string folderName = mountPath_;
        auto pos = folderName.find_last_of("/\\");
        if (pos != std::string::npos && pos + 1 < folderName.size()) {
            folderName = folderName.substr(pos + 1);
        }
        lv_label_set_text(pathLabel_, folderName.c_str());
    } else {
        // Disconnected look (gray, no shadow)
        lv_obj_set_style_bg_color(slot_, lv_color_hex(COLOR_UNMOUNTED), 0);
        lv_obj_set_style_bg_opa(slot_, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(slot_, 0, 0);
        lv_obj_set_style_shadow_opa(slot_, 0, 0);

        lv_obj_set_style_text_color(iconLabel_, lv_color_hex(COLOR_LBL_OFF), 0);
        lv_obj_set_style_text_color(pathLabel_, lv_color_hex(COLOR_LBL_OFF), 0);
        lv_label_set_text(pathLabel_, "");
    }
}

/* ── Click handler ───────────────────────────────────────────────────── */

void EmuSdCardSlot::onSlotClicked(lv_event_t* e)
{
    auto* self = static_cast<EmuSdCardSlot*>(lv_event_get_user_data(e));

    if (self->mounted_) {
        // Already mounted — unmount
        self->setMounted(false);
        if (self->onUnmount_) self->onUnmount_();
        printf("[SDCard] Unmounted by user\n");
    } else {
        // Not mounted — open folder picker
        std::string folder = openFolderDialog();
        if (!folder.empty()) {
            self->setMounted(true, folder);
            if (self->onMount_) self->onMount_(folder);
            printf("[SDCard] Mounted by user: %s\n", folder.c_str());
        }
    }
}

/* ── Win32 folder picker dialog ──────────────────────────────────────── */

#ifdef _WIN32
std::string EmuSdCardSlot::openFolderDialog()
{
    std::string result;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool needUninit = (hr == S_OK);  // S_FALSE means already initialized

    IFileOpenDialog* pDialog = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                          IID_IFileOpenDialog, (void**)&pDialog);
    if (SUCCEEDED(hr)) {
        DWORD options;
        pDialog->GetOptions(&options);
        pDialog->SetOptions(options | FOS_PICKFOLDERS);
        pDialog->SetTitle(L"Select SD Card Working Directory");

        hr = pDialog->Show(NULL);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = nullptr;
            hr = pDialog->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR wpath = nullptr;
                pItem->GetDisplayName(SIGDN_FILESYSPATH, &wpath);
                if (wpath) {
                    // Convert WCHAR to UTF-8
                    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
                    if (len > 0) {
                        std::string utf8(len - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8.data(), len, NULL, NULL);
                        // Normalize backslashes to forward slashes
                        for (char& c : utf8) {
                            if (c == '\\') c = '/';
                        }
                        result = utf8;
                    }
                    CoTaskMemFree(wpath);
                }
                pItem->Release();
            }
        }
        pDialog->Release();
    }

    if (needUninit) CoUninitialize();

    return result;
}
#else
std::string EmuSdCardSlot::openFolderDialog()
{
    printf("[SDCard] Folder picker not implemented on this platform\n");
    return "";
}
#endif
