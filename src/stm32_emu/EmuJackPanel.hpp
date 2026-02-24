#pragma once

/**
 * @file EmuJackPanel.hpp
 * @brief Visual audio/MIDI jack connectors for the CrossPad PC emulator.
 *
 * Renders thick bars on the device body edges representing physical jack
 * connectors. When connected, audio jack bars become live VU meters showing
 * real-time signal levels. All jacks are clickable for device selection.
 */

#include "lvgl/lvgl.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

class EmuJackPanel {
public:
    enum JackId : int {
        AUDIO_OUT1 = 0,
        AUDIO_OUT2 = 1,
        AUDIO_IN1  = 2,
        AUDIO_IN2  = 3,
        MIDI_OUT   = 4,
        MIDI_IN    = 5,
        JACK_COUNT = 6
    };

    /// Callback: (jackId, deviceIndex) where deviceIndex is index into the
    /// device list set via setDeviceList (0 = "(None)").
    using DeviceSelectedCb = std::function<void(int jackId, unsigned int deviceIndex)>;

    EmuJackPanel() = default;

    /// Build all jack visuals on the emulator screen.
    void create(lv_obj_t* parent);

    /// Update a jack's device-name label.
    void setDeviceName(JackId id, const std::string& name);

    /// Update a jack's connection status (green = connected, gray = none).
    void setConnected(JackId id, bool connected);

    /// Set stereo audio levels for a jack (used when connected).
    /// Values are int16 amplitude (0-32767). Only meaningful for audio jacks.
    void setLevel(JackId id, int16_t left, int16_t right);

    /// Populate a jack's dropdown device list.
    /// @param devices  List of device names (index 0 should be "(None)")
    /// @param currentIndex  Currently selected index (-1 for none)
    void setDeviceList(JackId id,
                       const std::vector<std::string>& devices,
                       int currentIndex);

    /// Set callback for when user selects a device from a dropdown.
    void setOnDeviceSelected(DeviceSelectedCb cb);

    /// Periodic update (called from Stm32EmuWindow timer).
    void update();

private:
    struct Jack {
        lv_obj_t* bar      = nullptr;
        lv_obj_t* vuBarL   = nullptr;   // VU meter left channel (child of bar)
        lv_obj_t* vuBarR   = nullptr;   // VU meter right channel (child of bar)
        lv_obj_t* label    = nullptr;
        lv_obj_t* dropdown = nullptr;
        bool      connected = false;
        bool      vertical  = false;    // true for AUDIO_OUT1/OUT2 (vertical bars)
        JackId    id = AUDIO_OUT1;
        int16_t   levelL = 0;
        int16_t   levelR = 0;
    };

    Jack jacks_[JACK_COUNT] = {};
    lv_obj_t* parent_ = nullptr;
    DeviceSelectedCb deviceSelectedCb_;

    void createJack(lv_obj_t* parent, JackId id,
                    int32_t barX, int32_t barY, int32_t barW, int32_t barH,
                    int32_t lblX, int32_t lblY, int32_t lblW,
                    const char* defaultLabel, bool clickable,
                    lv_dir_t dropdownDir);

    void applyBarStyle(Jack& jack);
    void updateVuBars(Jack& jack);
    void closeAllDropdowns(int exceptJackId = -1);

    static void onBarClicked(lv_event_t* e);
    static void onDropdownChanged(lv_event_t* e);
    static void onScreenClicked(lv_event_t* e);
    static void onDropdownListCreated(lv_event_t* e);
};
