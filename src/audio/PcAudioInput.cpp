/**
 * @file PcAudioInput.cpp
 * @brief PC audio input implementation via RtAudio (WASAPI on Windows)
 */

#include "PcAudioInput.hpp"
#include <cstdio>
#include <cstring>

PcAudioInput::PcAudioInput() = default;

PcAudioInput::~PcAudioInput() {
    end();
}

bool PcAudioInput::begin(unsigned int inputDevice, uint32_t sampleRate,
                          uint32_t bufferFrames) {
    sampleRate_ = sampleRate;
    bufferFrames_ = bufferFrames;

    printf("[AudioIn] Initializing audio input (RtAudio)...\n");

    rtAudio_ = std::make_unique<RtAudio>();
    if (rtAudio_->getDeviceCount() == 0) {
        printf("[AudioIn] No audio devices found!\n");
        rtAudio_.reset();
        return false;
    }

    // Enumerate input devices
    std::vector<unsigned int> ids = rtAudio_->getDeviceIds();
    unsigned int defaultInId = rtAudio_->getDefaultInputDevice();

    printf("[AudioIn] Available INPUT devices (%u):\n", (unsigned)ids.size());
    for (unsigned int id : ids) {
        RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(id);
        if (info.inputChannels > 0) {
            printf("  [%u] %s (%u ch, %u Hz%s)\n", id, info.name.c_str(),
                   info.inputChannels, info.preferredSampleRate,
                   (id == defaultInId) ? ", default" : "");
        }
    }

    // Select input device
    unsigned int inDeviceId;
    if (inputDevice == 0) {
        inDeviceId = defaultInId;
    } else {
        inDeviceId = inputDevice;
    }

    // Verify the device has input channels
    RtAudio::DeviceInfo inInfo = rtAudio_->getDeviceInfo(inDeviceId);
    if (inInfo.inputChannels < 2) {
        printf("[AudioIn] Device %u has only %u input channels, need 2\n",
               inDeviceId, inInfo.inputChannels);
        rtAudio_.reset();
        return false;
    }

    // Use device's preferred sample rate if available
    if (inInfo.preferredSampleRate > 0) {
        sampleRate_ = inInfo.preferredSampleRate;
    }

    // Size ring buffer: 8 buffers worth of stereo samples
    inputRing_.resize(bufferFrames_ * 2 * 8);

    // Setup stream parameters — input only
    RtAudio::StreamParameters inParams;
    inParams.deviceId = inDeviceId;
    inParams.nChannels = 2;
    inParams.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY;

    unsigned int actualBufferFrames = bufferFrames_;

    RtAudioErrorType err = rtAudio_->openStream(
        nullptr, &inParams, RTAUDIO_SINT16,
        sampleRate_, &actualBufferFrames,
        &PcAudioInput::rtAudioCallback, this, &options);

    if (err != RTAUDIO_NO_ERROR) {
        printf("[AudioIn] Failed to open stream: %s\n", rtAudio_->getErrorText());
        rtAudio_.reset();
        return false;
    }

    bufferFrames_ = actualBufferFrames;
    streamOpen_ = true;
    currentDeviceId_ = inDeviceId;
    currentDeviceName_ = inInfo.name;

    err = rtAudio_->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        printf("[AudioIn] Failed to start stream: %s\n", rtAudio_->getErrorText());
        rtAudio_->closeStream();
        streamOpen_ = false;
        currentDeviceName_.clear();
        rtAudio_.reset();
        return false;
    }

    printf("[AudioIn] Stream started: device=[%u] %s, %u Hz, %u frames/buffer\n",
           inDeviceId, inInfo.name.c_str(), sampleRate_, bufferFrames_);

    return true;
}

void PcAudioInput::end() {
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
    inputRing_.reset();
    currentDeviceName_.clear();
    printf("[AudioIn] Shutdown complete.\n");
}

uint32_t PcAudioInput::read(int16_t* interleavedSamples, uint32_t frameCount) {
    if (!streamOpen_) return 0;
    size_t sampleCount = static_cast<size_t>(frameCount) * 2;
    // Clamp to frame-aligned available data
    size_t avail = inputRing_.available() & ~size_t(1);
    if (sampleCount > avail) sampleCount = avail;
    size_t rd = inputRing_.read(interleavedSamples, sampleCount);
    return static_cast<uint32_t>(rd / 2);
}

uint32_t PcAudioInput::getSampleRate() const {
    return sampleRate_;
}

uint32_t PcAudioInput::getBufferSize() const {
    return bufferFrames_;
}

void PcAudioInput::getInputLevel(int16_t& left, int16_t& right) const {
    left = inPeakL_.load(std::memory_order_relaxed);
    right = inPeakR_.load(std::memory_order_relaxed);
}

unsigned int PcAudioInput::getInputDeviceCount() const {
    if (!rtAudio_) {
        // Use a temporary instance for enumeration
        try {
            RtAudio probe;
            unsigned int count = 0;
            for (unsigned int id : probe.getDeviceIds()) {
                RtAudio::DeviceInfo info = probe.getDeviceInfo(id);
                if (info.inputChannels > 0) count++;
            }
            return count;
        } catch (...) { return 0; }
    }
    unsigned int count = 0;
    for (unsigned int id : rtAudio_->getDeviceIds()) {
        RtAudio::DeviceInfo info = rtAudio_->getDeviceInfo(id);
        if (info.inputChannels > 0) count++;
    }
    return count;
}

std::string PcAudioInput::getInputDeviceName(unsigned int index) const {
    try {
        RtAudio probe;
        RtAudio::DeviceInfo info = probe.getDeviceInfo(index);
        return info.name;
    } catch (...) { return ""; }
}

unsigned int PcAudioInput::getDefaultInputDevice() const {
    try {
        RtAudio probe;
        return probe.getDefaultInputDevice();
    } catch (...) { return 0; }
}

bool PcAudioInput::isOpen() const {
    return streamOpen_;
}

bool PcAudioInput::switchDevice(unsigned int deviceId) {
    end();
    return begin(deviceId, sampleRate_, bufferFrames_);
}

std::string PcAudioInput::getCurrentDeviceName() const {
    return currentDeviceName_;
}

unsigned int PcAudioInput::getCurrentDeviceId() const {
    return currentDeviceId_;
}

// -- RtAudio callback --

int PcAudioInput::rtAudioCallback(void* /*outputBuffer*/, void* inputBuffer,
                                   unsigned int nFrames, double /*streamTime*/,
                                   RtAudioStreamStatus status, void* userData) {
    auto* self = static_cast<PcAudioInput*>(userData);
    return self->handleCallback(static_cast<const int16_t*>(inputBuffer), nFrames, status);
}

int PcAudioInput::handleCallback(const int16_t* inputBuffer, unsigned int nFrames,
                                  RtAudioStreamStatus status) {
    if (status & RTAUDIO_INPUT_OVERFLOW) {
        printf("[AudioIn] Input overflow!\n");
    }

    if (!inputBuffer) return 0;

    // Write captured samples into ring buffer
    size_t sampleCount = static_cast<size_t>(nFrames) * 2;
    inputRing_.write(inputBuffer, sampleCount);

    // Compute peak levels
    int16_t maxL = 0, maxR = 0;
    for (unsigned int i = 0; i < nFrames; i++) {
        int16_t absL = inputBuffer[i * 2];
        int16_t absR = inputBuffer[i * 2 + 1];
        if (absL < 0) absL = (absL == INT16_MIN) ? INT16_MAX : -absL;
        if (absR < 0) absR = (absR == INT16_MIN) ? INT16_MAX : -absR;
        if (absL > maxL) maxL = absL;
        if (absR > maxR) maxR = absR;
    }
    inPeakL_.store(maxL, std::memory_order_relaxed);
    inPeakR_.store(maxR, std::memory_order_relaxed);

    return 0;
}
