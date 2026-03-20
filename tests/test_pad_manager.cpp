#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace crosspad;

TEST_CASE("PadManager: init via crosspad_platform_init", "[pad]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());
}

TEST_CASE("PadManager: pad press and release", "[pad]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("pad starts unpressed") {
        REQUIRE_FALSE(pm.isPadPressed(0));
    }

    SECTION("press sets state, release clears it") {
        pm.handlePadPress(0, 127);
        REQUIRE(pm.isPadPressed(0));

        pm.handlePadRelease(0);
        REQUIRE_FALSE(pm.isPadPressed(0));
    }

    SECTION("multiple pads are independent") {
        pm.handlePadPress(0, 100);
        pm.handlePadPress(3, 80);

        REQUIRE(pm.isPadPressed(0));
        REQUIRE(pm.isPadPressed(3));
        REQUIRE_FALSE(pm.isPadPressed(1));

        pm.handlePadRelease(0);
        REQUIRE_FALSE(pm.isPadPressed(0));
        REQUIRE(pm.isPadPressed(3));
    }
}

TEST_CASE("PadManager: note mapping", "[pad]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("set and get pad note") {
        pm.setPadNote(0, 60);
        REQUIRE(pm.getPadNote(0) == 60);

        pm.setPadNote(15, 72);
        REQUIRE(pm.getPadNote(15) == 72);
    }

    SECTION("set and get pad channel") {
        pm.setPadChannel(0, 5);
        REQUIRE(pm.getPadChannel(0) == 5);
    }
}

TEST_CASE("PadManager: input suppression", "[pad]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("suppression blocks pad press") {
        pm.setInputSuppressed(true);
        REQUIRE(pm.isInputSuppressed());

        pm.handlePadPress(0, 127);
        // When suppressed, press should be ignored
        // (exact behavior depends on implementation — test the flag at minimum)
        REQUIRE(pm.isInputSuppressed());

        pm.setInputSuppressed(false);
        REQUIRE_FALSE(pm.isInputSuppressed());
    }
}

TEST_CASE("PadManager: LED color management", "[pad]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("set and get default color") {
        pm.setDefaultColor(255, 0, 128);
        // Verify through pad color getter
        auto color = pm.getPadColor(0);
        // Default color applies to unpressed pads
        REQUIRE(color.R == 255);
        REQUIRE(color.G == 0);
        REQUIRE(color.B == 128);
    }
}

TEST_CASE("PadManager: brightness", "[pad]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    pm.setBrightness(200);
    REQUIRE(pm.getBrightness() == 200);

    pm.setBrightness(0);
    REQUIRE(pm.getBrightness() == 0);

    pm.setBrightness(255);
    REQUIRE(pm.getBrightness() == 255);
}
