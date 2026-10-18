// SPDX-License-Identifier: GPL-3.0-or-later
#include "ControlJson.hpp"

#include "JsonText.hpp"

using namespace remote::json;
using namespace crosspad::control;

namespace remote {

namespace {
/// Room for "%.6f" of any float: 39 integer digits, sign, point, 6 decimals.
constexpr size_t kFixedFloatChars = 48;
}

std::string controlJsonKitStatus(const KitStatus& st) {
    return std::string("{") + json_bool("ok", true) + "," +
           json_int("count", st.count) + "," +
           json_int("current", st.current) + "," +
           json_string("name", st.name ? st.name : "") + "," +
           json_bool("parsed", st.parsed) + "," +
           json_bool("loading", st.loading) + "}";
}

std::string controlJsonKitRow(const KitListRow& row) {
    return "{" + json_int("id", row.id) + "," +
           json_string("name", row.name ? row.name : "") + "," +
           json_bool("parsed", row.parsed) + "}";
}

std::string controlJsonKitList(int count, int current, const std::string& kits) {
    return "{" + json_bool("ok", true) + "," + json_int("count", count) + "," +
           json_int("current", current) + ",\"kits\":[" + kits + "]}";
}

std::string controlJsonAudioLevel(int stream, const AudioLevel& lv) {
    char ls[kFixedFloatChars], rs[kFixedFloatChars];
    snprintf(ls, sizeof(ls), "%.6f", static_cast<double>(lv.left));
    snprintf(rs, sizeof(rs), "%.6f", static_cast<double>(lv.right));
    return std::string("{") + json_bool("ok", true) + "," +
           json_int("stream", stream) + "," +
           "\"left\":" + ls + "," +
           "\"right\":" + rs + "}";
}

std::string controlJsonLedState(const LedState& st) {
    std::string colors = "[";
    for (int i = 0; i < LedState::kPads; ++i) {
        const crosspad::RgbColor& c = st.colors[i];
        char b[10];
        snprintf(b, sizeof(b), "\"%02X%02X%02X\"", c.R, c.G, c.B);
        if (i) colors += ",";
        colors += b;
    }
    colors += "]";
    return "{" + json_bool("ok", true) + "," +
           json_int("brightness", (int)st.brightness) + "," +
           json_bool("animating", st.animating) + "," +
           json_bool("coalesce", st.coalesced) + "," +
           "\"colors\":" + colors + "}";
}

std::string controlJsonMixChannel(const MixerChannelLevel& row) {
    std::string routes;
    for (uint8_t o = 0; o < row.outputs && o < kMixerRouteBits; ++o) routes += (row.routes >> o) & 1u ? '1' : '0';
    return "{" + json_int("ch", row.ch) + "," +
           json_string("name", row.name ? row.name : "") + "," +
           json_float("pk_l", row.peakL) + "," + json_float("pk_r", row.peakR) + "," +
           json_float("vol", row.volume) + "," +
           json_bool("mute", row.muted) + "," +
           json_string("route", routes) + "," +
           json_float("trim", row.trim) + "}";
}

std::string controlJsonMixOutput(const MixerOutputLevel& row) {
    return "{" + json_int("out", row.out) + "," +
           json_float("pk_l", row.peakL) + "," + json_float("pk_r", row.peakR) + "," +
           json_float("vol", row.volume) + "," +
           json_bool("mute", row.muted) + "," +
           json_float("lim", row.limit) + "," +
           json_float("ceil", row.ceiling) + "}";
}

std::string controlJsonMixLevels(uint8_t outputs, const std::string& channels, const std::string& outs,
                                 const MixerTapLevel& tap) {
    return "{" + json_bool("ok", true) + "," + json_int("outputs", outputs) +
           ",\"channels\":[" + channels + "],\"out\":[" + outs + "]," +
           json_float("tap_lim", tap.limit) + "}";
}

std::string controlJsonPitchedStatus(const PitchedStatus& st) {
    std::string roots = "[";
    for (uint8_t i = 0; i < st.zones && i < crosspad::SampleBank::kMaxZones; ++i) {
        if (i) roots += ",";
        char b[16];
        snprintf(b, sizeof(b), "%.2f", (double)st.roots[i]);
        roots += b;
    }
    roots += "]";
    return "{" + json_bool("ok", true) + "," + json_bool("active", st.active) + "," +
           json_int("zones", st.zones) + ",\"roots\":" + roots + "," +
           json_int("active_voices", st.activeVoices) + "," +
           json_int("sounding_voices", st.soundingVoices) + "," +
           json_int("steals", (int)st.steals) + "," +
           json_float("last_note_hz", st.lastNoteHz) + "," +
           json_int("bank_kb", (int)(st.bankBytes / 1024)) + "}";
}

std::string controlJsonWaveStatus(const WaveStatus& st) {
    return "{" + json_bool("ok", true) + "," +
           json_bool("running", st.loaderRunning) + "," +
           json_int("requested", (int)st.requested) + "," +
           json_int("loaded", (int)st.loaded) + "," +
           json_int("queued", (int)st.queued) + "," +
           json_int("retried", (int)st.retried) + "," +
           json_int("dropped", (int)st.dropped) + "," +
           json_int("failed_info", (int)st.failedInfo) + "," +
           json_int("failed_read", (int)st.failedRead) + "," +
           json_int("alloc_fail", (int)st.allocFail) + "}";
}

} // namespace remote
