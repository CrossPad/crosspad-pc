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
/* Apps whose PC branch includes only this header still use the transport
 * bar's height and the toast (crosspad-arrange's ArrangeApp.cpp). */
#if __has_include("crosspad-gui/components/transport_bar.h")
#include "crosspad-gui/components/transport_bar.h"
#endif
#if __has_include("crosspad-gui/components/ui_toast.h")
#include "crosspad-gui/components/ui_toast.h"
#endif

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
