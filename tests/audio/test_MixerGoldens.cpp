// SPDX-License-Identifier: MIT
//
// Tier 3 — Golden audio E2E for the mixer pipeline.
//
// Strategy: drive AudioMixerEngine with a deterministic SineSynth and mock
// IAudioInput so the captured tap buffer is byte-identical across runs.
// Compare against committed reference WAVs in tests/golden/.
//
// Set CROSSPAD_UPDATE_GOLDENS=1 in the environment to regenerate the WAVs.
// Review the diff before committing — a change here should always be the
// result of an intentional pipeline change, never a flaky test.

#include <catch2/catch_test_macros.hpp>

#include "audio_test_helpers.hpp"
#include "audio_test_stubs.hpp"
#include "wav_io.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/audio/AudioInputNode.hpp>
#include <crosspad/audio/SynthEngineNode.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace crosspad_test;

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFrames     = 128;          // chunk size
constexpr uint32_t kTotal      = 48000;        // 1 second @ 48kHz
static_assert(kTotal % kFrames == 0, "kTotal must be a multiple of kFrames");

constexpr int kMaxAbsDiff = 1;

bool shouldUpdateGoldens() {
    const char* v = std::getenv("CROSSPAD_UPDATE_GOLDENS");
    return v && v[0] == '1';
}

std::filesystem::path goldenDir() {
    auto here = std::filesystem::path(__FILE__).parent_path();   // tests/audio/
    return here.parent_path() / "golden";                         // tests/golden/
}

/// Predefined IN1/IN2/SYNTH layout — same as PC bootstrap so MixerInput
/// enums (IN1=0, IN2=1, SYNTH=2) stay valid.
struct GoldenFixture {
    AudioMixerEngine mixer;
    crosspad::AudioInputNode  in1Node{&pc_platform_get_audio_input, 0, "IN1"};
    crosspad::AudioInputNode  in2Node{&pc_platform_get_audio_input, 1, "IN2"};
    crosspad::SynthEngineNode synthNode;

    GoldenFixture() {
        mixer.setup(kFrames, kSampleRate, 2);
        mixer.addChannel(&in1Node,   "IN1");
        mixer.addChannel(&in2Node,   "IN2");
        mixer.addChannel(&synthNode, "SYNTH");
        mixer.setDefaults();
    }

    void setSynth(crosspad::ISynthEngine* s) { synthNode.setEngine(s); }
};

/// Drive the mixer for kTotal frames in kFrames chunks; return tap (OUT1 int16).
std::vector<int16_t> renderViaTap(AudioMixerEngine& mixer) {
    crosspad::AudioRingBuffer<int16_t> tap(kTotal * 2 * 2);
    mixer.setTapBuffer(&tap);
    mixer.setTapOutput(MixerOutput::OUT1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    float* buses[2] = { out0.data(), out1.data() };

    const uint32_t iters = kTotal / kFrames;
    for (uint32_t i = 0; i < iters; ++i) {
        mixer.render(buses, 2, kFrames);
    }

    std::vector<int16_t> samples(kTotal * 2, 0);
    const size_t got = tap.read(samples.data(), kTotal * 2);
    samples.resize(got);

    mixer.setTapBuffer(nullptr);
    return samples;
}

void assertGolden(const std::string& name, std::vector<int16_t>& actual) {
    const auto dir = goldenDir();
    std::filesystem::create_directories(dir);
    const auto path = (dir / (name + ".wav")).string();

    WavFile wav;
    wav.sampleRate = kSampleRate;
    wav.channels   = 2;
    wav.samples    = actual;

    if (shouldUpdateGoldens() || !std::filesystem::exists(path)) {
        REQUIRE(wavWrite(path, wav));
        WARN("Golden " << path << " written/updated. Inspect & commit.");
        return;
    }

    WavFile ref;
    REQUIRE(wavRead(path, ref));
    REQUIRE(ref.sampleRate == kSampleRate);
    REQUIRE(ref.channels   == 2);
    REQUIRE(ref.samples.size() == actual.size());

    const int worst = wavMaxAbsDiff(ref.samples, actual);
    INFO("worst sample diff = " << worst << " (tol=" << kMaxAbsDiff << ")");
    REQUIRE(worst <= kMaxAbsDiff);
}

} // namespace

TEST_CASE("Golden: mixer_sine_440_1s — SYNTH→OUT1, 440Hz sine", "[audio][golden][mixer]") {
    GoldenFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);

    SineSynth synth(440.0f, 0.5f, kSampleRate);
    f.setSynth(&synth);

    auto samples = renderViaTap(f.mixer);
    assertGolden("mixer_sine_440_1s", samples);
}

TEST_CASE("Golden: mixer_sine_plus_silent_input — regression for second-route noise",
          "[audio][golden][mixer]") {
    GoldenFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1, true);
    f.mixer.setRouteVolume (MixerInput::IN1,   MixerOutput::OUT1, 1.0f);

    SineSynth synth(440.0f, 0.5f, kSampleRate);
    ConstAudioInput silentInput(0);
    f.setSynth(&synth);
    setTestAudioInput(0, &silentInput);

    auto samples = renderViaTap(f.mixer);
    assertGolden("mixer_sine_plus_silent_input", samples);
    clearTestAudioInputs();
}

TEST_CASE("Golden: mixer_outvol_half — output volume halves", "[audio][golden][mixer]") {
    GoldenFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setOutputVolume(MixerOutput::OUT1, 0.5f);

    SineSynth synth(440.0f, 0.5f, kSampleRate);
    f.setSynth(&synth);

    auto samples = renderViaTap(f.mixer);
    assertGolden("mixer_outvol_half", samples);
}

TEST_CASE("Golden: mixer_in1_const_to_out1 — input-only path", "[audio][golden][mixer]") {
    GoldenFixture f;
    f.mixer.setRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1, true);
    f.mixer.setRouteVolume (MixerInput::IN1,   MixerOutput::OUT1, 1.0f);

    ConstAudioInput in1(8000);   // ≈ 0.244
    setTestAudioInput(0, &in1);

    auto samples = renderViaTap(f.mixer);
    assertGolden("mixer_in1_const_to_out1", samples);
    clearTestAudioInputs();
}
