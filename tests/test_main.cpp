/**
 * @file    test_main.cpp
 * @brief   Catch2 entry point + FreeRTOS test platform bootstrap.
 *
 * Provides TestPlatform — a reusable helper that sets up FreeRTOS scheduler,
 * FreeRtosEventBus, PcClock, PcLedStrip, NullMidiOutput, and calls
 * crosspad_platform_init(). Each test file includes test_helpers.hpp and
 * calls TestPlatform::init() in a fixture or setup block.
 */

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#ifdef _MSC_VER
  #include <Windows.h>
#endif

#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>
#include <atomic>

// ── FreeRTOS hooks (required by kernel config) ──

extern "C" {

void vApplicationMallocFailedHook(void)
{
    printf("[TEST] Malloc failed!\n");
    for (;;);
}

void vApplicationIdleHook(void)
{
#ifdef _MSC_VER
    Sleep(1);
#endif
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName)
{
    printf("[TEST] Stack overflow in task %s\n", pcTaskName);
    for (;;);
}

void vApplicationTickHook(void) {}

} // extern "C"

// ── Run Catch2 inside a FreeRTOS task ──

static std::atomic<int> s_testResult{0};
static int s_argc = 0;
static char** s_argv = nullptr;

static void testRunnerTask(void* pvParameters)
{
    (void)pvParameters;
    s_testResult = Catch::Session().run(s_argc, s_argv);
    vTaskEndScheduler();
}

int main(int argc, char* argv[])
{
    s_argc = argc;
    s_argv = argv;

    if (xTaskCreate(testRunnerTask, "Tests", 16384, NULL, 1, NULL) != pdPASS) {
        printf("[TEST] Failed to create test runner task\n");
        return 1;
    }

    vTaskStartScheduler();

    return s_testResult.load();
}
