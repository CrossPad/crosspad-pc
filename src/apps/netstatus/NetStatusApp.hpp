#pragma once

#include "lvgl.h"

class App;

lv_obj_t* NetStatus_create(lv_obj_t* parent, App* app);
void NetStatus_destroy(lv_obj_t* app_obj);
