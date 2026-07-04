// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file audio_test_stubs.hpp
 * @brief Test-only injection points for pc_platform audio globals.
 *
 * AudioMixerEngine.cpp pulls inputs via pc_platform_get_audio_input() and
 * the synth via pc_platform_get_synth_engine(). For tests we:
 *   - Inject IAudioInput mocks directly via setTestAudioInput(idx, ptr)
 *   - Inject ISynthEngine via crosspad::getPlatformServices().setSynthEngine()
 */

#include <crosspad/synth/IAudioInput.hpp>

namespace crosspad_test {

void setTestAudioInput(int index, crosspad::IAudioInput* input);
void clearTestAudioInputs();

} // namespace crosspad_test

// Test-only stub of the PC platform getter; AudioInputNode resolves IAudioInput
// through this every render so hot-swapped mocks take effect immediately.
extern "C++" crosspad::IAudioInput* pc_platform_get_audio_input(int index);
