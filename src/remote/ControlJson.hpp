// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ControlJson.hpp
 * @brief The control port's JSON of the verbs whose state crosspad-core
 *        gathers (crosspad/control/) -- the simulator's side of the board's
 *        CDC verbs of the same names. No SDL, no LVGL: the replies the MCP
 *        and the PC tools parse are pinned by tests/test_control_json.cpp.
 */

#include <cstdint>
#include <string>

#include "crosspad/control/AudioLevel.hpp"
#include "crosspad/control/KitList.hpp"
#include "crosspad/control/KitStatus.hpp"
#include "crosspad/control/LedState.hpp"
#include "crosspad/control/MixerLevels.hpp"
#include "crosspad/control/PitchedStatus.hpp"
#include "crosspad/control/WaveStatus.hpp"

namespace remote {

/// kit_status: count, current, name, parsed, loading.
std::string controlJsonKitStatus(const crosspad::control::KitStatus& st);

/// One element of kit_list's "kits" array: id, name, parsed.
std::string controlJsonKitRow(const crosspad::control::KitListRow& row);
/// The whole kit_list reply around those rows, comma-joined.
std::string controlJsonKitList(int count, int current, const std::string& kits);

/// audio_level: @p stream as the request gave it, the level read for it.
std::string controlJsonAudioLevel(int stream, const crosspad::control::AudioLevel& lv);

/// led_state: brightness, animating, coalesce, colors.
std::string controlJsonLedState(const crosspad::control::LedState& st);

/// One element of mix_lvl's "channels" array.
std::string controlJsonMixChannel(const crosspad::control::MixerChannelLevel& row);
/// One element of mix_lvl's "out" array.
std::string controlJsonMixOutput(const crosspad::control::MixerOutputLevel& row);
/// The whole mix_lvl reply around the rows above, each list comma-joined.
std::string controlJsonMixLevels(uint8_t outputs, const std::string& channels, const std::string& outs);

/// pitched_status: active, zones, roots, voices, steals, last note, bank size.
std::string controlJsonPitchedStatus(const crosspad::control::PitchedStatus& st);

/// wave_status: the loader's running flag and counters.
std::string controlJsonWaveStatus(const crosspad::control::WaveStatus& st);

} // namespace remote
