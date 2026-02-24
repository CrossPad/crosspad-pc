/**
 * @file MixerPadLogic.cpp
 * @brief Pad handler for mixer mute/solo/route toggles.
 */

#include "MixerPadLogic.hpp"
#include "AudioMixerEngine.hpp"

#include <crosspad/pad/PadManager.hpp>
#include <cstdio>

MixerPadLogic::MixerPadLogic(AudioMixerEngine& engine)
    : engine_(engine) {}

void MixerPadLogic::onActivate(crosspad::PadManager& padManager)
{
    printf("[MixerPad] Activated\n");
    updatePadColors(padManager);
}

void MixerPadLogic::onDeactivate(crosspad::PadManager& padManager)
{
    printf("[MixerPad] Deactivated\n");
    for (uint8_t i = 0; i < 16; i++) {
        padManager.setPadColor(i, 0, 0, 0);
    }
}

void MixerPadLogic::onPadPress(crosspad::PadManager& padManager,
                                uint8_t padIdx, uint8_t /*velocity*/)
{
    if (padIdx >= 16) return;

    switch (padIdx) {
    // Row 0: Channel mute (IN1=0, IN2=1, SYN=2)
    case 0: case 1: case 2: {
        auto ch = static_cast<MixerInput>(padIdx);
        engine_.setChannelMute(ch, !engine_.isChannelMuted(ch));
        break;
    }

    // Row 1: Channel solo (IN1=4, IN2=5, SYN=6)
    case 4: case 5: case 6: {
        auto ch = static_cast<MixerInput>(padIdx - 4);
        engine_.setChannelSolo(ch, !engine_.isChannelSoloed(ch));
        break;
    }

    // Row 2: Route to OUT1 (IN1=8, IN2=9, SYN=10)
    case 8: case 9: case 10: {
        auto ch = static_cast<MixerInput>(padIdx - 8);
        engine_.setRouteEnabled(ch, MixerOutput::OUT1,
                                !engine_.isRouteEnabled(ch, MixerOutput::OUT1));
        break;
    }
    // Pad 11: OUT1 mute
    case 11:
        engine_.setOutputMute(MixerOutput::OUT1,
                              !engine_.isOutputMuted(MixerOutput::OUT1));
        break;

    // Row 3: Route to OUT2 (IN1=12, IN2=13, SYN=14)
    case 12: case 13: case 14: {
        auto ch = static_cast<MixerInput>(padIdx - 12);
        engine_.setRouteEnabled(ch, MixerOutput::OUT2,
                                !engine_.isRouteEnabled(ch, MixerOutput::OUT2));
        break;
    }
    // Pad 15: OUT2 mute
    case 15:
        engine_.setOutputMute(MixerOutput::OUT2,
                              !engine_.isOutputMuted(MixerOutput::OUT2));
        break;

    default:
        break;
    }

    updatePadColors(padManager);

    if (stateChangedCb_) stateChangedCb_();
}

void MixerPadLogic::onPadRelease(crosspad::PadManager& /*padManager*/,
                                  uint8_t /*padIdx*/) {}

void MixerPadLogic::onPadPressure(crosspad::PadManager& /*padManager*/,
                                   uint8_t /*padIdx*/, uint8_t /*pressure*/) {}

void MixerPadLogic::updatePadColors(crosspad::PadManager& padManager)
{
    // Row 0: Mute status (green = active, red = muted)
    for (int i = 0; i < 3; i++) {
        auto ch = static_cast<MixerInput>(i);
        if (engine_.isChannelMuted(ch))
            padManager.setPadColor(i, 80, 0, 0);   // red
        else
            padManager.setPadColor(i, 0, 80, 0);   // green
    }
    padManager.setPadColor(3, 0, 0, 0);  // unused

    // Row 1: Solo status (yellow = soloed, dim = off)
    for (int i = 0; i < 3; i++) {
        auto ch = static_cast<MixerInput>(i);
        if (engine_.isChannelSoloed(ch))
            padManager.setPadColor(4 + i, 80, 80, 0);  // yellow
        else
            padManager.setPadColor(4 + i, 15, 15, 15);  // dim
    }
    padManager.setPadColor(7, 0, 0, 0);  // unused

    // Row 2: Route to OUT1 (cyan = enabled, dark = disabled) + OUT1 mute
    for (int i = 0; i < 3; i++) {
        auto ch = static_cast<MixerInput>(i);
        if (engine_.isRouteEnabled(ch, MixerOutput::OUT1))
            padManager.setPadColor(8 + i, 0, 60, 80);   // cyan
        else
            padManager.setPadColor(8 + i, 15, 15, 15);   // dim
    }
    if (engine_.isOutputMuted(MixerOutput::OUT1))
        padManager.setPadColor(11, 80, 0, 0);   // red
    else
        padManager.setPadColor(11, 0, 80, 0);   // green

    // Row 3: Route to OUT2 + OUT2 mute
    for (int i = 0; i < 3; i++) {
        auto ch = static_cast<MixerInput>(i);
        if (engine_.isRouteEnabled(ch, MixerOutput::OUT2))
            padManager.setPadColor(12 + i, 0, 60, 80);
        else
            padManager.setPadColor(12 + i, 15, 15, 15);
    }
    if (engine_.isOutputMuted(MixerOutput::OUT2))
        padManager.setPadColor(15, 80, 0, 0);
    else
        padManager.setPadColor(15, 0, 80, 0);
}
