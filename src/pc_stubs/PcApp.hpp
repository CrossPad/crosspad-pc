#pragma once

/**
 * @file PcApp.hpp
 * @brief Lightweight App class for PC — compatible with crosspad-gui launcher
 *
 * Mirrors the ESP-IDF ApplicationSystem's App class but without ESP-IDF
 * dependencies (no Sequencer, CLI, FreeRTOS). Just enough to support the
 * launcher button grid and app lifecycle.
 */

#include <cstdint>
#include <cstddef>
#include <memory>

#include "lvgl.h"
#include "crosspad/app/AppFlags.hpp"
#include "crosspad-gui/components/app_lifecycle.h"

// Forward declarations
namespace crosspad {
class PadManager;
class PadLedController;
class CrosspadSettings;
class CrosspadStatus;
}

using crosspad::CrosspadSettings;
using crosspad::CrosspadStatus;

class App : public std::enable_shared_from_this<App> {
public:
    using AppFlags = crosspad::AppFlags;

    lv_obj_t* app = nullptr;
    lv_obj_t* app_c = nullptr;

    virtual ~App() = default;

    App(lv_obj_t* app_c, const char* name, const char* icon,
        lv_obj_t* (*create)(lv_obj_t* parent, App* a),
        void (*destroy)(lv_obj_t* app) = lv_Default_onDestroy)
        : app_c(app_c), name_(name), icon_(icon), create_(create), destroy_(destroy) {}

    void AddFlag(AppFlags flag) { type_ = type_ | flag; }
    void RemoveFlag(AppFlags flag) { type_ = type_ & ~flag; }
    bool HasFlag(AppFlags flag) { return (type_ & flag) == flag; }

    void start(lv_obj_t* parent = lv_screen_active()) {
        if (started_) return;
        if (!create_) return;

        app = create_(parent, this);
        app_c = parent;
        started_ = true;

        if (show_) {
            show_(app);
            visible_ = true;
        }
    }

    void pause() {
        if (app && started_) {
            hide_(app);
            visible_ = false;
        }
    }

    void resume() {
        if (!app || !started_ || visible_) return;
        if (show_) {
            show_(app);
            visible_ = true;
        }
    }

    void destroyApp() {
        if (!app) return;
        if (!started_ && !visible_) return;

        visible_ = false;
        started_ = false;

        lv_obj_t* app_to_destroy = app;
        app = nullptr;

        if (destroy_) destroy_(app_to_destroy);
    }

    void setOnShow(void (*show)(lv_obj_t*)) { show_ = show; }
    void setOnHide(void (*hide)(lv_obj_t*)) { hide_ = hide; }
    void setOnDestroy(void (*destroy)(lv_obj_t*)) { destroy_ = destroy; }

    lv_obj_t* getAppContainer() { return app; }
    const char* getName() { return name_; }
    const char* getIcon() { return icon_; }
    bool isVisible() { return visible_; }
    bool isStarted() { return started_; }
    bool isPaused() { return !visible_; }

    // Convenience accessors (implemented in PcApp.cpp)
    crosspad::PadManager& padManager();
    crosspad::PadLedController& ledController();
    CrosspadStatus& status();
    CrosspadSettings& settings();

private:
    const char* name_;
    const char* icon_;
    bool visible_ = false;
    bool started_ = false;
    AppFlags type_ = crosspad::APP_FLAG_NONE;

    lv_obj_t* (*create_)(lv_obj_t* parent, App* a) = nullptr;
    void (*destroy_)(lv_obj_t* app) = lv_Default_onDestroy;
    void (*show_)(lv_obj_t* app) = lv_Default_onShow;
    void (*hide_)(lv_obj_t* app) = lv_Default_onHide;
};
