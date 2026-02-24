#pragma once

/**
 * @file PcAudioInput.hpp
 * @brief PC audio input via RtAudio (WASAPI on Windows)
 *
 * Implements crosspad::IAudioInput for the desktop simulator.
 * Mirrors PcAudioOutput pattern but captures audio instead of playing it.
 */

#include <crosspad/synth/IAudioInput.hpp>
#include <RtAudio.h>
#include <crosspad/audio/AudioRingBuffer.hpp>

#include <atomic>
#include <memory>
#include <string>

class PcAudioInput : public crosspad::IAudioInput {
public:
    PcAudioInput();
    ~PcAudioInput();

    PcAudioInput(const PcAudioInput&) = delete;
    PcAudioInput& operator=(const PcAudioInput&) = delete;

    // -- Lifecycle --
    bool begin(unsigned int inputDevice = 0,
               uint32_t sampleRate = 44100, uint32_t bufferFrames = 256);
    void end();

    // -- IAudioInput --
    uint32_t read(int16_t* interleavedSamples, uint32_t frameCount) override;
    uint32_t getSampleRate() const override;
    uint32_t getBufferSize() const override;
    void getInputLevel(int16_t& left, int16_t& right) const override;

    // -- Device management --
    unsigned int getInputDeviceCount() const;
    std::string getInputDeviceName(unsigned int index) const;
    unsigned int getDefaultInputDevice() const;
    bool isOpen() const;

    /// Stop current stream and re-open on a different device.
    bool switchDevice(unsigned int deviceId);

    /// Name of the currently open input device (empty if closed).
    std::string getCurrentDeviceName() const;
    unsigned int getCurrentDeviceId() const;

private:
    std::unique_ptr<RtAudio> rtAudio_;
    crosspad::AudioRingBuffer<int16_t> inputRing_;

    uint32_t sampleRate_    = 44100;
    uint32_t bufferFrames_  = 256;
    bool     streamOpen_    = false;
    unsigned int currentDeviceId_ = 0;
    std::string  currentDeviceName_;

    std::atomic<int16_t> inPeakL_{0};
    std::atomic<int16_t> inPeakR_{0};

    static int rtAudioCallback(void* outputBuffer, void* inputBuffer,
                                unsigned int nFrames, double streamTime,
                                RtAudioStreamStatus status, void* userData);
    int handleCallback(const int16_t* inputBuffer, unsigned int nFrames,
                       RtAudioStreamStatus status);
};
