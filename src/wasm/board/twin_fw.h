#pragma once
/**
 * @file twin_fw.h
 * @brief The Firmware app's view of the board (TWIN_FW, parsed in
 *        src/wasm/twin_board.cpp), in the firmware's own types: its packages
 *        and slots as platform-idf main/include/fw_library.h and fw_slots.h
 *        lay them out. Built only with the firmware's main/include on the
 *        include path.
 */
#include <cstddef>
#include <cstdint>

#include "fw_library.h"
#include "fw_slots.h"

struct TwinFw {
    FwSlotInfo slots[2] = {{'A', false, FwSlotState::Empty, "", "", "", false},
                           {'B', false, FwSlotState::Empty, "", "", "", false}};
    FwPackage  pkgs[16] = {};
    bool       stmOlder[16] = {};
    size_t     count = 0;
    bool       rollback = true, usbDefault = true, autoAccept = false, flow = false;
    char       version[32] = "";
    uint32_t   gen = 0;   // moves with every TWIN_FW reply: views reread what they show
};
TwinFw &twin_fw();
