// SPDX-License-Identifier: MIT
// Singleton owning the one pw_thread_loop + pw_core connection used by the
// native PipeWire virtual-audio backend. Do NOT include pipewire headers
// here — this header must stay includable from non-PipeWire TUs.
#pragma once

struct pw_core;
struct pw_thread_loop;
struct pw_context;

namespace crosspad_pc {

class PwContext {
public:
    static PwContext& instance();

    // Idempotent: safe to call repeatedly. Returns false if headers/daemon
    // are unavailable (a failed attempt is remembered, not retried).
    bool init();

    // Safe to call multiple times, including on an already-shutdown context.
    void shutdown();

    bool isConnected() const;

    struct pw_core* core() const { return core_; }
    struct pw_thread_loop* loop() const { return loop_; }

    // RAII pw_thread_loop_lock/unlock around access to PipeWire proxies.
    class Lock {
    public:
        explicit Lock(PwContext& c);
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    private:
        PwContext& c_;
    };

private:
    PwContext() = default;
    ~PwContext() = default;
    PwContext(const PwContext&) = delete;
    PwContext& operator=(const PwContext&) = delete;

    struct pw_thread_loop* loop_ = nullptr;
    struct pw_context* context_ = nullptr;
    struct pw_core* core_ = nullptr;
    bool initialized_ = false;
};

} // namespace crosspad_pc
