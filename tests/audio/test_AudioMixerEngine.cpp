#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio_test_helpers.hpp"
#include "audio_test_stubs.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/audio/AudioInputNode.hpp>
#include <crosspad/audio/SynthEngineNode.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace crosspad_test;

namespace {

constexpr uint32_t kFrames = 64;

/// Test fixture: builds a mixer with the legacy IN1/IN2/SYNTH layout
/// (channel ids 0/1/2) so MixerInput enums stay valid. Routing starts
/// fully cleared via setDefaults(); each test enables only what it needs.
struct MixerFixture {
    AudioMixerEngine mixer;
    crosspad::AudioInputNode  in1Node{&pc_platform_get_audio_input, 0, "IN1"};
    crosspad::AudioInputNode  in2Node{&pc_platform_get_audio_input, 1, "IN2"};
    crosspad::SynthEngineNode synthNode;

    MixerFixture() {
        mixer.setup(kFrames, 48000, 2);
        mixer.addChannel(&in1Node,   "IN1");
        mixer.addChannel(&in2Node,   "IN2");
        mixer.addChannel(&synthNode, "SYNTH");
        mixer.setDefaults();
    }

    void setSynth(crosspad::ISynthEngine* s) { synthNode.setEngine(s); }

    void render(std::vector<float>& out0, std::vector<float>& out1) {
        float* buses[2] = { out0.data(), out1.data() };
        mixer.render(buses, 2, kFrames);
    }
};

} // namespace

TEST_CASE("AudioMixerEngine: setDefaults clears all routes", "[audio][mixer]") {
    MixerFixture f;

    REQUIRE_FALSE(f.mixer.isRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1));
    REQUIRE_FALSE(f.mixer.isRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1));
    REQUIRE_FALSE(f.mixer.isRouteEnabled(MixerInput::IN2,   MixerOutput::OUT1));

    REQUIRE(f.mixer.getChannelVolume(MixerInput::SYNTH) == 1.0f);
    REQUIRE(f.mixer.getOutputVolume(MixerOutput::OUT1)  == 1.0f);
}

TEST_CASE("AudioMixerEngine: render synth-only into OUT1", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);

    ConstSynth synth(0.25f);
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    REQUIRE(synth.processCalls == 1);
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.25f, 1e-6f));
    for (float v : out1) REQUIRE_THAT(v, WithinAbs(0.0f,  1e-6f));
}

TEST_CASE("AudioMixerEngine: route disabled → output silent", "[audio][mixer]") {
    MixerFixture f;  // setDefaults already cleared all routes

    ConstSynth synth(0.5f);
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 1.0f);  // pre-poison
    std::vector<float> out1(kFrames * 2, 1.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE(v == 0.0f);  // mixer clears outputs
    for (float v : out1) REQUIRE(v == 0.0f);
}

TEST_CASE("AudioMixerEngine: per-route volume scales mix", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setRouteVolume(MixerInput::SYNTH, MixerOutput::OUT1, 0.5f);

    ConstSynth synth(0.4f);
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("AudioMixerEngine: multi-route mixing (SYNTH + IN1 → OUT1)", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1, true);
    f.mixer.setRouteVolume(MixerInput::IN1,    MixerOutput::OUT1, 0.5f);
    f.mixer.setRouteVolume(MixerInput::SYNTH,  MixerOutput::OUT1, 1.0f);

    ConstSynth synth(0.25f);
    ConstAudioInput in1(16384);  // = 0.5 float
    f.setSynth(&synth);
    setTestAudioInput(0, &in1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    // synth: 0.25 * 1.0 = 0.25
    // in1:   0.5  * 0.5 = 0.25
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.5f, 1e-3f));

    clearTestAudioInputs();
}

TEST_CASE("AudioMixerEngine: output volume halves bus", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setOutputVolume(MixerOutput::OUT1, 0.5f);

    ConstSynth synth(0.4f);
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("AudioMixerEngine: output mute → silent regardless of routes", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setOutputMute(MixerOutput::OUT1, true);

    ConstSynth synth(0.5f);
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE(v == 0.0f);
}

TEST_CASE("AudioMixerEngine: solo channel mutes others", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1, true);
    f.mixer.setRouteVolume(MixerInput::IN1,    MixerOutput::OUT1, 1.0f);
    // Solo SYNTH — IN1 must be silent in mix
    f.mixer.setChannelSolo(MixerInput::SYNTH, true);

    ConstSynth synth(0.25f);
    ConstAudioInput in1(16384);  // 0.5
    f.setSynth(&synth);
    setTestAudioInput(0, &in1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.25f, 1e-6f));
    clearTestAudioInputs();
}

TEST_CASE("AudioMixerEngine: channel mute drops contribution", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setRouteEnabled(MixerInput::IN1,   MixerOutput::OUT1, true);
    f.mixer.setChannelMute(MixerInput::SYNTH, true);

    ConstSynth synth(0.5f);
    ConstAudioInput in1(16384);
    f.setSynth(&synth);
    setTestAudioInput(0, &in1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    // Only IN1 → 0.5 * 1.0 = 0.5
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.5f, 1e-3f));
    clearTestAudioInputs();
}

TEST_CASE("AudioMixerEngine: clamp on overdrive", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.mixer.setRouteVolume(MixerInput::SYNTH, MixerOutput::OUT1, 4.0f);

    ConstSynth synth(0.5f);  // 0.5 * 4.0 = 2.0 → clamps to 1.0
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE(v == 1.0f);
}

TEST_CASE("AudioMixerEngine: tap buffer captures OUT1 int16", "[audio][mixer][tap]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);

    ConstSynth synth(0.5f);
    f.setSynth(&synth);

    crosspad::AudioRingBuffer<int16_t> tap(kFrames * 2 * 4);
    f.mixer.setTapBuffer(&tap);
    f.mixer.setTapOutput(MixerOutput::OUT1);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    REQUIRE(tap.available() == kFrames * 2);
    std::vector<int16_t> readBuf(kFrames * 2);
    REQUIRE(tap.read(readBuf.data(), kFrames * 2) == kFrames * 2);

    const int16_t expected = static_cast<int16_t>(0.5f * 32767.0f);
    for (int16_t v : readBuf) REQUIRE(std::abs(static_cast<int>(v - expected)) <= 1);

    f.mixer.setTapBuffer(nullptr);
}

TEST_CASE("AudioMixerEngine: peak meter populates output level", "[audio][mixer][meter]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);

    ConstSynth synth(0.25f);
    f.setSynth(&synth);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    f.render(out0, out1);

    int16_t peakL = 0, peakR = 0;
    f.mixer.getOutputLevel(MixerOutput::OUT1, peakL, peakR);
    const int16_t expected = static_cast<int16_t>(0.25f * 32767.0f);
    REQUIRE(std::abs(static_cast<int>(peakL - expected)) <= 1);
    REQUIRE(std::abs(static_cast<int>(peakR - expected)) <= 1);
}

TEST_CASE("AudioMixerEngine: null synth → silence", "[audio][mixer]") {
    MixerFixture f;
    f.mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    f.setSynth(nullptr);
    clearTestAudioInputs();

    std::vector<float> out0(kFrames * 2, 9.0f);
    std::vector<float> out1(kFrames * 2, 9.0f);
    f.render(out0, out1);

    for (float v : out0) REQUIRE(v == 0.0f);
    for (float v : out1) REQUIRE(v == 0.0f);
}

TEST_CASE("AudioMixerEngine: dynamic addChannel registers a new sound source", "[audio][mixer][dynamic]") {
    AudioMixerEngine mixer;
    mixer.setup(kFrames, 48000, 2);
    mixer.setDefaults();

    REQUIRE(mixer.numActiveChannels() == 0);

    ConstSynth synthA(0.1f);
    ConstSynth synthB(0.2f);
    crosspad::SynthEngineNode nodeA(&synthA);
    crosspad::SynthEngineNode nodeB(&synthB);

    auto chA = mixer.addChannel(&nodeA, "A");
    auto chB = mixer.addChannel(&nodeB, "B");
    REQUIRE(chA == 0);
    REQUIRE(chB == 1);
    REQUIRE(mixer.numActiveChannels() == 2);
    REQUIRE(std::string(mixer.getChannelName(chA)) == "A");

    mixer.setRouteEnabled(chA, 0, true);
    mixer.setRouteEnabled(chB, 0, true);

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    float* buses[2] = { out0.data(), out1.data() };
    mixer.render(buses, 2, kFrames);

    // 0.1 + 0.2 = 0.3 (no clamp needed)
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.3f, 1e-6f));

    mixer.removeChannel(chB);
    REQUIRE(mixer.numActiveChannels() == 1);
    REQUIRE_FALSE(mixer.isChannelActive(chB));

    // Re-render — only A contributes now
    std::memset(out0.data(), 0, out0.size() * sizeof(float));
    std::memset(out1.data(), 0, out1.size() * sizeof(float));
    mixer.render(buses, 2, kFrames);
    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.1f, 1e-6f));
}

TEST_CASE("AudioMixerEngine: removed slot is reusable", "[audio][mixer][dynamic]") {
    AudioMixerEngine mixer;
    mixer.setup(kFrames, 48000, 2);
    mixer.setDefaults();

    ConstSynth s1(0.1f);
    crosspad::SynthEngineNode n1(&s1);
    auto ch1 = mixer.addChannel(&n1, "first");
    REQUIRE(ch1 == 0);

    mixer.removeChannel(ch1);

    ConstSynth s2(0.2f);
    crosspad::SynthEngineNode n2(&s2);
    auto ch2 = mixer.addChannel(&n2, "second");
    REQUIRE(ch2 == 0);  // first slot reclaimed
}

TEST_CASE("AudioMixerEngine: routes to multiple outputs simultaneously", "[audio][mixer][multibus]") {
    AudioMixerEngine mixer;
    mixer.setup(kFrames, 48000, 2);
    mixer.setDefaults();

    ConstSynth synth(0.4f);
    crosspad::SynthEngineNode node(&synth);
    auto ch = mixer.addChannel(&node, "synth");

    mixer.setRouteEnabled(ch, 0, true);
    mixer.setRouteEnabled(ch, 1, true);
    mixer.setRouteVolume (ch, 1, 0.5f);  // half on OUT2

    std::vector<float> out0(kFrames * 2, 0.0f);
    std::vector<float> out1(kFrames * 2, 0.0f);
    float* buses[2] = { out0.data(), out1.data() };
    mixer.render(buses, 2, kFrames);

    for (float v : out0) REQUIRE_THAT(v, WithinAbs(0.4f, 1e-6f));
    for (float v : out1) REQUIRE_THAT(v, WithinAbs(0.2f, 1e-6f));
}
