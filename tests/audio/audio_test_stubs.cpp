// SPDX-License-Identifier: MIT
//
// Test-only stubs for pc_platform globals consumed by AudioMixerEngine.cpp.
// Real implementations live in src/pc_stubs/PcPlatformStubs.cpp, which we
// don't pull into the test binary. Keep these in sync with the real signatures.

#include "audio_test_stubs.hpp"
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_test {

static crosspad::IAudioInput* s_audioInputs[2] = {nullptr, nullptr};

void setTestAudioInput(int index, crosspad::IAudioInput* input) {
    if (index >= 0 && index < 2) s_audioInputs[index] = input;
}

void clearTestAudioInputs() {
    s_audioInputs[0] = nullptr;
    s_audioInputs[1] = nullptr;
}

} // namespace crosspad_test

// ── Symbols expected by AudioMixerEngine.cpp ─────────────────────────────

crosspad::IAudioInput* pc_platform_get_audio_input(int index) {
    if (index >= 0 && index < 2) return crosspad_test::s_audioInputs[index];
    return nullptr;
}

crosspad::ISynthEngine* pc_platform_get_synth_engine() {
    return crosspad::getPlatformServices().synthEngine;
}
