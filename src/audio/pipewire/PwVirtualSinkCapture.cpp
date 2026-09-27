// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwVirtualSinkCapture.hpp"
#include "PwContext.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cstdio>
#include <cstring>
#include <climits>

namespace crosspad_pc {

PwVirtualSinkCapture::~PwVirtualSinkCapture() {
    stop();
}

void PwVirtualSinkCapture::onProcess(void* userdata)
{
    // RT thread — ring buffer + atomics only. No alloc/lock/log.
    auto* self = static_cast<PwVirtualSinkCapture*>(userdata);
    struct pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) return;
    struct spa_data& d = b->buffer->datas[0];
    if (d.data && d.chunk && d.chunk->size > 0) {
        const auto* samples = reinterpret_cast<const int16_t*>(
            static_cast<uint8_t*>(d.data) + d.chunk->offset);
        const uint32_t nSamples = d.chunk->size / sizeof(int16_t); // interleaved L,R
        self->diagProcessCalls_.fetch_add(1, std::memory_order_relaxed);
        if (self->ring_.write(samples, nSamples) < nSamples)
            self->diagRingOverflows_.fetch_add(1, std::memory_order_relaxed);
        int16_t pl = 0, pr = 0;
        for (uint32_t i = 0; i + 1 < nSamples; i += 2) {
            int16_t l = samples[i], r = samples[i + 1];
            if (l == INT16_MIN) l = INT16_MAX; else if (l < 0) l = -l;
            if (r == INT16_MIN) r = INT16_MAX; else if (r < 0) r = -r;
            if (l > pl) pl = l;
            if (r > pr) pr = r;
        }
        self->peakL_.store(pl, std::memory_order_relaxed);
        self->peakR_.store(pr, std::memory_order_relaxed);
    }
    pw_stream_queue_buffer(self->stream_, b);
}

void PwVirtualSinkCapture::onStateChanged(void* userdata, enum pw_stream_state /*old*/,
                                           enum pw_stream_state state, const char* error)
{
    auto* self = static_cast<PwVirtualSinkCapture*>(userdata);
    if (error) printf("[PwSink] %s stream error: %s\n", self->nodeName_.c_str(), error);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING ||
        state == PW_STREAM_STATE_ERROR)
        pw_thread_loop_signal(PwContext::instance().loop(), false);
}

bool PwVirtualSinkCapture::start(const char* nodeName, const char* description,
                                  uint32_t sampleRate, uint32_t bufferFrames)
{
    auto& ctx = PwContext::instance();
    if (!ctx.init()) return false;

    sampleRate_   = sampleRate;
    bufferFrames_ = bufferFrames;
    nodeName_     = nodeName;
    ring_.resize(static_cast<size_t>(bufferFrames) * 2 * 32); // 32 stereo buffers of headroom

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
    events_.version       = PW_VERSION_STREAM_EVENTS;
    events_.process       = &PwVirtualSinkCapture::onProcess;        // static, void(void*)
    events_.state_changed = &PwVirtualSinkCapture::onStateChanged;
    pw_stream_add_listener(stream_, &listener_, &events_, this);

    uint8_t podBuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuf, sizeof podBuf);
    struct spa_audio_info_raw info{};
    info.format   = SPA_AUDIO_FORMAT_S16;   // adapter converts/resamples for us
    info.rate     = sampleRate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const struct spa_pod* params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };

    int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                           PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) { stopLocked(); return false; }

    // Wait (bounded) until the node reaches PAUSED/STREAMING — PAUSED is fine:
    // a sink idles until an app links to it.
    struct timespec abstime;
    pw_thread_loop_get_time(ctx.loop(), &abstime, 2 * SPA_NSEC_PER_SEC);
    bool ok = false;
    while (true) {
        enum pw_stream_state st = pw_stream_get_state(stream_, nullptr);
        if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) { ok = true; break; }
        if (st == PW_STREAM_STATE_ERROR) { ok = false; break; }
        if (pw_thread_loop_timed_wait_full(ctx.loop(), &abstime) < 0) {
            // Timed out. If we're still just CONNECTING, treat as failure
            // rather than hang forever waiting for a node that may never
            // appear (e.g. daemon overloaded).
            st = pw_stream_get_state(stream_, nullptr);
            ok = (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING);
            break;
        }
    }
    if (!ok) { stopLocked(); return false; }

    open_.store(true, std::memory_order_release);
    printf("[PwSink] '%s' up (%s, S16/%u/2ch)\n", nodeName, description, sampleRate);
    return true;
}

void PwVirtualSinkCapture::stopLocked()
{
    if (stream_) {
        spa_hook_remove(&listener_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    open_.store(false, std::memory_order_release);
}

void PwVirtualSinkCapture::stop()
{
    if (!stream_) { open_.store(false, std::memory_order_release); return; }
    auto& ctx = PwContext::instance();
    {
        PwContext::Lock lock(ctx);
        stopLocked();
    }
    ring_.reset();
}

uint32_t PwVirtualSinkCapture::read(int16_t* out, uint32_t frameCount)
{
    if (!isOpen()) return 0;
    size_t want = static_cast<size_t>(frameCount) * 2;
    size_t avail = ring_.available() & ~size_t(1);
    if (want > avail) want = avail;
    size_t got = ring_.read(out, want);
    return static_cast<uint32_t>(got / 2);
}

void PwVirtualSinkCapture::getInputLevel(int16_t& left, int16_t& right) const
{
    left  = peakL_.load(std::memory_order_relaxed);
    right = peakR_.load(std::memory_order_relaxed);
}

} // namespace crosspad_pc

#endif // __linux__
