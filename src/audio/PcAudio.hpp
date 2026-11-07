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
#include <crosspad/audio/AudioRingBuffer.hpp>

#include <atomic>
#include <memory>
#include <string>

struct PcSinkStream;

class PcAudioOutput : public crosspad::IAudioOutput {
public:
    PcAudioOutput();
    ~PcAudioOutput();

    PcAudioOutput(const PcAudioOutput&) = delete;
    PcAudioOutput& operator=(const PcAudioOutput&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────
    /// Open the output device RtAudio calls @p deviceName ("" = its default
    /// output). The name is resolved by the RtAudio instance that opens it:
    /// RtAudio numbers devices per instance, so an id from another
    /// enumeration can name a different device, or one with no outputs.
    bool begin(const std::string& deviceName = {},
               uint32_t sampleRate = 44100, uint32_t bufferFrames = 256);

    /// Open a stream created on the sound-server sink @p sinkName (Linux,
    /// PulseAudio or PipeWire). Born on that sink, it never follows the
    /// system default. False where there is no sound server, or the sink
    /// is not there.
    bool beginOnSink(const std::string& sinkName,
                     uint32_t sampleRate = 48000, uint32_t bufferFrames = 256);
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
    bool switchDevice(const std::string& deviceName);

    /// Name of the currently open output device (empty if closed).
    std::string getCurrentDeviceName() const;
    unsigned int getCurrentDeviceId() const;

private:
    std::unique_ptr<RtAudio> rtAudio_;
    crosspad::AudioRingBuffer<int16_t> outputRing_;

    uint32_t sampleRate_ = 44100;
    uint32_t bufferFrames_ = 256;
    bool streamOpen_ = false;
    unsigned int currentDeviceId_ = 0;
    std::string  currentDeviceName_;

    std::atomic<int16_t> outPeakL_{0};
    std::atomic<int16_t> outPeakR_{0};

    // beginOnSink(): the sound-server stream, shared with the thread feeding it.
    std::shared_ptr<PcSinkStream> sink_;

public:
    // Diagnostic counters (readable from any thread; written on the RtAudio
    // callback thread — counters only, logging happens on non-RT threads).
    std::atomic<uint32_t> diagRingShortfalls_{0};  // callback found < nFrames in ring
    std::atomic<uint32_t> diagAlsaUnderflows_{0};  // RTAUDIO_OUTPUT_UNDERFLOW status
    std::atomic<uint32_t> diagCallbacks_{0};

private:

    static int rtAudioCallback(void* outputBuffer, void* inputBuffer,
                                unsigned int nFrames, double streamTime,
                                RtAudioStreamStatus status, void* userData);
    int handleCallback(int16_t* outputBuffer, unsigned int nFrames,
                       RtAudioStreamStatus status);
};
