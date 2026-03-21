#pragma once

/**
 * @file EmuSdCardSlot.hpp
 * @brief Virtual micro SD card slot for the CrossPad PC emulator.
 *
 * Renders a clickable SD card slot on the device body. When clicked,
 * opens a native folder picker dialog to select a working directory
 * that simulates the SD card. Shows mounted/unmounted state visually.
 */

#include "lvgl/lvgl.h"
#include <string>
#include <functional>

class EmuSdCardSlot {
public:
    using MountCallback   = std::function<void(const std::string& folderPath)>;
    using UnmountCallback = std::function<void()>;

    EmuSdCardSlot() = default;

    /// Build the SD card slot visual on the emulator screen.
    void create(lv_obj_t* parent);

    /// Update mounted state and path (visual only — does not fire callbacks).
    void setMounted(bool mounted, const std::string& path = "");

    bool isMounted() const { return mounted_; }
    const std::string& getMountPath() const { return mountPath_; }

    /// Set callback for when user mounts a folder via the dialog.
    void setOnMount(MountCallback cb) { onMount_ = std::move(cb); }

    /// Set callback for when user unmounts.
    void setOnUnmount(UnmountCallback cb) { onUnmount_ = std::move(cb); }

private:
    lv_obj_t* slot_      = nullptr;   ///< clickable outer rectangle (SD slot shape)
    lv_obj_t* iconLabel_ = nullptr;   ///< "SD" text inside slot
    lv_obj_t* pathLabel_ = nullptr;   ///< folder name label next to slot

    bool        mounted_ = false;
    std::string mountPath_;

    MountCallback   onMount_;
    UnmountCallback onUnmount_;

    void updateVisual();

    /// Open native folder picker dialog. Returns selected path or empty string.
    static std::string openFolderDialog();

    static void onSlotClicked(lv_event_t* e);
};
