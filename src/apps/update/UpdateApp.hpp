#pragma once

#include "lvgl.h"

class App;

lv_obj_t* Update_create(lv_obj_t* parent, App* app);
void Update_destroy(lv_obj_t* app_obj);
