#pragma once
/* platform-idf components/stm32_hal/include/stm32_hal_init.h, for the twin: no
 * STM here, so no charger telemetry (Settings -> Info takes the board's rows
 * through TWIN_INFO). */
#include <cstdint>
#include "esp_err.h"
#include "crosspad/stm32/reg_map.h"

typedef struct {
    uint16_t vbus_mv, vsys_mv, vbat_mv, vbat_adc_mv, ichg_ma;
    uint8_t  ts_pct, vbus_stat, chrg_stat;
    uint16_t iinlim_ma, idpm_lim_ma, vindpm_mv;
    uint8_t  ntc_fault, flags, src_active;
    uint16_t target_iinlim_ma;
    uint8_t  soc_pct;
    uint16_t soc_permil;
    uint8_t  soc_flags;
} stm32_charger_telemetry_t;

inline esp_err_t stm32_hal_get_charger_telemetry(stm32_charger_telemetry_t *) { return ESP_FAIL; }
