/**
 * @file GuiThread.hpp
 * @brief The one thread LVGL runs on, and a way onto it from any other.
 *
 * LVGL is built without an OS layer (LV_USE_OS = LV_OS_NONE) and with its own
 * allocator, so nothing in it may be called from a second thread -- not even
 * lv_async_call(), which allocates a timer and links it into the timer list.
 * The RtMidi and SimpleBLE callbacks, the audio-device worker threads and the
 * FreeRTOS worker tasks all called it, racing the LVGL task's allocator: a
 * corrupt TLSF heap, and a segfault somewhere later.
 *
 * post() is lv_async_call() for every thread. On Linux the simulator links
 * with --wrap=lv_async_call, so the calls crosspad-gui and the apps make
 * themselves come through here too.
 */
#pragma once

#include "lvgl/lvgl.h"

namespace pc_gui {

/// The calling thread is the LVGL thread from now on. Once, on the LVGL
/// task, after lv_init(): it also creates the timer that runs what other
/// threads post.
void bindThread();

/// True on the thread bindThread() was called on.
bool onGuiThread();

/// Run @p cb(@p user) on the LVGL thread: lv_async_call() there, queued for
/// the next lv_timer_handler() pass from anywhere else.
void post(lv_async_cb_t cb, void* user);

} // namespace pc_gui
