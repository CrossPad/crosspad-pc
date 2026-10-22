// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file JsonText.hpp
 * @brief The control port's JSON writers -- no dependency, one copy for the
 *        server (RemoteControl.cpp) and the verb renderers (ControlJson.cpp).
 */

#include <cstdio>
#include <string>

namespace remote::json {

inline std::string json_string(const std::string& key, const std::string& val) {
    return "\"" + key + "\":\"" + val + "\"";
}

inline std::string json_bool(const std::string& key, bool val) {
    return "\"" + key + "\":" + (val ? "true" : "false");
}

inline std::string json_int(const std::string& key, int val) {
    return "\"" + key + "\":" + std::to_string(val);
}

inline std::string json_float(const std::string& key, float val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", (double)val);
    return "\"" + key + "\":" + buf;
}

/// Minimal JSON string escaping for device labels (PipeWire descriptions can
/// carry quotes). Only " and \ need escaping for a valid one-line value.
inline std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

} // namespace remote::json
