// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file FirmwareApp.cpp
 * @brief The Firmware app in the simulator: crosspad-gui's app and views
 *        (crosspad-gui/fw/fw_app.h) over PcFwViewService's made-up card,
 *        slots and catalog, so the screens can be looked at and walked
 *        without a board.
 */

#if USE_LVGL

#include "PcFwViewService.hpp"
#include "pc_stubs/PcApp.hpp"

#include <crosspad/app/AppRegistrar.hpp>
#include "crosspad-gui/components/app_icon.h"
#include "crosspad-gui/fw/fw_app.h"
#include "crosspad-gui/platform/IGuiPlatform.h"

static PcFwViewService s_service;
static lv_group_t* s_launcherGroup = nullptr;   // the encoder's group when the app opened

static lv_group_t* launcher_group() { return s_launcherGroup; }

static const crosspad_gui::FwAppHooks kHooks = {nullptr, nullptr, nullptr, launcher_group};

static lv_obj_t* lv_CreateFirmware(lv_obj_t* parent, App* a) {
    crosspad_gui::setFwViewService(&s_service);
    lv_indev_t* nav = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    s_launcherGroup = nav ? lv_indev_get_group(nav) : nullptr;
    return crosspad_gui::fw_app_create(parent, a, kHooks);
}

static void lv_DestroyFirmware(lv_obj_t* app) {
    crosspad_gui::fw_app_destroy(app);
    s_launcherGroup = nullptr;
}

REGISTER_APP_PL(Firmware, nullptr, crosspad_gui::resolveAppIcon("ic_update.png"),
                lv_CreateFirmware, lv_DestroyFirmware,
                nullptr, nullptr, nullptr, 8, "Firmware")

#endif // USE_LVGL
