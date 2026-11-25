#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio_test_helpers.hpp"

#include "audio/PcFxBus.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/audio/AudioInputNode.hpp>
#include <crosspad/audio/IFxBus.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <vector>

using Catch::Matchers::WithinAbs;
using crosspad::FxOutput;
using crosspad::FxSource;
using namespace crosspad_test;

namespace {

constexpr uint32_t kFrames = 64;
constexpr uint8_t kCapture = 2;

/// The processor an FX app attaches: twice what it is given.
struct Doubler : crosspad::IAudioNode {
    int prepared = 0;
    void process(const float* in, float* out, uint32_t frames) override
    {
        for (uint32_t i = 0; i < frames * 2; ++i) out[i] = in[i] * 2.0f;
    }
    void onPrepare(uint32_t, uint32_t) override { ++prepared; }
    const char* name() const override { return "Doubler"; }
};

/// IN1 (the mics) and IN2 (Line 1/2) at a constant level, no USB channel;
/// IN1 routed to Out 1/2 and Out 3/4, IN2 to Out 1/2.
struct FxFixture {
    ConstAudioInput mic{3277};      // ~0.1
    ConstAudioInput line{6554};     // ~0.2
    crosspad::AudioInputNode micNode{&mic, "IN1"};
    crosspad::AudioInputNode lineNode{&line, "IN2"};
    AudioMixerEngine mixer;
    uint8_t chMic = 0, chLine = 0;
    crosspad::IFxBus* bus = nullptr;

    FxFixture()
    {
        mixer.setup(kFrames, 48000, 3);
        chMic = mixer.addChannel(&micNode, "IN1");
        chLine = mixer.addChannel(&lineNode, "IN2");
        mixer.setDefaults();
        mixer.setRouteEnabled(chMic, 0, true);
        mixer.setRouteEnabled(chMic, 1, true);
        mixer.setRouteEnabled(chLine, 0, true);
        crosspad_pc::fx_bus_init(mixer, chMic, chLine, kMixerInvalidChannelId, kCapture);
        bus = crosspad::getPlatformServices().fxBus;
    }
    ~FxFixture() { bus->detach(); }

    float out0()
    {
        std::vector<float> b0(kFrames * 2, 0.0f), b1(kFrames * 2, 0.0f), b2(kFrames * 2, 0.0f);
        float* buses[3] = {b0.data(), b1.data(), b2.data()};
        mixer.render(buses, 3, kFrames);
        return b0[10];
    }
};

} // namespace

TEST_CASE("PcFxBus: attached, the sources' routes are off; detached, they are back", "[audio][fxbus]")
{
    FxFixture f;
    REQUIRE(f.bus != nullptr);
    Doubler fx;

    f.bus->setSend(FxSource::Mic, 1.0f);
    f.bus->attach(&fx);
    CHECK(fx.prepared == 1);
    CHECK_FALSE(f.mixer.isRouteEnabled(f.chMic, 0));
    CHECK_FALSE(f.mixer.isRouteEnabled(f.chMic, 1));
    CHECK_FALSE(f.mixer.isRouteEnabled(f.chLine, 0));
    CHECK(f.mixer.getFxSend(f.chMic) == 1.0f);
    CHECK(f.mixer.getFxReturn(0) == 1.0f);   // Out 1/2 returns by default

    f.bus->detach();
    CHECK(f.mixer.isRouteEnabled(f.chMic, 0));
    CHECK(f.mixer.isRouteEnabled(f.chMic, 1));
    CHECK(f.mixer.isRouteEnabled(f.chLine, 0));
    CHECK_FALSE(f.mixer.isRouteEnabled(f.chLine, 1));
    CHECK(f.mixer.getFxSend(f.chMic) == 0.0f);
    CHECK(f.mixer.getFxReturn(0) == 0.0f);
}

TEST_CASE("PcFxBus: the processor is the sources' only way out while attached", "[audio][fxbus]")
{
    FxFixture f;
    Doubler fx;
    const float dry = f.out0();                       // IN1 + IN2 straight to Out 1/2
    REQUIRE_THAT(dry, WithinAbs(0.3f, 0.01f));

    f.bus->setSend(FxSource::Mic, 1.0f);
    f.bus->setSend(FxSource::Line12, 0.0f);
    f.bus->attach(&fx);
    CHECK_THAT(f.out0(), WithinAbs(0.2f, 0.01f));     // the mics doubled; Line 1/2 not sent, not heard

    f.bus->setReturn(FxOutput::Out12, false);
    CHECK_THAT(f.out0(), WithinAbs(0.0f, 0.001f));

    f.bus->detach();
    CHECK_THAT(f.out0(), WithinAbs(dry, 0.001f));
}

TEST_CASE("PcFxBus: no Line 3/4 and no USB source in the simulator", "[audio][fxbus]")
{
    FxFixture f;
    CHECK(f.bus->hasSource(FxSource::Mic));
    CHECK(f.bus->sourceAvailable(FxSource::Line12));
    CHECK_FALSE(f.bus->hasSource(FxSource::Line34));
    CHECK_FALSE(f.bus->sourceAvailable(FxSource::Usb));
    CHECK(f.bus->hasOutput(FxOutput::Usb));
}
