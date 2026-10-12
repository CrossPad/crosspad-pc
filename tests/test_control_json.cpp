// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden replies of the control-port verbs whose state crosspad-core gathers.
// Every expected string here is what the simulator answered before the verb
// moved to crosspad/control/ -- the MCP and the PC tools parse exactly these,
// so a change to one is a protocol change, not a refactor.
#include <catch2/catch_test_macros.hpp>

#include "remote/ControlJson.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/audio/SynthEngineNode.hpp>

#include <string>

using namespace crosspad::control;

TEST_CASE("control json: kit_status", "[remote][control]") {
    KitStatus st;
    st.count = 3;
    st.current = 2;
    st.name = "808 Kit";
    st.parsed = true;
    CHECK(remote::controlJsonKitStatus(st) ==
          R"({"ok":true,"count":3,"current":2,"name":"808 Kit","parsed":true,"loading":false})");

    KitStatus none;
    none.loading = true;
    CHECK(remote::controlJsonKitStatus(none) ==
          R"({"ok":true,"count":0,"current":-1,"name":"","parsed":false,"loading":true})");
}

TEST_CASE("control json: led_state", "[remote][control]") {
    LedState st;
    st.brightness = 100;
    st.coalesced = true;
    st.settingsBrightness = 80;   // the board's line only
    st.colors[0] = crosspad::RgbColor(0xFF, 0x00, 0x00);
    st.colors[15] = crosspad::RgbColor(0x00, 0xFF, 0x80);
    CHECK(remote::controlJsonLedState(st) ==
          R"({"ok":true,"brightness":100,"animating":false,"coalesce":true,"colors":["FF0000","000000",)"
          R"("000000","000000","000000","000000","000000","000000","000000","000000","000000","000000",)"
          R"("000000","000000","000000","00FF80"]})");
}

TEST_CASE("control json: mix_lvl", "[remote][control]") {
    MixerChannelLevel a;
    a.ch = 0;
    a.name = "Sampler";
    a.peakL = 0.5f;
    a.peakR = 0.25f;
    a.volume = 1.0f;
    a.outputs = 2;
    a.routes = 0b01;
    MixerChannelLevel b;
    b.ch = 2;
    b.volume = 0.8f;
    b.muted = true;
    b.outputs = 2;
    b.routes = 0b10;
    MixerOutputLevel o0;
    o0.peakL = 0.125f;
    o0.volume = 1.0f;
    MixerOutputLevel o1;
    o1.out = 1;
    o1.volume = 0.5f;
    o1.muted = true;
    const std::string chans = remote::controlJsonMixChannel(a) + "," + remote::controlJsonMixChannel(b);
    const std::string outs = remote::controlJsonMixOutput(o0) + "," + remote::controlJsonMixOutput(o1);
    CHECK(remote::controlJsonMixLevels(2, chans, outs) ==
          R"({"ok":true,"outputs":2,"channels":[{"ch":0,"name":"Sampler","pk_l":0.5,"pk_r":0.25,"vol":1,)"
          R"("mute":false,"route":"10"},{"ch":2,"name":"","pk_l":0,"pk_r":0,"vol":0.8,"mute":true,"route":"01"}],)"
          R"("out":[{"out":0,"pk_l":0.125,"pk_r":0,"vol":1,"mute":false},{"out":1,"pk_l":0,"pk_r":0,"vol":0.5,"mute":true}]})");
}

TEST_CASE("control json: mix_lvl rows read off the real mixer", "[remote][control]") {
    AudioMixerEngine mixer;
    crosspad::SynthEngineNode synth;
    mixer.setup(64, 48000, 2);
    mixer.addChannel(&synth, "SYNTH");
    mixer.setDefaults();
    mixer.setRouteEnabled(0, 1, true);
    mixer.setChannelVolume(0, 0.5f);
    mixer.setOutputMute(1, true);

    std::string chans, outs;
    int nch = 0, nout = 0;
    readMixerLevels(mixer,
        [&](const MixerChannelLevel& row) { ++nch; chans += remote::controlJsonMixChannel(row); },
        [&](const MixerOutputLevel& row) { ++nout; if (!outs.empty()) outs += ","; outs += remote::controlJsonMixOutput(row); });
    CHECK(nch == 1);
    CHECK(nout == 2);
    CHECK(chans == R"({"ch":0,"name":"SYNTH","pk_l":0,"pk_r":0,"vol":0.5,"mute":false,"route":"01"})");
    CHECK(outs.find(R"({"out":1,"pk_l":0,"pk_r":0,)") != std::string::npos);
    CHECK(outs.find(R"("mute":true})") != std::string::npos);
}

TEST_CASE("control json: pitched_status", "[remote][control]") {
    PitchedStatus st;
    st.active = true;
    st.zones = 2;
    st.roots[0] = 48.0f;
    st.roots[1] = 60.5f;
    st.bankBytes = 1234567;
    st.activeVoices = 3;
    st.soundingVoices = 2;
    st.steals = 4;
    st.lastNoteHz = 261.6256f;
    CHECK(remote::controlJsonPitchedStatus(st) ==
          R"({"ok":true,"active":true,"zones":2,"roots":[48.00,60.50],"active_voices":3,"sounding_voices":2,)"
          R"("steals":4,"last_note_hz":261.626,"bank_kb":1205})");
}

TEST_CASE("control json: wave_status", "[remote][control]") {
    WaveStatus st;
    st.loaderRunning = true;
    st.requested = 1;
    st.dropped = 2;
    st.loaded = 3;
    st.retried = 4;
    st.failedInfo = 5;
    st.failedRead = 6;
    st.allocFail = 7;
    st.queued = 10;
    CHECK(remote::controlJsonWaveStatus(st) ==
          R"({"ok":true,"running":true,"requested":1,"loaded":3,"queued":10,"retried":4,"dropped":2,)"
          R"("failed_info":5,"failed_read":6,"alloc_fail":7})");
}
