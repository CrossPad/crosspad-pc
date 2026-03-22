#pragma once

/**
 * @file PcUpdater.hpp
 * @brief Auto-update system — checks GitHub Releases, downloads, and self-replaces
 *
 * Windows-only. Uses WinHTTP for binary downloads and Windows Shell COM for
 * zip extraction. Self-update works via a batch script that replaces the exe
 * after the process exits.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

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
    std::string releaseNotes;      // release body text (truncated)
    uint64_t    assetSize = 0;     // in bytes
    bool        updateAvailable = false;
    std::string errorMessage;      // non-empty on error
};

using UpdateProgressCallback = std::function<void(UpdateState state, int progressPct, const std::string& message)>;

class PcUpdater {
public:
    /// Check GitHub Releases API for a new version (synchronous, blocks on HTTP)
    UpdateInfo checkForUpdate();

    /// Download + extract the update zip (synchronous, calls progressCb with state updates)
    bool downloadUpdate(const UpdateInfo& info, UpdateProgressCallback progressCb);

    /// Write the batch script that replaces the exe and restarts. Returns path to script.
    std::string prepareInstall();

    /// Launch the batch script and exit the application.
    void installAndRestart();

    /// Get the current exe path
    static std::string getCurrentExePath();

private:
    std::string tempDir_;       // %TEMP%\crosspad_update
    std::string downloadPath_;  // path to downloaded zip
    std::string extractDir_;    // path to extracted contents
};

/// Compare two semver strings. Returns >0 if a > b, <0 if a < b, 0 if equal.
int compareSemver(const std::string& a, const std::string& b);
