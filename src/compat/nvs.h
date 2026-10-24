#pragma once
/* ESP-IDF nvs.h, for the twin (the apps it builds with ESP_PLATFORM): there is
 * no flash to keep anything in. Nothing opens, so an app reads its defaults
 * and what it saves goes nowhere, as on a board fresh from the factory. */
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_ERR_NVS_NOT_FOUND 0x1102

inline esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline void      nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_FAIL; }
inline esp_err_t nvs_erase_key(nvs_handle_t, const char *) { return ESP_FAIL; }
inline esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t) { return ESP_FAIL; }
inline esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t) { return ESP_FAIL; }
inline esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t) { return ESP_FAIL; }
inline esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *) { return ESP_FAIL; }
