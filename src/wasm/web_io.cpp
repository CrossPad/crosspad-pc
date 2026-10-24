/**
 * @file    web_io.cpp
 * @brief   The browser twin's I/O backends: RtMidi over Web MIDI, RtAudio over
 *          WebAudio (pull model), cooperative sleeps, and the service list the
 *          LVGL loop drives in place of the PC's helper threads.
 *
 * The page side lives in simtwin.js (Module.webMidi / Module.webAudio).
 */
#include <emscripten.h>

#include <RtAudio.h>
#include <RtMidi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <pthread.h>
#include <deque>
#include <vector>

#include "FreeRTOS.h"
#include "task.h"

extern "C" BaseType_t xPortWasmInTask(void);

/* ── Page glue ───────────────────────────────────────────────────────── */

EM_JS(int, js_midi_count, (int out), {
    const w = Module.webMidi; if (!w) return 0;
    return (out ? w.outputs() : w.inputs()).length;
});
EM_JS(char*, js_midi_name, (int out, int i), {
    const w = Module.webMidi; const l = w ? (out ? w.outputs() : w.inputs()) : [];
    return stringToNewUTF8(l[i] ? l[i].name : "");
});
EM_JS(void, js_midi_open, (int out, int i, int open), { Module.webMidi && Module.webMidi.open(out, i, !!open); });
EM_JS(void, js_midi_send, (int i, const unsigned char* p, int n), {
    Module.webMidi && Module.webMidi.send(i, HEAPU8.slice(p, p + n));
});
EM_JS(int, js_mic_enabled, (void), { return Module.webAudio && Module.webAudio.micEnabled ? 1 : 0; });

/* ── Services: what the PC ran on helper threads ─────────────────────── */

namespace {
struct Service { void (*fn)(void*); void* ctx; };
std::vector<Service> s_services;

struct Producer { void (*fn)(void*); void* ctx; uint32_t block; double owed; };
std::vector<Producer> s_producers;
}

extern "C" void wasm_service_add(void (*fn)(void*), void* ctx) { s_services.push_back({fn, ctx}); }
extern "C" void wasm_service_remove(void* ctx) {
    s_services.erase(std::remove_if(s_services.begin(), s_services.end(),
                                    [ctx](const Service& s) { return s.ctx == ctx; }), s_services.end());
}
/** An audio producer: fn(ctx) renders one block of @p block frames into the output rings. */
extern "C" void wasm_audio_add_producer(void (*fn)(void*), void* ctx, uint32_t block) {
    s_producers.push_back({fn, ctx, block ? block : 256, 0});
}
extern "C" void wasm_audio_remove_producer(void* ctx) {
    s_producers.erase(std::remove_if(s_producers.begin(), s_producers.end(),
                                     [ctx](const Producer& p) { return p.ctx == ctx; }), s_producers.end());
}

/* ── Cooperative sleeps ──────────────────────────────────────────────── */
// A task fiber that sleeps must hand the CPU to the others (and to the page),
// not spin: std::this_thread::sleep_for / usleep inside a task become
// vTaskDelay. Outside a task (the page calling in) they stay real waits.
extern "C" int __real_nanosleep(const struct timespec* req, struct timespec* rem);
extern "C" int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    if (req && xPortWasmInTask()) {
        const double ms = req->tv_sec * 1000.0 + req->tv_nsec / 1e6;
        vTaskDelay(std::max<TickType_t>(1, (TickType_t)std::ceil(ms / portTICK_PERIOD_MS)));
        if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
        return 0;
    }
    return __real_nanosleep(req, rem);
}
extern "C" int __real_sched_yield(void);
extern "C" int __wrap_sched_yield(void) {
    if (xPortWasmInTask()) { taskYIELD(); return 0; }
    return __real_sched_yield();
}

/* ── std::thread → FreeRTOS tasks ──────────────────────────────────────── */
// libc++ starts a std::thread with pthread_create, which a no-pthreads build
// refuses. The PC code spawns short helpers (MIDI reconnect, jack-panel
// switching, …): each becomes a task fiber, scheduled like any other.
namespace {
struct PThread {
    void* (*fn)(void*);
    void* arg;
    void* ret = nullptr;
    bool done = false;
    bool detached = false;
};
void pthreadEntry(void* p) {
    auto* t = static_cast<PThread*>(p);
    t->ret = t->fn(t->arg);
    t->done = true;
    if (t->detached) delete t;
    vTaskDelete(nullptr);
}
}
extern "C" int __wrap_pthread_create(pthread_t* th, const pthread_attr_t*, void* (*fn)(void*), void* arg) {
    auto* t = new PThread{fn, arg};
    if (xTaskCreate(pthreadEntry, "pthread", 4096, t, 1, nullptr) != pdPASS) { delete t; return EAGAIN; }
    *th = (pthread_t)t;
    return 0;
}
extern "C" int __wrap_pthread_join(pthread_t th, void** ret) {
    auto* t = (PThread*)th;
    if (!t) return EINVAL;
    while (!t->done) {
        if (!xPortWasmInTask()) return EDEADLK;   // the page cannot wait on a task
        vTaskDelay(1);
    }
    if (ret) *ret = t->ret;
    delete t;
    return 0;
}
extern "C" int __wrap_pthread_detach(pthread_t th) {
    auto* t = (PThread*)th;
    if (!t) return EINVAL;
    if (t->done) delete t; else t->detached = true;
    return 0;
}

/* ── RtMidi ──────────────────────────────────────────────────────────── */

namespace {
std::vector<RtMidiIn*> s_midiIns;
struct MidiMsg { int port; double ts; std::vector<unsigned char> data; };
std::deque<MidiMsg> s_midiQueue;
uint8_t s_midiBuf[4096];

std::string takeString(char* p) { std::string s = p ? p : ""; free(p); return s; }

bool ignored(const RtMidiIn* in, const std::vector<unsigned char>& m) {
    if (m.empty()) return true;
    const unsigned char st = m[0];
    if (st == 0xF0 || st == 0xF7) return in->ignSysex_;
    if (st == 0xF1 || st == 0xF8) return in->ignTime_;
    if (st == 0xFE) return in->ignSense_;
    return false;
}
}

RtMidiIn::RtMidiIn(RtMidi::Api, const std::string&, unsigned int) { s_midiIns.push_back(this); }
RtMidiIn::~RtMidiIn() {
    closePort();
    s_midiIns.erase(std::remove(s_midiIns.begin(), s_midiIns.end(), this), s_midiIns.end());
}
unsigned int RtMidiIn::getPortCount() { return (unsigned)js_midi_count(0); }
std::string RtMidiIn::getPortName(unsigned int i) { return takeString(js_midi_name(0, (int)i)); }
void RtMidiIn::openPort(unsigned int i, const std::string&) {
    if (i >= getPortCount()) throw RtMidiError("no such MIDI input", RtMidiError::INVALID_PARAMETER);
    port_ = (int)i;
    js_midi_open(0, port_, 1);
}
void RtMidiIn::closePort() {
    if (port_ < 0) return;
    const int p = port_;
    port_ = -1;
    bool used = false;
    for (auto* in : s_midiIns) used |= in->port_ == p;
    if (!used) js_midi_open(0, p, 0);
}

unsigned int RtMidiOut::getPortCount() { return (unsigned)js_midi_count(1); }
std::string RtMidiOut::getPortName(unsigned int i) { return takeString(js_midi_name(1, (int)i)); }
void RtMidiOut::openPort(unsigned int i, const std::string&) {
    if (i >= getPortCount()) throw RtMidiError("no such MIDI output", RtMidiError::INVALID_PARAMETER);
    port_ = (int)i;
    js_midi_open(1, port_, 1);
}
void RtMidiOut::sendMessage(const unsigned char* m, size_t n) {
    if (port_ >= 0 && m && n) js_midi_send(port_, m, (int)n);
}

extern "C" {
/** Scratch the page writes an incoming message into before wasm_midi_in(). */
EMSCRIPTEN_KEEPALIVE uint8_t* wasm_midi_buf() { return s_midiBuf; }
EMSCRIPTEN_KEEPALIVE void wasm_midi_in(int port, int len, double ts) {
    if (len <= 0 || len > (int)sizeof(s_midiBuf)) return;
    s_midiQueue.push_back({port, ts, std::vector<unsigned char>(s_midiBuf, s_midiBuf + len)});
    if (s_midiQueue.size() > 4096) s_midiQueue.pop_front();
}
}

/* ── RtAudio ─────────────────────────────────────────────────────────── */

namespace {
std::vector<RtAudio*> s_streams;
std::vector<int16_t> s_i16;
std::vector<float> s_mix;
double s_lastPull = -1e9, s_fallbackAt = 0;
uint64_t s_framesDone = 0;

void runProducers(uint32_t frames) {
    for (auto& p : s_producers) {
        p.owed += frames;
        while (p.owed >= p.block) { p.fn(p.ctx); p.owed -= p.block; }
    }
}

/** One pull of @p frames: inputs, then producers, then outputs into s_mix. */
void renderBlock(uint32_t frames, const float* in) {
    for (auto& s : s_services) s.fn(s.ctx);
    const size_t n = (size_t)frames * 2;
    if (s_i16.size() < n) s_i16.resize(n);
    if (s_mix.size() < n) s_mix.resize(n);
    const double t = (double)s_framesDone / WEB_AUDIO_RATE;

    for (auto* st : s_streams) {
        if (!st->running_ || !st->in_ || !st->cb_) continue;
        for (size_t i = 0; i < n; ++i) {
            const float v = in ? std::max(-1.f, std::min(1.f, in[i])) : 0.f;
            s_i16[i] = (int16_t)std::lrint(v * 32767.f);
        }
        st->time_ = t;
        st->cb_(nullptr, s_i16.data(), frames, t, 0, st->ud_);
    }

    runProducers(frames);

    std::fill(s_mix.begin(), s_mix.begin() + n, 0.f);
    for (auto* st : s_streams) {
        if (!st->running_ || !st->out_ || !st->cb_) continue;
        std::fill(s_i16.begin(), s_i16.begin() + n, 0);
        st->time_ = t;
        st->cb_(s_i16.data(), nullptr, frames, t, 0, st->ud_);
        for (size_t i = 0; i < n; ++i) s_mix[i] += s_i16[i] * (1.f / 32768.f);
    }
    s_framesDone += frames;
}
}

unsigned int RtAudio::getDeviceCount() { return js_mic_enabled() ? 2 : 1; }
std::vector<unsigned int> RtAudio::getDeviceIds() {
    std::vector<unsigned int> ids{WEB_AUDIO_OUT_ID};
    if (js_mic_enabled()) ids.push_back(WEB_AUDIO_IN_ID);
    return ids;
}
std::vector<std::string> RtAudio::getDeviceNames() {
    std::vector<std::string> names;
    for (unsigned id : getDeviceIds()) names.push_back(getDeviceInfo(id).name);
    return names;
}
RtAudio::DeviceInfo RtAudio::getDeviceInfo(unsigned int id) {
    DeviceInfo d;
    d.ID = id;
    d.sampleRates = {WEB_AUDIO_RATE};
    d.currentSampleRate = d.preferredSampleRate = WEB_AUDIO_RATE;
    d.nativeFormats = RTAUDIO_SINT16 | RTAUDIO_FLOAT32;
    if (id == WEB_AUDIO_OUT_ID) {
        d.name = "Browser audio";
        d.outputChannels = 2;
        d.isDefaultOutput = true;
    } else if (id == WEB_AUDIO_IN_ID && js_mic_enabled()) {
        d.name = "Browser microphone";
        d.inputChannels = 2;
        d.isDefaultInput = true;
    }
    return d;
}

RtAudioErrorType RtAudio::openStream(StreamParameters* o, StreamParameters* i, RtAudioFormat format,
                                     unsigned int sampleRate, unsigned int* bufferFrames,
                                     RtAudioCallback callback, void* userData, StreamOptions*) {
    if (open_) { error_ = "stream already open"; return RTAUDIO_INVALID_USE; }
    if (format != RTAUDIO_SINT16) { error_ = "web backend speaks int16 only"; return RTAUDIO_INVALID_PARAMETER; }
    if (sampleRate != WEB_AUDIO_RATE) { error_ = "web backend runs at 48 kHz"; return RTAUDIO_INVALID_PARAMETER; }
    if (o && (o->deviceId != WEB_AUDIO_OUT_ID || o->nChannels != 2)) { error_ = "no such output"; return RTAUDIO_INVALID_DEVICE; }
    if (i && (i->deviceId != WEB_AUDIO_IN_ID || !js_mic_enabled())) { error_ = "no such input"; return RTAUDIO_INVALID_DEVICE; }
    if (!o && !i) { error_ = "nothing to open"; return RTAUDIO_INVALID_PARAMETER; }
    out_ = o != nullptr;
    in_ = i != nullptr;
    cb_ = callback;
    ud_ = userData;
    frames_ = bufferFrames && *bufferFrames ? *bufferFrames : 512;
    open_ = true;
    running_ = false;
    s_streams.push_back(this);
    return RTAUDIO_NO_ERROR;
}

void RtAudio::closeStream() {
    if (!open_) return;
    open_ = running_ = false;
    s_streams.erase(std::remove(s_streams.begin(), s_streams.end(), this), s_streams.end());
}

extern "C" {
/**
 * The page's audio callback: @p frames of stereo at 48 kHz. @p in is the
 * microphone (interleaved float) or null. Returns interleaved float stereo.
 */
EMSCRIPTEN_KEEPALIVE float* wasm_audio_render(int frames, const float* in) {
    s_lastPull = emscripten_get_now();
    if (frames <= 0) return nullptr;
    renderBlock((uint32_t)frames, in);
    return s_mix.data();
}
/** Scratch for the microphone block the page hands to wasm_audio_render. */
EMSCRIPTEN_KEEPALIVE float* wasm_audio_in_buf(int frames) {
    static std::vector<float> buf;
    if (buf.size() < (size_t)frames * 2) buf.resize((size_t)frames * 2);
    return buf.data();
}
}

/**
 * Every LVGL loop: incoming MIDI, the services, and — while the page is not
 * pulling audio (no AudioContext yet, or a suspended one) — the audio clock
 * by wall time, so the sequencer, meters and song engine keep running as on
 * the board, whose codec never stops.
 */
extern "C" void wasm_io_service(void) {
    while (!s_midiQueue.empty()) {
        MidiMsg m = std::move(s_midiQueue.front());
        s_midiQueue.pop_front();
        for (auto* in : s_midiIns) {
            if (in->port_ != m.port || !in->cb_ || ignored(in, m.data)) continue;
            in->cb_(m.ts / 1000.0, &m.data, in->ud_);
        }
    }

    const double now = emscripten_get_now();
    if (now - s_lastPull < 250) { s_fallbackAt = now; return; }
    uint32_t frames = (uint32_t)((now - s_fallbackAt) * WEB_AUDIO_RATE / 1000.0);
    if (frames < 256) {
        for (auto& s : s_services) s.fn(s.ctx);
        return;
    }
    s_fallbackAt += frames * 1000.0 / WEB_AUDIO_RATE;
    frames = std::min<uint32_t>(frames, WEB_AUDIO_RATE / 10);
    while (frames) {
        const uint32_t f = std::min<uint32_t>(frames, 1024);
        renderBlock(f, nullptr);
        frames -= f;
    }
}
