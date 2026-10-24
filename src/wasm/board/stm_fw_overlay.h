#pragma once
/* platform-idf main/include/stm_fw_overlay.h (and the stm32_hal types it
 * brings), for the twin: no STM here. The twin's Settings takes the board's
 * coprocessor rows through its own ISettingsUI (twin_board.cpp). */
#include <cstddef>
#include <cstdint>

#define STM32_FW_BUNDLED_PATH "/assets/stm32_fw.bin"

typedef struct {
    bool     linked;
    uint16_t proto_version;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  pcb_rev;
    uint8_t  fw_pcb;
} stm32_link_info_t;

typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint16_t proto;
    uint8_t  pcb;
    size_t   size;
} stm32_fw_image_info_t;

typedef struct {
    stm32_link_info_t     running;
    stm32_fw_image_info_t bundled;
    bool have_image;
    bool due;
    bool battery_low;
    bool pcb_mismatch;
} stm32_fw_update_status_t;

inline bool stm32_fw_update_check(const char *, stm32_fw_update_status_t *out) { *out = {}; return false; }
inline void stm_fw_overlay_request(void) {}
