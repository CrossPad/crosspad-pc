#pragma once

/**
 * @file MlPianoSynth.hpp
 * @brief ISynthEngine wrapper around ML_SynthTools FmSynth
 *
 * Wraps the FmSynth_* global API into crosspad::ISynthEngine interface.
 * Audio thread calls process() to fill buffers; GUI/MIDI thread calls
 * noteOn/noteOff. A mutex protects parameter changes from the audio thread.
 */

#include <crosspad/synth/ISynthEngine.hpp>
#include <cstdint>
#include <atomic>
#include <mutex>

class MlPianoSynth : public crosspad::ISynthEngine {
public:
    MlPianoSynth() = default;
    ~MlPianoSynth() override = default;

    void init() override;
    void cleanup() override;

    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;

    void setPitchBend(int16_t bend) override;

    void setAttack(float value) override;
    void setDecay(float value) override;
    void setSustain(uint8_t value) override;
    uint8_t getSustain() override;
    void setRelease(float value) override;

    void setFilterCutoff(float value) override;
    void setFilterReso(float value) override;

    void setWaveform(uint8_t osc, uint8_t waveformIdx) override;
    void setOscVolume(uint8_t osc, float volume) override;
    void setOscPitch(uint8_t osc, float pitch) override;

    void setDelayEnabled(bool en) override;
    void setDelayTime(float value) override;
    void setDelayFeedback(float value) override;
    void setDelayMix(float value) override;

    void setReverbEnabled(bool en) override;
    void setReverbDecay(float value) override;
    void setReverbMix(float value) override;

    void getLevel(int16_t& left, int16_t& right) override;

    /// Set the FM synth MIDI channel (0-15) — selects from built-in presets
    void setMidiChannel(uint8_t ch);
    uint8_t getMidiChannel() const { return midiChannel_; }

    /**
     * @brief Process audio: generate stereo int16 interleaved samples
     * @param stereoOut  Output buffer (interleaved L,R,L,R,...)
     * @param frames     Number of stereo frames to generate
     */
    void process(int16_t* stereoOut, uint32_t frames);

    uint32_t getSampleRate() const { return sampleRate_; }
    void setSampleRate(uint32_t sr) { sampleRate_ = sr; }
    void setFeedback(float value);

private:
    uint32_t sampleRate_ = 44100;
    uint8_t midiChannel_ = 0;
    bool initialized_ = false;
    std::mutex mutex_;
    std::atomic<int16_t> peakL_{0};
    std::atomic<int16_t> peakR_{0};
};
