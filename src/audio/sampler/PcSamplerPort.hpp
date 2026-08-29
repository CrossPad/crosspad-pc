// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PcSamplerPort.hpp
 * @brief Everything the crosspad-sampler app needs from this platform.
 *
 * The desktop counterpart of `initSamplerPlatformCallbacks()` in
 * platform-idf's `main/gui/gui.cpp`: the same callback table, filled from the
 * same `SampleStreamPlayer_*` API, over the PC sample engine instead of the
 * board's player. It also owns the two things the simulator had no equivalent
 * of at all — an `IKitManager`, and the kit-selector gate in front of an app
 * that needs a kit.
 */

#include <string>

namespace crosspad { class IAudioNode; }
namespace crosspad_gui { class ILvglApp; }
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace crosspad_pc {

/// Bring up the engine, register the kit manager, install the sampler's
/// platform callbacks and make it the system instrument.
/// @return the generator node to register with the mixer or audio module, or
///         nullptr when the engine could not start.
crosspad::IAudioNode* sampler_port_init();

/// Point storage at the folder mounted as the virtual SD card (empty when it
/// is unmounted) and rescan for kits. Safe to call before sampler_port_init();
/// the path is remembered and applied when the engine comes up.
void sampler_port_set_sdcard(const std::string& root);

/// Pre-launch gate for the app orchestrator: an app that needs a kit and has
/// none gets the kit selector first, and launches once a kit has loaded.
/// Returns true when the app may start immediately.
bool sampler_port_pre_launch(crosspad_gui::ILvglApp* app, lv_obj_t* container);

} // namespace crosspad_pc
