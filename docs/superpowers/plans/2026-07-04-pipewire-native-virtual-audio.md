# Native PipeWire Virtual Audio + Session Orchestration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the pactl/libpulse-simple virtual-audio path with a native libpipewire client (virtual sinks + virtual source in-process), add automatic default-sink takeover on startup / restore on shutdown, fix RT-safety bugs found in review, and document the plugin (LV2/VST/CLAP) roadmap.

**Architecture:** One `pw_thread_loop` + `pw_core` (PwContext singleton). Virtual sinks are `pw_stream`s with `media.class=Audio/Sink` implementing `crosspad::IAudioInput` (ring-buffer decoupled). A virtual source (`Audio/Source/Virtual`) implements `crosspad::IAudioStream` and is fed as an aux tap from `PcAudioModule::processMixer`. Default-sink orchestration goes through the PipeWire `default` metadata object (`default.configured.audio.sink`), with crash-safe restore persisted in DevicePreferences. RtAudio remains the output (A-bus) backend and the non-Linux fallback; the old pactl path remains a runtime fallback.

**Tech Stack:** C++17, libpipewire-0.3 (1.0.5), Catch2, CMake+Ninja, existing crosspad-core audio graph.

## Global Constraints

- Build/test through MCP tools: `crosspad_build platform=pc`, `crosspad_test_run` (they handle env). Baseline: 58 test cases / 3098 assertions green.
- PipeWire dev headers: prefer `pkg-config libpipewire-0.3`; fallback prefix `~/.cache/crosspad/pw-dev/usr` (extracted 1.0.5 debs) + link `/usr/lib/x86_64-linux-gnu/libpipewire-0.3.so.0` directly. Never require sudo.
- All new PipeWire code: Linux-only (`#ifdef __linux__` in sources is NOT enough — gate at CMake level; files may still carry the guard for safety), behind CMake option `USE_PIPEWIRE` (default ON on Linux).
- RT contract (crosspad-core `IAudioNode` doc): no heap alloc / locks / syscalls in any audio-thread callback (`onProcess`, `read`, `pushSamples` hot paths). Ring buffers + atomics only.
- Node names stay `crosspad_vin1` / `crosspad_vin2` (the `crosspad_vin*` prefix is load-bearing: `enumeratePulseSinks` filters it out of the OUT dropdown; `cleanupStaleSinks` in the pactl fallback matches it). New source node: `crosspad_out`.
- Sample rates: unified fallback is **48000** everywhere (this plan removes the last 44100).
- Each task ends with: build green + full test suite green + commit (message ends with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`).
- Test tag for daemon-dependent tests: `[pipewire]`, each test starts with `if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }`. Tests must NOT mutate the user's default sink unless env `CROSSPAD_PW_TEST_MUTATE=1`.

---

### Task 0: Commit the in-flight dynamic-mixer migration

The working tree already holds a coherent, green migration (dynamic `addChannel()` API in tests + `PcAudioModule::processMixer` render signature + submodule pointer bumps for crosspad-core `b6ecfba` and crosspad-gui). It is the base for everything below.

**Files:**
- Commit as-is: `apps.json`, `lib/crosspad-core` (gitlink), `lib/crosspad-gui` (gitlink), `src/audio/PcAudioModule.cpp`, `src/crosspad_app.cpp`, `tests/audio/audio_test_stubs.hpp`, `tests/audio/test_AudioMixerEngine.cpp`, `tests/audio/test_MixerGoldens.cpp`, `tests/audio/test_PcAudioModule.cpp`

- [ ] **Step 1: Verify tests green** — `crosspad_test_run` → expect 58 cases / 3098 assertions, 0 failed.
- [ ] **Step 2: Check crosspad-gui submodule state** — `git -C lib/crosspad-gui status`; if dirty (status shows lowercase `m`), inspect with `git -C lib/crosspad-gui diff` — if it's unrelated noise, leave it out of the commit (do NOT `git add lib/crosspad-gui`).
- [ ] **Step 3: Commit**

```bash
git add apps.json lib/crosspad-core src/audio/PcAudioModule.cpp src/crosspad_app.cpp tests/audio/
git commit -m "feat(mixer): migrate PC bootstrap + tests to dynamic addChannel API

Bump crosspad-core to b6ecfba (IAudioNode RT contract + onPrepare,
AudioInputNode + conversion cache). Mixer channels registered IN1,IN2,SYNTH
to keep legacy enum indices valid; render() through multi-bus API.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 1: RT-safety fixes in MlPianoSynth

**Files:**
- Modify: `src/synth/MlPianoSynth.cpp` (both `process()` overloads, `init()`)
- Modify: `src/synth/MlPianoSynth.hpp` (only if monoBuf_ prealloc constant needs a home)

**Interfaces:** unchanged (`ISynthEngine`).

Problems (from review): (a) on `try_lock` failure the previous buffer is re-emitted — comment claims silence; produces tonal artifact; (b) `monoBuf_.resize(frames)` can allocate on the audio thread when frames > 512.

- [ ] **Step 1: In `init()` (or `begin`, wherever `monoBuf_` is first sized), pre-allocate for the maximum settings frameCount:**

```cpp
// AudioEngineSettings allows frameCount up to 512; PcAudioModule floors at 128.
// Pre-size for 2048 so process() never allocates on the audio thread even if
// limits grow.
monoBuf_.assign(2048, 0.0f);
```

- [ ] **Step 2: Fix both `process()` overloads** — replace the lock section in `process(int16_t*, ...)` (MlPianoSynth.cpp:140-146) and in `process(float*, ...)` (MlPianoSynth.cpp:178-181) with:

```cpp
    // Guard against audio-thread allocation: monoBuf_ is pre-sized in init().
    // If a larger request ever arrives, emit silence instead of resizing here.
    if (monoBuf_.size() < frames) {
        std::memset(stereoOut, 0, frames * 2 * sizeof(*stereoOut));
        return;
    }

    // try_lock: never block the audio thread on MIDI-callback param changes.
    if (mutex_.try_lock()) {
        FmSynth_Process(nullptr, monoBuf_.data(), static_cast<int>(frames));
        mutex_.unlock();
    } else {
        // Couldn't render this cycle — output true silence, not the stale
        // previous buffer (repeating a chunk is an audible tonal artifact).
        std::fill(monoBuf_.begin(), monoBuf_.begin() + frames, 0.0f);
    }
```

(Keep the existing conversion loops below unchanged; delete the old `if (monoBuf_.size() < frames) monoBuf_.resize(frames);` lines and the stale-data comment.)

- [ ] **Step 3: Build + full tests** — `crosspad_build platform=pc`, `crosspad_test_run` → all green.
- [ ] **Step 4: Commit** — `fix(synth): silence instead of stale buffer on lock miss; no audio-thread alloc`

---

### Task 2: Sample-rate fallback unification + overflow logging

**Files:**
- Modify: `src/crosspad_app.cpp:679` (44100 → 48000)
- Modify: `src/audio/PcAudioModule.cpp` (`processMixer` push loop)
- Modify: `src/audio/PcAudioModule.hpp` (drop counter member)

- [ ] **Step 1:** `src/crosspad_app.cpp:679`: `uint32_t outSampleRate = pcAudio.isOpen() ? pcAudio.getSampleRate() : 48000;` (comment: keep in sync with mixer/module fallback — one constant, no resampler exists).
- [ ] **Step 2:** In `PcAudioModule::processMixer`, capture `pushSamples` return and rate-limit-log drops:

```cpp
uint32_t pushed = stream.pushSamples(pushScratchInt16_[i].data(),
                                     crosspad::AudioFormat::Int16, frames);
if (pushed < frames) {
    // Ring overflow — we are producing faster than RtAudio drains. Log at
    // most once a second so sustained overrun is visible but not spammy.
    if (++overflowCount_ % overflowLogEvery_ == 1) {
        printf("[PcAudioModule] OUT%u overflow: dropped %u frames (total events %u)\n",
               unsigned(i + 1), frames - pushed, overflowCount_);
    }
}
```

with members in the hpp: `uint32_t overflowCount_ = 0; uint32_t overflowLogEvery_ = 400;` (400 cycles ≈ 1 s at 128/48000). Note: printf on the module's own paced thread is acceptable here (it is not the RtAudio RT callback), matches existing logging style in this file.

- [ ] **Step 3:** Build + tests green. Commit — `fix(audio): unify 48k SR fallback; log ring overflow drops`

---

### Task 3: CMake PipeWire detection + PwContext skeleton

**Files:**
- Modify: `CMakeLists.txt` (option near USE_VIRTUAL_AUDIO; detection block after RtAudio; sources block in the `if(USE_AUDIO)` section)
- Create: `src/audio/pipewire/PwContext.hpp`, `src/audio/pipewire/PwContext.cpp`
- Test: `tests/audio/test_PwVirtualAudio.cpp` (+ register in the tests build — find where `tests/audio/*.cpp` get added; if GLOB, nothing to do)

**Interfaces (Produces):**
```cpp
namespace crosspad_pc {
class PwContext {
public:
    static PwContext& instance();
    bool init();                 // idempotent; false if headers/daemon unavailable
    void shutdown();             // safe to call multiple times
    bool isConnected() const;
    struct pw_core* core() const { return core_; }
    struct pw_thread_loop* loop() const { return loop_; }
    class Lock {                 // RAII pw_thread_loop_lock
    public:
        explicit Lock(PwContext& c);
        ~Lock();
    private: PwContext& c_;
    };
};
} // namespace crosspad_pc
```

- [ ] **Step 1: CMake option + detection.** After the RtAudio block (~CMakeLists.txt:129):

```cmake
# Native PipeWire backend for virtual audio (Linux). Falls back to the pactl
# null-sink path at runtime when unavailable.
option(USE_PIPEWIRE "Native PipeWire virtual sinks/source (Linux)" ON)
set(CROSSPAD_PIPEWIRE_OK FALSE)
if(USE_PIPEWIRE AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND USE_AUDIO AND USE_VIRTUAL_AUDIO)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE libpipewire-0.3)
    endif()
    if(PIPEWIRE_FOUND)
        set(CROSSPAD_PW_INCLUDE_DIRS ${PIPEWIRE_INCLUDE_DIRS})
        set(CROSSPAD_PW_LIBS ${PIPEWIRE_LINK_LIBRARIES})
        set(CROSSPAD_PIPEWIRE_OK TRUE)
    else()
        # Dev headers extracted without root (apt-get download + dpkg -x).
        set(_PW_LOCAL "$ENV{HOME}/.cache/crosspad/pw-dev/usr")
        set(_PW_SO "/usr/lib/x86_64-linux-gnu/libpipewire-0.3.so.0")
        if(EXISTS "${_PW_LOCAL}/include/pipewire-0.3/pipewire/pipewire.h" AND EXISTS "${_PW_SO}")
            set(CROSSPAD_PW_INCLUDE_DIRS
                "${_PW_LOCAL}/include/pipewire-0.3"
                "${_PW_LOCAL}/include/spa-0.2")
            set(CROSSPAD_PW_LIBS "${_PW_SO}")
            set(CROSSPAD_PIPEWIRE_OK TRUE)
            message(STATUS "PipeWire: using local header prefix ${_PW_LOCAL}")
        else()
            message(STATUS "PipeWire dev headers not found — native virtual audio disabled (install libpipewire-0.3-dev)")
        endif()
    endif()
endif()
```

- [ ] **Step 2: Source wiring.** Inside the existing `if(USE_VIRTUAL_AUDIO)` block (CMakeLists.txt:252-266) add:

```cmake
        if(CROSSPAD_PIPEWIRE_OK)
            list(APPEND MAIN_SOURCES
                src/audio/pipewire/PwContext.cpp
            )
            list(APPEND MAIN_LIBS ${CROSSPAD_PW_LIBS})
            add_compile_definitions(USE_PIPEWIRE=1)
            # Headers are C99-style; include dirs needed by any TU that includes ours
            include_directories(${CROSSPAD_PW_INCLUDE_DIRS})
        endif()
```

(Later tasks append their .cpp files to this same list.)

- [ ] **Step 3: PwContext implementation.** `src/audio/pipewire/PwContext.cpp`:

```cpp
// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwContext.hpp"
#include <pipewire/pipewire.h>
#include <cstdio>

namespace crosspad_pc {

PwContext& PwContext::instance() { static PwContext ctx; return ctx; }

PwContext::Lock::Lock(PwContext& c) : c_(c) { pw_thread_loop_lock(c_.loop_); }
PwContext::Lock::~Lock() { pw_thread_loop_unlock(c_.loop_); }

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
        shutdown();
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
}

bool PwContext::isConnected() const { return core_ != nullptr; }

} // namespace crosspad_pc
#endif // __linux__
```

Header holds the class from **Interfaces** plus private members `struct pw_thread_loop* loop_ = nullptr; struct pw_context* context_ = nullptr; struct pw_core* core_ = nullptr; bool initialized_ = false;` and forward declarations (`struct pw_core; struct pw_thread_loop; struct pw_context;`) — do NOT include pipewire headers in the .hpp (keeps non-PW TUs clean).

- [ ] **Step 4: Failing test first.** `tests/audio/test_PwVirtualAudio.cpp`:

```cpp
// Integration tests for the native PipeWire virtual audio backend.
// Skipped gracefully when no PipeWire daemon is reachable (CI, containers).
#if defined(__linux__) && defined(USE_PIPEWIRE)
#include <catch2/catch_test_macros.hpp>
#include "audio/pipewire/PwContext.hpp"

static bool pwTestAvailable() {
    return crosspad_pc::PwContext::instance().init();
}

TEST_CASE("PwContext connects to the daemon or degrades cleanly", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    REQUIRE(crosspad_pc::PwContext::instance().isConnected());
}
#endif
```

Check how `tests/` builds (look at the tests CMake wiring; add `test_PwVirtualAudio.cpp` + `src/audio/pipewire/PwContext.cpp` + include dirs + `${CROSSPAD_PW_LIBS}` to the test target under the same `CROSSPAD_PIPEWIRE_OK` condition).

- [ ] **Step 5:** Reconfigure + build + run: `crosspad_build platform=pc mode=reconfigure`, then `crosspad_test_run filter=[pipewire]` → 1 case passes (connected on this machine). Full suite green.
- [ ] **Step 6: Commit** — `feat(audio): PipeWire context skeleton + no-root header detection`

---

### Task 4: Native virtual sinks (PwVirtualSinkCapture + manager + factory fallback)

**Files:**
- Create: `src/audio/pipewire/PwVirtualSinkCapture.hpp/.cpp`
- Create: `src/audio/pipewire/PwVirtualSinkManager.hpp/.cpp`
- Modify: `src/audio/virtual/IVirtualSinkManager.hpp` (add `input()` accessor with default)
- Modify: `src/audio/virtual/VirtualSinkFactory.cpp` (prefer native, fallback pactl)
- Modify: `CMakeLists.txt` (append the two new .cpp)
- Test: extend `tests/audio/test_PwVirtualAudio.cpp`

**Interfaces (Produces):**
```cpp
// IVirtualSinkManager gains:
/// Native backends expose the sink directly as an IAudioInput (no monitor
/// device round-trip). Returns nullptr for pactl/RtAudio-based backends —
/// callers must then fall back to captureDeviceName matching.
virtual crosspad::IAudioInput* input(uint32_t /*index*/) { return nullptr; }

// PwVirtualSinkCapture : public crosspad::IAudioInput
bool start(const char* nodeName, const char* description,
           uint32_t sampleRate, uint32_t bufferFrames = 256);
void stop();
bool isOpen() const;
// + IAudioInput: read(int16_t*, frames), getSampleRate, getBufferSize, getInputLevel
```

Key implementation points for `PwVirtualSinkCapture.cpp` (complete file to write):

```cpp
// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwVirtualSinkCapture.hpp"
#include "PwContext.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <cstdio>
#include <cstring>

namespace crosspad_pc {

void PwVirtualSinkCapture::onProcess(void* userdata)
{
    // RT thread — ring buffer + atomics only.
    auto* self = static_cast<PwVirtualSinkCapture*>(userdata);
    struct pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) return;
    struct spa_data& d = b->buffer->datas[0];
    if (d.data && d.chunk && d.chunk->size > 0) {
        const auto* samples = reinterpret_cast<const int16_t*>(
            static_cast<uint8_t*>(d.data) + d.chunk->offset);
        const uint32_t nSamples = d.chunk->size / sizeof(int16_t); // interleaved L,R
        self->ring_.write(samples, nSamples);
        int16_t pl = 0, pr = 0;
        for (uint32_t i = 0; i + 1 < nSamples; i += 2) {
            int16_t l = samples[i], r = samples[i + 1];
            if (l == INT16_MIN) l = INT16_MAX; if (l < 0) l = -l;
            if (r == INT16_MIN) r = INT16_MAX; if (r < 0) r = -r;
            if (l > pl) pl = l;
            if (r > pr) pr = r;
        }
        self->peakL_.store(pl, std::memory_order_relaxed);
        self->peakR_.store(pr, std::memory_order_relaxed);
    }
    pw_stream_queue_buffer(self->stream_, b);
}

void PwVirtualSinkCapture::onStateChanged(void* userdata, enum pw_stream_state,
                                          enum pw_stream_state state, const char* error)
{
    auto* self = static_cast<PwVirtualSinkCapture*>(userdata);
    if (error) printf("[PwSink] %s stream error: %s\n", self->nodeName_.c_str(), error);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING ||
        state == PW_STREAM_STATE_ERROR)
        pw_thread_loop_signal(PwContext::instance().loop(), false);
}

static const pw_stream_events kSinkEvents = {
    PW_VERSION_STREAM_EVENTS,
    /*destroy*/ nullptr, PwVirtualSinkCapture::onStateChangedTramp, /*control_info*/ nullptr,
    /*io_changed*/ nullptr, /*param_changed*/ nullptr, /*add_buffer*/ nullptr,
    /*remove_buffer*/ nullptr, PwVirtualSinkCapture::onProcessTramp,
    /*drained*/ nullptr, /*command*/ nullptr, /*trigger_done*/ nullptr,
};
// (Use designated-init style instead if the compiler accepts it in C++:
//  static pw_stream_events kSinkEvents{}; then assign fields in start(). That
//  is the safer, version-proof pattern — prefer it.)

bool PwVirtualSinkCapture::start(const char* nodeName, const char* description,
                                 uint32_t sampleRate, uint32_t bufferFrames)
{
    auto& ctx = PwContext::instance();
    if (!ctx.init()) return false;

    sampleRate_ = sampleRate;
    bufferFrames_ = bufferFrames;
    nodeName_ = nodeName;
    ring_.resize(bufferFrames * 2 * 32);   // 32 stereo buffers of headroom

    char latency[32];
    snprintf(latency, sizeof latency, "%u/%u", bufferFrames, sampleRate);

    PwContext::Lock lock(ctx);
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_CLASS,       "Audio/Sink",
        PW_KEY_NODE_NAME,         nodeName,
        PW_KEY_NODE_DESCRIPTION,  description,
        PW_KEY_NODE_NICK,         description,
        PW_KEY_NODE_VIRTUAL,      "true",
        PW_KEY_NODE_LATENCY,      latency,
        PW_KEY_AUDIO_CHANNELS,    "2",
        SPA_KEY_AUDIO_POSITION,   "FL,FR",
        nullptr);

    stream_ = pw_stream_new(ctx.core(), description, props);
    if (!stream_) return false;

    spa_zero(events_);
    events_.version = PW_VERSION_STREAM_EVENTS;
    events_.process = &PwVirtualSinkCapture::onProcess;         // static, void(void*)
    events_.state_changed = &PwVirtualSinkCapture::onStateChanged;
    pw_stream_add_listener(stream_, &listener_, &events_, this);

    uint8_t podBuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuf, sizeof podBuf);
    struct spa_audio_info_raw info = {};
    info.format   = SPA_AUDIO_FORMAT_S16;   // adapter converts/resamples for us
    info.rate     = sampleRate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const struct spa_pod* params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };

    int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) { stopLocked(); return false; }

    // Wait (bounded) until the node reaches PAUSED/STREAMING — PAUSED is fine:
    // a sink idles until an app links to it.
    struct timespec abstime;
    pw_thread_loop_get_time(ctx.loop(), &abstime, 2 * SPA_NSEC_PER_SEC);
    while (true) {
        enum pw_stream_state st = pw_stream_get_state(stream_, nullptr);
        if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) break;
        if (st == PW_STREAM_STATE_ERROR) { stopLocked(); return false; }
        if (pw_thread_loop_timed_wait_full(ctx.loop(), &abstime) < 0) break; // timeout
    }
    open_.store(true, std::memory_order_release);
    printf("[PwSink] '%s' up (%s, S16/%u/2ch)\n", nodeName, description, sampleRate);
    return true;
}
```

`read()` = drain `ring_` frame-aligned exactly like `PulseMonitorCapture::read` (copy that logic; zero-copy not needed). `stop()` takes the context lock, disconnects+destroys the stream, `spa_hook_remove(&listener_)`, `open_=false`. `stopLocked()` is the lock-free-precondition variant used inside `start()`. Members: `pw_stream* stream_`, `spa_hook listener_`, `pw_stream_events events_`, `AudioRingBuffer<int16_t> ring_`, `std::atomic<bool> open_`, peaks, `std::string nodeName_`.
**Header include hygiene:** the .hpp forward-declares only (`struct pw_stream;`) and stores `events_` via `#include <pipewire/stream.h>`… that leaks. Instead: keep `events_` in the .cpp as a per-instance member is required — so the .hpp includes `<pipewire/stream.h>` guarded by `#ifdef __linux__`; acceptable since only PW-gated TUs include this header.

`PwVirtualSinkManager` (implements `IVirtualSinkManager`):
- `setup(n)`: `PwContext::init()`; for i<n (max 2): `caps_[i].start(("crosspad_vin"+std::to_string(i+1)).c_str(), i==0?"CrossPad IN#1":"CrossPad IN#2", 48000)`; success if ≥1 started.
- `teardown()`: stop all (idempotent).
- `list()`: `{displayName=description, captureDeviceName=nodeName+".monitor", channelCount=2}` (monitor name kept for display/debug only).
- `input(i)`: `caps_[i].isOpen() ? &caps_[i] : nullptr`.
- `isAvailable()`: `PwContext::instance().init()`.
- `errorHint()`: "PipeWire daemon not reachable".

`VirtualSinkFactory.cpp` change:

```cpp
std::unique_ptr<IVirtualSinkManager> makeVirtualSinkManager() {
#if defined(__linux__)
#if defined(USE_PIPEWIRE)
    {
        auto native = std::make_unique<PwVirtualSinkManager>();
        if (native->isAvailable()) return native;
        printf("[VirtSink] native PipeWire unavailable, falling back to pactl\n");
    }
#endif
    return makeLinuxPipewireSinks();   // legacy pactl null-sink path
#else
    return std::make_unique<NullSinkManager>();
#endif
}
```

- [ ] **Step 1: Write failing tests** (append to `test_PwVirtualAudio.cpp`):

```cpp
TEST_CASE("PwVirtualSinkCapture exposes an OS sink and reads silence when idle", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    crosspad_pc::PwVirtualSinkCapture cap;
    REQUIRE(cap.start("crosspad_test_vin", "CrossPad TEST sink", 48000));
    REQUIRE(cap.isOpen());
    int16_t buf[256 * 2];
    // No app is linked to the test sink — read must not block and returns 0..n.
    uint32_t got = cap.read(buf, 256);
    REQUIRE(got <= 256);
    cap.stop();
    REQUIRE_FALSE(cap.isOpen());
}

TEST_CASE("PwVirtualSinkManager fills IVirtualSinkManager contract", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    crosspad_pc::PwVirtualSinkManager mgr;
    REQUIRE(mgr.setup(2));
    REQUIRE(mgr.list().size() == 2);
    REQUIRE(mgr.input(0) != nullptr);
    mgr.teardown();          // idempotent
    mgr.teardown();
    REQUIRE(mgr.input(0) == nullptr);
}
```

- [ ] **Step 2:** Run `[pipewire]` → FAIL (missing symbols). Implement the three files + factory change + CMake append.
- [ ] **Step 3:** `crosspad_test_run filter=[pipewire]` → green. Verify node visibility manually: `pw-cli ls Node | grep -A2 crosspad` while the test binary sleeps is impractical — instead trust the state==PAUSED assertion, and do a live check in Task 7's verification.
- [ ] **Step 4:** Full suite + build green. Commit — `feat(audio): native PipeWire virtual sinks (pw_stream Audio/Sink) with pactl fallback`

---

### Task 5: Default-sink takeover/restore (PwDefaultSinkGuard)

**Files:**
- Create: `src/audio/pipewire/PwDefaultSinkGuard.hpp/.cpp`
- Modify: `CMakeLists.txt` (append .cpp)
- Test: extend `tests/audio/test_PwVirtualAudio.cpp`

**Interfaces (Produces):**
```cpp
class PwDefaultSinkGuard {
public:
    /// Read current default sink name from the "default" metadata (blocking,
    /// bounded). Empty string when metadata/default missing.
    std::string queryCurrentDefault();
    /// Save current default (unless overridden via setPreviousSink) and make
    /// `sinkNodeName` the configured default. Returns false when metadata
    /// object not found (e.g. WirePlumber absent).
    bool takeover(const std::string& sinkNodeName);
    /// Restore the saved default; clears the configured key when none saved.
    /// Idempotent.
    void restore();
    bool active() const;
    const std::string& previousSink() const;
    /// Crash recovery: inject the value persisted in DevicePreferences before
    /// calling takeover() so a crashed previous run's sink is what we restore.
    void setPreviousSink(const std::string& name);
};
```

Implementation core (`PwDefaultSinkGuard.cpp`): bind registry, find the metadata global with `metadata.name == "default"`, bind `pw_metadata`, listen for `property` events to capture `default.audio.sink` (value is JSON `{"name":"..."}` — extract with a small string scan, no JSON lib), roundtrip with `pw_core_sync` + `done` event + `pw_thread_loop_wait`. Set via:

```cpp
pw_metadata_set_property(meta_, 0 /* PW_ID_CORE subject */,
    "default.configured.audio.sink", "Spa:String:JSON", json.c_str());
```

where `json = "{\"name\":\"" + sinkNodeName + "\"}"`. `restore()`: if `previous_` non-empty → set the same key to the previous name; else `pw_metadata_set_property(meta_, 0, "default.configured.audio.sink", nullptr, nullptr)` (clears — WirePlumber re-picks best). Keep the metadata proxy bound between takeover and restore (guard owns `pw_proxy*`); release in destructor/`restore()`.
JSON name extraction helper (also used by tests):

```cpp
// "{"name":"alsa_output.foo"}" → alsa_output.foo ; tolerant of spaces.
std::string pwExtractJsonName(const char* value);
```

- [ ] **Step 1: Failing tests:**

```cpp
TEST_CASE("pwExtractJsonName parses metadata values", "[pipewire][unit]") {
    using crosspad_pc::pwExtractJsonName;
    CHECK(pwExtractJsonName("{\"name\":\"alsa_output.pci.analog-stereo\"}")
          == "alsa_output.pci.analog-stereo");
    CHECK(pwExtractJsonName("{ \"name\" : \"x\" }") == "x");
    CHECK(pwExtractJsonName("garbage") == "");
    CHECK(pwExtractJsonName(nullptr) == "");
}

TEST_CASE("PwDefaultSinkGuard reads current default without mutating", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    crosspad_pc::PwDefaultSinkGuard guard;
    std::string cur = guard.queryCurrentDefault();
    SUCCEED("current default: " + (cur.empty() ? "<none>" : cur));
}

TEST_CASE("PwDefaultSinkGuard takeover+restore roundtrip", "[pipewire][.mutate]") {
    // Hidden test (leading '.') — only run explicitly; mutates user session.
    if (!pwTestAvailable() || !getenv("CROSSPAD_PW_TEST_MUTATE")) {
        SUCCEED("skipped (set CROSSPAD_PW_TEST_MUTATE=1)"); return;
    }
    crosspad_pc::PwVirtualSinkCapture cap;
    REQUIRE(cap.start("crosspad_test_default", "CrossPad TEST default", 48000));
    crosspad_pc::PwDefaultSinkGuard guard;
    std::string before = guard.queryCurrentDefault();
    REQUIRE(guard.takeover("crosspad_test_default"));
    REQUIRE(guard.previousSink() == before);
    guard.restore();
    REQUIRE(guard.queryCurrentDefault() == before);
    cap.stop();
}
```

- [ ] **Step 2:** Implement; `crosspad_test_run filter=[pipewire]` green (mutate test hidden by default).
- [ ] **Step 3:** Full suite + commit — `feat(audio): default-sink takeover/restore via PipeWire metadata`

---

### Task 6: Virtual source "CrossPad Out" (B-bus) + PcAudioModule aux tap

**Files:**
- Create: `src/audio/pipewire/PwVirtualSource.hpp/.cpp`
- Modify: `src/audio/PcAudioModule.hpp` (aux stream hook), `src/audio/PcAudioModule.cpp` (`processMixer` tap)
- Modify: `CMakeLists.txt` (append .cpp)
- Test: extend `tests/audio/test_PwVirtualAudio.cpp` + a non-PW unit test for the aux tap

**Interfaces (Produces):**
```cpp
// PwVirtualSource : public crosspad::IAudioStream  (Audio/Source/Virtual node;
// CrossPad shows up as a "microphone" in OBS/DAWs)
bool start(const char* nodeName, const char* description, uint32_t sampleRate,
           uint32_t bufferFrames = 256);
void stop();
// IAudioStream: supportedFormats()==Int16, pushSamples() writes the ring
// (called from PcAudioModule's paced thread), isOpen(), getSampleRate().

// PcAudioModule gains:
/// Optional post-master tap of OUT1: every processMixer cycle the int16
/// conversion of bus 0 is also pushed here. Pass nullptr to detach.
void setAuxStream(crosspad::IAudioStream* aux) { aux_ = aux; }
```

`PwVirtualSource::onProcess` (RT, direction OUTPUT — the graph *pulls* from us):

```cpp
auto* self = static_cast<PwVirtualSource*>(userdata);
struct pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
if (!b) return;
struct spa_data& d = b->buffer->datas[0];
const uint32_t stride = 2 * sizeof(int16_t);
uint32_t want = d.maxsize / stride;
if (b->requested) want = std::min<uint32_t>(want, (uint32_t)b->requested);
auto* out = static_cast<int16_t*>(d.data);
uint32_t got = self->ring_.read(out, want * 2) / 2;   // samples→frames
if (got < want)                                        // underrun → silence tail
    std::memset(out + got * 2, 0, (want - got) * stride);
d.chunk->offset = 0;
d.chunk->stride = stride;
d.chunk->size   = want * stride;
pw_stream_queue_buffer(self->stream_, b);
```

Stream connect: `PW_DIRECTION_OUTPUT`, props `media.class=Audio/Source/Virtual`, node name `crosspad_out`, desc "CrossPad Out", same S16/2ch format param, flags `MAP_BUFFERS|RT_PROCESS`.

`PcAudioModule::processMixer` addition (after the existing per-stream push loop; `pushScratchInt16_[0]` already holds OUT1 as int16):

```cpp
    // B-bus tap: mirror OUT1 into the aux stream (PipeWire virtual source).
    if (aux_ && aux_->isOpen())
        aux_->pushSamples(pushScratchInt16_[0].data(),
                          crosspad::AudioFormat::Int16, frames);
```

Edge case: when NO physical stream is open, the int16 conversion of bus 0 must still happen for the aux tap — check `processMixer`'s conversion loop ordering and hoist the bus0→int16 conversion out of the `stream.isOpen()` conditional if it is currently inside.

- [ ] **Step 1: Failing unit test** (no PW needed — use the existing `CapturingInt16Stream` from `tests/audio/audio_test_helpers.hpp` as aux target on a `TestablePcAudioModule` with mixer configured as in `test_PcAudioModule.cpp`): assert aux captures `frames*2` samples per `process()` and equals OUT1 content.
- [ ] **Step 2:** Implement aux hook; unit test green.
- [ ] **Step 3: `[pipewire]` test:** start source, `pushSamples` a ramp, assert `isOpen()`; stop. (Content verification through the graph needs a linked client — out of scope; state + no-crash is the bar.)
- [ ] **Step 4:** Full suite + commit — `feat(audio): CrossPad Out virtual source (B-bus) + OUT1 aux tap`

---

### Task 7: App integration — startup orchestration, shutdown, prefs, UI toggle

**Files:**
- Modify: `src/crosspad_app.cpp` (statics ~:136-147, init ~:683-750, shutdown `crosspad_app_shutdown()` :1447, DevicePreferences struct :156 + its load/save JSON functions)
- Possibly modify: audio settings GUI (locate the screen that owns the OUT dropdown — search `enumeratePulseSinks` callers / `settings` UI sources; if a natural place exists add a switch, else defer to remote settings)

**Behavior spec:**
1. Prefs gain: `bool pwTakeoverDefault = true;` and `std::string pwPrevDefaultSink;` (serialize both in the existing prefs JSON load/save).
2. Init (after `s_virtualSinkManager->setup(2)` succeeded, before input binding):
   - If manager provides native inputs (`input(i) != nullptr`): register them via `pc_platform_set_audio_input(i, ...)`, skip both PulseMonitorCapture and RtAudio fallback for those slots.
   - Else keep the existing PulseMonitorCapture path untouched (pactl fallback still works).
3. Default takeover (only when native PW active AND `pwTakeoverDefault`):

```cpp
#ifdef USE_PIPEWIRE
    if (s_devicePrefs.pwTakeoverDefault && s_virtualSinkManager &&
        s_virtualSinkManager->input(0) != nullptr) {
        // Crash recovery: a leftover pwPrevDefaultSink means the previous run
        // died before restore — that value, not the current default (which is
        // still our own sink), is what we must eventually restore.
        if (!s_devicePrefs.pwPrevDefaultSink.empty())
            s_pwGuard.setPreviousSink(s_devicePrefs.pwPrevDefaultSink);
        if (s_pwGuard.takeover("crosspad_vin1")) {
            s_devicePrefs.pwPrevDefaultSink = s_pwGuard.previousSink();
            saveDevicePrefs();
            printf("[Audio] system default sink -> CrossPad IN#1 (was: %s)\n",
                   s_devicePrefs.pwPrevDefaultSink.c_str());
        }
    }
#endif
```

4. Virtual source start (after `s_audioModule.start()`): `s_pwSource.start("crosspad_out", "CrossPad Out", cfg.sampleRate)` + `s_audioModule.setAuxStream(&s_pwSource)`.
5. Shutdown (`crosspad_app_shutdown`, BEFORE `s_virtualSinkManager->teardown()`):

```cpp
#ifdef USE_PIPEWIRE
    s_pwGuard.restore();
    s_devicePrefs.pwPrevDefaultSink.clear();
    saveDevicePrefs();
    s_audioModule.setAuxStream(nullptr);
    s_pwSource.stop();
#endif
```

then existing teardown; add `crosspad_pc::PwContext::instance().shutdown()` last.
6. UI: locate the audio settings screen. If it has a devices section, add an LVGL switch "System default → CrossPad" bound to `pwTakeoverDefault` with immediate `takeover()/restore()` on toggle + `saveDevicePrefs()`. If no obvious host screen exists, skip UI this round and note it in the docs task (the pref is hand-editable + the behavior is on by default).

- [ ] **Step 1:** Prefs fields + JSON round-trip (find existing prefs (de)serialization; mirror style).
- [ ] **Step 2:** Init/shutdown wiring per spec.
- [ ] **Step 3:** Build; **live verification** (this is the day's money shot):
  - `crosspad_run`, wait 3 s, then from a shell: `pw-cli ls Node | grep -B2 -A4 crosspad` → expect `crosspad_vin1`, `crosspad_vin2`, `crosspad_out` nodes.
  - `wpctl status | head -40` (or `pw-metadata 0 | grep default.configured`) → default sink is `crosspad_vin1`.
  - Play anything (e.g. `pw-play /usr/share/sounds/alsa/Front_Center.wav` with no target → default) → check IN1 VU lights via `crosspad_screenshot`.
  - `crosspad_kill` (graceful) → `pw-metadata 0 | grep default.configured` → previous sink restored; `pw-cli ls Node | grep crosspad` → empty.
  - If the sandboxed shell cannot reach the user session's PipeWire socket, run the checks through `crosspad_log` output + ask-free fallback: `XDG_RUNTIME_DIR=/run/user/$(id -u) pw-cli ...`.
- [ ] **Step 4:** Full suite + commit — `feat(audio): PipeWire session orchestration — default-sink takeover, native inputs, B-bus source`

---

### Task 8: Docs + final review

**Files:**
- Modify: `docs/virtual-audio.md` (architecture: native pw_stream design, fallback matrix Linux-native/Linux-pactl/Win-loopback-planned, orchestration semantics, crash recovery)
- Create: `docs/audio-plugins.md` (plugin roadmap: Tier 1 = IAudioNode DSP portable to ESP; Tier 2 = `pw_context_load_module("libpipewire-module-filter-chain", ...)` for LV2/LADSPA inserts; Tier 3 = insert patch-points + external host (Carla via pipewire-jack) for VST2/3; Tier 4 = embedded CLAP host (MIT, C API) as the long-term in-process answer; note: VST2 SDK licensing is dead upstream — support arrives via Carla/host, never in-tree)
- Modify: `CLAUDE.md` (Source Layout: add `src/audio/pipewire/`; CMake options table: add `USE_PIPEWIRE`)

- [ ] **Step 1:** Write docs (reference the researched property/latency specifics: `media.class`, `node.latency=256/48000`, metadata `default.configured.audio.sink`).
- [ ] **Step 2:** Run `/code-review`-style self-check on the day's diff: `git diff master...HEAD --stat`, re-read new files for RT violations (alloc/lock/log in `onProcess`/`read`/`pushSamples` hot paths), header hygiene, `#ifdef` symmetry.
- [ ] **Step 3:** Final full build + suite + `[pipewire]` pass. Commit — `docs(audio): native PipeWire architecture + plugin hosting roadmap`

---

## Self-Review notes

- Spec coverage: virtual sinks (T4), source (T6), default takeover+restore incl. crash (T5+T7), UI/UX (T7 step 6, with an honest deferral path), VST future (T8 docs + T6/T4 keep IAudioNode as the portable plugin surface), review fixes (T1-T2), pactl fallback preserved (T4 factory).
- Windows: untouched this round (NullSinkManager path compiles as before; `USE_PIPEWIRE` never defined there). WASAPI loopback is a future plan item documented in T8.
- Type consistency: `IVirtualSinkManager::input(uint32_t)` used in T4 (definition) and T7 (consumption); `setAuxStream(crosspad::IAudioStream*)` defined T6, consumed T7; guard API defined T5, consumed T7.
- Known risk: `pw_stream_events` designated init from C++ — use zero-init + field assignment (noted in T4). `pw_thread_loop_timed_wait_full` availability in 1.0.5 — if absent, use `pw_thread_loop_wait` + manual deadline check.
