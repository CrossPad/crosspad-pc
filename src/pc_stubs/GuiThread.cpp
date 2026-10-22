/**
 * @file GuiThread.cpp
 * @brief See GuiThread.hpp.
 */

#include "pc_stubs/GuiThread.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef CROSSPAD_WRAP_LV_ASYNC
extern "C" lv_result_t __real_lv_async_call(lv_async_cb_t cb, void* user);
#define LV_ASYNC_CALL_DIRECT __real_lv_async_call
#else
#define LV_ASYNC_CALL_DIRECT lv_async_call
#endif

namespace pc_gui {

namespace {

struct Posted {
    lv_async_cb_t cb;
    void*         user;
};

std::thread::id   s_guiThread;
std::atomic<bool> s_bound{false};

std::mutex          s_mutex;
std::vector<Posted> s_posted;

/* Every lv_timer_handler() pass: what the other threads posted, in order. */
void drain_cb(lv_timer_t*)
{
    std::vector<Posted> batch;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        batch.swap(s_posted);
    }
    for (const Posted& p : batch) p.cb(p.user);
}

} // namespace

void bindThread()
{
    s_guiThread = std::this_thread::get_id();
    s_bound.store(true, std::memory_order_release);
    lv_timer_create(drain_cb, 1, nullptr);
}

bool onGuiThread()
{
    return s_bound.load(std::memory_order_acquire) &&
           std::this_thread::get_id() == s_guiThread;
}

void post(lv_async_cb_t cb, void* user)
{
    if (!cb) return;
    /* Before bindThread() there is no other thread to race: the LVGL task is
     * still in lv_init(). */
    if (onGuiThread() || !s_bound.load(std::memory_order_acquire)) {
        LV_ASYNC_CALL_DIRECT(cb, user);
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_posted.push_back({cb, user});
}

} // namespace pc_gui

#ifdef CROSSPAD_WRAP_LV_ASYNC
extern "C" lv_result_t __wrap_lv_async_call(lv_async_cb_t cb, void* user)
{
    pc_gui::post(cb, user);
    return LV_RESULT_OK;
}
#endif
