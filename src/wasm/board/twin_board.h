#pragma once
/**
 * @file twin_board.h
 * @brief What the browser twin knows of its board that no input carries: the
 *        state its Settings and Firmware apps show, as the board reports it
 *        over the companion channel (TWIN_STATE, TWIN_INFO, TWIN_FW; parsed
 *        in src/wasm/twin_board.cpp). The headers beside this one stand in
 *        for the board's own (platform-idf main/include) so that its
 *        settings_app.cpp and update_app.cpp build unchanged; everything they
 *        would do to hardware does nothing here -- the twin follows.
 */
#include <cstddef>
#include <cstdint>

#include "fw_library.h"
#include "fw_slots.h"

/** The Firmware app's view of the board (TWIN_FW). */
struct TwinFw {
    FwSlotInfo slots[2] = {{'A', false, FwSlotState::Empty, "", "", "", false},
                           {'B', false, FwSlotState::Empty, "", "", "", false}};
    FwPackage  pkgs[16] = {};
    bool       stmOlder[16] = {};
    size_t     count = 0;
    bool       rollback = true, usbDefault = true, autoAccept = false, flow = false;
    char       version[32] = "";
};
TwinFw &twin_fw();

/** The board's radio and motion MIDI as TWIN_STATE reports them. */
const char *twin_ble_state();
const char *twin_ble_peer();      // "" when nobody is connected
uint32_t    twin_imu_sent();

/* Fed by wasm_bridge.cpp as TWIN_STATE comes in. */
void twin_board_set_ble(const char *state, const char *peer);
void twin_board_set_imu(uint32_t sent);
/** The twin's ISettingsUI: the board's platform name, Info rows, coprocessor. */
void twin_board_attach();
