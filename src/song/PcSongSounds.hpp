// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <crosspad/kit/KitInfo.hpp>

#include <functional>

/**
 * @file PcSongSounds.hpp
 * @brief The open song's kit and Pitch instrument on PC: core's SongSounds
 *        (crosspad/song/SongSounds.hpp) over the sampler port, the instrument
 *        host and the toast, as platform-idf's song_sounds does on the board.
 */

namespace crosspad_pc {

/// LVGL thread, after song_engine_init(), sampler_port_init() and
/// instrument_host_init(): every song that opens from now on is followed,
/// and the one open now is the first.
void song_sounds_start();

/// LVGL thread, from a kit load that finished: the open song takes the kit.
void song_sounds_kit_loaded();

/// The instrument host's worker, after a request that succeeded: the open
/// song's Pitch track keeps @p spec (SongSounds::keepInstrument).
void song_sounds_keep_instrument(const crosspad::KitInfo& spec,
                                 const std::function<bool(const crosspad::KitInfo&)>& save);

} // namespace crosspad_pc
