#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio_test_helpers.hpp"
#include "audio_test_stubs.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace crosspad_test;

namespace {

/// RAII fixture: installs the synth into PlatformServices for the test
/// duration, restores nullptr on destruction so subsequent tests are clean.
struct SynthScope {
    explicit SynthScope(crosspad::ISynthEngine* s) {
        crosspad::getPlatformServices().setSynthEngine(s);
    }
    ~SynthScope() {
        crosspad::getPlatformServices().setSynthEngine(nullptr);
        clearTestAudioInputs();
    }
};

constexpr uint32_t kFrames = 64;

void clearBuffers(std::vector<float>& a, std::vector<float>& b) {
    std::memset(a.data(), 0, a.size() * sizeof(float));
    std::memset(b.data(), 0, b.size() * sizeof(float));
}

} // namespace

TEST_CASE("AudioMixerEngine: defaults route SYNTH→OUT1 only", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();

    REQUIRE(mixer.isRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1));
    REQUIRE_FALSE(mixer.isRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT2));
    REQUIRE_FALSE(mixer.isRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1));
    REQUIRE_FALSE(mixer.isRouteEnabled(MixerInput::IN2,   MixerOutput::OUT1));

    REQUIRE(mixer.getChannelVolume(MixerInput::SYNTH) == 1.0f);
    REQUIRE(mixer.getOutputVolume(MixerOutput::OUT1)  == 1.0f);
}

TEST_CASE("AudioMixerEngine: render synth-only into OUT1", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();

    ConstSynth synth(0.25f);
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);

    mixer.render(out0.data(), out1.data(), kFrames);

    REQUIRE(synth.processCalls == 1);
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.25f, 1e-6f));
    for (float v : out1) REQUIRE_THAT(v, WithinAbs(0.0f,  1e-6f));
}

TEST_CASE("AudioMixerEngine: route disabled → output silent", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, false);

    ConstSynth synth(0.5f);
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 1.0f);  // pre-poison
    std::vector<float> out1(kFrames * 2, 1.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    for (float v : out0) REQUIRE(v == 0.0f);  // mixer clears outputs
    for (float v : out1) REQUIRE(v == 0.0f);
}

TEST_CASE("AudioMixerEngine: per-route volume scales mix", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setRouteVolume(MixerInput::SYNTH, MixerOutput::OUT1, 0.5f);

    ConstSynth synth(0.4f);
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    // 0.4 * 0.5 = 0.2
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("AudioMixerEngine: multi-route mixing (SYNTH + IN1 → OUT1)", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setRouteEnabled(MixerInput::IN1, MixerOutput::OUT1, true);
    mixer.setRouteVolume(MixerInput::IN1, MixerOutput::OUT1, 0.5f);
    mixer.setRouteVolume(MixerInput::SYNTH, MixerOutput::OUT1, 1.0f);

    ConstSynth synth(0.25f);
    ConstAudioInput in1(16384);  // = 0.5 float
    SynthScope scope(&synth);
    setTestAudioInput(0, &in1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    // synth: 0.25 * 1.0 = 0.25
    // in1:   0.5  * 0.5 = 0.25
    // sum: 0.5
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("AudioMixerEngine: output volume halves bus", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setOutputVolume(MixerOutput::OUT1, 0.5f);

    ConstSynth synth(0.4f);
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("AudioMixerEngine: output mute → silent regardless of routes", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setOutputMute(MixerOutput::OUT1, true);

    ConstSynth synth(0.5f);
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    for (float v : out0) REQUIRE(v == 0.0f);
}

TEST_CASE("AudioMixerEngine: solo channel mutes others", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setRouteEnabled(MixerInput::IN1, MixerOutput::OUT1, true);
    mixer.setRouteVolume(MixerInput::IN1, MixerOutput::OUT1, 1.0f);
    // Solo SYNTH — IN1 should be muted in mix even though its route is enabled
    mixer.setChannelSolo(MixerInput::SYNTH, true);

    ConstSynth synth(0.25f);
    ConstAudioInput in1(16384);  // 0.5
    SynthScope scope(&synth);
    setTestAudioInput(0, &in1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("AudioMixerEngine: channel mute drops contribution", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setRouteEnabled(MixerInput::IN1, MixerOutput::OUT1, true);
    mixer.setChannelMute(MixerInput::SYNTH, true);

    ConstSynth synth(0.5f);
    ConstAudioInput in1(16384);
    SynthScope scope(&synth);
    setTestAudioInput(0, &in1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    // Only IN1 → 0.5 * 1.0 = 0.5 (channel vol 1.0, route vol 1.0)
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("AudioMixerEngine: clamp on overdrive", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    mixer.setRouteVolume(MixerInput::SYNTH, MixerOutput::OUT1, 4.0f);

    ConstSynth synth(0.5f);  // 0.5 * 4.0 = 2.0 → clamps to 1.0
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    for (float v : out0) REQUIRE(v == 1.0f);
}

TEST_CASE("AudioMixerEngine: tap buffer captures OUT1 int16", "[audio][mixer][tap]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();

    ConstSynth synth(0.5f);
    SynthScope scope(&synth);

    crosspad::AudioRingBuffer<int16_t> tap(kFrames * 2 * 4);
    mixer.setTapBuffer(&tap);
    mixer.setTapOutput(MixerOutput::OUT1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    REQUIRE(tap.available() == kFrames * 2);
    std::vector<int16_t> readBuf(kFrames * 2);
    REQUIRE(tap.read(readBuf.data(), kFrames * 2) == kFrames * 2);

    const int16_t expected = static_cast<int16_t>(0.5f * 32767.0f);
    for (int16_t v : readBuf) REQUIRE(std::abs(static_cast<int>(v - expected)) <= 1);

    mixer.setTapBuffer(nullptr);
}

TEST_CASE("AudioMixerEngine: peak meter populates output level", "[audio][mixer][meter]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();

    ConstSynth synth(0.25f);
    SynthScope scope(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    int16_t peakL = 0, peakR = 0;
    mixer.getOutputLevel(MixerOutput::OUT1, peakL, peakR);
    const int16_t expected = static_cast<int16_t>(0.25f * 32767.0f);
    REQUIRE(std::abs(static_cast<int>(peakL - expected)) <= 1);
    REQUIRE(std::abs(static_cast<int>(peakR - expected)) <= 1);
}

TEST_CASE("AudioMixerEngine: null synth → silence", "[audio][mixer]") {
    AudioMixerEngine mixer;
    mixer.setDefaults();
    SynthScope scope(nullptr);
    clearTestAudioInputs();

    std::vector<float> out0(kFrames * 2, 9.0f);
    std::vector<float> out1(kFrames * 2, 9.0f);
    mixer.render(out0.data(), out1.data(), kFrames);

    for (float v : out0) REQUIRE(v == 0.0f);
    for (float v : out1) REQUIRE(v == 0.0f);
}
