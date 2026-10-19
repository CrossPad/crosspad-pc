// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file InstanceEnv.hpp
 * @brief Per-instance identity of a simulator process, from the environment.
 *
 * A scripted or test simulator runs beside the one somebody is playing, so it
 * must not share that one's audio nodes, default sink or profile files.
 *
 *   CROSSPAD_INSTANCE=<id>   suffix for the PipeWire/Pulse node names
 *                            (crosspad_vin1_<id>, crosspad_out_<id>). Without
 *                            it a non-default CROSSPAD_REMOTE_PORT is the id.
 *   CROSSPAD_AUDIO=on|isolated|off
 *       on        (default) the user's simulator: plain node names, takes
 *                 over the desktop's default sink.
 *       isolated  real audio, suffixed node names, no default-sink takeover.
 *                 The default when an instance id is set.
 *       off       no audio device, no virtual sinks, no takeover.
 *
 * A mode other than `on` with no id gets the id "test", so it can never reuse
 * the primary instance's names.
 */
#pragma once

#include <cstdlib>
#include <string>

namespace crosspad_pc::instance {

enum class AudioMode { On, Isolated, Off };

namespace detail {

constexpr long kDefaultRemotePort = 19840;

inline std::string sanitize(const char* v) {
    std::string out;
    for (const char* p = v; p && *p; ++p) {
        const char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            out += c;
        else if (c == '-' || c == '_')
            out += '_';
    }
    return out;
}

/// The id the environment names, "" when it names none.
inline std::string envId() {
    if (const char* v = std::getenv("CROSSPAD_INSTANCE")) {
        std::string id = sanitize(v);
        if (!id.empty()) return id;
    }
    if (const char* v = std::getenv("CROSSPAD_REMOTE_PORT")) {
        const long p = std::strtol(v, nullptr, 10);
        if (p > 0 && p < 65536 && p != kDefaultRemotePort) return std::to_string(p);
    }
    return {};
}

inline bool explicitMode(AudioMode& out) {
    const char* v = std::getenv("CROSSPAD_AUDIO");
    if (!v) return false;
    const std::string s = v;
    if (s == "on")       { out = AudioMode::On;       return true; }
    if (s == "isolated") { out = AudioMode::Isolated; return true; }
    if (s == "off")      { out = AudioMode::Off;      return true; }
    return false;
}

} // namespace detail

inline AudioMode audioMode() {
    static const AudioMode mode = [] {
        AudioMode m;
        if (detail::explicitMode(m)) return m;
        return detail::envId().empty() ? AudioMode::On : AudioMode::Isolated;
    }();
    return mode;
}

/// "" for the primary instance, otherwise the id (never "" when audioMode()
/// is not On).
inline const std::string& id() {
    static const std::string value = [] {
        std::string v = detail::envId();
        if (v.empty() && audioMode() != AudioMode::On) v = "test";
        return v;
    }();
    return value;
}

/// "" or "_<id>".
inline std::string suffix() { return id().empty() ? std::string() : "_" + id(); }

/// Node name of this instance: crosspad_vin1 -> crosspad_vin1_<id>.
inline std::string nodeName(const std::string& base) { return base + suffix(); }

/// Description shown in audio panels: "CrossPad IN#1" -> "CrossPad IN#1 (<id>)".
inline std::string description(const std::string& base) {
    return id().empty() ? base : base + " (" + id() + ")";
}

inline bool audioEnabled() { return audioMode() != AudioMode::Off; }

/// Only the primary instance in `on` mode takes the desktop's default sink.
inline bool defaultSinkTakeoverAllowed() {
    return audioMode() == AudioMode::On && id().empty();
}

} // namespace crosspad_pc::instance
