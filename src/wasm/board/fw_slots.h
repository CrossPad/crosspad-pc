#pragma once
/* platform-idf main/include/fw_slots.h, for the twin: the board's two slots
 * as TWIN_FW reported them; installing and switching are the board's. */
#include <cstdint>

#include "fw_library.h"

enum class FwSlotState : uint8_t { Empty, Installed, Trial, Confirmed, RolledBack };
struct FwSlotInfo {
    char label;
    bool running;
    FwSlotState state;
    char package[FW_PKG_NAME_LEN];
    char version[32];
    char kit[FW_KIT_ID_LEN];
    bool hasDesc;
};

void fw_slots_get(FwSlotInfo out[2]);
bool fw_rollback_supported(void);
bool fw_package_stm_proto_older(const FwPackage &pkg);
inline bool fw_install_start(const char *, bool, const char **error) { if (error) *error = "twin"; return false; }
inline bool fw_switch_request(const char **error) { if (error) *error = "twin"; return false; }
inline void fw_confirm_running(void) {}
