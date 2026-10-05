#pragma once
/* ESP-IDF esp_err.h, for the twin: the type and its name, nothing returns one here. */
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#define ESP_FAIL -1
inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
