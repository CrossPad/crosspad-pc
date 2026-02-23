/**
 * @file PianoPadLogic.cpp
 * @brief Chromatic pad logic for ML Piano app
 *
 * Maps 16 pads (0-15) to consecutive chromatic notes starting from baseNote_.
 * Pad 0 = bottom-left = lowest note. Colors: white for naturals, dark for sharps.
 */

#include "PianoPadLogic.hpp"
#include "synth/MlPianoSynth.hpp"
#include <crosspad/pad/PadManager.hpp>
#include <cstdio>

PianoPadLogic::PianoPadLogic(MlPianoSynth& synth)
    : synth_(synth)
{
}

bool PianoPadLogic::isBlackKey(uint8_t semitone)
{
    // C=0,C#=1,D=2,D#=3,E=4,F=5,F#=6,G=7,G#=8,A=9,A#=10,B=11
    uint8_t mod = semitone % 12;
    return (mod == 1 || mod == 3 || mod == 6 || mod == 8 || mod == 10);
}

uint8_t PianoPadLogic::noteForPad(uint8_t padIdx) const
{
    return baseNote_ + padIdx;
}

void PianoPadLogic::colorPads(crosspad::PadManager& padManager)
{
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t note = noteForPad(i);
        if (isBlackKey(note)) {
            padManager.setPadColor(i, crosspad::RgbColor(20, 10, 40));  // dark purple for accidentals
        } else {
            padManager.setPadColor(i, crosspad::RgbColor(60, 60, 80));  // light blue-gray for naturals
        }
    }
}

void PianoPadLogic::onActivate(crosspad::PadManager& padManager)
{
    printf("[PianoPad] Activated (baseNote=%u)\n", baseNote_);
    colorPads(padManager);
}

void PianoPadLogic::onDeactivate(crosspad::PadManager& padManager)
{
    allNotesOff();
    for (uint8_t i = 0; i < 16; i++) {
        padManager.setPadColor(i, crosspad::RgbColor(0, 0, 0));
    }
    printf("[PianoPad] Deactivated\n");
}

void PianoPadLogic::onPadPress(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t velocity)
{
    if (padIdx >= 16) return;
    uint8_t note = noteForPad(padIdx);
    synth_.noteOn(note, velocity);

    // Light up pad bright
    if (isBlackKey(note)) {
        padManager.setPadColor(padIdx, crosspad::RgbColor(160, 80, 255));  // bright purple
    } else {
        padManager.setPadColor(padIdx, crosspad::RgbColor(120, 200, 255)); // bright cyan
    }
}

void PianoPadLogic::onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx)
{
    if (padIdx >= 16) return;
    uint8_t note = noteForPad(padIdx);
    synth_.noteOff(note);

    // Restore idle color
    if (isBlackKey(note)) {
        padManager.setPadColor(padIdx, crosspad::RgbColor(20, 10, 40));
    } else {
        padManager.setPadColor(padIdx, crosspad::RgbColor(60, 60, 80));
    }
}

void PianoPadLogic::onPadPressure(crosspad::PadManager& /*padManager*/, uint8_t /*padIdx*/, uint8_t /*pressure*/)
{
    // Not used for piano
}

void PianoPadLogic::octaveUp()
{
    if (baseNote_ + 12 + 15 <= 127) {
        allNotesOff();
        baseNote_ += 12;
        printf("[PianoPad] Octave up -> baseNote=%u\n", baseNote_);
    }
}

void PianoPadLogic::octaveDown()
{
    if (baseNote_ >= 12) {
        allNotesOff();
        baseNote_ -= 12;
        printf("[PianoPad] Octave down -> baseNote=%u\n", baseNote_);
    }
}

void PianoPadLogic::allNotesOff()
{
    for (uint8_t i = 0; i < 16; i++) {
        synth_.noteOff(noteForPad(i));
    }
}
