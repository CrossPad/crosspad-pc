// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PwDefaultSinkGuard.hpp
 * @brief Takes over the PipeWire session's default audio sink (via
 *        WirePlumber's "default" metadata object) so a CrossPad virtual
 *        sink can be made the system default while active, and restores
 *        whatever was configured before once CrossPad is done.
 *
 * Only PipeWire-gated translation units include this header (guarded by
 * __linux__), matching PwVirtualSinkCapture.hpp's pattern.
 */

#ifdef __linux__

#include <spa/utils/hook.h>

#include <string>

struct pw_metadata;

namespace crosspad_pc {

/// Extracts the "name" field out of a metadata JSON value such as
/// `{"name":"alsa_output.foo"}`. Pure string scan (no JSON library),
/// tolerant of spacing; returns "" for nullptr/garbage input.
std::string pwExtractJsonName(const char* value);

class PwDefaultSinkGuard {
public:
    PwDefaultSinkGuard() = default;
    ~PwDefaultSinkGuard();

    PwDefaultSinkGuard(const PwDefaultSinkGuard&) = delete;
    PwDefaultSinkGuard& operator=(const PwDefaultSinkGuard&) = delete;

    /// Reads the current default sink name from the "default" metadata
    /// object (blocking, bounded ~2s per roundtrip). Performs its own
    /// registry+metadata discovery each call — independent of any
    /// in-progress takeover(). Empty string when metadata/"default" is
    /// missing (e.g. no WirePlumber) or the daemon is unreachable.
    std::string queryCurrentDefault();

    /// Saves the current default (unless already overridden via
    /// setPreviousSink()) and configures `sinkNodeName` as the new default
    /// sink. Keeps the metadata proxy bound until restore(). Returns false
    /// when the "default" metadata object can't be found/bound (e.g.
    /// WirePlumber absent) or the daemon is unreachable.
    bool takeover(const std::string& sinkNodeName);

    /// Restores the saved default (or clears the configured key so
    /// WirePlumber re-picks its own default when nothing was saved).
    /// Releases the bound metadata proxy. Idempotent — safe to call
    /// without a prior successful takeover(), and safe to call twice.
    void restore();

    /// Crash recovery WITHOUT takeover: a previous run died leaving the
    /// configured default sink pointed at a CrossPad virtual sink, but this
    /// run is not taking over (pref off / native backend unavailable). Binds
    /// the "default" metadata object fresh (independent of any takeover
    /// state), sets default.configured.audio.sink back to `previousSinkName`
    /// (or clears the key when empty), releases the proxy, and returns true on
    /// success. Returns false when the metadata object can't be found/bound
    /// (e.g. WirePlumber absent or daemon unreachable) so the caller can keep
    /// the recovery marker and retry next launch.
    bool restoreStale(const std::string& previousSinkName);

    bool active() const { return active_; }
    const std::string& previousSink() const { return previous_; }

    /// Crash recovery: inject the sink name that should be restored,
    /// overriding whatever takeover() would otherwise capture. Call before
    /// takeover().
    void setPreviousSink(const std::string& name) { previous_ = name; havePrevious_ = true; }

private:
    static int onMetaProperty(void* data, uint32_t subject, const char* key,
                               const char* type, const char* value);

    struct pw_metadata* meta_ = nullptr;
    struct spa_hook metaListener_{};
    std::string lastAudioSinkValue_;   // raw JSON value of "default.audio.sink"

    std::string previous_;
    bool havePrevious_ = false;
    bool active_ = false;
};

} // namespace crosspad_pc

#endif // __linux__
