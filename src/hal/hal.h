/**
 * @file hal.h
 *
 */

#ifndef LV_VSCODE_HAL_H
#define LV_VSCODE_HAL_H

#include "lvgl/lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize the Hardware Abstraction Layer (HAL) for the LVGL graphics
 * library
 */
lv_display_t * sdl_hal_init(int32_t w, int32_t h);

/**
 * The rotary encoder input device (LVGL's SDL mousewheel driver: wheel to
 * rotate, middle click to push). NULL before sdl_hal_init().
 */
lv_indev_t * sdl_hal_get_encoder(void);

/* The mouse, as the board's touch panel (see hal.c). */
lv_indev_t * sdl_hal_get_mouse(void);

/* The SDL window a display draws into. A board's LVGL 9.3 has no
 * lv_sdl_window_get_window(); there it is the window of the display's renderer. */
struct SDL_Window;
struct SDL_Window * sdl_hal_window(lv_display_t * disp);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_VSCODE_HAL_H*/
