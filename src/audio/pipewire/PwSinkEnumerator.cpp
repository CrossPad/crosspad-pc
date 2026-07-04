// SPDX-License-Identifier: MIT
#ifdef __linux__

#include "PwSinkEnumerator.hpp"
#include "PwContext.hpp"

#include <pipewire/pipewire.h>
#include <spa/utils/defs.h>

#include <cstring>

namespace crosspad_pc {
namespace {

struct SyncCtx {
    int  pending = -1;
    bool done    = false;
};

void onCoreDone(void* data, uint32_t id, int seq)
{
    if (id != PW_ID_CORE) return;
    auto* ctx = static_cast<SyncCtx*>(data);
    if (seq != ctx->pending) return;
    ctx->done = true;
    pw_thread_loop_signal(PwContext::instance().loop(), false);
}

// Same idiom as PwDefaultSinkGuard: pw_core_sync + bounded wait for the
// matching `done`. Must be called with the PwContext lock held.
bool syncRoundtrip(struct pw_core* core, struct pw_thread_loop* loop)
{
    SyncCtx sctx;
    struct spa_hook coreListener{};
    struct pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = onCoreDone;
    pw_core_add_listener(core, &coreListener, &events, &sctx);

    sctx.pending = pw_core_sync(core, PW_ID_CORE, 0);

    struct timespec abstime;
    pw_thread_loop_get_time(loop, &abstime, 2 * SPA_NSEC_PER_SEC);
    bool ok = true;
    while (!sctx.done) {
        if (pw_thread_loop_timed_wait_full(loop, &abstime) < 0) { ok = false; break; }
    }
    spa_hook_remove(&coreListener);
    return ok;
}

struct CollectCtx {
    std::vector<PwSinkEntry> sinks;
};

void onRegistryGlobal(void* data, uint32_t /*id*/, uint32_t /*permissions*/,
                      const char* type, uint32_t /*version*/,
                      const struct spa_dict* props)
{
    auto* ctx = static_cast<CollectCtx*>(data);
    if (!type || !props) return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass || strcmp(mediaClass, "Audio/Sink") != 0) return;

    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name) return;
    // Never offer CrossPad's own virtual sinks as an OUT target — that
    // routes the mixer's output straight back into its input.
    if (strncmp(name, "crosspad_", 9) == 0) return;

    const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!desc || !*desc) desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    if (!desc || !*desc) desc = name;

    ctx->sinks.push_back({name, desc});
}

} // namespace

std::vector<PwSinkEntry> pwEnumerateSinks()
{
    auto& ctx = PwContext::instance();
    if (!ctx.init()) return {};

    PwContext::Lock lock(ctx);

    struct pw_registry* registry =
        pw_core_get_registry(ctx.core(), PW_VERSION_REGISTRY, 0);
    if (!registry) return {};

    CollectCtx collect;
    struct spa_hook registryListener{};
    struct pw_registry_events registryEvents{};
    registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    registryEvents.global = onRegistryGlobal;
    pw_registry_add_listener(registry, &registryListener, &registryEvents, &collect);

    bool ok = syncRoundtrip(ctx.core(), ctx.loop());

    spa_hook_remove(&registryListener);
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry));

    if (!ok) return {};
    return std::move(collect.sinks);
}

} // namespace crosspad_pc

#endif // __linux__
