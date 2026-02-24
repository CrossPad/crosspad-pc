/**
 * @file    freertos_main.cpp
 * @brief   CrossPad PC — FreeRTOS entry point.
 *
 * Runs the same CrossPad GUI as main.cpp but under FreeRTOS scheduler.
 */

#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
#endif
#include <SDL.h>
#include "lvgl/lvgl.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hal/hal.h"
#include "stm32_emu/Stm32EmuWindow.hpp"
#include "crosspad_app.hpp"

#include <cstdio>

/* ── FreeRTOS hooks (required by kernel config) ───────────────────────── */

extern "C" {

void vApplicationMallocFailedHook(void)
{
    printf("Malloc failed! Available heap: %ld bytes\n", xPortGetFreeHeapSize());
    for (;;);
}

void vApplicationIdleHook(void) { Sleep(1); }

void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName)
{
    printf("Stack overflow in task %s\n", pcTaskName);
    for (;;);
}

void vApplicationTickHook(void) {}

} // extern "C"

/* ── LVGL task ────────────────────────────────────────────────────────── */

static void lvgl_task(void* pvParameters)
{
    (void)pvParameters;
    printf("Starting LVGL task\n");
    lv_init();
    sdl_hal_init(Stm32EmuWindow::WIN_W, Stm32EmuWindow::WIN_H);
    crosspad_app_init();

    while (true) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (xTaskCreate(lvgl_task, "LVGL", 8192, NULL, 1, NULL) != pdPASS) {
        printf("Error creating LVGL task\n");
    }

    vTaskStartScheduler();
    return 0;
}
