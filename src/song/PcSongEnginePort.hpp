#pragma once

#include <cstdint>

class AudioMixerEngine;

namespace crosspad_pc {

/// The song engine on PC (crosspad/song/SongEngine): adds its "Song" channel
/// to @p mixer on the first @p audibleOutputs outputs, publishes it as
/// PlatformServices::songEngine, and records bounces from @p captureBus.
/// Needs the sequencer up first (sequencer_init).
void song_engine_init(AudioMixerEngine& mixer, uint8_t audibleOutputs, uint8_t captureBus);

/// The audio thread's capture-bus tap (PcAudioModule::setCaptureTap).
void song_engine_rt_capture(const float* bus, uint32_t frames);

} // namespace crosspad_pc
