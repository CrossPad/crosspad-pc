// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class AudioMixerEngine;

namespace crosspad {
class IAudioInput;
}

namespace crosspad_pc {

/// The sample tools on PC (crosspad/sampletools/SampleToolsEngine), behind
/// the Recorder and the Slicer: adds the "Preview" channel to @p mixer on the
/// first @p audibleOutputs outputs and publishes it as
/// PlatformServices::sampleTools. Takes go to <sdcard_path>/crosspad/recordings.
void sample_tools_init(AudioMixerEngine& mixer, uint8_t audibleOutputs);

/// The audio thread's capture-bus tap (PcAudioModule::setCaptureTap), after
/// the mixer: the pads bus a PADS take records.
void sample_tools_rt_bus(const float* bus, uint32_t frames);

/// IN1 (@p index 0, the Recorder's MIC) or IN2 (1, its LINE) as the mixer's
/// input channels read it -- pc_platform_get_audio_input(), with every block
/// read also handed to the sample tools. Never null.
crosspad::IAudioInput* sample_tools_input(int index);

} // namespace crosspad_pc
