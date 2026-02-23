#pragma once

/**
 * @file MlPianoApp.hpp
 * @brief ML Piano app — FM synthesizer with 4x4 chromatic pad grid
 *
 * LVGL GUI app for CrossPad PC simulator. Uses ML_SynthTools FmSynth engine
 * with 16 built-in FM presets selectable via MIDI channel.
 */

// Forward declarations for LVGL app registration
class App;

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

lv_obj_t* MlPiano_create(lv_obj_t* parent, App* app);
void MlPiano_destroy(lv_obj_t* app_obj);
