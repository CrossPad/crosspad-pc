#include <catch2/catch_test_macros.hpp>
#include "audio/OutputRoute.hpp"

using namespace crosspad_pc::out_route;

namespace {
const std::string kAnalog = "alsa_output.pci-0000_00_1f.3.analog-stereo";
const std::string kHdmi   = "alsa_output.pci-0000_00_1f.3.hdmi-stereo";
const std::string kUsb    = "alsa_output.usb-Example_Headset-00.analog-stereo";

std::vector<std::string> names(const std::vector<Candidate>& cs) {
    std::vector<std::string> out;
    for (const auto& c : cs) out.push_back(c.sink);
    return out;
}
} // namespace

TEST_CASE("OutputRoute: every instance's virtual sink is CrossPad's", "[audio][route]") {
    REQUIRE(isCrossPadSink("crosspad_vin1"));
    REQUIRE(isCrossPadSink("crosspad_vin2"));
    REQUIRE(isCrossPadSink("crosspad_vin1_test"));
    REQUIRE(isCrossPadSink("crosspad_vin2_19861"));
    REQUIRE_FALSE(isCrossPadSink(kAnalog));
    REQUIRE_FALSE(isCrossPadSink(""));
    REQUIRE_FALSE(isCrossPadSink("my_crosspad_vin1"));
}

TEST_CASE("OutputRoute: the user's default first, then the crash marker, then the rest",
          "[audio][route]") {
    const auto cs = safeSinks(kHdmi, kUsb, {kAnalog, kHdmi, kUsb});
    REQUIRE(names(cs) == std::vector<std::string>{kHdmi, kUsb, kAnalog});
    REQUIRE(cs[0].why == Why::UserDefault);
    REQUIRE(cs[1].why == Why::BeforeCrash);
    REQUIRE(cs[2].why == Why::AnyReal);
}

TEST_CASE("OutputRoute: a CrossPad default is never a candidate", "[audio][route]") {
    // The takeover read its own sink as the "previous" default (an earlier
    // run died after taking over): it must not be where the output goes.
    const auto cs = safeSinks("crosspad_vin1", kAnalog, {kHdmi, kAnalog, "crosspad_vin1_test"});
    REQUIRE(names(cs) == std::vector<std::string>{kAnalog, kHdmi});
    for (const auto& c : cs) REQUIRE_FALSE(isCrossPadSink(c.sink));
}

TEST_CASE("OutputRoute: a sink that is gone is skipped, none twice", "[audio][route]") {
    const auto cs = safeSinks(kUsb /* unplugged */, kAnalog, {kAnalog, kHdmi});
    REQUIRE(names(cs) == std::vector<std::string>{kAnalog, kHdmi});
    REQUIRE(cs[0].why == Why::BeforeCrash);

    const auto same = safeSinks(kAnalog, kAnalog, {kAnalog});
    REQUIRE(same.size() == 1);
    REQUIRE(same[0].why == Why::UserDefault);
}

TEST_CASE("OutputRoute: no real sink, no candidate", "[audio][route]") {
    REQUIRE(safeSinks(kAnalog, kHdmi, {}).empty());
    REQUIRE(safeSinks("", "", {"crosspad_vin1", "crosspad_vin2"}).empty());
}
