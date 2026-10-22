// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcSamplerPort.hpp
 * @brief Everything the crosspad-sampler app needs from this platform.
 *
 * The desktop counterpart of the board's sampler glue in platform-idf's
 * `main/gui/gui.cpp`: the same callback table over the same engine, core's
 * `crosspad::SamplerEngine` (PcSamplerEngine here, SamplerEnginePort there).
 * It also owns the one thing the simulator had no equivalent of at all — an
 * `IKitManager`. The
 * kit-selector gate in front of an app that needs a kit is crosspad-sampler's
 * (`crosspad-sampler/logic/kit_gate.hpp`), as on the board.
 */

#include <string>

namespace crosspad { class IAudioNode; }
namespace crosspad_sampler { struct KitGateHost; }

namespace crosspad_pc {

/// Bring up the engine, register the kit manager, install the sampler's
/// platform callbacks (with the kit gate on @p gate) and make it the system
/// instrument.
/// @return the generator node to register with the mixer or audio module.
crosspad::IAudioNode* sampler_port_init(const crosspad_sampler::KitGateHost& gate);

/// Point storage at the folder mounted as the virtual SD card (empty when it
/// is unmounted) and rescan for kits. Safe to call before sampler_port_init();
/// the path is remembered and applied when the engine comes up.
void sampler_port_set_sdcard(const std::string& root);

/// Load kit @p kitId of the kit list on the kit loader's worker.
void sampler_port_load_kit(int kitId);

/// No kit current: every pad's samples wiped, its pitched bank gone (unless an
/// app holds the pitched engine) and the pads' colours cleared.
void sampler_port_unload_kit();

} // namespace crosspad_pc
