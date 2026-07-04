// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwContext.hpp"
#include <pipewire/pipewire.h>
#include <cstdio>

namespace crosspad_pc {

PwContext& PwContext::instance() { static PwContext ctx; return ctx; }

PwContext::Lock::Lock(PwContext& c) : c_(c) { if (c_.loop_) pw_thread_loop_lock(c_.loop_); }
PwContext::Lock::~Lock() { if (c_.loop_) pw_thread_loop_unlock(c_.loop_); }

bool PwContext::init()
{
    if (initialized_) return core_ != nullptr;
    initialized_ = true;

    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("crosspad-pw", nullptr);
    if (!loop_) return false;
    if (pw_thread_loop_start(loop_) != 0) {
        pw_thread_loop_destroy(loop_); loop_ = nullptr; return false;
    }
    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_)
        core_ = pw_context_connect(context_, nullptr, 0);
    pw_thread_loop_unlock(loop_);

    if (!core_) {
        printf("[PwContext] PipeWire daemon unreachable — native virtual audio off\n");
        shutdown();            // tears everything down and resets initialized_
        initialized_ = true;   // remember the failed attempt; don't retry every call
        return false;
    }
    printf("[PwContext] connected (libpipewire %s)\n", pw_get_library_version());
    return true;
}

void PwContext::shutdown()
{
    if (loop_) pw_thread_loop_lock(loop_);
    if (core_)    { pw_core_disconnect(core_);    core_ = nullptr; }
    if (context_) { pw_context_destroy(context_); context_ = nullptr; }
    if (loop_) {
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    // A clean shutdown makes the context re-initializable. init()'s failure
    // path re-sets initialized_ = true right after calling shutdown() so a
    // failed attempt is still remembered (no retry storm).
    initialized_ = false;
}

bool PwContext::isConnected() const { return core_ != nullptr; }

} // namespace crosspad_pc
#endif // __linux__
