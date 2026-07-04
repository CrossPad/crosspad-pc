// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PwSinkEnumerator.hpp
 * @brief Native enumeration of system audio sinks via the PipeWire registry.
 *
 * Replaces locale-fragile `pactl list sinks` parsing for the OUT-device
 * dropdown: node.description always exists in the graph, so the UI never
 * falls back to raw ALSA ids like "alsa_output.pci-0000_79_00.1...".
 *
 * Trade-off vs the pactl path: port availability (unplugged HDMI/jack) is a
 * device-route property not exposed on the node, so entries are reported
 * regardless of physical plug state.
 */

#ifdef __linux__

#include <string>
#include <vector>

namespace crosspad_pc {

struct PwSinkEntry {
    std::string name;         ///< node.name — stable id (usable with move-sink-input)
    std::string description;  ///< node.description — human-readable UI label
};

/// Enumerate `media.class == Audio/Sink` nodes. Skips CrossPad's own
/// `crosspad_*` virtual sinks (routing OUT into them would loop back into
/// the mixer). Returns an empty vector when the daemon is unreachable —
/// callers fall back to the pactl parser.
std::vector<PwSinkEntry> pwEnumerateSinks();

} // namespace crosspad_pc

#endif // __linux__
