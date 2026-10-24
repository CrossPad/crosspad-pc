#pragma once
/* ESP-IDF esp_mac.h, for the twin: no MAC here (Settings -> Info takes the
 * board's rows through TWIN_INFO). */
#include <cstdint>
#ifndef ESP_OK
#define ESP_OK 0
#endif
inline int esp_efuse_mac_get_default(uint8_t *) { return -1; }
