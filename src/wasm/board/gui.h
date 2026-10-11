#pragma once
/* platform-idf main/include/gui.h, for the twin. */
#include "lvgl/lvgl.h"
#include "crosspad-gui/crosspad_gui.h"

extern lv_obj_t *app_c;   // the app container (crosspad_app.cpp)

void crosspad_app_go_home();
inline lv_obj_t *LoadMainScreen(lv_obj_t *) { crosspad_app_go_home(); return nullptr; }
