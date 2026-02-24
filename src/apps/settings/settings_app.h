#pragma once

#ifdef USE_LVGL
#include "lvgl.h"
#include "pc_stubs/PcApp.hpp"

lv_obj_t * lv_CreateSettings(lv_obj_t * parent, App * a);
void lv_DestroySettings(lv_obj_t * obj);

#endif // USE_LVGL
