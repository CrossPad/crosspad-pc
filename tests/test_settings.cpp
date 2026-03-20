#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace crosspad;

TEST_CASE("CrosspadSettings: singleton access", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();
    REQUIRE(settings != nullptr);

    // Calling again returns the same instance
    auto* settings2 = CrosspadSettings::getInstance();
    REQUIRE(settings == settings2);
}

TEST_CASE("CrosspadSettings: default values", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();

    REQUIRE(settings->LCDbrightness == 100);
    REQUIRE(settings->RGBbrightness == 100);
    REQUIRE(settings->AudioEngineEnabled == true);
    REQUIRE(settings->Kit == 0);
    REQUIRE(settings->themeColorIndex == 0);
}

TEST_CASE("MemoryKVStore: save and load round-trip", "[settings]") {
    test::MemoryKVStore store;
    REQUIRE(store.init());

    SECTION("bool round-trip") {
        store.saveBool("test", "flag", true);
        REQUIRE(store.readBool("test", "flag", false) == true);

        store.saveBool("test", "flag", false);
        REQUIRE(store.readBool("test", "flag", true) == false);
    }

    SECTION("u8 round-trip") {
        store.saveU8("test", "brightness", 42);
        REQUIRE(store.readU8("test", "brightness", 0) == 42);
    }

    SECTION("i32 round-trip") {
        store.saveI32("test", "offset", -100);
        REQUIRE(store.readI32("test", "offset", 0) == -100);
    }

    SECTION("default returned for missing key") {
        REQUIRE(store.readBool("test", "missing", true) == true);
        REQUIRE(store.readU8("test", "missing", 55) == 55);
        REQUIRE(store.readI32("test", "missing", 999) == 999);
    }

    SECTION("eraseAll clears all data") {
        store.saveBool("ns", "a", true);
        store.saveU8("ns", "b", 10);
        REQUIRE(store.size() == 2);

        store.eraseAll();
        REQUIRE(store.size() == 0);
        REQUIRE(store.readBool("ns", "a", false) == false); // returns default
    }
}

TEST_CASE("CrosspadSettings: save and load via KVStore", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();
    test::MemoryKVStore store;
    store.init();

    // Modify a setting
    uint8_t originalBrightness = settings->LCDbrightness;
    settings->LCDbrightness = 75;

    // Save to store
    settings->saveTo(store);

    // Reset and reload
    settings->LCDbrightness = 50;
    settings->loadFrom(store);

    REQUIRE(settings->LCDbrightness == 75);

    // Restore
    settings->LCDbrightness = originalBrightness;
}
