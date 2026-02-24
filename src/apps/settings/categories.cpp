#ifdef USE_LVGL

#include "settings_nav.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad-gui/styles/styles.h"
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/components/ui_toast.h"
#include "pc_stubs/pc_platform.h"

extern CrosspadSettings * settings;

// ===== Display =====

void cat_build_display(lv_obj_t * parent, lv_group_t * group) {
    settings_row_slider(parent, group, "Brightness", 0, 127,
                        &settings->LCDbrightness, "cfg_display", "brightness");

    settings_row_switch(parent, group, "Auto Brightness",
                        &settings->LCDAutoBrightness, "cfg_display", "auto_bright");
}

// ===== Audio =====

void cat_build_audio(lv_obj_t * parent, lv_group_t * group) {
    settings_row_switch(parent, group, "Audio Engine",
                        &settings->AudioEngineEnabled, "cfg_audio", "engine_en");

    settings_row_switch(parent, group, "Master Mute",
                        &settings->masterFX.mute, "cfg_audio", "mute");

    settings_row_slider(parent, group, "Input Volume", 0, 127,
                        &settings->masterFX.inVolume, "cfg_audio", "in_vol");

    settings_row_slider(parent, group, "Output Volume", 0, 127,
                        &settings->masterFX.outVolume, "cfg_audio", "out_vol");

    settings_section_header(parent, "Effects Bypass");

    settings_row_switch(parent, group, "Delay",
                        &settings->masterFX.delay.bypass, "cfg_audio", "delay_byp");

    settings_row_switch(parent, group, "Reverb",
                        &settings->masterFX.reverb.bypass, "cfg_audio", "reverb_byp");

    settings_row_switch(parent, group, "Distortion",
                        &settings->masterFX.distortion.bypass, "cfg_audio", "dist_byp");

    settings_row_switch(parent, group, "Chorus",
                        &settings->masterFX.chorus.bypass, "cfg_audio", "chorus_byp");

    settings_row_switch(parent, group, "Flanger",
                        &settings->masterFX.flanger.bypass, "cfg_audio", "flangr_byp");
}

// ===== Pads & LEDs =====

static void rgb_brightness_hw_cb(lv_event_t * e) {
    (void)e;
    crosspad::getPadLedController().setBrightness(settings->RGBbrightness);
}

void cat_build_pads(lv_obj_t * parent, lv_group_t * group) {
    settings_row_slider(parent, group, "RGB Brightness", 0, 255,
                        &settings->RGBbrightness, "cfg_rgb", "brightness",
                        rgb_brightness_hw_cb);

    settings_section_header(parent, "Behavior");

    settings_row_switch(parent, group, "Note Off on Release",
                        &settings->keypad.noteOffOnRelease, "cfg_keypad", "noteoff_rel");

    settings_row_switch(parent, group, "Inactive Lights",
                        &settings->keypad.inactiveLights, "cfg_keypad", "inactive_lt");

    settings_row_switch(parent, group, "Upper Row Functions",
                        &settings->keypad.upperRowFunctions, "cfg_keypad", "upper_fn");

    settings_row_switch(parent, group, "Eco Mode",
                        &settings->keypad.ecoMode, "cfg_keypad", "eco");
}

// ===== MIDI =====

void cat_build_midi(lv_obj_t * parent, lv_group_t * group) {
    settings_section_header(parent, "Keypad Routing");

    settings_row_switch(parent, group, "Send to STM",
                        &settings->keypad.send2STM, "cfg_keypad", "send_stm");

    settings_row_switch(parent, group, "Send to BLE",
                        &settings->keypad.send2BLE, "cfg_keypad", "send_ble");

    settings_row_switch(parent, group, "Send to USB",
                        &settings->keypad.send2USB, "cfg_keypad", "send_usb");

    settings_row_switch(parent, group, "Send CC",
                        &settings->keypad.sendCC, "cfg_keypad", "send_cc");

    settings_section_header(parent, "STM32 Routing");

    settings_row_switch(parent, group, "Routing Enabled",
                        &settings->stmRouting.RoutingMatrixEnabled, "cfg_stm_rt", "routing_en");

    settings_row_switch(parent, group, "MIDI -> Thru",
                        &settings->stmRouting.Midi2MidiThru, "cfg_stm_rt", "m2mt");

    settings_row_switch(parent, group, "MIDI -> USB",
                        &settings->stmRouting.Midi2USB, "cfg_stm_rt", "m2usb");

    settings_row_switch(parent, group, "MIDI -> ESP",
                        &settings->stmRouting.Midi2ESP, "cfg_stm_rt", "m2esp");

    settings_row_switch(parent, group, "USB -> MIDI",
                        &settings->stmRouting.USB2Midi, "cfg_stm_rt", "u2midi");

    settings_row_switch(parent, group, "USB -> Thru",
                        &settings->stmRouting.USB2USBThru, "cfg_stm_rt", "u2ut");

    settings_row_switch(parent, group, "USB -> ESP",
                        &settings->stmRouting.USB2ESP, "cfg_stm_rt", "u2esp");

    settings_row_switch(parent, group, "ESP -> MIDI",
                        &settings->stmRouting.ESP2Midi, "cfg_stm_rt", "e2midi");

    settings_row_switch(parent, group, "ESP -> USB",
                        &settings->stmRouting.ESP2USB, "cfg_stm_rt", "e2usb");

    settings_row_switch(parent, group, "ESP -> Thru",
                        &settings->stmRouting.ESP2ESPThru, "cfg_stm_rt", "e2et");

    settings_section_header(parent, "ESP32 Routing");

    settings_row_switch(parent, group, "BLE -> MIDI",
                        &settings->esp32Routing.BLE2Midi, "cfg_esp_rt", "ble2midi");

    settings_row_switch(parent, group, "BLE -> Audio",
                        &settings->esp32Routing.BLE2Audio, "cfg_esp_rt", "ble2audio");

    settings_row_switch(parent, group, "MIDI -> BLE",
                        &settings->esp32Routing.Midi2BLE, "cfg_esp_rt", "midi2ble");

    settings_row_switch(parent, group, "MIDI -> Audio",
                        &settings->esp32Routing.Midi2Audio, "cfg_esp_rt", "midi2audio");
}

// ===== System =====

static void factory_reset_cb(lv_event_t * e) {
    (void)e;
    // Erase all persisted settings and reload defaults
    auto * s = CrosspadSettings::getInstance();
    // Re-init with defaults
    *s = *new CrosspadSettings(*s);  // not great, but simple
    pc_platform_save_settings();
    ui_toast_show("Settings reset to defaults", 2000, UI_TOAST_WARNING);
}

void cat_build_system(lv_obj_t * parent, lv_group_t * group) {
    settings_section_header(parent, "Information");

    settings_row_info(parent, "Platform", "PC Simulator");
    settings_row_info(parent, "Profile", pc_platform_get_profile_dir());

    settings_section_header(parent, "Maintenance");

    settings_row_action(parent, group, NULL, "Factory Reset",
                        factory_reset_cb, NULL);
}

#endif // USE_LVGL
