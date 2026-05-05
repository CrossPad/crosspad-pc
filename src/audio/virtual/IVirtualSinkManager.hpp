#pragma once

/**
 * @file IVirtualSinkManager.hpp
 * @brief OS-visible virtual audio sinks for system-wide mixing.
 *
 * Each platform implementation creates/detects "virtual speakers" that other
 * apps (DAWs, browsers, games) can route their audio to. CrossPad then
 * captures from the sinks' monitor sources as IN#1 / IN#2 inputs for its mixer.
 *
 *   Linux:   pactl module-null-sink created at runtime (zero deps for user)
 *   Windows: detect VB-CABLE A/B by name in RtAudio device list
 *   macOS:   detect BlackHole 2ch by name in RtAudio device list
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crosspad_pc {

class IVirtualSinkManager {
public:
    struct VirtualSink {
        std::string displayName;        // e.g. "CrossPad virtual IN#1"
        std::string captureDeviceName;  // substring to match in RtAudio input device list
        uint32_t    channelCount = 2;
    };

    virtual ~IVirtualSinkManager() = default;

    /// Create (or detect) `sinkCount` virtual sinks. Returns true on success.
    virtual bool setup(uint32_t sinkCount) = 0;

    /// Tear down any sinks that were created at setup time.
    /// Must be idempotent and safe to call from signal handlers / atexit.
    virtual void teardown() = 0;

    /// Sinks that are currently live. Empty if setup() wasn't called or failed.
    virtual std::vector<VirtualSink> list() const = 0;

    /// True if the platform implementation can work on this system.
    virtual bool isAvailable() const = 0;

    /// Human-readable hint for the user if isAvailable()==false.
    virtual std::string errorHint() const = 0;
};

std::unique_ptr<IVirtualSinkManager> makeVirtualSinkManager();

/// Real OS-level audio output for the OUT dropdown — used in addition to
/// (not as a replacement for) RtAudio enumeration. Solves the case where
/// PipeWire holds the motherboard analog jack / active HDMI display as
/// defaults and RtAudio's ALSA backend therefore can't list them.
struct PulseSinkInfo {
    std::string name;         // PA sink name (passed to pactl move-sink-input)
    std::string description;  // human-readable description for UI
    bool        available = true; // false when active port reports "not available"
                                  // (e.g. unplugged HDMI) — UI filters these out
};

/// List PulseAudio sinks. Filters out CrossPad's own crosspad_vin* sinks
/// to prevent loopback. By default also drops sinks whose active port is
/// "not available" (unplugged jacks); pass includeUnavailable=true to keep them.
/// Returns empty on non-Linux or when pactl isn't available.
std::vector<PulseSinkInfo> enumeratePulseSinks(bool includeUnavailable = false);

/// Move the CrossPad playback stream for slot 0/1 to the given PA sink.
/// Operates on the existing RtAudio "PulseAudio Sound Server" / "Default
/// ALSA Device" stream — does NOT touch CrossPad's audio backend itself.
/// Returns false on non-Linux or if the move command failed.
bool movePulseOutputToSink(int slot, const std::string& targetSinkName);

} // namespace crosspad_pc
