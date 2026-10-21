// SPDX-License-Identifier: GPL-3.0-or-later

#include "PcSecondKit.hpp"
#include "PcKitLoad.hpp"

#include <crosspad/audio/IAudioNode.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/sampler/BlockPool.hpp>
#include <crosspad/sampler/ISecondKit.hpp>
#include <crosspad/sampler/KitTake.hpp>
#include <crosspad/sampler/SampleStore.hpp>
#include <crosspad/sampler/SamplerEngine.hpp>
#include <crosspad/sampler/SamplerKit.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace crosspad_pc {
namespace {

using crosspad::BlockPool;
using crosspad::ISecondKit;
using crosspad::KitInfo;
using crosspad::SamplerEngine;
using crosspad::SamplerKit;
using crosspad::SampleStore;
using crosspad::TakeSpec;
using namespace kit_load;

using Clock = std::chrono::steady_clock;

/* The streaming pass runs once a millisecond, the board's audio_bg rate. */
constexpr auto kIoPeriod    = std::chrono::milliseconds(1);
/* Bounds on a release's waits: the fades, a streaming pass to leave. Each is a
 * wait for another thread, never a spin. */
constexpr auto kFadeWait    = std::chrono::milliseconds(100);
constexpr auto kQuiesceWait = std::chrono::milliseconds(500);
constexpr auto kQuiescePoll = std::chrono::milliseconds(1);
constexpr auto kFadePoll    = std::chrono::milliseconds(5);
constexpr auto kGatePoll    = std::chrono::microseconds(200);

uint32_t now_us() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

void note_peak(std::atomic<float>& meter, float v) {
    float cur = meter.load(std::memory_order_relaxed);
    while (v > cur && !meter.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

/* The "Kit 2" channel. Every way into the engine -- the render, a note --
 * goes through enter()/leave(): a release clears `active`, then waits for
 * `inside` to reach zero, so once it has, nobody is in the engine and nobody
 * can get in until the next load sets `active` again. Both sides use
 * sequentially consistent operations on the two flags (enter() raises
 * `inside` before it re-reads `active`), which is what makes the hand-shake
 * sound: either the release sees the entrant, or the entrant sees the closed
 * gate. */
class Kit2Node : public crosspad::IAudioNode {
public:
    const char* name() const override { return "Kit 2"; }
    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override {
        engine.onPrepare(maxFrames, sampleRate);
    }

    void mix(float* out, uint32_t frames) override {
        if (!enter()) return;
        engine.mix(out, frames);
        float l = 0.0f, r = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            const float a = out[i * 2] < 0.0f ? -out[i * 2] : out[i * 2];
            const float b = out[i * 2 + 1] < 0.0f ? -out[i * 2 + 1] : out[i * 2 + 1];
            if (a > l) l = a;
            if (b > r) r = b;
        }
        note_peak(meter[0], l);
        note_peak(meter[1], r);
        leave();
    }

    /// True: the engine is open and this caller is counted inside it.
    bool enter() {
        if (!active.load(std::memory_order_acquire)) return false;
        inside.fetch_add(1, std::memory_order_seq_cst);
        if (active.load(std::memory_order_seq_cst)) return true;
        inside.fetch_sub(1, std::memory_order_release);
        return false;
    }
    void leave() { inside.fetch_sub(1, std::memory_order_release); }

    /// Worker: close the gate and wait for whoever is inside to leave. A
    /// render pass or a note is short, so this is a wait, not a timeout.
    void close() {
        active.store(false, std::memory_order_seq_cst);
        while (inside.load(std::memory_order_seq_cst) != 0) std::this_thread::sleep_for(kGatePoll);
    }

    SamplerEngine engine;
    std::atomic<bool> active{false};
    std::atomic<uint32_t> inside{0};
    std::atomic<float> meter[2] = {};
};

/* Allocated once and never destroyed: the worker and the streaming thread run
 * for the life of the process, and static destructors at exit would free the
 * store and the pool under them. */
struct Kit2State {
    Kit2Node        node;
    PcSampleSource  source;
    BlockPool       pool;
    SampleStore     store;
    SamplerKit      kit;
    SamplerEngine::Params params;

    /* What the keeper asks for, what is loaded, what would not load -- under mu. */
    mutable std::mutex mu;
    std::condition_variable cv;
    std::string want, have, failed;
    bool pending = false;   ///< a request the worker has not taken yet
    bool running = false;   ///< the worker is carrying one out
    bool started = false;
};

Kit2State& st() {
    static Kit2State* s = new Kit2State();
    return *s;
}

/* Every voice faded while the render still plays the engine, then the engine
 * off the render and every streaming pass out of it. */
bool quiesce(Kit2State& s) {
    SamplerEngine& e = s.node.engine;
    e.beginMutation();
    if (s.node.active.load(std::memory_order_acquire)) {
        e.allNotesOff();
        wait_for([&] { e.serviceIo(); return !e.anySounding(); }, kFadeWait, kFadePoll);
        s.node.close();
    }
    wait_for([&] { return e.serviceIo(); }, kQuiesceWait, kQuiescePoll);
    const bool stuck = e.anySounding();
    const bool done = wait_for([&] { return e.abandonVoices(); }, kQuiesceWait, kQuiescePoll);
    if (!done) {
        std::printf("[kit2] quiesce: a streaming pass held the voices; going on\n");
    } else if (stuck) {
        std::printf("[kit2] voices abandoned: the render never ended them\n");
    }
    wait_for([&] { return e.ioIdle(); }, kQuiesceWait, kQuiescePoll);
    return done && e.ioIdle();
}

/* The kit given back, sample by sample. Worker only. Leaves the note path
 * closed; a load opens it. */
void release_kit(Kit2State& s) {
    const bool idle = quiesce(s);
    s.node.engine.setKit(nullptr);
    s.store.unload();
    s.source.forget();
    if (idle) {
        s.pool.releaseAll();
    } else {
        std::printf("[kit2] release: pool pages forgotten, not freed (a streaming pass never left)\n");
        s.pool.forgetPages();
    }
    s.kit = SamplerKit{};
    std::lock_guard<std::mutex> lk(s.mu);
    s.have.clear();
}

enum class Outcome : uint8_t { Loaded, Failed };

/* The kit at @p path into the engine. Worker only. */
Outcome load_kit(Kit2State& s, const std::string& path) {
    /* The copy: off the card, on this worker, the kit list untouched. */
    crosspad::IKitManager* km = crosspad::getKitManager();
    auto info = std::make_unique<KitInfo>();
    auto spec = std::make_unique<TakeSpec>();
    const bool read = km && km->readKitCopy(path, *info);
    if (!read || !crosspad::buildTakeSpec(*info, -1, *spec)) {
        std::printf("[kit2] %s: %s\n", path.c_str(), !read ? "would not read" : "not a sampler kit");
        return Outcome::Failed;
    }

    release_kit(s);
    SamplerEngine& e = s.node.engine;
    s.params.streamBlocks = kStreamBlocks;
    e.setParams(s.params);
    s.store.bind(&s.source, &heap_alloc, &heap_free, nullptr);
    e.bind(&s.pool, &s.store);

    const uint32_t headBytes = plan_heads(s.source, spec->kitPath, spec->pads, spec->layers);
    uint16_t loaded = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
        loaded += build_pad(s.source, s.store, spec->kitPath, spec->pads[p], spec->layers, p, headBytes,
                            s.kit.pad[p]);
    }
    if (loaded == 0) {
        std::printf("[kit2] %s: no sample could be read\n", path.c_str());
        s.store.unload();
        s.source.forget();
        s.kit = SamplerKit{};
        e.endMutation();
        return Outcome::Failed;
    }
    const auto stats = s.store.stats();
    if (stats.slots > stats.completeSlots) {
        s.pool.reserve(static_cast<uint32_t>(s.params.maxStreamingVoices) * kStreamBlocks);
    }
    e.setKit(&s.kit);
    e.resetCounters();
    e.endMutation();
    s.node.active.store(true, std::memory_order_seq_cst);
    std::printf("[kit2] '%s': %u samples, head %u kB each, heads %u kB\n", spec->name, (unsigned)stats.slots,
                (unsigned)(headBytes / 1024), (unsigned)(stats.headBytes / 1024));
    return Outcome::Loaded;
}

void worker_loop() {
    Kit2State& s = st();
    for (;;) {
        std::string want;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.cv.wait(lk, [&] { return s.pending; });
            s.pending = false;
            s.running = true;
            want = s.want;
        }
        Outcome out = Outcome::Loaded;
        if (want.empty()) release_kit(s);
        else              out = load_kit(s, want);
        std::lock_guard<std::mutex> lk(s.mu);
        s.running = false;
        if (out == Outcome::Loaded && !want.empty()) s.have = want;
        else if (out == Outcome::Failed) s.failed = want;
    }
}

void io_loop() {
    Kit2State& s = st();
    for (;;) {
        if (s.node.active.load(std::memory_order_acquire)) s.node.engine.serviceIo();
        std::this_thread::sleep_for(kIoPeriod);
    }
}

/* Called with st().mu held. */
void ensure_started(Kit2State& s) {
    if (s.started) return;
    s.started = true;

    BlockPool::Config c;
    c.blockBytes = kBlockBytes;
    c.pageBytes  = kPageBytes;
    c.maxPages   = kMaxPages;
    c.alloc = &heap_alloc;
    c.free  = &heap_free;
    c.tableAlloc = &heap_alloc;
    c.tableFree  = &heap_free;
    if (!s.pool.configure(c)) std::printf("[kit2] block pool refused its configuration\n");
    s.node.engine.setClock(&now_us);

    std::thread(worker_loop).detach();
    std::thread(io_loop).detach();
}

class PcSecondKit final : public ISecondKit {
public:
    void hold(const std::string& kitPath) override {
        Kit2State& s = st();
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (kitPath == s.want) return;
            ensure_started(s);
            s.want = kitPath;
            s.failed.clear();
            s.pending = true;
        }
        s.cv.notify_one();
    }

    ISecondKit::State state() const override {
        auto& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.pending || s.running) return ISecondKit::State::Loading;
        if (!s.want.empty() && s.want == s.failed) return ISecondKit::State::Failed;
        return s.node.active.load(std::memory_order_acquire) ? ISecondKit::State::Ready
                                                             : ISecondKit::State::Idle;
    }

    std::string kit() const override {
        std::lock_guard<std::mutex> lk(st().mu);
        return st().want;
    }

    void noteOn(uint8_t pad, uint8_t velocity) override {
        Kit2Node& n = st().node;
        if (!n.enter()) return;
        n.engine.noteOn(pad, velocity);
        n.leave();
    }

    void noteOff(uint8_t pad) override {
        Kit2Node& n = st().node;
        if (!n.enter()) return;
        n.engine.noteOff(pad);
        n.leave();
    }

    void takeLevel(float& left, float& right) override {
        Kit2Node& n = st().node;
        left = n.meter[0].exchange(0.0f, std::memory_order_relaxed);
        right = n.meter[1].exchange(0.0f, std::memory_order_relaxed);
    }
};

PcSecondKit s_secondKit;

} // namespace

crosspad::IAudioNode* second_kit_node() { return &st().node; }

crosspad::ISecondKit* second_kit() { return &s_secondKit; }

} // namespace crosspad_pc
