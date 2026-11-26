// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class AudioMixerEngine;

namespace crosspad_pc {

/// The effects bus (crosspad::IFxBus) over the simulator's mixer, as the
/// board has it (platform-idf crosspad-platform-idf/FxBusPort.cpp): registers
/// PlatformServices::fxBus. @p chMic, @p chLine12, @p chUsb are the mixer
/// channels of the mics (IN1), Line 1/2 (IN2) and the host's playback,
/// kMixerInvalidChannelId where there is none; @p captureBus is the USB rec
/// bus. The simulator has no Line 3/4 and hears both inputs at once, so it
/// has no input pairs to pick.
void fx_bus_init(AudioMixerEngine& mixer, uint8_t chMic, uint8_t chLine12, uint8_t chUsb, uint8_t captureBus);

} // namespace crosspad_pc
