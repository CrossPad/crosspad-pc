#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include <crosspad/settings/CrosspadSettings.hpp>
#include <crosspad/settings/AudioEngineSettings.hpp>

using namespace crosspad;

TEST_CASE("AudioEngineSettings: validity checks", "[settings][audio]") {
    REQUIRE(AudioEngineSettings::isValidSampleRate(44100));
    REQUIRE(AudioEngineSettings::isValidSampleRate(48000));
    REQUIRE(AudioEngineSettings::isValidSampleRate(96000));
    REQUIRE_FALSE(AudioEngineSettings::isValidSampleRate(22050));
    REQUIRE_FALSE(AudioEngineSettings::isValidSampleRate(192000));
    REQUIRE_FALSE(AudioEngineSettings::isValidSampleRate(0));

    for (uint32_t fc : {32u, 48u, 64u, 128u, 256u, 512u}) {
        REQUIRE(AudioEngineSettings::isValidFrameCount(fc));
    }
    REQUIRE_FALSE(AudioEngineSettings::isValidFrameCount(96));
    REQUIRE_FALSE(AudioEngineSettings::isValidFrameCount(1024));
    REQUIRE_FALSE(AudioEngineSettings::isValidFrameCount(0));
}

TEST_CASE("AudioEngineSettings: defaults", "[settings][audio]") {
    AudioEngineSettings s;
    REQUIRE(s.sampleRate == 48000);
    REQUIRE(s.frameCount == 64);
    REQUIRE(AudioEngineSettings::isValidSampleRate(s.sampleRate));
    REQUIRE(AudioEngineSettings::isValidFrameCount(s.frameCount));
}

TEST_CASE("CrosspadSettings: audioEngine save/load round-trip", "[settings][audio]") {
    test::MemoryKVStore store;
    store.init();

    auto* s = CrosspadSettings::getInstance();
    s->audioEngine.sampleRate = 96000;
    s->audioEngine.frameCount = 128;

    s->saveTo(store);
    REQUIRE(store.size() > 0);

    s->audioEngine.sampleRate = 44100;
    s->audioEngine.frameCount = 32;

    s->loadFrom(store);
    REQUIRE(s->audioEngine.sampleRate == 96000);
    REQUIRE(s->audioEngine.frameCount == 128);

    // Restore defaults
    s->audioEngine.sampleRate = 48000;
    s->audioEngine.frameCount = 64;
    s->saveTo(store);
}

TEST_CASE("CrosspadSettings: missing audioEngine keys → default 48000/64", "[settings][audio]") {
    test::MemoryKVStore store;
    store.init();   // empty store

    auto* s = CrosspadSettings::getInstance();
    s->audioEngine.sampleRate = 22050;  // bogus
    s->audioEngine.frameCount = 99;     // bogus

    s->loadFrom(store);
    REQUIRE(s->audioEngine.sampleRate == 48000);
    REQUIRE(s->audioEngine.frameCount == 64);
}

TEST_CASE("CrosspadSettings: every valid SR persists and reloads", "[settings][audio]") {
    test::MemoryKVStore store;
    store.init();
    auto* s = CrosspadSettings::getInstance();

    for (uint32_t sr : {44100u, 48000u, 96000u}) {
        s->audioEngine.sampleRate = sr;
        s->saveTo(store);

        s->audioEngine.sampleRate = 0;  // wipe
        s->loadFrom(store);
        REQUIRE(s->audioEngine.sampleRate == sr);
    }
}
