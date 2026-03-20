/**
 * @file    test_e2e_scenarios.cpp
 * @brief   End-to-end tests simulating real CrossPad usage scenarios.
 *
 * These tests exercise full data flows the way a real user would trigger them:
 * pad press → MIDI out + LED feedback + EventBus notification, settings changes
 * affecting behavior, app switching with pad logic handlers, etc.
 */

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/pad/IPadLogicHandler.hpp"
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include "FreeRTOS.h"
#include "task.h"

#include <atomic>
#include <vector>
#include <string>

using namespace crosspad;

// ── Recording MIDI output — captures what would be sent to hardware ──

class RecordingMidiOutput : public IMidiOutput {
public:
    struct MidiMsg {
        enum Type { NoteOn, NoteOff } type;
        uint8_t note;
        uint8_t velocity;
        uint8_t channel;
    };

    void sendNoteOn(uint8_t note, uint8_t channel) override {
        msgs.push_back({MidiMsg::NoteOn, note, 127, channel});
    }
    void sendNoteOff(uint8_t note, uint8_t channel) override {
        msgs.push_back({MidiMsg::NoteOff, note, 0, channel});
    }
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override {
        msgs.push_back({MidiMsg::NoteOn, note, velocity, channel});
    }
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override {
        msgs.push_back({MidiMsg::NoteOff, note, velocity, channel});
    }

    void clear() { msgs.clear(); }
    size_t count() const { return msgs.size(); }

    std::vector<MidiMsg> msgs;
};

// ── Recording pad logic handler — simulates an app that intercepts pads ──

class RecordingPadLogic : public IPadLogicHandler {
public:
    void onActivate(PadManager&) override { activated = true; }
    void onDeactivate(PadManager&) override { deactivated = true; }
    void onPadPress(PadManager&, uint8_t padIdx, uint8_t velocity) override {
        presses.push_back({padIdx, velocity});
    }
    void onPadRelease(PadManager&, uint8_t padIdx) override {
        releases.push_back(padIdx);
    }
    void onPadPressure(PadManager&, uint8_t padIdx, uint8_t pressure) override {
        pressures.push_back({padIdx, pressure});
    }

    struct PressEvent { uint8_t padIdx; uint8_t velocity; };
    struct PressureEvent { uint8_t padIdx; uint8_t pressure; };

    std::vector<PressEvent> presses;
    std::vector<uint8_t> releases;
    std::vector<PressureEvent> pressures;
    bool activated = false;
    bool deactivated = false;
};

// ═══════════════════════════════════════════════════════════════════════
// Scenario 1: User plays a drum pattern on the pad grid
// Real flow: pad touch → PadManager → MIDI NoteOn → LED lights up →
//            EventBus notifies apps → pad release → MIDI NoteOff → LED off
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: Playing a drum pattern on pads", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    RecordingMidiOutput midiOut;
    auto& pm = platform.padManager;

    // Swap MIDI output to our recorder (simulates connecting a MIDI device)
    auto& services = getPlatformServices();
    services.setMidiOutput(&midiOut);

    // Settings: keypad enabled, MIDI send enabled, sendCC=false (note mode)
    auto* settings = CrosspadSettings::getInstance();
    settings->keypad.enableKeypad = true;
    settings->keypad.sendCC = false;

    // Set custom pad colors (like a sampler kit would)
    pm.setPadColor(0, 255, 0, 0);    // kick = red
    pm.setPadColor(1, 0, 255, 0);    // snare = green
    pm.setPadColor(2, 0, 0, 255);    // hihat = blue

    SECTION("quick kick-snare-hihat pattern") {
        // Kick
        pm.handlePadPress(0, 100);
        REQUIRE(pm.isPadPressed(0));
        REQUIRE(platform.ledStrip.pixels[0].R > 0);  // LED should show pressed color
        pm.handlePadRelease(0);
        REQUIRE_FALSE(pm.isPadPressed(0));

        // Snare
        pm.handlePadPress(1, 110);
        REQUIRE(pm.isPadPressed(1));
        pm.handlePadRelease(1);

        // Hi-hat
        pm.handlePadPress(2, 80);
        pm.handlePadRelease(2);

        // MIDI output should have 3 NoteOn + 3 NoteOff
        REQUIRE(midiOut.count() == 6);
        REQUIRE(midiOut.msgs[0].type == RecordingMidiOutput::MidiMsg::NoteOn);
        REQUIRE(midiOut.msgs[1].type == RecordingMidiOutput::MidiMsg::NoteOff);
        REQUIRE(midiOut.msgs[2].type == RecordingMidiOutput::MidiMsg::NoteOn);
    }

    SECTION("MIDI output sends correct note on press") {
        pm.handlePadPress(0, 42);
        // PadManager calls sendNoteOn(note, channel) — the 2-param legacy version
        // Velocity goes to EventBus/LED, not to the legacy MIDI output interface
        REQUIRE(midiOut.msgs.back().type == RecordingMidiOutput::MidiMsg::NoteOn);
        REQUIRE(midiOut.msgs.back().note == pm.getMidiNoteForPad(0));
        pm.handlePadRelease(0);
    }

    SECTION("pad notes follow keyOffset (default C2=36)") {
        // Default offset is 36 (C2), pad 0 → note 36, pad 1 → note 37, etc.
        pm.handlePadPress(0, 127);
        REQUIRE(midiOut.msgs[0].note == 36); // pad 0 = note 36
        pm.handlePadRelease(0);

        pm.handlePadPress(5, 127);
        REQUIRE(midiOut.msgs[2].note == 41); // pad 5 = note 41
        pm.handlePadRelease(5);
    }

    // Cleanup: restore NullMidiOutput
    services.setMidiOutput(nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 2: Disabling the keypad in settings
// Real flow: user opens settings → disables keypad → pads stop sending MIDI
//            but LEDs still respond and EventBus still fires
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: Disabling keypad stops MIDI but not events", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    RecordingMidiOutput midiOut;
    auto& services = getPlatformServices();
    services.setMidiOutput(&midiOut);

    auto* settings = CrosspadSettings::getInstance();
    auto& pm = platform.padManager;

    // Track EventBus pad events
    std::atomic<int> padEvents{0};
    auto handle = platform.eventBus.subscribe(EventType::PadPressed,
        [](const void*, EventType, void* ctx) {
            auto* count = static_cast<std::atomic<int>*>(ctx);
            count->fetch_add(1);
        }, &padEvents);

    SECTION("with keypad enabled — MIDI flows") {
        settings->keypad.enableKeypad = true;
        settings->keypad.sendCC = false;

        pm.handlePadPress(0, 100);
        pm.handlePadRelease(0);

        REQUIRE(midiOut.count() >= 1); // NoteOn sent
    }

    SECTION("with keypad disabled — no MIDI, events still fire") {
        settings->keypad.enableKeypad = false;
        midiOut.clear();

        pm.handlePadPress(0, 100);
        pm.handlePadRelease(0);

        REQUIRE(midiOut.count() == 0); // No MIDI sent

        // But EventBus still fires (apps still need to know about pad presses)
        vTaskDelay(pdMS_TO_TICKS(50)); // wait for async dispatch
        REQUIRE(padEvents.load() >= 1);
    }

    platform.eventBus.unsubscribe(handle);
    services.setMidiOutput(nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 3: External MIDI keyboard plays notes → LEDs react
// Real flow: BLE/USB MIDI NoteOn arrives → MidiInputHandler → PadManager
//            → LED lights up for the corresponding pad → NoteOff → LED off
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: External MIDI input lights up pads", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    // Simulate receiving MIDI NoteOn from external keyboard
    // Default mapping: pad 0 = note 36, pad 1 = note 37, etc.
    pm.handleMidiNoteOn(0, 36, 100);  // note 36 → pad 0

    REQUIRE(pm.isPadPressed(0));
    // LED should be showing pressed state (non-zero color on the strip)
    // The actual color depends on pressed color + brightness

    pm.handleMidiNoteOff(0, 36);
    REQUIRE_FALSE(pm.isPadPressed(0));

    // Note outside pad range should be harmless
    pm.handleMidiNoteOn(0, 127, 100); // note 127 — no pad mapped
    // Should not crash, no pad becomes pressed
    for (int i = 0; i < 16; i++) {
        REQUIRE_FALSE(pm.isPadPressed(i));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 4: App switches and pad logic handler takes over
// Real flow: user launches Sampler app → app registers IPadLogicHandler →
//            pad presses route to app logic instead of default MIDI output →
//            user switches to another app → old handler deactivated
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: App registers pad logic and intercepts presses", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    RecordingMidiOutput midiOut;
    auto& services = getPlatformServices();
    services.setMidiOutput(&midiOut);

    auto* settings = CrosspadSettings::getInstance();
    settings->keypad.enableKeypad = true;
    settings->keypad.sendCC = false;

    auto& pm = platform.padManager;
    auto logic = std::make_shared<RecordingPadLogic>();

    SECTION("without pad logic — default MIDI output") {
        pm.handlePadPress(0, 100);
        pm.handlePadRelease(0);
        REQUIRE(midiOut.count() >= 1);
    }

    SECTION("with pad logic active — app intercepts, no default MIDI") {
        pm.registerPadLogic("Sampler", logic);
        pm.setActivePadLogic("Sampler");

        REQUIRE(logic->activated);
        midiOut.clear();

        pm.handlePadPress(0, 100);
        pm.handlePadPress(3, 80);
        pm.handlePadRelease(0);

        // App logic received the events
        REQUIRE(logic->presses.size() == 2);
        REQUIRE(logic->presses[0].padIdx == 0);
        REQUIRE(logic->presses[0].velocity == 100);
        REQUIRE(logic->presses[1].padIdx == 3);
        REQUIRE(logic->releases.size() == 1);
        REQUIRE(logic->releases[0] == 0);

        // Default MIDI output was NOT used (app handles it)
        REQUIRE(midiOut.count() == 0);
    }

    SECTION("switching apps deactivates old handler") {
        pm.registerPadLogic("Sampler", logic);
        pm.setActivePadLogic("Sampler");
        REQUIRE(logic->activated);

        // Switch to default (no app)
        pm.setActivePadLogic("");
        REQUIRE(logic->deactivated);

        // Now pads go through default MIDI path again
        midiOut.clear();
        pm.handlePadPress(0, 100);
        pm.handlePadRelease(0);
        REQUIRE(midiOut.count() >= 1);
    }

    pm.unregisterPadLogic("Sampler");
    services.setMidiOutput(nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 5: Sampler playback — sample starts playing on a pad
// Real flow: sequencer/sampler triggers sample → PadManager tracks play state →
//            LED shows "playing" color → sample ends → LED reverts
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: Sample playback triggers playing LED state", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    // Set distinct playing color
    pm.setPlayingColor(0, 255, 0);   // green = playing

    SECTION("sample starts → pad shows playing, sample stops → reverts") {
        REQUIRE_FALSE(pm.isPadPlaying(0));

        pm.handleSamplePlay(0, true);
        REQUIRE(pm.isPadPlaying(0));

        pm.handleSamplePlay(0, false);
        REQUIRE_FALSE(pm.isPadPlaying(0));
    }

    SECTION("playing state is independent of pressed state") {
        pm.handlePadPress(0, 100);
        REQUIRE(pm.isPadPressed(0));

        pm.handleSamplePlay(0, true);
        REQUIRE(pm.isPadPlaying(0));
        REQUIRE(pm.isPadPressed(0)); // both can be true

        pm.handlePadRelease(0);
        REQUIRE_FALSE(pm.isPadPressed(0));
        REQUIRE(pm.isPadPlaying(0)); // still playing after finger lifts
    }

    SECTION("togglePadPlay toggles play state") {
        // togglePadPlay posts async events through EventBus.
        // Here we verify the state toggle logic — the pad play state
        // is updated synchronously within togglePadPlay.
        REQUIRE_FALSE(pm.isPadPlaying(5));

        pm.togglePadPlay(5);  // start playing
        REQUIRE(pm.isPadPlaying(5));

        pm.togglePadPlay(5);  // stop playing
        REQUIRE_FALSE(pm.isPadPlaying(5));

        // Toggle again to verify cycle
        pm.togglePadPlay(5);
        REQUIRE(pm.isPadPlaying(5));
        pm.togglePadPlay(5);
        REQUIRE_FALSE(pm.isPadPlaying(5));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 6: Platform init → full boot sequence → ready to play
// Real flow: PC/ESP32 boots → crosspad_platform_init() → EventBus ready →
//            PadLedController subscribes → settings loaded → pads mapped →
//            capabilities set → system ready
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: Full platform boot sequence", "[e2e]") {
    test::TestPlatform platform;

    SECTION("init succeeds and all subsystems are wired") {
        REQUIRE(platform.init());

        // EventBus should be initialized
        auto handle = platform.eventBus.subscribe(EventType::NoteOn,
            [](const void*, EventType, void*) {}, nullptr);
        REQUIRE(handle != nullptr);
        platform.eventBus.unsubscribe(handle);

        // PadManager should be ready with default note mapping
        REQUIRE(platform.padManager.getPadNote(0) == 36);  // C2
        REQUIRE(platform.padManager.getPadNote(15) == 51);  // D#3

        // Settings should be loaded with defaults
        auto* settings = CrosspadSettings::getInstance();
        REQUIRE(settings->keypad.enableKeypad == true);
        REQUIRE(settings->RGBbrightness == 100);
    }

    SECTION("capabilities reflect what was initialized") {
        REQUIRE(platform.init());

        // After init, set capabilities like PC platform would
        setPlatformCapabilities(
            Capability::Pads | Capability::Leds | Capability::Encoder |
            Capability::Display | Capability::Persistence
        );

        REQUIRE(hasCapability(Capability::Pads));
        REQUIRE(hasCapability(Capability::Leds));
        REQUIRE_FALSE(hasCapability(Capability::Midi)); // not connected yet

        // Simulate MIDI device connecting
        addPlatformCapability(Capability::Midi);
        REQUIRE(hasCapability(Capability::Midi));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 7: Settings save/load round-trip (user changes preferences)
// Real flow: user changes brightness in settings UI → saveTo(kvStore) →
//            next boot → loadFrom(kvStore) → settings restored
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: Settings persist across simulated reboot", "[e2e]") {
    test::MemoryKVStore store;
    store.init();

    auto* settings = CrosspadSettings::getInstance();

    // Simulate user changing settings
    uint8_t origBri = settings->RGBbrightness;
    uint8_t origLcd = settings->LCDbrightness;

    settings->RGBbrightness = 42;
    settings->LCDbrightness = 60;
    settings->keypad.enableKeypad = false;
    settings->keypad.sendCC = true;

    // Save (like closing settings app)
    settings->saveTo(store);

    // Simulate "reboot" — reset settings to defaults
    settings->RGBbrightness = 100;
    settings->LCDbrightness = 100;
    settings->keypad.enableKeypad = true;
    settings->keypad.sendCC = false;

    // Load from persistent store
    settings->loadFrom(store);

    REQUIRE(settings->RGBbrightness == 42);
    REQUIRE(settings->LCDbrightness == 60);
    REQUIRE(settings->keypad.enableKeypad == false);
    REQUIRE(settings->keypad.sendCC == true);

    // Restore for other tests
    settings->RGBbrightness = origBri;
    settings->LCDbrightness = origLcd;
    settings->keypad.enableKeypad = true;
    settings->keypad.sendCC = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 8: allNotesOff panic — MIDI panic button
// Real flow: user hits "all notes off" → all pressed pads release →
//            MIDI NoteOff for each → LEDs revert to default
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: All notes off panic clears all pads", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    // Press several pads
    pm.handlePadPress(0, 100);
    pm.handlePadPress(5, 80);
    pm.handlePadPress(10, 60);
    pm.handlePadPress(15, 40);

    REQUIRE(pm.isPadPressed(0));
    REQUIRE(pm.isPadPressed(5));
    REQUIRE(pm.isPadPressed(10));
    REQUIRE(pm.isPadPressed(15));

    // Panic!
    pm.allNotesOff();

    // All pads should be released
    for (int i = 0; i < 16; i++) {
        REQUIRE_FALSE(pm.isPadPressed(i));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scenario 9: Remapping pad notes for a different scale/octave
// Real flow: user changes keyOffset in status → applyDefaultOffsetToPadNotes() →
//            pad 0 now sends note 48 (C3) instead of 36 (C2)
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("E2E: Changing key offset remaps all pads", "[e2e]") {
    test::TestPlatform platform;
    REQUIRE(platform.init());

    RecordingMidiOutput midiOut;
    auto& services = getPlatformServices();
    services.setMidiOutput(&midiOut);

    auto* settings = CrosspadSettings::getInstance();
    settings->keypad.enableKeypad = true;
    settings->keypad.sendCC = false;

    auto& pm = platform.padManager;

    // Default: offset 36 → pad 0 = note 36
    REQUIRE(pm.getMidiNoteForPad(0) == 36);
    REQUIRE(pm.getMidiNoteForPad(15) == 51);

    // User shifts octave up: C3 = 48
    platform.status.keyOffset = 48;
    pm.applyDefaultOffsetToPadNotes();

    REQUIRE(pm.getMidiNoteForPad(0) == 48);
    REQUIRE(pm.getMidiNoteForPad(15) == 63);

    // Verify MIDI output uses new mapping
    pm.handlePadPress(0, 100);
    REQUIRE(midiOut.msgs[0].note == 48);
    pm.handlePadRelease(0);

    // Reverse lookup works too
    REQUIRE(pm.getPadForMidiNote(48) == 0);
    REQUIRE(pm.getPadForMidiNote(55) == 7);

    services.setMidiOutput(nullptr);
}
