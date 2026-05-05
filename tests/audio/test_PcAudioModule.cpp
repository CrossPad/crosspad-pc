#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio_test_helpers.hpp"
#include "audio_test_stubs.hpp"

#include "audio/PcAudioModule.hpp"
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/audio/IAudioNode.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <vector>

using Catch::Matchers::WithinAbs;
using namespace crosspad_test;
using crosspad_pc::PcAudioModule;

namespace {

/// Test subclass that returns mock streams instead of PcRtAudioOutputStream.
/// Lets us drive process() without RtAudio devices.
class TestablePcAudioModule : public PcAudioModule {
public:
    crosspad::IAudioStream* getOutputStream(uint8_t i) override {
        if (i == 0) return s0;
        if (i == 1) return s1;
        return nullptr;
    }
    CapturingInt16Stream* s0 = nullptr;
    CapturingInt16Stream* s1 = nullptr;
};

constexpr uint32_t kFrames = 64;

class ConstGenerator : public crosspad::IAudioNode {
public:
    explicit ConstGenerator(float v) : value_(v) {}
    const char* name() const override { return "ConstGen"; }
    bool isGenerator() const override { return true; }
    void mix(float* out, uint32_t frames) override {
        for (uint32_t i = 0; i < frames * 2; ++i) out[i] += value_;
    }
private:
    float value_;
};

} // namespace

TEST_CASE("PcAudioModule: setup allocates per-stream buses", "[audio][pc-module]") {
    TestablePcAudioModule m;
    crosspad::AudioModuleConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameCount = kFrames;
    REQUIRE(m.setup(cfg));
    REQUIRE(m.getConfig().frameCount == kFrames);
}

TEST_CASE("PcAudioModule: default node-chain process pushes to both streams",
          "[audio][pc-module]") {
    TestablePcAudioModule m;
    CapturingInt16Stream s0, s1;
    m.s0 = &s0; m.s1 = &s1;

    crosspad::AudioModuleConfig cfg;
    cfg.frameCount = kFrames;
    REQUIRE(m.setup(cfg));

    ConstGenerator gen(0.25f);
    m.addNode(&gen);

    m.process();   // single tick, no thread

    REQUIRE(s0.pushes == 1);
    REQUIRE(s1.pushes == 1);
    const int16_t expected = static_cast<int16_t>(0.25f * 32767.0f);
    for (int16_t v : s0.captured) REQUIRE(std::abs(static_cast<int>(v - expected)) <= 1);
    for (int16_t v : s1.captured) REQUIRE(std::abs(static_cast<int>(v - expected)) <= 1);
}

TEST_CASE("PcAudioModule: closed stream is skipped", "[audio][pc-module]") {
    TestablePcAudioModule m;
    CapturingInt16Stream s0, s1;
    s1.open = false;
    m.s0 = &s0; m.s1 = &s1;

    crosspad::AudioModuleConfig cfg;
    cfg.frameCount = kFrames;
    REQUIRE(m.setup(cfg));
    ConstGenerator gen(0.5f);
    m.addNode(&gen);

    m.process();
    REQUIRE(s0.pushes == 1);
    REQUIRE(s1.pushes == 0);
}

TEST_CASE("PcAudioModule: mixer override path renders via mixer.render",
          "[audio][pc-module][mixer]") {
    TestablePcAudioModule m;
    CapturingInt16Stream s0, s1;
    m.s0 = &s0; m.s1 = &s1;

    AudioMixerEngine mixer;
    mixer.setDefaults();   // SYNTH→OUT1 only

    ConstSynth synth(0.5f);
    crosspad::getPlatformServices().setSynthEngine(&synth);
    clearTestAudioInputs();

    m.setMixerEngine(&mixer);

    crosspad::AudioModuleConfig cfg;
    cfg.frameCount = kFrames;
    REQUIRE(m.setup(cfg));

    m.process();

    // OUT1: synth at 0.5 → int16 ≈ 16383
    REQUIRE(s0.pushes == 1);
    REQUIRE(s1.pushes == 1);
    const int16_t expected0 = static_cast<int16_t>(0.5f * 32767.0f);
    for (int16_t v : s0.captured) REQUIRE(std::abs(static_cast<int>(v - expected0)) <= 1);
    // OUT2: silent — only SYNTH→OUT1 is enabled
    for (int16_t v : s1.captured) REQUIRE(v == 0);

    crosspad::getPlatformServices().setSynthEngine(nullptr);
    m.setMixerEngine(nullptr);
}

TEST_CASE("PcAudioModule: peak meter tracks bus0 in mixer mode",
          "[audio][pc-module][mixer]") {
    TestablePcAudioModule m;
    CapturingInt16Stream s0, s1;
    m.s0 = &s0; m.s1 = &s1;

    AudioMixerEngine mixer;
    mixer.setDefaults();
    ConstSynth synth(0.4f);
    crosspad::getPlatformServices().setSynthEngine(&synth);
    m.setMixerEngine(&mixer);

    crosspad::AudioModuleConfig cfg;
    cfg.frameCount = kFrames;
    REQUIRE(m.setup(cfg));
    m.process();

    float l = 0, r = 0;
    m.getOutputLevel(0, l, r);
    REQUIRE_THAT(l, WithinAbs(0.4f, 1e-3f));
    REQUIRE_THAT(r, WithinAbs(0.4f, 1e-3f));

    crosspad::getPlatformServices().setSynthEngine(nullptr);
    m.setMixerEngine(nullptr);
}
