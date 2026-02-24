#pragma once

/**
 * @file MixerApp.hpp
 * @brief Audio Mixer/Router app for CrossPad PC.
 *
 * Full routing matrix (3 inputs x 2 outputs) with per-channel volume,
 * mute/solo, VU meters, and 4x4 pad control.
 */

class App;

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

lv_obj_t* Mixer_create(lv_obj_t* parent, App* app);
void Mixer_destroy(lv_obj_t* app_obj);
