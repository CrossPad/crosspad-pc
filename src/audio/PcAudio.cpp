/**
 * @file PcAudio.cpp
 * @brief PC audio output implementation via RtAudio (WASAPI on Windows)
 */

#include "PcAudio.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

#if defined(__linux__) && defined(USE_VIRTUAL_AUDIO)
#define CP_SINK_OUTPUT 1
#include <chrono>
#include <thread>
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

// Cached device list — only log when devices change
static std::vector<std::string> s_lastOutputDevices;

PcAudioOutput::PcAudioOutput() = default;

PcAudioOutput::~PcAudioOutput() {
    end();
}

bool PcAudioOutput::begin(const std::string& deviceName, uint32_t sampleRate,
                           uint32_t bufferFrames) {
    sampleRate_ = sampleRate;
    bufferFrames_ = bufferFrames;

    printf("[Audio] Initializing audio subsystem (RtAudio)...\n");

    rtAudio_ = std::make_unique<RtAudio>();
    if (rtAudio_->getDeviceCount() == 0) {
        printf("[Audio] No audio devices found!\n");
        rtAudio_.reset();
        return false;
    }

    // Enumerate devices
    std::vector<unsigned int> ids = rtAudio_->getDeviceIds();
    unsigned int defaultOutId = rtAudio_->getDefaultOutputDevice();

    std::vector<std::string> currentDevices;
    for (unsigned int id : ids) {
        RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(id);
        if (info.outputChannels > 0)
            currentDevices.push_back(info.name);
    }

    if (currentDevices != s_lastOutputDevices) {
        s_lastOutputDevices = currentDevices;
        printf("[Audio] Available OUTPUT devices (%u):\n", (unsigned)ids.size());
        for (unsigned int id : ids) {
            RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(id);
            if (info.outputChannels > 0) {
                printf("  [%u] %s (%u ch, %u Hz%s)\n", id, info.name.c_str(),
                       info.outputChannels, info.preferredSampleRate,
                       (id == defaultOutId) ? ", default" : "");
            }
        }
    }

    // Select output device: by name, in this instance's own numbering.
    unsigned int outDeviceId = defaultOutId;
    if (!deviceName.empty()) {
        outDeviceId = 0;
        for (unsigned int id : ids) {
            if (rtAudio_->getDeviceInfo(id).name == deviceName) { outDeviceId = id; break; }
        }
        if (outDeviceId == 0) {
            printf("[Audio] Output device '%s' is not there\n", deviceName.c_str());
            rtAudio_.reset();
            return false;
        }
    }

    // Verify the selected device exists and has output channels
    RtAudio::DeviceInfo outInfo = rtAudio_->getDeviceInfo(outDeviceId);
    if (outInfo.outputChannels < 2) {
        printf("[Audio] Device [%u] '%s' has only %u output channels, need 2\n",
               outDeviceId, outInfo.name.c_str(), outInfo.outputChannels);
        rtAudio_.reset();
        return false;
    }

    // Use device's preferred sample rate if available
    if (outInfo.preferredSampleRate > 0) {
        sampleRate_ = outInfo.preferredSampleRate;
    }

    // Size ring buffer: 32 buffers worth of stereo samples.
    // Larger buffer tolerates timing jitter from Windows sleep granularity
    // and synth mutex contention without causing underruns.
    outputRing_.resize(bufferFrames_ * 2 * 32);

    // Setup stream parameters
    RtAudio::StreamParameters outParams;
    outParams.deviceId = outDeviceId;
    outParams.nChannels = 2;
    outParams.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY;

    unsigned int actualBufferFrames = bufferFrames_;

    RtAudioErrorType err = rtAudio_->openStream(
        &outParams, nullptr, RTAUDIO_SINT16,
        sampleRate_, &actualBufferFrames,
        &PcAudioOutput::rtAudioCallback, this, &options);

    if (err != RTAUDIO_NO_ERROR) {
        printf("[Audio] Failed to open stream: %s\n", rtAudio_->getErrorText().c_str());
        rtAudio_.reset();
        return false;
    }

    bufferFrames_ = actualBufferFrames;
    streamOpen_ = true;

    err = rtAudio_->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        printf("[Audio] Failed to start stream: %s\n", rtAudio_->getErrorText().c_str());
        rtAudio_->closeStream();
        streamOpen_ = false;
        rtAudio_.reset();
        return false;
    }

    currentDeviceId_ = outDeviceId;
    currentDeviceName_ = outInfo.name;

    printf("[Audio] Stream started: device=[%u] %s, %u Hz, %u frames/buffer\n",
           outDeviceId, outInfo.name.c_str(), sampleRate_, bufferFrames_);

    return true;
}

#ifdef CP_SINK_OUTPUT
/// One beginOnSink() stream. The thread feeding it holds it too: a write to a
/// sink that never plays (an HDMI port with nothing on it) blocks for as long
/// as the sink stays silent, and end() must not wait for that.
struct PcSinkStream {
    pa_simple*        pa = nullptr;
    std::atomic<bool> run{true};
    std::atomic<bool> done{false};
};

bool PcAudioOutput::beginOnSink(const std::string& sinkName, uint32_t sampleRate,
                                uint32_t bufferFrames) {
    if (sinkName.empty()) return false;
    sampleRate_   = sampleRate;
    bufferFrames_ = bufferFrames;

    // Naming the sink here makes the server create the stream on it (its
    // target), instead of on the default and moved afterwards: no moment on
    // the default, and nothing to find among other programs' streams.
    const pa_sample_spec spec{PA_SAMPLE_S16LE, sampleRate_, 2};
    const uint32_t frameBytes = 2 * sizeof(int16_t);
    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength   = bufferFrames_ * 4 * frameBytes;   // ~21 ms at 256/48k
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = bufferFrames_ * frameBytes;
    attr.fragsize  = static_cast<uint32_t>(-1);
    int err = 0;
    pa_simple* pa = pa_simple_new(nullptr, "CrossPad", PA_STREAM_PLAYBACK, sinkName.c_str(),
                                  "Output", &spec, nullptr, &attr, &err);
    if (!pa) {
        printf("[Audio] Sink '%s' would not open: %s\n", sinkName.c_str(), pa_strerror(err));
        return false;
    }

    outputRing_.resize(bufferFrames_ * 2 * 32);
    currentDeviceId_   = 0;
    currentDeviceName_ = sinkName;
    streamOpen_        = true;
    sink_ = std::make_shared<PcSinkStream>();
    sink_->pa = pa;
    std::thread([this, st = sink_, frames = bufferFrames_, name = sinkName]() {
        std::vector<int16_t> block(static_cast<size_t>(frames) * 2);
        while (st->run.load()) {
            handleCallback(block.data(), frames, 0);
            int werr = 0;
            // Blocks until the server has room: this is the stream's clock.
            if (pa_simple_write(st->pa, block.data(), block.size() * sizeof(int16_t), &werr) < 0) {
                printf("[Audio] Sink '%s' write failed: %s\n", name.c_str(), pa_strerror(werr));
                break;
            }
        }
        pa_simple_free(st->pa);
        st->done.store(true);
    }).detach();
    printf("[Audio] Stream started on sink '%s', %u Hz, %u frames/buffer\n",
           sinkName.c_str(), sampleRate_, bufferFrames_);
    return true;
}
#else
bool PcAudioOutput::beginOnSink(const std::string&, uint32_t, uint32_t) { return false; }
#endif

void PcAudioOutput::end() {
#ifdef CP_SINK_OUTPUT
    if (sink_) {
        // The feeding thread stops after its write returns; a sink that
        // stopped playing may hold that write, so wait a while, then leave
        // the stream to the thread (which frees it and never touches this
        // output again once it sees run == false).
        sink_->run.store(false);
        for (int i = 0; i < 50 && !sink_->done.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!sink_->done.load())
            printf("[Audio] Sink '%s' is not playing; its stream is left to close on its own\n",
                   currentDeviceName_.c_str());
        sink_.reset();
        streamOpen_ = false;
    }
#endif
    if (rtAudio_ && streamOpen_) {
        if (rtAudio_->isStreamRunning()) {
            rtAudio_->stopStream();
        }
        if (rtAudio_->isStreamOpen()) {
            rtAudio_->closeStream();
        }
        streamOpen_ = false;
    }
    rtAudio_.reset();
    outputRing_.reset();
    currentDeviceName_.clear();
    printf("[Audio] Shutdown complete.\n");
}

uint32_t PcAudioOutput::write(const int16_t* interleavedSamples, uint32_t frameCount) {
    if (!streamOpen_) return 0;
    size_t sampleCount = static_cast<size_t>(frameCount) * 2; // stereo interleaved
    // Clamp to frame-aligned space so we never split a L/R pair
    size_t space = outputRing_.space() & ~size_t(1);
    if (sampleCount > space) sampleCount = space;
    size_t written = outputRing_.write(interleavedSamples, sampleCount);
    return static_cast<uint32_t>(written / 2); // return frames written
}

uint32_t PcAudioOutput::getSampleRate() const {
    return sampleRate_;
}

uint32_t PcAudioOutput::getBufferSize() const {
    return bufferFrames_;
}

void PcAudioOutput::getOutputLevel(int16_t& left, int16_t& right) const {
    left = outPeakL_.load(std::memory_order_relaxed);
    right = outPeakR_.load(std::memory_order_relaxed);
}

unsigned int PcAudioOutput::getOutputDeviceCount() const {
    if (!rtAudio_) return 0;
    unsigned int count = 0;
    for (unsigned int id : rtAudio_->getDeviceIds()) {
        RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(id);
        if (info.outputChannels > 0) count++;
    }
    return count;
}

std::string PcAudioOutput::getOutputDeviceName(unsigned int index) const {
    if (!rtAudio_) return "";
    RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(index);
    return info.name;
}

unsigned int PcAudioOutput::getDefaultOutputDevice() const {
    if (!rtAudio_) return 0;
    return rtAudio_->getDefaultOutputDevice();
}

bool PcAudioOutput::isOpen() const {
    return streamOpen_;
}

bool PcAudioOutput::switchDevice(const std::string& deviceName) {
    end();
    return begin(deviceName, sampleRate_, bufferFrames_);
}

std::string PcAudioOutput::getCurrentDeviceName() const {
    return currentDeviceName_;
}

unsigned int PcAudioOutput::getCurrentDeviceId() const {
    return currentDeviceId_;
}

// ── RtAudio callback ───────────────────────────────────────────────────

int PcAudioOutput::rtAudioCallback(void* outputBuffer, void* /*inputBuffer*/,
                                    unsigned int nFrames, double /*streamTime*/,
                                    RtAudioStreamStatus status, void* userData) {
    auto* self = static_cast<PcAudioOutput*>(userData);
    return self->handleCallback(static_cast<int16_t*>(outputBuffer), nFrames, status);
}

int PcAudioOutput::handleCallback(int16_t* outputBuffer, unsigned int nFrames,
                                   RtAudioStreamStatus status) {
    diagCallbacks_.fetch_add(1, std::memory_order_relaxed);
    if (status & RTAUDIO_OUTPUT_UNDERFLOW) {
        // ALSA/device-level xrun reported by RtAudio. Counter only — no
        // printf on the RT callback thread; the module thread reports.
        diagAlsaUnderflows_.fetch_add(1, std::memory_order_relaxed);
    }

    // Pull from ring buffer into output
    size_t sampleCount = static_cast<size_t>(nFrames) * 2;
    size_t read = outputRing_.read(outputBuffer, sampleCount);

    // Zero-fill any remaining (underrun — silence)
    if (read < sampleCount) {
        diagRingShortfalls_.fetch_add(1, std::memory_order_relaxed);
        std::memset(outputBuffer + read, 0, (sampleCount - read) * sizeof(int16_t));
    }

    // Compute peak levels
    int16_t maxL = 0, maxR = 0;
    for (unsigned int i = 0; i < nFrames; i++) {
        int16_t absL = outputBuffer[i * 2];
        int16_t absR = outputBuffer[i * 2 + 1];
        if (absL < 0) absL = (absL == INT16_MIN) ? INT16_MAX : -absL;
        if (absR < 0) absR = (absR == INT16_MIN) ? INT16_MAX : -absR;
        if (absL > maxL) maxL = absL;
        if (absR > maxR) maxR = absR;
    }
    outPeakL_.store(maxL, std::memory_order_relaxed);
    outPeakR_.store(maxR, std::memory_order_relaxed);

    return 0;
}
