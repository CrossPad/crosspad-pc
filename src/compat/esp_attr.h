#pragma once
/* ESP-IDF esp_attr.h, for the twin (and the apps it builds with ESP_PLATFORM):
 * one memory, no placement. */
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define WORD_ALIGNED_ATTR
