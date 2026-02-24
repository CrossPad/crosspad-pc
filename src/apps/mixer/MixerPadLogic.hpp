#pragma once

/**
 * @file MixerPadLogic.hpp
 * @brief Pad handler for mute/solo/route toggles in the Mixer app.
 *
 * Pad layout (4x4 grid):
 *   Row 3: 12=IN1->OUT2  13=IN2->OUT2  14=SYN->OUT2  15=OUT2 Mute
 *   Row 2:  8=IN1->OUT1   9=IN2->OUT1  10=SYN->OUT1  11=OUT1 Mute
 *   Row 1:  4=IN1 Solo    5=IN2 Solo    6=SYN Solo     7=(unused)
 *   Row 0:  0=IN1 Mute    1=IN2 Mute    2=SYN Mute     3=(unused)
 */

#include <crosspad/pad/IPadLogicHandler.hpp>

class AudioMixerEngine;

class MixerPadLogic : public crosspad::IPadLogicHandler {
public:
    explicit MixerPadLogic(AudioMixerEngine& engine);

    void onActivate(crosspad::PadManager& padManager) override;
    void onDeactivate(crosspad::PadManager& padManager) override;
    void onPadPress(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t velocity) override;
    void onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx) override;
    void onPadPressure(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t pressure) override;

    void updatePadColors(crosspad::PadManager& padManager);

    using StateChangedCb = void(*)();
    void setOnStateChanged(StateChangedCb cb) { stateChangedCb_ = cb; }

private:
    AudioMixerEngine& engine_;
    StateChangedCb stateChangedCb_ = nullptr;
};
