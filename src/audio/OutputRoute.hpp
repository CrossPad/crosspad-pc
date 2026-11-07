// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file OutputRoute.hpp
 * @brief Which sink an output may play into: never one of CrossPad's own.
 *
 * With the default-sink takeover on, the system default IS crosspad_vin1, so
 * an output that follows the default plays into the simulator's own input:
 * the user hears nothing and the mixer feeds itself. Another simulator's
 * takeover does the same to this one. Such an output is opened on a named
 * sink instead, the first of safeSinks() that opens. Pure string work, so
 * the order is unit-tested without a sound server.
 */

#include <string>
#include <vector>

namespace crosspad_pc::out_route {

/// A CrossPad virtual sink, this instance's or another's (crosspad_vin1,
/// crosspad_vin2_test, ...). No real sink carries the prefix.
inline bool isCrossPadSink(const std::string& sink) {
    return sink.rfind("crosspad_", 0) == 0;
}

/// Why a sink was picked, for the log.
enum class Why { UserDefault, BeforeCrash, AnyReal };

inline const char* describe(Why why) {
    switch (why) {
    case Why::UserDefault: return "the user's default sink";
    case Why::BeforeCrash: return "the default before an earlier run took it over";
    case Why::AnyReal:     return "a real sink";
    }
    return "";
}

struct Candidate {
    std::string sink;
    Why         why;
};

/**
 * The sinks an output that may not follow the system default tries, in order:
 * the user's own default (@p userDefault: the one from before this run's
 * takeover, or the current default), the one an earlier run that died
 * recorded (@p beforeCrash), then every other sink of @p realSinks in its
 * order. A name not in @p realSinks (gone, unplugged) or naming a CrossPad
 * sink is skipped, and each sink comes once.
 */
inline std::vector<Candidate> safeSinks(const std::string& userDefault,
                                        const std::string& beforeCrash,
                                        const std::vector<std::string>& realSinks) {
    std::vector<Candidate> out;
    auto add = [&](const std::string& sink, Why why) {
        if (sink.empty() || isCrossPadSink(sink)) return;
        bool real = false;
        for (const auto& r : realSinks) real = real || r == sink;
        if (!real) return;
        for (const auto& c : out) if (c.sink == sink) return;
        out.push_back({sink, why});
    };
    add(userDefault, Why::UserDefault);
    add(beforeCrash, Why::BeforeCrash);
    for (const auto& r : realSinks) add(r, Why::AnyReal);
    return out;
}

} // namespace crosspad_pc::out_route
