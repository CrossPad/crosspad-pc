#pragma once

/**
 * @file PcApp.hpp
 * @brief PC App class — inherits LVGL lifecycle from crosspad_gui::AppBase<App>
 *
 * Only adds PC-specific resource accessors (padManager, ledController, etc.).
 * All lifecycle code (start, pause, resume, destroyApp, flag management)
 * is inherited from AppBase.
 */

#include "crosspad-gui/app/AppBase.hpp"

// Forward declarations
namespace crosspad {
class PadManager;
class PadLedController;
class CrosspadSettings;
class CrosspadStatus;
}

using crosspad::CrosspadSettings;
using crosspad::CrosspadStatus;

class App : public crosspad_gui::AppBase<App> {
public:
    using AppBase::AppBase;  // inherit all constructors

    // PC-specific resource accessors (implemented in PcApp.cpp)
    crosspad::PadManager& padManager();
    crosspad::PadLedController& ledController();
    CrosspadStatus& status();
    CrosspadSettings& settings();
};
