#pragma once
// ESP-IDF layout shim for the PC build: <freertos/FreeRTOS.h> -> the POSIX port's FreeRTOS.h
#include <FreeRTOS.h>

// ESP-IDF's spinlock-taking critical sections (taskENTER_CRITICAL(&mux)): one
// core here, so the mux is only a token and the section is the port's own.
#include <task.h>
#ifndef portMUX_INITIALIZER_UNLOCKED
typedef struct { uint32_t owner; uint32_t count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0, 0 }
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#define taskENTER_CRITICAL(...) portENTER_CRITICAL()
#define taskEXIT_CRITICAL(...)  portEXIT_CRITICAL()
#endif
