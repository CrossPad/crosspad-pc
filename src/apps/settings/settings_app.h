#pragma once

#ifdef USE_LVGL
#include "lvgl.h"
#include "pc_stubs/PcApp.hpp"

lv_obj_t * lv_CreateSettings(lv_obj_t * parent, App * a);
void lv_DestroySettings(lv_obj_t * obj);

#endif // USE_LVGL

#ifdef USE_BLE
#include <cstdint>
/// Log raw BLE MIDI messages for the settings monitor (thread-safe)
void ble_settings_log_midi_in(uint8_t status, uint8_t d1, uint8_t d2);
void ble_settings_log_midi_out(uint8_t status, uint8_t d1, uint8_t d2);
#endif
