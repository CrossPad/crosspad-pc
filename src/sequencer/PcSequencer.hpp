#pragma once

#include <cstdint>

class AudioMixerEngine;

namespace crosspad { class PatternSequencer; }

/// The pattern sequencer, as the board runs it: the metronome is a mixer
/// channel and the clock, the sequencer plays through the pads, and the
/// portable controller is published in PlatformServices for the apps.
crosspad::PatternSequencer& getSequencer();

namespace crosspad_pc {

/// Adds the metronome ("Click") to @p mixer on its first @p audibleOutputs
/// outputs, publishes the sequencer and starts its poll and scenes-store
/// threads. The scenes are one file, <sdcard>/crosspad/sequences.json.
void sequencer_init(AudioMixerEngine& mixer, uint8_t audibleOutputs);

} // namespace crosspad_pc
