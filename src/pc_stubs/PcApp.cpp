#include "PcApp.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

extern CrosspadSettings* settings;
extern CrosspadStatus status;

crosspad::PadManager& App::padManager() {
    return crosspad::getPadManager();
}

crosspad::PadLedController& App::ledController() {
    return crosspad::getPadLedController();
}

CrosspadStatus& App::status() {
    return ::status;
}

CrosspadSettings& App::settings() {
    return *::settings;
}
