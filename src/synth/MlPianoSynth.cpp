/**
 * @file MlPianoSynth.cpp
 * @brief ISynthEngine implementation wrapping ML_SynthTools FmSynth
 */

#include "MlPianoSynth.hpp"
#include <ml_fm.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

void MlPianoSynth::init()
{
    if (initialized_) return;

    printf("[MlPianoSynth] Initializing FM synth (sr=%u, ch=%u)\n", sampleRate_, midiChannel_);
    FmSynth_Init(static_cast<float>(sampleRate_));
    initialized_ = true;
}

void MlPianoSynth::cleanup()
{
    initialized_ = false;
}

void MlPianoSynth::noteOn(uint8_t note, uint8_t velocity)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    float vel = velocity / 127.0f;
    FmSynth_NoteOn(midiChannel_, note, vel);
}

void MlPianoSynth::noteOff(uint8_t note)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_NoteOff(midiChannel_, note);
}

void MlPianoSynth::setPitchBend(int16_t bend)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_PitchBend(midiChannel_, bend / 8192.0f);
}

void MlPianoSynth::setAttack(float value)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_Attack(0, value);
}

void MlPianoSynth::setDecay(float value)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_Decay1(0, value);
}

void MlPianoSynth::setSustain(uint8_t value)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_DecayL(0, value / 127.0f);
}

uint8_t MlPianoSynth::getSustain()
{
    return 64; // stub
}

void MlPianoSynth::setRelease(float value)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_Release(0, value);
}

void MlPianoSynth::setFeedback(float value)
{
    if (!initialized_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    FmSynth_Feedback(0, value);
}

void MlPianoSynth::setFilterCutoff(float) {}
void MlPianoSynth::setFilterReso(float) {}

void MlPianoSynth::setWaveform(uint8_t, uint8_t) {}
void MlPianoSynth::setOscVolume(uint8_t, float) {}
void MlPianoSynth::setOscPitch(uint8_t, float) {}

void MlPianoSynth::setDelayEnabled(bool) {}
void MlPianoSynth::setDelayTime(float) {}
void MlPianoSynth::setDelayFeedback(float) {}
void MlPianoSynth::setDelayMix(float) {}

void MlPianoSynth::setReverbEnabled(bool) {}
void MlPianoSynth::setReverbDecay(float) {}
void MlPianoSynth::setReverbMix(float) {}

void MlPianoSynth::getLevel(int16_t& left, int16_t& right)
{
    left = peakL_.load(std::memory_order_relaxed);
    right = peakR_.load(std::memory_order_relaxed);
}

void MlPianoSynth::setMidiChannel(uint8_t ch)
{
    if (ch < 16) {
        midiChannel_ = ch;
        printf("[MlPianoSynth] Channel set to %u\n", ch);
    }
}

void MlPianoSynth::process(int16_t* stereoOut, uint32_t frames)
{
    if (!initialized_ || frames == 0) {
        std::memset(stereoOut, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    // Temporary mono float buffer
    thread_local std::vector<float> monoBuf;
    if (monoBuf.size() < frames) {
        monoBuf.resize(frames);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        FmSynth_Process(nullptr, monoBuf.data(), static_cast<int>(frames));
    }

    // Convert float mono -> int16 stereo + track peak
    int16_t maxAbs = 0;
    for (uint32_t i = 0; i < frames; i++) {
        float sample = monoBuf[i];
        // Soft clamp
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;

        auto s = static_cast<int16_t>(sample * 16384.0f);
        stereoOut[i * 2]     = s;
        stereoOut[i * 2 + 1] = s;

        int16_t abs_s = (s < 0) ? (int16_t)(s == INT16_MIN ? INT16_MAX : -s) : s;
        if (abs_s > maxAbs) maxAbs = abs_s;
    }

    peakL_.store(maxAbs, std::memory_order_relaxed);
    peakR_.store(maxAbs, std::memory_order_relaxed);
}
