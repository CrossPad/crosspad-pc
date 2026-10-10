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
#elif defined(__linux__)
#include <cerrno>
#include <unistd.h>
/* ── Linux folder picker: zenity → kdialog fallback ─────────────────────
 * Blocking popen, same modality as the Win32 IFileDialog path. Both tools
 * are spawned with stderr silenced (GTK/Qt warnings) and their trailing
 * newline stripped. Empty result = cancelled or neither tool installed.
 *
 * The pipe is read with read(2) in a loop that retries on EINTR, not with
 * fgets(): the FreeRTOS POSIX port ticks this process with SIGALRM at 1 kHz
 * and installs the handler without SA_RESTART, so the first tick during the
 * dialog interrupted fgets() with nothing read, the stream was closed under
 * the picker (SIGPIPE, status 141) and every selection looked cancelled. */
static std::string runPickerCommand(const char* cmd)
{
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";
    const int fd = fileno(pipe);
    char buf[1024];
    std::string out;
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    int status;
    do { status = pclose(pipe); } while (status == -1 && errno == EINTR);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    // Non-zero exit = cancel (zenity: 1) or tool missing (127).
    if (status != 0) return "";
    return out;
}

/* A simulator started from a snap-packaged IDE (VS Code) inherits the snap's
 * LD_LIBRARY_PATH, GTK_* and glib schema/pixbuf paths; the host's zenity then
 * dies with "symbol lookup error ... GLIBC_PRIVATE" (status 127) before any
 * window appears. The picker is spawned with those unset, and the XDG dirs
 * put back from the *_VSCODE_SNAP_ORIG copies the snap launcher leaves. */
static constexpr const char* kPickerEnvPrefix =
    "env -u LD_LIBRARY_PATH -u LD_PRELOAD -u GTK_PATH -u GTK_EXE_PREFIX "
    "-u GTK_IM_MODULE_FILE -u GIO_MODULE_DIR -u GSETTINGS_SCHEMA_DIR "
    "-u GDK_PIXBUF_MODULE_FILE -u GDK_PIXBUF_MODULEDIR -u LOCPATH -u XDG_DATA_HOME "
    "XDG_DATA_DIRS=\"${XDG_DATA_DIRS_VSCODE_SNAP_ORIG:-${XDG_DATA_DIRS:-/usr/local/share:/usr/share}}\" "
    "XDG_CONFIG_DIRS=\"${XDG_CONFIG_DIRS_VSCODE_SNAP_ORIG:-${XDG_CONFIG_DIRS:-/etc/xdg}}\" ";

std::string EmuSdCardSlot::openFolderDialog()
{
    std::string dir = runPickerCommand((std::string(kPickerEnvPrefix) +
        "zenity --file-selection --directory "
        "--title='Select SD Card Working Directory' 2>/dev/null").c_str());
    if (!dir.empty()) return dir;

    dir = runPickerCommand((std::string(kPickerEnvPrefix) +
        "kdialog --getexistingdirectory \"$HOME\" "
        "--title 'Select SD Card Working Directory' 2>/dev/null").c_str());
    if (!dir.empty()) return dir;

    printf("[SDCard] Folder picker: cancelled, or neither 'zenity' nor 'kdialog' "
           "runs here; set sdcard_path in device_preferences.json\n");
    return "";
}
#else
std::string EmuSdCardSlot::openFolderDialog()
{
    printf("[SDCard] Folder picker not implemented on this platform\n");
    return "";
}
#endif
