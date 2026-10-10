// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwVirtualSource.hpp"
#include "PwContext.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace crosspad_pc {

PwVirtualSource::~PwVirtualSource() {
    stop();
}

void PwVirtualSource::onProcess(void* userdata)
{
    // RT thread — ring buffer + atomics only. No alloc/lock/log.
    // Direction OUTPUT: the graph *pulls* from us — dequeue a buffer, fill
    // it from the ring (produced by pushSamples() off the RT thread), and
    // zero-fill the tail on underrun.
    auto* self = static_cast<PwVirtualSource*>(userdata);
    struct pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) return;
    struct spa_data& d = b->buffer->datas[0];
    const uint32_t stride = 2 * sizeof(int16_t);
    uint32_t want = d.maxsize / stride;
    if (b->requested) want = std::min<uint32_t>(want, static_cast<uint32_t>(b->requested));
    auto* out = static_cast<int16_t*>(d.data);
    uint32_t got = static_cast<uint32_t>(self->ring_.read(out, static_cast<size_t>(want) * 2)) / 2; // samples→frames
    if (got < want) // underrun → silence tail
        std::memset(out + static_cast<size_t>(got) * 2, 0, static_cast<size_t>(want - got) * stride);
    d.chunk->offset = 0;
    d.chunk->stride = stride;
    d.chunk->size   = want * stride;
    pw_stream_queue_buffer(self->stream_, b);
}

void PwVirtualSource::onStateChanged(void* userdata, enum pw_stream_state /*old*/,
                                      enum pw_stream_state state, const char* error)
{
    auto* self = static_cast<PwVirtualSource*>(userdata);
    if (error) printf("[PwSource] %s stream error: %s\n", self->nodeName_.c_str(), error);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING ||
        state == PW_STREAM_STATE_ERROR)
        pw_thread_loop_signal(PwContext::instance().loop(), false);
}

bool PwVirtualSource::start(const char* nodeName, const char* description,
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
        PW_KEY_MEDIA_CLASS,       "Audio/Source/Virtual",
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
    events_.process       = &PwVirtualSource::onProcess;        // static, void(void*)
    events_.state_changed = &PwVirtualSource::onStateChanged;
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

    int res = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                           PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) { stopLocked(); return false; }

    // Wait (bounded) until the node reaches PAUSED/STREAMING — PAUSED is fine:
    // a source idles until an app links to it.
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
    printf("[PwSource] '%s' up (%s, S16/%u/2ch)\n", nodeName, description, sampleRate);
    return true;
}

void PwVirtualSource::stopLocked()
{
    if (stream_) {
        spa_hook_remove(&listener_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    open_.store(false, std::memory_order_release);
}

void PwVirtualSource::stop()
{
    if (!stream_) { open_.store(false, std::memory_order_release); return; }
    auto& ctx = PwContext::instance();
    {
        PwContext::Lock lock(ctx);
        stopLocked();
    }
    ring_.reset();
}

uint32_t PwVirtualSource::supportedFormats() const {
    return crosspad::audioFormatMask(crosspad::AudioFormat::Int16);
}

uint32_t PwVirtualSource::pushSamples(const void* samples, crosspad::AudioFormat fmt, uint32_t frames)
{
    // Called from PcAudioModule's paced (non-RT) audio thread — ring write
    // only, no PipeWire calls here.
    if (!isOpen()) return 0;
    if (fmt != crosspad::AudioFormat::Int16) return 0;
    const auto* src = static_cast<const int16_t*>(samples);
    size_t written = ring_.write(src, static_cast<size_t>(frames) * 2);
    return static_cast<uint32_t>(written / 2);
}

} // namespace crosspad_pc

#endif // __linux__
