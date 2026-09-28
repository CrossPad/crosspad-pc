#pragma once
/* platform-idf main/include/fw_flow_overlay.h, for the twin: whether a flow
 * runs is the board's (TWIN_FW); the twin starts none. */
bool fw_flow_active(void);
inline bool fw_flow_install_and_restart(const char *, bool) { return false; }
inline bool fw_flow_switch_and_restart(bool) { return false; }
