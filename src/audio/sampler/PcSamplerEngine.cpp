// SPDX-License-Identifier: GPL-3.0-or-later

#include "PcSamplerEngine.hpp"

#include <crosspad/audio/IAudioNode.hpp>
#include <crosspad/audio/ScopeTap.hpp>
#include <crosspad/audio/WavLayout.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/pad/PadLedController.hpp>
#include <crosspad/sampler/BlockPool.hpp>
#include <crosspad/sampler/KitTake.hpp>
#include <crosspad/sampler/SampleStore.hpp>
#include <crosspad/sampler/SamplerEngine.hpp>
#include <crosspad/sampler/SamplerKit.hpp>
#include <crosspad/sequencer/MetronomeNode.hpp>

#include <crosspad-sampler/ui/sampler_panel.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace crosspad_pc {
namespace {

using crosspad::BlockPool;
using crosspad::HeadPlan;
using crosspad::IKitManager;
using crosspad::KitInfo;
using crosspad::KitPad;
using crosspad::PadMode;
using crosspad::SampleDesc;
using crosspad::SamplerEngine;
using crosspad::SamplerKit;
using crosspad::SampleStore;
using crosspad::TakeLayer;
using crosspad::TakePad;
using crosspad::TakePadSpec;
using crosspad::TakeSpec;

using Clock = std::chrono::steady_clock;

uint32_t now_us() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

/* The board's numbers, kept: 16 kB blocks, six to a voice's ring, sixteen
 * rings at most, so a kit streams here the way it does on the card. */
constexpr size_t   kBlockBytes   = 16384;
constexpr uint8_t  kStreamBlocks = 6;
constexpr size_t   kPageBytes    = kStreamBlocks * kBlockBytes;
constexpr uint32_t kMaxPages     = 16;
constexpr uint32_t kPreviewBlocks = crosspad::StreamWindow::kDefaultBlocks;

/* What a sample asks to keep resident, as on the board -- halved by HeadPlan
 * until the kit fits the budget. The board's budget is what its arena has
 * left; the heap here has no such wall, so it is a plain cap that still
 * leaves a long sample streaming. */
constexpr uint32_t kHeadBytes   = 256u * 1024;
constexpr uint32_t kHeadsBudget = 32u * 1024 * 1024;

/* The streaming pass runs once a millisecond, the board's audio_bg rate --
 * what kPreviewPrimeMaxPasses and the ring's lookahead are sized for. */
constexpr auto     kIoPeriod        = std::chrono::milliseconds(1);
/* How often a sample the last kit no longer names is looked at again. */
constexpr uint32_t kRetirePollPasses = 50;

/* Bounds on a teardown's waits: the fades, one render call in flight, a
 * streaming pass to leave. Each is a wait for another thread, never a spin. */
constexpr auto kFadeWait       = std::chrono::milliseconds(100);
constexpr auto kRenderBarrier  = std::chrono::milliseconds(20);
constexpr auto kQuiesceWait    = std::chrono::milliseconds(500);
constexpr auto kQuiescePoll    = std::chrono::milliseconds(1);
constexpr auto kFadePoll       = std::chrono::milliseconds(5);

/* Peak per bin is read through this many frames at a time. */
constexpr uint32_t kWaveChunkFrames = 1024;

/* The 0..127 pan the engine's helper takes has its centre here; kits store a
 * signed offset from it. */
constexpr int kPanCentre = 64;
/* Volume 100 is unity in a kit; what an audition asked at 0 plays at. */
constexpr int kUnityVolume = 100;
constexpr int kMaxSemitones = 24;

// ── Where bytes come from ───────────────────────────────────────────────

/* A WAV is parsed once per path, when the store loads its head, and kept
 * here: a read only gets a path and a frame number. Written by the loader,
 * read by the streaming thread -- one mutex, held for the copy only. */
class PcSampleSource : public crosspad::ISampleSource {
public:
    struct Entry {
        uint32_t dataOffset = 0;
        uint32_t frames = 0;
        uint32_t rate = 0;
        uint8_t  channels = 0;
    };

    static bool parse(const char* path, Entry& e) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        crosspad::WavLayout lay;
        const bool ok = lay.parse(f) && lay.pcm16();
        std::fclose(f);
        if (!ok) return false;
        e.dataOffset = lay.dataOffset;
        e.frames = lay.frames;
        e.rate = lay.rate;
        e.channels = static_cast<uint8_t>(lay.channels);
        return true;
    }

    static uint32_t readEntry(const char* path, const Entry& e, uint32_t first, uint32_t frames,
                              int16_t* dst) {
        if (frames == 0 || first >= e.frames) return 0;
        if (first + frames > e.frames) frames = e.frames - first;
        FILE* f = std::fopen(path, "rb");
        if (!f) return 0;
        const uint32_t frameBytes = e.channels * sizeof(int16_t);
        uint32_t got = 0;
        if (std::fseek(f, static_cast<long>(e.dataOffset + first * frameBytes), SEEK_SET) == 0) {
            got = static_cast<uint32_t>(std::fread(dst, frameBytes, frames, f));
        }
        std::fclose(f);
        return got;
    }

    bool describe(const char* path, SampleDesc& out) override {
        Entry e;
        if (!parse(path, e)) return false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            table_[path] = e;
        }
        out.frames = e.frames;
        out.sampleRate = e.rate;
        out.channels = e.channels;
        out.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t frames, int16_t* dst) override {
        Entry e;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto it = table_.find(path);
            if (it == table_.end()) return 0;
            e = it->second;
        }
        return readEntry(path, e, first, frames, dst);
    }

    void forget() {
        std::lock_guard<std::mutex> lk(mu_);
        table_.clear();
    }

private:
    std::mutex mu_;
    std::map<std::string, Entry> table_;
};

/* The audition's: one file outside the kit, described and read on the
 * streaming thread only. */
class PcPreviewSource : public crosspad::ISampleSource {
public:
    bool describe(const char* path, SampleDesc& out) override {
        if (!PcSampleSource::parse(path, entry_)) { path_.clear(); return false; }
        path_ = path;
        out.frames = entry_.frames;
        out.sampleRate = entry_.rate;
        out.channels = entry_.channels;
        out.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t frames, int16_t* dst) override {
        if (path_ != path) return 0;
        return PcSampleSource::readEntry(path, entry_, first, frames, dst);
    }

private:
    std::string path_;
    PcSampleSource::Entry entry_;
};

// ── Voice reports, off the render thread ────────────────────────────────

/* The engine reports a voice's start and end from the render. Nothing there
 * may paint an LED or touch the GUI's bookkeeping, so the reports queue here
 * (one producer, the render; one consumer, the streaming thread) and are
 * handed on from there -- the board's render records, its pad_anim paints. */
struct VoiceReport {
    uint8_t pad;
    uint8_t layer;
    bool    on;
};

class VoiceReports {
public:
    void push(const VoiceReport& r) {
        const uint32_t w = w_.load(std::memory_order_relaxed);
        if (w - r_.load(std::memory_order_acquire) >= kSize) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ring_[w % kSize] = r;
        w_.store(w + 1, std::memory_order_release);
    }

    template <class F>
    void drain(F&& f) {
        uint32_t r = r_.load(std::memory_order_relaxed);
        const uint32_t w = w_.load(std::memory_order_acquire);
        for (; r != w; ++r) f(ring_[r % kSize]);
        r_.store(r, std::memory_order_release);
    }

private:
    static constexpr uint32_t kSize = 256;
    VoiceReport ring_[kSize] = {};
    std::atomic<uint32_t> w_{0}, r_{0}, dropped_{0};
};

// ── The node ────────────────────────────────────────────────────────────

/* The mixer's "Sampler" channel: the kit's voices, and the audition -- which
 * on the board has a "Preview" channel of its own; here it shares this one,
 * as the simulator's auditions always have. */
class NativeSamplerNode : public crosspad::IAudioNode {
public:
    NativeSamplerNode() { scope.setEnabled(false); }

    const char* name() const override { return "Sampler"; }
    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override {
        engine.onPrepare(maxFrames, sampleRate);
        scope.setSampleRate(sampleRate);
        rate.store(sampleRate, std::memory_order_relaxed);
    }

    void onSampleRateChange(uint32_t sampleRate) override {
        scope.setSampleRate(sampleRate);
        rate.store(sampleRate, std::memory_order_relaxed);
    }

    static constexpr uint32_t kScopeChunk = 32;

    void mix(float* out, uint32_t frames) override {
        const uint32_t t0 = now_us();
        if (active.load(std::memory_order_acquire)) engine.mix(out, frames);
        if (engine.previewState() != 0) engine.mixPreview(out, frames);
        const uint32_t dt = now_us() - t0;

        const uint32_t sr = rate.load(std::memory_order_relaxed);
        if (sr && frames) {
            const float load = static_cast<float>(dt) * sr / (static_cast<float>(frames) * 1.0e6f);
            if (load > renderLoad.load(std::memory_order_relaxed)) {
                renderLoad.store(load, std::memory_order_relaxed);
            }
        }

        /* The mixer hands every channel a cleared buffer, so this is the
         * engine's own output: what smpl_peak and the scope report. */
        float m = 0.0f;
        for (uint32_t i = 0; i < frames * 2; ++i) {
            const float a = out[i] < 0.0f ? -out[i] : out[i];
            if (a > m) m = a;
        }
        if (m > peak.load(std::memory_order_relaxed)) peak.store(m, std::memory_order_relaxed);

        if (!scope.enabled()) return;
        float mono[kScopeChunk];
        uint32_t n = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            mono[n++] = (out[i * 2] + out[i * 2 + 1]) * 0.5f;
            if (n == kScopeChunk) { scope.write(mono, n); n = 0; }
        }
        if (n) scope.write(mono, n);
    }

    SamplerEngine engine;
    crosspad::ScopeTap scope;
    std::atomic<bool> active{false};
    std::atomic<uint32_t> rate{0};
    std::atomic<float> peak{0.0f};
    std::atomic<float> renderLoad{0.0f};
};

// ── State ───────────────────────────────────────────────────────────────

/* Allocated once and never destroyed: the streaming thread runs for the
 * life of the process, and static destructors at exit would free the store
 * and the pool under it. */
struct State {
    NativeSamplerNode node;
    PcSampleSource    source;
    PcPreviewSource   previewSource;
    BlockPool         pool;
    SampleStore       store;
    SamplerKit        kit;
    SamplerEngine::Params params;
    VoiceReports      reports;

    /* Every loader-side change to the store, the pool and the kit: the kit
     * loader's worker, the pad editor, a release. Recursive: a swap that
     * finds no kit held takes one first. */
    std::recursive_mutex loader;

    std::mutex  rootMu;
    std::string root;

    uint32_t headBytes = 0;
    uint64_t retired = 0;   ///< store slots no pad names, each waiting for its last voice
    std::atomic<int> kitId{-1};
    int takeFailed = -1;    ///< the kit manager's id of a kit no sample of which could be read

    std::thread io;
    bool started = false;
};

State& st() {
    static State* s = new State();
    return *s;
}

void* heap_alloc(size_t bytes, void*) { return std::malloc(bytes); }
void  heap_free(void* p, void*)       { std::free(p); }

uint16_t slots_incomplete() {
    const auto s = st().store.stats();
    return static_cast<uint16_t>(s.slots - s.completeSlots);
}

// ── Which file a layer plays ────────────────────────────────────────────

/* Pitch the engine does itself; filters and reverse it does not, and the
 * sampler renders a copy with them (crosspad::renderedSamplePath) -- that
 * copy is what plays when it is there, with the step baked in. Reversed, the
 * trim's end is where the copy starts. The board's layer_file(), on stdio. */
struct LayerFile {
    std::string path;
    uint32_t    start = 0;
    uint32_t    end   = 0;
    bool        pitchInFile = false;
};

LayerFile layer_file(const char* kitPath, const TakePad& pad, const TakeLayer& layer) {
    LayerFile f;
    const std::string logical = crosspad::takeLayerLogical(kitPath, layer);
    f.path  = native_sampler_fs_path(logical.c_str());
    f.start = layer.start;
    f.end   = layer.end;

    const std::string copyLogical = crosspad::takeLayerCopy(pad, logical);
    if (copyLogical.empty()) return f;
    const std::string copy = native_sampler_fs_path(copyLogical.c_str());
    struct stat sb;
    if (::stat(copy.c_str(), &sb) != 0) return f;

    if (pad.reverse) {
        SampleDesc d;
        if (!st().source.describe(f.path.c_str(), d) || d.frames == 0) return f;
        const uint32_t n = d.frames;
        const uint32_t e = (f.end && f.end < n) ? f.end : n;
        const uint32_t b = f.start < e ? f.start : e;
        f.start = n - e;
        f.end   = b ? n - b : 0;
    }
    f.path = copy;
    f.pitchInFile = true;
    return f;
}

/* One pad from a copy of the kit model into @p kp, its samples loaded.
 * @return layers that found their file. */
uint16_t build_pad(const char* kitPath, const TakePad& tp, const TakeLayer* layers, uint8_t p,
                   uint32_t headBytes, KitPad& kp) {
    State& s = st();
    kp = KitPad{};
    kp.mode  = static_cast<PadMode>(tp.mode <= static_cast<uint8_t>(PadMode::Loop) ? tp.mode : 0);
    kp.choke = static_cast<uint8_t>(tp.choke & 0x07);
    kp.gain  = SamplerKit::volumeToGain(tp.volume);
    kp.pan   = SamplerKit::panToBalance(tp.pan + kPanCentre);
    kp.semitones = tp.pitch;
    kp.roundRobin = tp.cycle;

    uint16_t loaded = 0;
    for (uint8_t i = 0; i < tp.layerCount && i < KitPad::kMaxLayers; ++i) {
        const LayerFile lf = layer_file(kitPath, tp, layers[tp.firstLayer + i]);
        SampleDesc d;
        if (!s.source.describe(lf.path.c_str(), d)) {
            std::printf("[sampler] pad %u layer %u: cannot use '%s'\n", (unsigned)p, (unsigned)i,
                        lf.path.c_str());
            continue;
        }
        const uint32_t frameBytes = d.channels * sizeof(int16_t);
        const uint16_t slot = s.store.load(lf.path.c_str(), headBytes / frameBytes, lf.start, lf.end);
        if (slot == SampleStore::kNoSlot) {
            std::printf("[sampler] pad %u layer %u: no slot for '%s'\n", (unsigned)p, (unsigned)i,
                        lf.path.c_str());
            continue;
        }
        kp.layers[kp.layerCount].slot  = slot;
        kp.layers[kp.layerCount].start = lf.start;
        kp.layers[kp.layerCount].end   = lf.end;
        ++kp.layerCount;
        ++loaded;
        if (lf.pitchInFile) kp.semitones = 0;
    }
    return loaded;
}

/* Head size for a kit, from every sample's length: what HeadPlan makes of
 * kHeadBytes a sample within kHeadsBudget. */
uint32_t plan_heads(const char* kitPath, const TakePad* pads, const TakeLayer* layers) {
    HeadPlan plan;
    plan.headBytes   = kHeadBytes;
    plan.budgetBytes = kHeadsBudget;
    uint32_t sizes[SampleStore::kMaxSlots];
    uint16_t n = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads && n < SampleStore::kMaxSlots; ++p) {
        const TakePad& tp = pads[p];
        for (uint8_t i = 0; i < tp.layerCount && n < SampleStore::kMaxSlots; ++i) {
            const LayerFile lf = layer_file(kitPath, tp, layers[tp.firstLayer + i]);
            SampleDesc d;
            if (!st().source.describe(lf.path.c_str(), d)) continue;
            const uint32_t from = SampleStore::trimStart(lf.start, lf.end, d.frames);
            sizes[n++] = (d.frames - from) * d.channels * sizeof(int16_t);
        }
    }
    return plan.resolve(sizes, n);
}

// ── Teardown ────────────────────────────────────────────────────────────

template <class Pred>
bool wait_for(Pred done, std::chrono::milliseconds bound, std::chrono::milliseconds poll) {
    const auto until = Clock::now() + bound;
    while (!done()) {
        if (Clock::now() >= until) return false;
        std::this_thread::sleep_for(poll);
    }
    return true;
}

/* The audition's ring is pool blocks, so it is back before the pool's pages
 * go: its fade is the render's, and one the render never played is ended
 * from here once no streaming pass is in it. */
void preview_stop_wait() {
    SamplerEngine& e = st().node.engine;
    if (!e.previewBusy()) return;
    e.previewStop();
    if (wait_for([&] { e.serviceIo(); return !e.previewBusy(); }, kFadeWait, kFadePoll)) return;
    wait_for([&] { return e.ioIdle(); }, kQuiesceWait, kQuiescePoll);
    if (wait_for([&] { return e.previewAbandon(); }, kQuiesceWait, kQuiescePoll)) {
        std::printf("[sampler] audition abandoned: the render never faded it\n");
    }
}

/* Every streaming pass out of the voices and every voice ended, before the
 * store's heads or the pool's pages go. False when a pass still held the
 * voices at the bound: the pool's pages are then forgotten, not freed. */
bool io_quiesce() {
    SamplerEngine& e = st().node.engine;
    wait_for([&] { return e.serviceIo(); }, kQuiesceWait, kQuiescePoll);
    const bool stuck = e.anySounding();
    const bool done = wait_for([&] { return e.abandonVoices(); }, kQuiesceWait, kQuiescePoll);
    if (!done) {
        std::printf("[sampler] io quiesce: a streaming pass held the voices; going on\n");
    } else if (stuck) {
        std::printf("[sampler] voices abandoned: the render never ended them\n");
    }
    preview_stop_wait();
    wait_for([&] { return e.ioIdle(); }, kQuiesceWait, kQuiescePoll);
    return done && e.ioIdle();
}

/* The kit's voices faded while the render still plays this engine, then
 * the engine off the render and every pass out. Leaves the note path closed;
 * the caller opens it. Under the loader lock. */
void release_engine() {
    State& s = st();
    SamplerEngine& e = s.node.engine;
    e.beginMutation();
    if (s.node.active.load(std::memory_order_acquire)) {
        e.allNotesOff();
        wait_for([&] { e.serviceIo(); return !e.anySounding(); }, kFadeWait, kFadePoll);
        s.node.active.store(false, std::memory_order_release);
        std::this_thread::sleep_for(kRenderBarrier);
    }
    const bool idle = io_quiesce();
    e.setKit(nullptr);
    s.store.unload();
    s.source.forget();
    if (idle) {
        s.pool.releaseAll();
    } else {
        std::printf("[sampler] release: pool pages forgotten, not freed (a streaming pass never left)\n");
        s.pool.forgetPages();
    }
    s.retired = 0;
    s.headBytes = 0;
    s.kit = SamplerKit{};
    s.kitId.store(-1, std::memory_order_release);
}

/* Room for an audition with no kit, or beside one that streams nothing. */
void reserve_rings() {
    State& s = st();
    const uint32_t rings = slots_incomplete() > 0
        ? static_cast<uint32_t>(s.params.maxStreamingVoices) * kStreamBlocks : 0;
    s.pool.reserve(rings + kPreviewBlocks);
}

// ── A kit, whole ────────────────────────────────────────────────────────

bool take_kit_now(const TakeSpec& spec) {
    State& s = st();
    release_engine();

    s.params = SamplerEngine::Params{};
    s.params.streamBlocks = kStreamBlocks;
    s.node.engine.setParams(s.params);

    const uint32_t headBytes = plan_heads(spec.kitPath, spec.pads, spec.layers);
    uint16_t loaded = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
        loaded += build_pad(spec.kitPath, spec.pads[p], spec.layers, p, headBytes, s.kit.pad[p]);
    }
    if (loaded == 0) {
        std::printf("[sampler] kit %d '%s': no sample could be read\n", spec.kitId, spec.name);
        s.store.unload();
        s.kit = SamplerKit{};
        reserve_rings();
        s.node.engine.endMutation();
        return false;
    }
    reserve_rings();

    s.headBytes = headBytes;
    s.node.engine.setKit(&s.kit);
    s.node.engine.resetCounters();
    s.kitId.store(spec.kitId, std::memory_order_release);
    s.node.active.store(true, std::memory_order_release);
    s.node.engine.endMutation();

    const auto ss = s.store.stats();
    const auto ps = s.pool.stats();
    std::printf("[sampler] kit %d '%s': %u samples (%u stream), head %u kB each, heads %u kB, pool %u kB%s\n",
                spec.kitId, spec.name, (unsigned)ss.slots, (unsigned)slots_incomplete(),
                (unsigned)(headBytes / 1024), (unsigned)(ss.headBytes / 1024),
                (unsigned)(ps.bytesHeld / 1024),
                spec.layersDropped ? " (layers past the store's 64 left out)" : "");
    return true;
}

// ── A kit swapped under a performance ───────────────────────────────────

bool kit_names(uint16_t slot) {
    for (const auto& kp : st().kit.pad) {
        for (uint8_t i = 0; i < kp.layerCount; ++i) {
            if (kp.layers[i].slot == slot) return true;
        }
    }
    return false;
}

/* Give back what no pad names and no voice reads any more. Under the lock. */
void sweep_retired() {
    State& s = st();
    uint64_t left = s.retired;
    while (left) {
        const uint16_t i = static_cast<uint16_t>(__builtin_ctzll(left));
        left &= left - 1;
        if (s.node.engine.slotInUse(i)) continue;
        s.store.unloadSlot(i);
        s.retired &= ~(1ull << i);
    }
}

/* Pad @p p from its copy; @p plan, the whole incoming kit's, for its first
 * pad -- its heads are sized over all of its files. Under the lock. */
void swap_pad(const TakePadSpec& ps, uint8_t p, const TakeSpec* plan) {
    State& s = st();
    if (plan) {
        s.headBytes = plan_heads(plan->kitPath, plan->pads, plan->layers);
        s.kitId.store(plan->kitId, std::memory_order_release);
    }
    sweep_retired();

    KitPad next;
    build_pad(ps.kitPath, ps.pad, ps.layers, p, s.headBytes, next);
    const KitPad old = s.kit.pad[p];
    /* One copy: a note reading the pad meanwhile plays a sample of either
     * kit, both loaded, and no old one goes back while a voice reads it. */
    s.kit.pad[p] = next;

    for (uint8_t i = 0; i < next.layerCount; ++i) {
        if (next.layers[i].slot < SampleStore::kMaxSlots) s.retired &= ~(1ull << next.layers[i].slot);
    }
    for (uint8_t i = 0; i < old.layerCount; ++i) {
        const uint16_t slot = old.layers[i].slot;
        if (slot < SampleStore::kMaxSlots && !kit_names(slot)) s.retired |= 1ull << slot;
    }
    if (slots_incomplete() > 0) reserve_rings();
    sweep_retired();
}

/* Once nothing old is left waiting, the rings a kit that no longer streams
 * does not need go back. @return true while an old sample still sounds. */
bool settle_retired() {
    State& s = st();
    sweep_retired();
    if (s.retired) return true;
    if (slots_incomplete() == 0) {
        s.pool.trim();
        s.pool.reserve(kPreviewBlocks);
    }
    return false;
}

KitInfo* current_sampler_kit() {
    IKitManager* km = crosspad::getKitManager();
    KitInfo* kit = km ? km->getCurrentKit() : nullptr;
    if (!kit || !kit->parsed || kit->engine == crosspad::KitEngine::Pitched) return nullptr;
    return kit;
}

// ── Callbacks and the streaming thread ──────────────────────────────────

void on_voice_start(uint8_t pad, uint8_t layer, void*) { st().reports.push({pad, layer, true}); }
void on_voice_end(uint8_t pad, uint8_t layer, void*)   { st().reports.push({pad, layer, false}); }

/* A Loop pad launches on the metronome's bar line: the sequencer's clock in
 * samples, a mixer channel added after this one, so in the render it still
 * stands where the block this engine mixes starts. */
bool bar_clock(void*, uint32_t& framesToBar) {
    const crosspad::MetronomeNode& m = getMetronome();
    if (!m.running()) return false;
    framesToBar = m.framesToBar();
    return true;
}

/* A Loop pad muted or waiting for its bar shows its playing colour dimmed;
 * the launch and a voice's end light it again (the reports). */
void loop_led(uint8_t pad, uint8_t key) {
    const auto ls = st().node.engine.loopState(pad, key);
    crosspad::getPadLedController().notifyNoteDimmed(
        pad, ls == SamplerEngine::LoopState::Queued || ls == SamplerEngine::LoopState::Muted);
}

void io_loop() {
    State& s = st();
    uint32_t passes = 0;
    for (;;) {
        if (s.node.active.load(std::memory_order_acquire) || s.node.engine.previewBusy()) {
            s.node.engine.serviceIo();
        }
        s.reports.drain([](const VoiceReport& r) {
            crosspad::getPadLedController().notifyNoteDimmed(r.pad, false);
            if (r.on) crosspad_sampler::sampler_on_engine_note_on(r.pad, r.layer);
            else      crosspad_sampler::sampler_on_engine_note_off(r.pad, r.layer);
        });
        if (++passes % kRetirePollPasses == 0) {
            std::unique_lock<std::recursive_mutex> lk(s.loader, std::try_to_lock);
            if (lk.owns_lock() && s.retired) settle_retired();
        }
        std::this_thread::sleep_for(kIoPeriod);
    }
}

void ensure_started() {
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.loader);
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
    if (!s.pool.configure(c)) std::printf("[sampler] block pool refused its configuration\n");

    s.store.bind(&s.source, &heap_alloc, &heap_free, nullptr);
    s.params.streamBlocks = kStreamBlocks;
    s.node.engine.setParams(s.params);
    s.node.engine.bind(&s.pool, &s.store);
    s.node.engine.setNoteCallbacks(&on_voice_start, &on_voice_end, nullptr);
    s.node.engine.setBarClock(&bar_clock, nullptr);
    s.node.engine.setClock(&now_us);
    s.node.engine.setPreviewSource(&s.previewSource);
    s.pool.reserve(kPreviewBlocks);

    s.io = std::thread(io_loop);
    s.io.detach();
}

bool holds_current(uint8_t pad) {
    State& s = st();
    if (!s.node.active.load(std::memory_order_acquire) || pad >= SamplerKit::kPads) return false;
    IKitManager* km = crosspad::getKitManager();
    return km && km->getCurrentKitId() == s.kitId.load(std::memory_order_acquire);
}

} // namespace

// ── Public ──────────────────────────────────────────────────────────────

void native_sampler_set_root(const std::string& root) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.rootMu);
    s.root = root;
}

std::string native_sampler_fs_path(const char* path) {
    if (!path || !path[0]) return {};
    std::string p(path);
    for (char& c : p) { if (c == '\\') c = '/'; }
    State& s = st();
    std::lock_guard<std::mutex> lk(s.rootMu);
    if (s.root.empty()) return p;
    if (p.compare(0, s.root.size(), s.root) == 0) return p;
    if (p[0] != '/') return s.root + "/" + p;
    return s.root + p;
}

crosspad::IAudioNode* native_sampler_node() {
    ensure_started();
    return &st().node;
}

bool native_sampler_active() {
    return st().node.active.load(std::memory_order_acquire);
}

bool native_sampler_swap_pad(uint8_t pad, bool fromLoader) {
    if (pad >= SamplerKit::kPads) return false;
    ensure_started();
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.loader);
    KitInfo* kit = current_sampler_kit();
    if (!kit) return false;
    IKitManager* km = crosspad::getKitManager();
    const int wantId = km->getCurrentKitId();

    if (!s.node.active.load(std::memory_order_acquire)) {
        /* The first kit, or the way back from a pitched one: taken whole,
         * every pad -- this one included. A kit none of whose samples could be
         * read is not tried again pad after pad of the same load; a pad edit
         * (a sample given to a kit that had none) tries again. */
        if (fromLoader && s.takeFailed == wantId) return false;
        auto spec = std::make_unique<TakeSpec>();
        if (!crosspad::buildTakeSpec(*kit, wantId, *spec)) return false;
        if (!take_kit_now(*spec)) {
            s.takeFailed = wantId;
            return false;
        }
        s.takeFailed = -1;
        return true;
    }

    auto ps = std::make_unique<TakePadSpec>();
    if (!crosspad::buildTakePadSpec(*kit, wantId, pad, *ps)) return false;
    std::unique_ptr<TakeSpec> plan;
    if (s.kitId.load(std::memory_order_acquire) != wantId) {
        plan = std::make_unique<TakeSpec>();
        if (!crosspad::buildTakeSpec(*kit, wantId, *plan)) return false;
    }
    swap_pad(*ps, pad, plan.get());
    return true;
}

void native_sampler_release() {
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.loader);
    if (!s.started || !s.node.active.load(std::memory_order_acquire)) return;
    release_engine();
    reserve_rings();
    s.node.engine.endMutation();
}

bool native_sampler_pad_ready(uint8_t pad) {
    return holds_current(pad) && st().kit.pad[pad].layerCount > 0;
}

void native_sampler_note_on(uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key) {
    if (!native_sampler_active()) return;
    st().node.engine.noteOn(pad, velocity, transpose, key);
    if (pad < SamplerKit::kPads) loop_led(pad, key);
}

void native_sampler_note_off(uint8_t pad, uint8_t key) {
    if (!native_sampler_active()) return;
    st().node.engine.noteOff(pad, key);
}

bool native_sampler_arm(uint8_t touch, uint8_t pad, int8_t transpose, uint8_t key) {
    if (!native_sampler_active()) return false;
    return st().node.engine.arm(touch, pad, transpose, key);
}

void native_sampler_play(uint8_t touch, uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key) {
    if (!native_sampler_active()) return;
    SamplerEngine& e = st().node.engine;
    if (!e.fire(touch, pad, velocity, transpose, key)) e.noteOn(pad, velocity, transpose, key);
    if (pad < SamplerKit::kPads) loop_led(pad, key);
}

void native_sampler_disarm(uint8_t touch) {
    st().node.engine.disarm(touch);
}

bool native_sampler_pad_sounding(uint8_t pad) {
    return native_sampler_active() && st().node.engine.padSounding(pad);
}

void native_sampler_set_pad_volume(uint8_t pad, int volume) {
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return;
    State& s = st();
    s.kit.pad[pad].gain = SamplerKit::volumeToGain(volume);
    s.node.engine.setPadMixGains(pad, s.kit.pad[pad].gain, s.kit.pad[pad].pan);
}

void native_sampler_set_pad_pan(uint8_t pad, int pan) {
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return;
    State& s = st();
    s.kit.pad[pad].pan = SamplerKit::panToBalance(pan + kPanCentre);
    s.node.engine.setPadMixGains(pad, s.kit.pad[pad].gain, s.kit.pad[pad].pan);
}

bool native_sampler_set_pad_pitch(uint8_t pad, int semitones) {
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return false;
    if (semitones < -kMaxSemitones) semitones = -kMaxSemitones;
    if (semitones >  kMaxSemitones) semitones =  kMaxSemitones;
    st().kit.pad[pad].semitones = static_cast<int8_t>(semitones);
    return true;
}

void native_sampler_stop_pad(uint8_t pad) {
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return;
    st().node.engine.stopPad(pad);
}

bool native_sampler_sync_pad_mode(uint8_t pad) {
    if (!holds_current(pad)) return false;
    const KitInfo* kit = crosspad::getKitManager()->getCurrentKit();
    if (!kit || !kit->parsed || pad >= KIT_PADS) return false;
    const uint8_t m = kit->pads[pad].playbackMode;
    st().kit.pad[pad].mode =
        static_cast<PadMode>(m <= static_cast<uint8_t>(PadMode::Loop) ? m : 0);
    return true;
}

int native_sampler_free_slots() {
    const int used = static_cast<int>(st().store.stats().slots);
    const int left = static_cast<int>(SampleStore::kMaxSlots) - used;
    return left > 0 ? left : 0;
}

bool native_sampler_describe(const char* path, uint32_t* frames, uint8_t* channels,
                             uint32_t* rate, uint8_t* bits) {
    const std::string fs = native_sampler_fs_path(path);
    if (fs.empty()) return false;
    State& s = st();
    {
        std::unique_lock<std::recursive_mutex> lk(s.loader, std::try_to_lock);
        if (lk.owns_lock()) {
            for (uint16_t i = 0; i < SampleStore::kMaxSlots; ++i) {
                const SampleStore::Slot* sl = s.store.slot(i);
                if (!sl || fs != sl->path || sl->desc.frames == 0) continue;
                if (frames)   *frames = sl->desc.frames;
                if (channels) *channels = sl->desc.channels;
                if (rate)     *rate = sl->desc.sampleRate;
                if (bits)     *bits = 16;
                return true;
            }
        }
    }
    PcSampleSource::Entry e;
    if (!PcSampleSource::parse(fs.c_str(), e)) return false;
    if (frames)   *frames = e.frames;
    if (channels) *channels = e.channels;
    if (rate)     *rate = e.rate;
    if (bits)     *bits = 16;
    return true;
}

bool native_sampler_waveform(const char* path, int16_t* out, uint32_t count, uint32_t start,
                             uint32_t end) {
    if (!out || count == 0) return false;
    const std::string fs = native_sampler_fs_path(path);
    PcSampleSource::Entry e;
    if (fs.empty() || !PcSampleSource::parse(fs.c_str(), e)) return false;
    if (end == 0 || end > e.frames) end = e.frames;
    if (start >= end) start = 0;
    const uint32_t range = end - start;
    if (range == 0) return false;

    /* Peak per bin: an averaged bin makes a transient look like a ramp. */
    std::vector<int16_t> tmp(static_cast<size_t>(kWaveChunkFrames) * e.channels);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t b0 = start + static_cast<uint32_t>((static_cast<uint64_t>(i) * range) / count);
        uint32_t b1 = start + static_cast<uint32_t>((static_cast<uint64_t>(i + 1) * range) / count);
        if (b1 > end) b1 = end;
        int16_t pk = 0;
        for (uint32_t f = b0; f < b1;) {
            const uint32_t want = (b1 - f) < kWaveChunkFrames ? (b1 - f) : kWaveChunkFrames;
            const uint32_t got = PcSampleSource::readEntry(fs.c_str(), e, f, want, tmp.data());
            if (got == 0) break;
            for (uint32_t k = 0; k < got; ++k) {
                const int16_t v = tmp[static_cast<size_t>(k) * e.channels];
                if (std::abs(static_cast<int>(v)) > std::abs(static_cast<int>(pk))) pk = v;
            }
            f += got;
        }
        out[i] = pk;
    }
    return true;
}

bool native_sampler_preview(const char* path, uint32_t start, uint32_t end, int volume, int pan) {
    ensure_started();
    SamplerEngine& e = st().node.engine;
    if (!path || !path[0]) { e.previewStop(); return true; }
    if (end == UINT32_MAX) end = 0;
    const std::string fs = native_sampler_fs_path(path);
    return e.previewStart(fs.c_str(), start, end,
                          SamplerKit::volumeToGain(volume > 0 ? volume : kUnityVolume),
                          SamplerKit::panToBalance(pan + kPanCentre));
}

void native_sampler_scope_enable(bool on) {
    crosspad::ScopeTap& scope = st().node.scope;
    if (!on) { scope.setEnabled(false); return; }
    if (scope.enabled()) return;
    scope.clear();
    scope.setEnabled(true);
}

uint32_t native_sampler_scope_snapshot(float* out, uint32_t maxSamples, uint32_t* sampleRate) {
    crosspad::ScopeTap& scope = st().node.scope;
    if (sampleRate) *sampleRate = scope.sampleRate();
    return scope.snapshot(out, maxSamples);
}

NativeSamplerStats native_sampler_stats() {
    State& s = st();
    NativeSamplerStats r;
    r.peak = s.node.peak.exchange(0.0f, std::memory_order_relaxed);
    r.renderLoad = s.node.renderLoad.exchange(0.0f, std::memory_order_relaxed);
    r.freeSlots = native_sampler_free_slots();
    r.activeVoices = s.node.engine.voicesActive();
    const auto c = s.node.engine.counters();
    r.underruns = c.underruns;
    r.droppedNotes = c.notesDropped;
    return r;
}

NativeSamplerLoops native_sampler_loops() {
    State& s = st();
    NativeSamplerLoops r;
    for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
        char c = '.';
        if (s.node.active.load(std::memory_order_acquire)) {
            switch (s.node.engine.loopState(p)) {
            case SamplerEngine::LoopState::Queued:  c = 'q'; break;
            case SamplerEngine::LoopState::Playing: c = 'P'; break;
            case SamplerEngine::LoopState::Muted:   c = 'm'; break;
            case SamplerEngine::LoopState::Idle:    break;
            }
        }
        r.states[p] = c;
    }
    const auto c = s.node.engine.counters();
    r.queued = c.loopsQueued;
    r.launched = c.loopsLaunched;
    r.clockRunning = bar_clock(nullptr, r.framesToBar);
    return r;
}

} // namespace crosspad_pc
