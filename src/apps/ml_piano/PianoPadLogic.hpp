#pragma once

/**
 * @file PianoPadLogic.hpp
 * @brief IPadLogicHandler for ML Piano app — chromatic note mapping on 4x4 pad grid
 */

#include <crosspad/pad/IPadLogicHandler.hpp>
#include <cstdint>

class MlPianoSynth;

class PianoPadLogic : public crosspad::IPadLogicHandler {
public:
    explicit PianoPadLogic(MlPianoSynth& synth);

    void onActivate(crosspad::PadManager& padManager) override;
    void onDeactivate(crosspad::PadManager& padManager) override;
    void onPadPress(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t velocity) override;
    void onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx) override;
    void onPadPressure(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t pressure) override;

    void octaveUp();
    void octaveDown();
    uint8_t getBaseNote() const { return baseNote_; }

    /// Returns true if the given semitone offset (0-11) is a black key
    static bool isBlackKey(uint8_t semitone);

    /// Set pad colors based on current base note (public for GUI use)
    void colorPads(crosspad::PadManager& padManager);

private:
    MlPianoSynth& synth_;
    uint8_t baseNote_ = 48; // C3 default

    void allNotesOff();
    uint8_t noteForPad(uint8_t padIdx) const;
};
