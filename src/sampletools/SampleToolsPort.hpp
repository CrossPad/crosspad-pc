// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

class AudioMixerEngine;

namespace crosspad {

/// Recording, previewing and slicing samples (crosspad::ISampleTools) on this
/// board. Registers the "Preview" mixer channel and PlatformServices::sampleTools.
void sample_tools_init(AudioMixerEngine& mixer);

/// RT: one block of a codec's ADC (0 = mics' codec, 1 = line-in's), from
/// wherever this block's read happened -- the IN1/IN2 mixer channels, or the
/// USB bridge's tap while the USB audio profile owns the ADCs.
void sample_tools_rt_input(int codec, const int16_t* block, uint32_t frames);

/// RT: a render block begins -- before any of it is read or mixed, so a take
/// can tell which of its frames came in with the block the song started in.
void sample_tools_rt_block_begin();

/// RT: the pads-only bus of this block, for a resample, after the mixer:
/// also where a take armed with the song sees the song start.
void sample_tools_rt_bus(const float* padsBus, uint32_t frames);

/// RT: whether this block's ADC of @p codec is wanted -- the USB profile reads
/// only the codecs something asks for.
bool sample_tools_wants_codec(int codec);

} // namespace crosspad
