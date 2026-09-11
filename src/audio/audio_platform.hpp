// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file audio_platform.hpp
 * @brief Platform-specific corners of the audio device selector, behind a
 *        portable API so the selector logic carries no #ifdef.
 *
 * The device *selector* (enumerate → label → sort → open) is portable RtAudio
 * code. Three corners are not: PipeWire source descriptions for nicer input
 * labels, the extra system sinks RtAudio's backend can miss, and the
 * default-sink-takeover feedback guard. Each is a compile-time platform choice,
 * so they live here with one implementation per platform (audio_platform_*.cpp)
 * instead of scattered `#if defined(USE_PIPEWIRE) …` blocks in crosspad_app.cpp.
 *
 * Not covered here: the system-wide virtual-mixer feature itself (null-sink
 * creation and monitor capture) is a Linux-only capability reached through
 * IVirtualSinkManager, whose factory already returns a no-op on Windows/macOS —
 * on those platforms these functions simply report "nothing extra", and the
 * selector runs on plain RtAudio devices.
 */

#include <string>
#include <vector>

namespace crosspad_pc {
namespace audio_platform {

/// True when this build can take over the system default sink (PipeWire).
/// The loopback guard is a no-op elsewhere, so the selector never has to ask
/// whether PipeWire exists.
bool supportsPwTakeover();

/// Relabel raw RtAudio input-device names with human-readable PipeWire source
/// descriptions where one matches. Returns a vector the same length and order
/// as @p rawNames. On platforms without PipeWire the names are returned
/// unchanged — the IN dropdown then shows whatever RtAudio reports.
std::vector<std::string> prettyInputLabels(const std::vector<std::string>& rawNames);

/// A system audio sink RtAudio's backend may not enumerate (motherboard analog
/// jack, active HDMI display) but which the sound server can route to.
struct SinkCandidate {
    std::string name;         ///< sink id for move-sink-input
    std::string description;  ///< human-readable UI label
};

/// Sinks to offer in the OUT dropdown in addition to RtAudio's own list.
/// Empty when virtual audio is disabled or the platform has no sound-server
/// sink enumeration (Windows/macOS).
std::vector<SinkCandidate> outputSinkCandidates();

} // namespace audio_platform
} // namespace crosspad_pc
