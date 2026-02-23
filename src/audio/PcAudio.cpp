/**
 * @file PcAudio.cpp
 * @brief PC audio output implementation via RtAudio (WASAPI on Windows)
 */

#include "PcAudio.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

PcAudioOutput::PcAudioOutput() = default;

PcAudioOutput::~PcAudioOutput() {
    end();
}

bool PcAudioOutput::begin(unsigned int outputDevice, uint32_t sampleRate,
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

    printf("[Audio] Available OUTPUT devices (%u):\n", (unsigned)ids.size());
    for (unsigned int id : ids) {
        RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(id);
        if (info.outputChannels > 0) {
            printf("  [%u] %s (%u ch, %u Hz%s)\n", id, info.name.c_str(),
                   info.outputChannels, info.preferredSampleRate,
                   (id == defaultOutId) ? ", default" : "");
        }
    }

    // Select output device
    unsigned int outDeviceId;
    if (outputDevice == 0) {
        outDeviceId = defaultOutId;
    } else {
        outDeviceId = outputDevice;
    }

    // Verify the selected device exists and has output channels
    RtAudio::DeviceInfo outInfo = rtAudio_->getDeviceInfo(outDeviceId);
    if (outInfo.outputChannels < 2) {
        printf("[Audio] Device %u has only %u output channels, need 2\n",
               outDeviceId, outInfo.outputChannels);
        rtAudio_.reset();
        return false;
    }

    // Use device's preferred sample rate if available
    if (outInfo.preferredSampleRate > 0) {
        sampleRate_ = outInfo.preferredSampleRate;
    }

    // Size ring buffer: 8 buffers worth of stereo samples
    outputRing_.resize(bufferFrames_ * 2 * 8);

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
        printf("[Audio] Failed to open stream: %s\n", rtAudio_->getErrorText());
        rtAudio_.reset();
        return false;
    }

    bufferFrames_ = actualBufferFrames;
    streamOpen_ = true;

    err = rtAudio_->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        printf("[Audio] Failed to start stream: %s\n", rtAudio_->getErrorText());
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

void PcAudioOutput::end() {
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

bool PcAudioOutput::switchDevice(unsigned int deviceId) {
    end();
    return begin(deviceId, sampleRate_, bufferFrames_);
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
    if (status & RTAUDIO_OUTPUT_UNDERFLOW) {
        printf("[Audio] Output underflow!\n");
    }

    // Pull from ring buffer into output
    size_t sampleCount = static_cast<size_t>(nFrames) * 2;
    size_t read = outputRing_.read(outputBuffer, sampleCount);

    // Zero-fill any remaining (underrun — silence)
    if (read < sampleCount) {
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
