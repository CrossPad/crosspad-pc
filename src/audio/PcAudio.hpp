#pragma once

/**
 * @file PcAudio.hpp
 * @brief PC audio output via RtAudio (WASAPI on Windows)
 *
 * Implements crosspad::IAudioOutput for the desktop simulator.
 * Pattern mirrors src/midi/PcMidi.hpp (RtMidi).
 */

#include <crosspad/synth/IAudioOutput.hpp>
#include <RtAudio.h>
#include "AudioRingBuffer.hpp"

#include <atomic>
#include <memory>
#include <string>

class PcAudioOutput : public crosspad::IAudioOutput {
public:
    PcAudioOutput();
    ~PcAudioOutput();

    PcAudioOutput(const PcAudioOutput&) = delete;
    PcAudioOutput& operator=(const PcAudioOutput&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────
    bool begin(unsigned int outputDevice = 0,
               uint32_t sampleRate = 44100, uint32_t bufferFrames = 256);
    void end();

    // ── IAudioOutput ───────────────────────────────────────────
    uint32_t write(const int16_t* interleavedSamples, uint32_t frameCount) override;
    uint32_t getSampleRate() const override;
    uint32_t getBufferSize() const override;

    // ── Level metering ─────────────────────────────────────────
    void getOutputLevel(int16_t& left, int16_t& right) const;

    // ── Device management ──────────────────────────────────────
    unsigned int getOutputDeviceCount() const;
    std::string getOutputDeviceName(unsigned int index) const;
    unsigned int getDefaultOutputDevice() const;
    bool isOpen() const;

    /// Stop current stream and re-open on a different device.
    bool switchDevice(unsigned int deviceId);

    /// Name of the currently open output device (empty if closed).
    std::string getCurrentDeviceName() const;
    unsigned int getCurrentDeviceId() const;

private:
    std::unique_ptr<RtAudio> rtAudio_;
    AudioRingBuffer<int16_t> outputRing_;

    uint32_t sampleRate_ = 44100;
    uint32_t bufferFrames_ = 256;
    bool streamOpen_ = false;
    unsigned int currentDeviceId_ = 0;
    std::string  currentDeviceName_;

    std::atomic<int16_t> outPeakL_{0};
    std::atomic<int16_t> outPeakR_{0};

    static int rtAudioCallback(void* outputBuffer, void* inputBuffer,
                                unsigned int nFrames, double streamTime,
                                RtAudioStreamStatus status, void* userData);
    int handleCallback(int16_t* outputBuffer, unsigned int nFrames,
                       RtAudioStreamStatus status);
};
