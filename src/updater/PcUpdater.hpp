#pragma once

/**
 * @file PcUpdater.hpp
 * @brief Auto-update system — checks GitHub Releases, downloads, caches, and self-replaces
 *
 * Windows-only. Uses WinHTTP for binary downloads and PowerShell for zip
 * extraction. Self-update works via a batch script that replaces the exe
 * after the process exits. Supports version caching and rollback.
 */

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class UpdateState {
    Idle,
    Checking,
    UpdateAvailable,
    Downloading,
    Extracting,
    ReadyToInstall,
    Error
};

struct UpdateInfo {
    std::string latestVersion;     // e.g. "1.2.0"
    std::string currentVersion;    // e.g. "0.1.0"
    std::string downloadUrl;       // browser_download_url for the zip asset
    std::string releaseNotes;      // full markdown release body
    uint64_t    assetSize = 0;     // in bytes
    bool        updateAvailable = false;
    std::string errorMessage;      // non-empty on error
};

struct ReleaseInfo {
    std::string version;           // e.g. "0.2.7"
    std::string tagName;           // e.g. "v0.2.7"
    std::string releaseName;       // e.g. "CrossPad PC v0.2.7"
    std::string downloadUrl;       // browser_download_url for the zip
    std::string releaseNotes;      // full markdown body
    uint64_t    assetSize = 0;
    bool        isCached = false;  // true if version is in local cache
    bool        isCurrent = false; // true if this is the running version
};

using UpdateProgressCallback = std::function<void(UpdateState state, int progressPct, const std::string& message)>;

class PcUpdater {
public:
    /// Check GitHub Releases API for the latest version (synchronous)
    UpdateInfo checkForUpdate();

    /// Check with option to include pre-releases
    UpdateInfo checkForUpdate(bool includePrereleases);

    /// Fetch list of available releases from GitHub (synchronous)
    std::vector<ReleaseInfo> listReleases();

    /// Download + extract + cache a version (synchronous, calls progressCb)
    bool downloadAndCache(const ReleaseInfo& release, UpdateProgressCallback progressCb);

    /// Download + extract the update zip (legacy, caches automatically)
    bool downloadUpdate(const UpdateInfo& info, UpdateProgressCallback progressCb);

    /// Prepare install from cache — writes batch script. Returns path to script.
    std::string prepareInstallFromCache(const std::string& version);

    /// Write batch script for current download (legacy). Returns path to script.
    std::string prepareInstall();

    /// Launch the batch script and exit the application.
    void installAndRestart();

    /// Install a specific cached version and restart.
    void installCachedAndRestart(const std::string& version);

    /// Get the current exe path
    static std::string getCurrentExePath();

    /// Get the cache directory path (<exe_dir>/versions/)
    static std::string getCacheDir();

    /// Check if a version is cached locally
    bool isCached(const std::string& version) const;

    /// List all locally cached versions
    std::vector<std::string> getCachedVersions() const;

private:
    std::string tempDir_;
    std::string downloadPath_;
    std::string extractDir_;
};

/// Compare two semver strings. Returns >0 if a > b, <0 if a < b, 0 if equal.
int compareSemver(const std::string& a, const std::string& b);

/* ── Global auto-check result cache ──────────────────────────────────── */

/// Store the result of a background update check (thread-safe)
void pc_updater_set_cached_check_result(const UpdateInfo& info);

/// Get the cached check result (empty UpdateInfo if no check done yet)
UpdateInfo pc_updater_get_cached_check_result();

/// Whether a cached auto-check result is available
bool pc_updater_has_cached_result();
