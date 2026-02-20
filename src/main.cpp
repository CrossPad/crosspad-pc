/**
 * @file main.cpp
 * @brief CrossPad PC — standard (non-FreeRTOS) entry point.
 */

#include <SDL.h>
#include "lvgl/lvgl.h"

#include "hal/hal.h"
#include "stm32_emu/Stm32EmuWindow.hpp"
#include "crosspad_app.hpp"

#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
#endif

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    lv_init();
    sdl_hal_init(Stm32EmuWindow::WIN_W, Stm32EmuWindow::WIN_H);
    crosspad_app_init();

    while (1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
#ifdef _MSC_VER
        Sleep(sleep_time_ms);
#else
        usleep(sleep_time_ms * 1000);
#endif
    }

    return 0;
}
