// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PcPitchedPort.hpp
 * @brief The pitched sample engine on the desktop: one PitchedInstrument.
 *
 * The PC counterpart of platform-idf's `PitchedKitPort` — and much thinner,
 * because core's `pitchedBuildBank()` already owns everything the board's
 * version had to spell out (WAV read, rate conversion, root detection). What
 * is left here is what only a platform knows: where a kit's samples live on
 * this filesystem, where the bank's memory comes from (plain malloc), and
 * where a log line goes.
 *
 * Loader-thread code. `pitched_port_load()` reads files and allocates
 * megabytes; it is reached from the sampler's kit-load worker through
 * `platform_reloadPad()`, never from the audio thread.
 */

#include <cstdint>

namespace crosspad { class IAudioNode; class PitchedInstrument; struct KitInfo; }

namespace crosspad_pc {

/// Bring the engine up and hand back the generator node to register with the
/// mixer. @p engineRate is the rate the mixer renders at — zone PCM is
/// converted to it at load time.
crosspad::IAudioNode* pitched_port_init(uint32_t engineRate);

/// Build and install the bank described by @p kit.
///
/// A failed load leaves **nothing** installed: whatever was playing is
/// unloaded too. The caller has already moved on to the new kit's pad_notes,
/// so a surviving bank would sound its zones under the wrong note map.
bool pitched_port_load(const crosspad::KitInfo& kit);

/// Silence the engine and free the bank. Safe when nothing is loaded.
void pitched_port_unload();

/// True while a bank is installed.
bool pitched_port_active();

/// The engine itself, for the pad dispatch. Valid after pitched_port_init().
crosspad::PitchedInstrument& pitched_port_instrument();

} // namespace crosspad_pc
