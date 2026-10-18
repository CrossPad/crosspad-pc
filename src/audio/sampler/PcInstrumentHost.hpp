// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcInstrumentHost.hpp
 * @brief The desktop's IInstrumentHost: an app borrows the pitched engine.
 *
 * The PC counterpart of platform-idf's instrument_host, over PcPitchedPort.
 * There is no arena to hand over here -- the kits' streamer and the pitched
 * bank live side by side in the process heap -- so taking the engine only
 * means the kits leave the pitched bank alone: a pitched kit's load does not
 * build its bank and its pads do not play while an app holds it. release()
 * reloads the current kit, which brings its bank back.
 *
 * Open and load are supported; save is not (nothing on the desktop makes
 * an instrument), so requestSave() is refused.
 */

namespace crosspad_pc {

/// Start the worker and publish PlatformServices::instrumentHost. Once,
/// after pitched_port_init() and sampler_port_init().
void instrument_host_init();

/// True from an app's first load until its release() has been carried out.
bool instrument_host_owns_engine();

} // namespace crosspad_pc
