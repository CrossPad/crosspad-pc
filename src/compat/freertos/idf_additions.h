#pragma once
// PC shim for ESP-IDF's FreeRTOS additions: no cores to pin to, no caps to honour.
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY 0x7FFFFFFF
#endif
static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char* name, uint32_t stack,
                                                         void* arg, UBaseType_t prio, TaskHandle_t* out, BaseType_t, unsigned)
{ return xTaskCreate(fn, name, (configSTACK_DEPTH_TYPE)(stack / sizeof(StackType_t) + 1024), arg, prio, out); }
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack,
                                                 void* arg, UBaseType_t prio, TaskHandle_t* out, BaseType_t)
{ return xTaskCreate(fn, name, (configSTACK_DEPTH_TYPE)(stack / sizeof(StackType_t) + 1024), arg, prio, out); }
static inline void vTaskDeleteWithCaps(TaskHandle_t t) { vTaskDelete(t); }
static inline QueueHandle_t xQueueCreateWithCaps(UBaseType_t len, UBaseType_t item, unsigned) { return xQueueCreate(len, item); }
static inline void vQueueDeleteWithCaps(QueueHandle_t q) { vQueueDelete(q); }
