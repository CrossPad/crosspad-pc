/**
 * @file    twin_updater.cpp
 * @brief   The board's firmware updater under its own Updates views
 *          (platform-idf main/gui/fw_updates_view.cpp in the Firmware app,
 *          main/app/settings/settings_updates.cpp in Settings; both built
 *          here unchanged), for the twin. Checks, downloads and installs are
 *          the board's: the twin takes a press as the board would and does
 *          nothing with it, so no refusal shows that the board's screen would
 *          not. What the twin knows is the board's last answer: an update
 *          found (TWIN_STATE's fwl=, the status frame's update flag); the
 *          running kit is the running slot's (TWIN_FW). Without a board the
 *          views read "Not checked yet" and offer nothing to install.
 */
#include <cstdio>
#include <cstring>

#include "board/twin_board.h"
#include "crosspad/status/CrosspadStatus.hpp"
#include "fw_image_desc.h"
#include "fw_updater.h"

extern crosspad::CrosspadStatus status;

namespace {
void put(char *out, size_t n, const char *s) { if (out && n) snprintf(out, n, "%s", s); }
} // namespace

/* fw_image_desc.h: the running slot's kit, as the board reported its slots. */
const fw_image_desc_t *fw_image_desc_running(void)
{
    static fw_image_desc_t d;
    d = {};
    const TwinFw &f = twin_fw();
    const FwSlotInfo &run = f.slots[f.slots[1].running ? 1 : 0];
    snprintf(d.kit_id, sizeof d.kit_id, "%s", run.kit);
    return &d;
}

/* fw_updater.h: idle, the board's last finding as the latest. */
fwu_state_t fw_updater_state(void) { return FWU_IDLE; }
bool fw_updater_busy(void) { return false; }
uint8_t fw_updater_progress(void) { return 0; }
bool fw_updater_check_start(char *refusal, size_t n) { put(refusal, n, ""); return true; }   // the board checks
bool fw_updater_download_start(const char *, const char *, bool, char *refusal, size_t n)
{
    put(refusal, n, "");
    return true;   // the board installs
}
void fw_updater_error(char *out, size_t n) { put(out, n, ""); }
void fw_updater_latest(char *out, size_t n) { put(out, n, status.fwUpdateAvailable ? status.fwLatest : ""); }
int64_t fw_updater_last_check(void) { return 0; }
bool fw_updater_checked(void) { return status.fwUpdateAvailable; }   // only a found update says a check ran
void fw_updater_target(char *out, size_t n) { put(out, n, ""); }
void fw_updater_ready_version(char *out, size_t n) { put(out, n, ""); }
bool fw_updater_boot_check(void) { return true; }   // the board's default; its NVS stays its own
void fw_updater_set_boot_check(bool) {}
bool fw_updater_channel_beta(const char *) { return false; }
bool fw_updater_set_channel(const char *, bool) { return true; }   // the board's to save
