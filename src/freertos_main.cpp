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
#include "crosspad_deps.hpp"
#include <SDL.h>
#include "lvgl/lvgl.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hal/hal.h"
#include "stm32_emu/Stm32EmuWindow.hpp"
#include "crosspad_app.hpp"
#include "remote/RemoteControl.hpp"

#include "crosspad/app/AppVersions.hpp"

#include <cstdio>
#include <cstring>

/* ── FreeRTOS hooks (required by kernel config) ───────────────────────── */

extern "C" {

void vApplicationMallocFailedHook(void)
{
    printf("Malloc failed! Available heap: %ld bytes\n", xPortGetFreeHeapSize());
    for (;;);
}

#ifdef _MSC_VER
void vApplicationIdleHook(void) { Sleep(1); }
#else
void vApplicationIdleHook(void) { usleep(1000); }
#endif

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
    lv_display_t* disp = sdl_hal_init(Stm32EmuWindow::WIN_W, Stm32EmuWindow::WIN_H);
    crosspad_app_init();

    // Start remote control server (TCP localhost:19840) for MCP integration
    remote::start(disp);

    while (true) {
        lv_timer_handler();
        remote::process_pending();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    // Report what this build was made of and exit — the simulator's equivalent
    // of the firmware's APP_VERSIONS over CDC, so the app manager can ask both
    // the same question.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--versions") != 0) continue;
        for (std::size_t v = 0; v < crosspad::appVersionCount(); ++v) {
            const crosspad::AppVersion* av = crosspad::appVersionGet(v);
            if (!av) continue;
            printf("APPVER: %s id=%s commit=%s ref=%s dirty=%d\n",
                   av->component,
                   av->appId && av->appId[0] ? av->appId : "-",
                   av->commit,
                   av->ref && av->ref[0] ? av->ref : "-",
                   av->dirty ? 1 : 0);
        }
        printf("APPVER: end count=%zu\n", crosspad::appVersionCount());
        return 0;
    }

    if (xTaskCreate(lvgl_task, "LVGL", 8192, NULL, 1, NULL) != pdPASS) {
        printf("Error creating LVGL task\n");
    }

    vTaskStartScheduler();
    return 0;
}
