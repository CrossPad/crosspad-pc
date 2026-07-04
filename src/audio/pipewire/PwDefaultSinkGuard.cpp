// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwDefaultSinkGuard.hpp"
#include "PwContext.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include <cstdio>
#include <cstring>

namespace crosspad_pc {

std::string pwExtractJsonName(const char* value)
{
    if (!value) return "";
    const char* p = strstr(value, "\"name\"");
    if (!p) return "";
    p += 6; // past "name"
    const char* colon = strchr(p, ':');
    if (!colon) return "";
    p = colon + 1;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return "";
    ++p;
    const char* end = strchr(p, '"');
    if (!end) return "";
    return std::string(p, static_cast<size_t>(end - p));
}

namespace {

// --- registry discovery: find the "default" metadata global's id ---

struct FindMetadataCtx {
    uint32_t id = SPA_ID_INVALID;
};

void onRegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/,
                       const char* type, uint32_t /*version*/,
                       const struct spa_dict* props)
{
    auto* ctx = static_cast<FindMetadataCtx*>(data);
    if (ctx->id != SPA_ID_INVALID) return; // already found
    if (!type || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0) return;
    const char* name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
    if (name && strcmp(name, "default") == 0) ctx->id = id;
}

// --- pw_core_sync roundtrip helper ---

struct SyncCtx {
    int pending = -1;
    bool done = false;
};

void onCoreDone(void* data, uint32_t id, int seq)
{
    if (id != PW_ID_CORE) return;
    auto* ctx = static_cast<SyncCtx*>(data);
    if (seq != ctx->pending) return;
    ctx->done = true;
    pw_thread_loop_signal(PwContext::instance().loop(), false);
}

// Issues pw_core_sync and blocks (bounded ~2s) until the matching `done`
// event arrives. Must be called with the PwContext lock held. Returns
// false on timeout.
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

// --- one-shot property capture used during discovery (before we decide
// whether to keep the metadata proxy bound or tear it down immediately) ---

struct CaptureCtx {
    std::string value;
    bool have = false;
};

int onCaptureProperty(void* data, uint32_t subject, const char* key,
                       const char* /*type*/, const char* value)
{
    auto* ctx = static_cast<CaptureCtx*>(data);
    if (subject == 0 && key && strcmp(key, "default.audio.sink") == 0) {
        ctx->value = value ? value : "";
        ctx->have = true;
    }
    return 0;
}

// Finds the "default" metadata global via the registry, binds it, and
// captures its current "default.audio.sink" property value. Must be
// called with the PwContext lock held. Returns nullptr if the metadata
// object can't be found/bound within the bounded wait.
struct pw_metadata* discoverAndBindDefaultMetadata(struct pw_core* core,
                                                    struct pw_thread_loop* loop,
                                                    std::string& outValue)
{
    struct pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry) return nullptr;

    FindMetadataCtx find;
    struct spa_hook registryListener{};
    struct pw_registry_events registryEvents{};
    registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    registryEvents.global = onRegistryGlobal;
    pw_registry_add_listener(registry, &registryListener, &registryEvents, &find);

    bool ok = syncRoundtrip(core, loop);
    spa_hook_remove(&registryListener);

    if (!ok || find.id == SPA_ID_INVALID) {
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry));
        return nullptr;
    }

    void* proxy = pw_registry_bind(registry, find.id, PW_TYPE_INTERFACE_Metadata,
                                    PW_VERSION_METADATA, 0);
    auto* meta = static_cast<struct pw_metadata*>(proxy);

    // The registry proxy isn't needed once the metadata global is bound —
    // the bound proxy is independent of it.
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry));

    if (!meta) return nullptr;

    CaptureCtx cap;
    struct spa_hook captureListener{};
    struct pw_metadata_events metaEvents{};
    metaEvents.version = PW_VERSION_METADATA_EVENTS;
    metaEvents.property = onCaptureProperty;
    pw_metadata_add_listener(meta, &captureListener, &metaEvents, &cap);

    syncRoundtrip(core, loop); // deliver the initial property burst

    spa_hook_remove(&captureListener);

    if (cap.have) outValue = cap.value;
    return meta;
}

} // namespace

int PwDefaultSinkGuard::onMetaProperty(void* data, uint32_t subject, const char* key,
                                        const char* /*type*/, const char* value)
{
    auto* self = static_cast<PwDefaultSinkGuard*>(data);
    if (subject == 0 && key && strcmp(key, "default.audio.sink") == 0)
        self->lastAudioSinkValue_ = value ? value : "";
    return 0;
}

std::string PwDefaultSinkGuard::queryCurrentDefault()
{
    auto& ctx = PwContext::instance();
    if (!ctx.init()) return "";

    PwContext::Lock lock(ctx);
    std::string value;
    struct pw_metadata* meta = discoverAndBindDefaultMetadata(ctx.core(), ctx.loop(), value);
    if (!meta) return "";
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(meta));
    return pwExtractJsonName(value.c_str());
}

bool PwDefaultSinkGuard::takeover(const std::string& sinkNodeName)
{
    auto& ctx = PwContext::instance();
    if (!ctx.init()) return false;

    PwContext::Lock lock(ctx);

    if (meta_) { // already active — release the old binding before rebinding
        spa_hook_remove(&metaListener_);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(meta_));
        meta_ = nullptr;
    }

    std::string value;
    meta_ = discoverAndBindDefaultMetadata(ctx.core(), ctx.loop(), value);
    if (!meta_) {
        printf("[PwDefaultSinkGuard] no \"default\" metadata object found "
               "(WirePlumber absent?)\n");
        return false;
    }

    if (!havePrevious_) previous_ = pwExtractJsonName(value.c_str());
    lastAudioSinkValue_ = value;

    spa_zero(metaListener_);
    // The events vtable must outlive the hook: spa_hook stores a POINTER to it
    // (it does not copy), and metaListener_ is a member that lives until
    // restore(). A stack-local vtable here would be reclaimed when takeover()
    // returns, so the next property callback on the PipeWire loop thread would
    // jump through a dangling pointer (SIGSEGV). The callback is constant, so a
    // function-local static is the correct, allocation-free backing store.
    static const struct pw_metadata_events kMetaEvents = [] {
        struct pw_metadata_events e{};
        e.version  = PW_VERSION_METADATA_EVENTS;
        e.property = &PwDefaultSinkGuard::onMetaProperty;
        return e;
    }();
    pw_metadata_add_listener(meta_, &metaListener_, &kMetaEvents, this);

    std::string json = "{\"name\":\"" + sinkNodeName + "\"}";
    pw_metadata_set_property(meta_, 0, "default.configured.audio.sink",
                              "Spa:String:JSON", json.c_str());
    syncRoundtrip(ctx.core(), ctx.loop());

    active_ = true;
    return true;
}

void PwDefaultSinkGuard::restore()
{
    auto& ctx = PwContext::instance();
    if (!meta_) { active_ = false; return; }

    if (ctx.isConnected()) {
        PwContext::Lock lock(ctx);
        if (!previous_.empty()) {
            std::string json = "{\"name\":\"" + previous_ + "\"}";
            pw_metadata_set_property(meta_, 0, "default.configured.audio.sink",
                                      "Spa:String:JSON", json.c_str());
        } else {
            pw_metadata_set_property(meta_, 0, "default.configured.audio.sink",
                                      nullptr, nullptr);
        }
        syncRoundtrip(ctx.core(), ctx.loop());

        spa_hook_remove(&metaListener_);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(meta_));
    }
    meta_ = nullptr;
    active_ = false;
}

bool PwDefaultSinkGuard::restoreStale(const std::string& previousSinkName)
{
    auto& ctx = PwContext::instance();
    if (!ctx.init()) return false;

    PwContext::Lock lock(ctx);

    // Bind the "default" metadata object fresh — this is a one-shot restore
    // that does NOT participate in takeover()/restore() state (meta_ stays
    // untouched).
    std::string value;
    struct pw_metadata* meta = discoverAndBindDefaultMetadata(ctx.core(), ctx.loop(), value);
    if (!meta) return false;

    if (!previousSinkName.empty()) {
        std::string json = "{\"name\":\"" + previousSinkName + "\"}";
        pw_metadata_set_property(meta, 0, "default.configured.audio.sink",
                                  "Spa:String:JSON", json.c_str());
    } else {
        pw_metadata_set_property(meta, 0, "default.configured.audio.sink",
                                  nullptr, nullptr);
    }
    syncRoundtrip(ctx.core(), ctx.loop());

    pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(meta));
    return true;
}

PwDefaultSinkGuard::~PwDefaultSinkGuard()
{
    restore();
}

} // namespace crosspad_pc

#endif // __linux__
