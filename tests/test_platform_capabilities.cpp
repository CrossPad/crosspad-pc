#include <catch2/catch_test_macros.hpp>
#include "crosspad/platform/PlatformCapabilities.hpp"

using namespace crosspad;

TEST_CASE("PlatformCapabilities: set and query", "[capabilities]") {
    // Reset to clean state
    setPlatformCapabilities(Capability::None);

    SECTION("initially empty") {
        REQUIRE_FALSE(hasCapability(Capability::Midi));
        REQUIRE_FALSE(hasCapability(Capability::Pads));
        REQUIRE(getPlatformCapabilities() == Capability::None);
    }

    SECTION("set multiple flags at once") {
        setPlatformCapabilities(Capability::Pads | Capability::Leds | Capability::Display);

        REQUIRE(hasCapability(Capability::Pads));
        REQUIRE(hasCapability(Capability::Leds));
        REQUIRE(hasCapability(Capability::Display));
        REQUIRE_FALSE(hasCapability(Capability::Midi));
    }

    SECTION("hasCapability requires ALL specified flags") {
        setPlatformCapabilities(Capability::Pads | Capability::Leds);

        REQUIRE(hasCapability(Capability::Pads | Capability::Leds));
        REQUIRE_FALSE(hasCapability(Capability::Pads | Capability::Midi));
    }

    SECTION("hasAnyCapability requires ANY specified flag") {
        setPlatformCapabilities(Capability::Pads);

        REQUIRE(hasAnyCapability(Capability::Pads | Capability::Midi));
        REQUIRE_FALSE(hasAnyCapability(Capability::Midi | Capability::WiFi));
    }
}

TEST_CASE("PlatformCapabilities: add and remove at runtime", "[capabilities]") {
    setPlatformCapabilities(Capability::Pads | Capability::Leds);

    SECTION("add capability") {
        addPlatformCapability(Capability::Midi);
        REQUIRE(hasCapability(Capability::Midi));
        REQUIRE(hasCapability(Capability::Pads)); // original still present
    }

    SECTION("remove capability") {
        removePlatformCapability(Capability::Leds);
        REQUIRE_FALSE(hasCapability(Capability::Leds));
        REQUIRE(hasCapability(Capability::Pads)); // other still present
    }

    SECTION("add then remove same flag") {
        addPlatformCapability(Capability::Synth);
        REQUIRE(hasCapability(Capability::Synth));
        removePlatformCapability(Capability::Synth);
        REQUIRE_FALSE(hasCapability(Capability::Synth));
    }
}

TEST_CASE("PlatformCapabilities: bitwise operators", "[capabilities]") {
    auto combined = Capability::Midi | Capability::AudioOut;
    REQUIRE((static_cast<uint32_t>(combined)) == 0x03);

    auto masked = combined & Capability::Midi;
    REQUIRE(masked == Capability::Midi);

    auto inverted = ~Capability::None;
    REQUIRE(static_cast<uint32_t>(inverted) == 0xFFFFFFFF);
}
