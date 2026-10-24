#pragma once

#include <cstdint>
#include <string>

class AudioMixerEngine;

namespace crosspad { class PatternSequencer; }

/// The pattern sequencer, as the board runs it: the metronome is a mixer
/// channel and the clock, the sequencer plays through the pads, and the
/// portable controller is published in PlatformServices for the apps.
crosspad::PatternSequencer& getSequencer();

namespace crosspad_pc {

/// Adds the metronome ("Click") to @p mixer on its first @p audibleOutputs
/// outputs, publishes the sequencer and starts its poll and scenes-store
/// threads. The scenes are the open song's, <song>/sequences.json
/// (crosspad/song/SongScenes.hpp).
void sequencer_init(AudioMixerEngine& mixer, uint8_t audibleOutputs);

/// A kit became current (its kit.json path): only a card that never had
/// shared scenes reads anything of it.
void sequencer_kit_changed(const std::string& kitJsonPath);

/// The board's bare SEQ reply (platform-idf hil_control.cpp), same fields in
/// the same order, for the remote's seq verb.
std::string sequencer_report();

} // namespace crosspad_pc
