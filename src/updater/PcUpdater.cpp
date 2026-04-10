/**
 * @file PcUpdater.cpp
 * @brief Auto-update implementation for Windows and Linux
 *
 * Cross-platform version check via IHttpClient + GitHub Releases API.
 * Windows: WinHTTP for binary downloads, PowerShell for zip extraction,
 *          batch script for exe replacement.
 * Linux:   curl for AppImage downloads, shell script for replacement.
 * Supports version caching and rollback on all platforms.
 */

#include "updater/PcUpdater.hpp"
#include "crosspad_pc_version.h"

#include <crosspad/net/IHttpClient.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include "crosspad-gui/platform/IGuiPlatform.h"

#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ── Semver compare ──────────────────────────────────────────────────── */

/// Parse pre-release suffix: "" → (false, 0), "-RC" → (true, 0), "-RC3" → (true, 3)
static void parsePreRelease(const std::string& ver, bool& isPreRelease, int& preNum)
{
    isPreRelease = false;
    preNum = 0;
    auto dash = ver.find('-');
    if (dash == std::string::npos) return;
    isPreRelease = true;
    std::string suffix = ver.substr(dash + 1);
    // Extract trailing number: "RC3" → 3, "RC" → 0, "alpha2" → 2
    size_t i = suffix.size();
    while (i > 0 && suffix[i - 1] >= '0' && suffix[i - 1] <= '9') --i;
    if (i < suffix.size())
        preNum = atoi(suffix.c_str() + i);
}

int compareSemver(const std::string& a, const std::string& b)
{
    int aMaj = 0, aMin = 0, aPat = 0;
    int bMaj = 0, bMin = 0, bPat = 0;
    sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPat);
    sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPat);

    if (aMaj != bMaj) return aMaj - bMaj;
    if (aMin != bMin) return aMin - bMin;
    if (aPat != bPat) return aPat - bPat;

    // Pre-release comparison: release (no suffix) > pre-release (has suffix)
    // e.g. 0.3.1 > 0.3.1-RC2 > 0.3.1-RC1 > 0.3.1-RC
    bool aPre, bPre;
    int aPreNum, bPreNum;
    parsePreRelease(a, aPre, aPreNum);
    parsePreRelease(b, bPre, bPreNum);

    if (!aPre && !bPre) return 0;    // both release
    if (!aPre &&  bPre) return 1;    // a is release, b is pre → a > b
    if ( aPre && !bPre) return -1;   // a is pre, b is release → a < b
    return aPreNum - bPreNum;         // both pre → compare numbers
}

/* ── Global auto-check result cache ──────────────────────────────────── */

static std::mutex s_cachedMutex;
static UpdateInfo s_cachedCheckResult;
static bool s_hasCachedResult = false;

void pc_updater_set_cached_check_result(const UpdateInfo& info)
{
    std::lock_guard<std::mutex> lock(s_cachedMutex);
    s_cachedCheckResult = info;
    s_hasCachedResult = true;
}

UpdateInfo pc_updater_get_cached_check_result()
{
    std::lock_guard<std::mutex> lock(s_cachedMutex);
    return s_cachedCheckResult;
}

bool pc_updater_has_cached_result()
{
    std::lock_guard<std::mutex> lock(s_cachedMutex);
    return s_hasCachedResult;
}

/* ── Helper: strip version prefix ────────────────────────────────────── */

static std::string stripVersionPrefix(const std::string& tag)
{
    std::string t = tag;
    // Strip leading 'v': "v0.3.1-RC1" → "0.3.1-RC1"
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V'))
        t = t.substr(1);
    return t;
}

/* ── Platform asset matching ─────────────────────────────────────────── */

static bool matchPlatformAsset(const char* name)
{
#ifdef _WIN32
    return strstr(name, "Windows") && strstr(name, ".zip");
#elif defined(__linux__)
    return strstr(name, "Linux") && strstr(name, ".AppImage");
#else
    (void)name;
    return false;
#endif
}

/* ── Helper: parse a single release JSON object into ReleaseInfo ────── */

static bool parseReleaseJson(JsonObject rel, ReleaseInfo& out, const char* currentVersion)
{
    const char* tag = rel["tag_name"] | "";
    if (strlen(tag) == 0) return false;

    out.tagName = tag;
    out.version = stripVersionPrefix(tag);
    out.releaseName = rel["name"] | "";
    out.releaseNotes = rel["body"] | "";
    out.isCurrent = (out.version == currentVersion);

    // Find platform-specific asset
    JsonArray assets = rel["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
        const char* name = asset["name"] | "";
        if (matchPlatformAsset(name)) {
            out.downloadUrl = asset["browser_download_url"] | "";
            out.assetSize = asset["size"] | (uint64_t)0;
            break;
        }
    }
    return !out.downloadUrl.empty();
}

/* ── Version Check (shared — uses IHttpClient) ──────────────────────── */

UpdateInfo PcUpdater::checkForUpdate()
{
    return checkForUpdate(false);
}

UpdateInfo PcUpdater::checkForUpdate(bool includePrereleases)
{
    UpdateInfo info;
    info.currentVersion = CROSSPAD_PC_VERSION;

    auto& http = crosspad::getHttpClient();
    if (!http.isAvailable()) {
        info.errorMessage = "HTTP client not available";
        return info;
    }

    crosspad::HttpRequest req;
    // /releases/latest skips prereleases; to include them, fetch the list
    if (includePrereleases) {
        req.url = "https://api.github.com/repos/CrossPad/crosspad-pc/releases?per_page=1";
    } else {
        req.url = "https://api.github.com/repos/CrossPad/crosspad-pc/releases/latest";
    }
    req.timeoutMs = 10000;
    req.headers.push_back({"Accept", "application/vnd.github.v3+json"});
    req.headers.push_back({"User-Agent", "CrossPad/" CROSSPAD_PC_VERSION});

    printf("[Updater] Checking for updates (prereleases=%s)...\n",
           includePrereleases ? "yes" : "no");
    auto resp = http.get(req);

    if (!resp.success()) {
        if (resp.statusCode == 0)
            info.errorMessage = resp.errorMessage.empty() ? "Network error" : resp.errorMessage;
        else if (resp.statusCode == 404)
            info.errorMessage = "No releases found";
        else
            info.errorMessage = "GitHub API error: HTTP " + std::to_string(resp.statusCode);
        return info;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp.body)) {
        info.errorMessage = "JSON parse error";
        return info;
    }

    // /releases?per_page=1 returns an array, /releases/latest returns an object
    JsonObject releaseObj;
    if (includePrereleases) {
        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() == 0) { info.errorMessage = "No releases found"; return info; }
        releaseObj = arr[0].as<JsonObject>();
    } else {
        releaseObj = doc.as<JsonObject>();
    }

    const char* tagName = releaseObj["tag_name"] | "";
    if (strlen(tagName) == 0) {
        info.errorMessage = "No tag_name in release";
        return info;
    }

    info.latestVersion = stripVersionPrefix(tagName);
    info.releaseNotes = releaseObj["body"] | "";

    JsonArray assets = releaseObj["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
        const char* name = asset["name"] | "";
        if (matchPlatformAsset(name)) {
            info.downloadUrl = asset["browser_download_url"] | "";
            info.assetSize = asset["size"] | (uint64_t)0;
            break;
        }
    }

    if (info.downloadUrl.empty()) {
        info.errorMessage = "No compatible asset in release";
        return info;
    }

    info.updateAvailable = compareSemver(info.latestVersion, info.currentVersion) > 0;

    printf("[Updater] Current: %s, Latest: %s, Update: %s\n",
           info.currentVersion.c_str(), info.latestVersion.c_str(),
           info.updateAvailable ? "YES" : "no");
    return info;
}

/* ── List Releases (shared — uses IHttpClient) ──────────────────────── */

std::vector<ReleaseInfo> PcUpdater::listReleases()
{
    std::vector<ReleaseInfo> releases;

    auto& http = crosspad::getHttpClient();
    if (!http.isAvailable()) return releases;

    crosspad::HttpRequest req;
    req.url = "https://api.github.com/repos/CrossPad/crosspad-pc/releases?per_page=10";
    req.timeoutMs = 10000;
    req.headers.push_back({"Accept", "application/vnd.github.v3+json"});
    req.headers.push_back({"User-Agent", "CrossPad/" CROSSPAD_PC_VERSION});

    auto resp = http.get(req);
    if (!resp.success()) return releases;

    JsonDocument doc;
    if (deserializeJson(doc, resp.body)) return releases;

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject rel : arr) {
        ReleaseInfo info;
        if (parseReleaseJson(rel, info, CROSSPAD_PC_VERSION)) {
            info.isCached = isCached(info.version);
            releases.push_back(info);
        }
    }

    printf("[Updater] Listed %zu releases\n", releases.size());
    return releases;
}

/* ══════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32

#include <Windows.h>
#include <winhttp.h>

#include <fstream>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

/* ── WinHTTP helpers ─────────────────────────────────────────────────── */

static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), ws.data(), len);
    return ws;
}

static std::string to_utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool https = true;
};

static bool parse_url(const std::string& url, UrlParts& out)
{
    std::wstring wurl = to_wide(url);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;
    out.host = hostBuf;
    out.path = pathBuf;
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

static std::string getTempUpdateDir()
{
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return to_utf8(std::wstring(tempPath) + L"crosspad_update");
}

/* ── Static helpers ──────────────────────────────────────────────────── */

std::string PcUpdater::getCurrentExePath()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return to_utf8(path);
}

std::string PcUpdater::getCacheDir()
{
    fs::path exeDir = fs::path(getCurrentExePath()).parent_path();
    return (exeDir / "versions").string();
}

bool PcUpdater::isCached(const std::string& version) const
{
    return fs::exists(fs::path(getCacheDir()) / ("CrossPad_v" + version + ".exe"));
}

std::vector<std::string> PcUpdater::getCachedVersions() const
{
    std::vector<std::string> versions;
    std::string cacheDir = getCacheDir();
    if (!fs::exists(cacheDir)) return versions;
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        std::string name = entry.path().filename().string();
        // Pattern: CrossPad_v0.2.7.exe
        if (name.find("CrossPad_v") == 0 && name.size() > 14 &&
            name.substr(name.size() - 4) == ".exe") {
            versions.push_back(name.substr(10, name.size() - 14));
        }
    }
    return versions;
}

/* ── WinHTTP binary download helper ──────────────────────────────────── */

static bool winHttpDownload(const std::string& url, const std::string& outputPath,
                            uint64_t expectedSize, UpdateProgressCallback progressCb)
{
    UrlParts parts;
    if (!parse_url(url, parts)) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Failed to parse URL");
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"CrossPad-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (progressCb) progressCb(UpdateState::Error, 0, "WinHttpOpen failed");
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    WinHttpSetTimeouts(hSession, 30000, 30000, 60000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        if (progressCb) progressCb(UpdateState::Error, 0, "Connection failed");
        return false;
    }

    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (progressCb) progressCb(UpdateState::Error, 0, "Request failed");
        return false;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (progressCb) progressCb(UpdateState::Error, 0, "HTTP request failed");
        return false;
    }

    DWORD statusCode = 0;
    DWORD sz = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (progressCb) progressCb(UpdateState::Error, 0, "HTTP " + std::to_string(statusCode));
        return false;
    }

    uint64_t totalSize = expectedSize;
    if (totalSize == 0) {
        wchar_t cl[64] = {};
        DWORD clSz = sizeof(cl);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, cl, &clSz, WINHTTP_NO_HEADER_INDEX))
            totalSize = _wtoi64(cl);
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (progressCb) progressCb(UpdateState::Error, 0, "Cannot create file");
        return false;
    }

    uint64_t bytesTotal = 0;
    DWORD bytesAvail = 0;
    bool success = true;

    do {
        bytesAvail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvail)) break;
        if (bytesAvail == 0) break;

        std::vector<char> buf(bytesAvail);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buf.data(), bytesAvail, &bytesRead)) {
            success = false;
            break;
        }
        outFile.write(buf.data(), bytesRead);
        if (!outFile.good()) { success = false; break; }
        bytesTotal += bytesRead;

        if (progressCb && totalSize > 0) {
            int pct = (int)((bytesTotal * 100) / totalSize);
            char msg[64];
            snprintf(msg, sizeof(msg), "%.1f / %.1f MB",
                     bytesTotal / 1048576.0, totalSize / 1048576.0);
            progressCb(UpdateState::Downloading, pct, msg);
        }
    } while (bytesAvail > 0);

    outFile.close();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!success) { fs::remove(outputPath); }
    return success;
}

/* ── Download, extract, and cache ────────────────────────────────────── */

static bool downloadExtractAndCache(const ReleaseInfo& release,
                                     UpdateProgressCallback progressCb,
                                     std::string& outExtractDir)
{
    const auto& downloadUrl = release.downloadUrl;
    const auto& assetSize   = release.assetSize;
    const auto& version     = release.version;
    const auto& releaseNotes = release.releaseNotes;
    std::string tempDir = getTempUpdateDir();
    fs::create_directories(tempDir);

    std::string zipPath = tempDir + "\\CrossPad-Windows-x64.zip";
    std::string extractDir = tempDir + "\\CrossPad";

    if (progressCb) progressCb(UpdateState::Downloading, 0, "Connecting...");

    if (!winHttpDownload(downloadUrl, zipPath, assetSize, progressCb))
        return false;

    // Extract
    if (progressCb) progressCb(UpdateState::Extracting, 0, "Extracting...");
    fs::create_directories(extractDir);
    std::string cmd = "powershell -NoProfile -Command \"Expand-Archive -Path '"
        + zipPath + "' -DestinationPath '" + extractDir + "' -Force\"";
    if (system(cmd.c_str()) != 0) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Extraction failed");
        return false;
    }

    // Find exe (may be nested)
    bool foundExe = false;
    for (auto& entry : fs::recursive_directory_iterator(extractDir)) {
        if (entry.path().filename() == "CrossPad.exe") {
            if (entry.path().parent_path() != fs::path(extractDir))
                extractDir = entry.path().parent_path().string();
            foundExe = true;
            break;
        }
    }
    if (!foundExe) {
        if (progressCb) progressCb(UpdateState::Error, 0, "CrossPad.exe not found");
        return false;
    }

    // Cache the exe, release notes, and metadata
    std::string cacheDir = PcUpdater::getCacheDir();
    fs::create_directories(cacheDir);
    std::string prefix = (fs::path(cacheDir) / ("CrossPad_v" + version)).string();

    std::string cachedExe = prefix + ".exe";
    std::error_code ec;
    fs::copy_file(fs::path(extractDir) / "CrossPad.exe", cachedExe, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        printf("[Updater] Cached exe: %s\n", cachedExe.c_str());
    }

    // Save raw release notes (markdown) for future parsing
    if (!releaseNotes.empty()) {
        std::ofstream nf(prefix + ".md");
        if (nf.is_open()) nf << releaseNotes;
    }

    // Save release metadata JSON
    {
        JsonDocument meta;
        meta["version"]       = version;
        meta["tag_name"]      = release.tagName;
        meta["release_name"]  = release.releaseName;
        meta["asset_size"]    = assetSize;
        meta["cached_at"]     = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ofstream mf(prefix + ".json");
        if (mf.is_open()) {
            serializeJsonPretty(meta, mf);
        }
    }

    outExtractDir = extractDir;
    return true;
}

/* ── Public download methods ─────────────────────────────────────────── */

bool PcUpdater::downloadUpdate(const UpdateInfo& info, UpdateProgressCallback progressCb)
{
    if (info.downloadUrl.empty()) {
        if (progressCb) progressCb(UpdateState::Error, 0, "No download URL");
        return false;
    }

    // Build a ReleaseInfo from UpdateInfo for unified cache path
    ReleaseInfo rel;
    rel.version      = info.latestVersion;
    rel.tagName      = "v" + info.latestVersion;
    rel.releaseName  = "";
    rel.downloadUrl  = info.downloadUrl;
    rel.releaseNotes = info.releaseNotes;
    rel.assetSize    = info.assetSize;

    tempDir_ = getTempUpdateDir();
    bool ok = downloadExtractAndCache(rel, progressCb, extractDir_);
    if (ok && progressCb)
        progressCb(UpdateState::ReadyToInstall, 100, "Ready to install");
    return ok;
}

bool PcUpdater::downloadAndCache(const ReleaseInfo& release, UpdateProgressCallback progressCb)
{
    if (release.downloadUrl.empty()) {
        if (progressCb) progressCb(UpdateState::Error, 0, "No download URL");
        return false;
    }

    tempDir_ = getTempUpdateDir();
    bool ok = downloadExtractAndCache(release, progressCb, extractDir_);
    if (ok && progressCb)
        progressCb(UpdateState::ReadyToInstall, 100, "Ready to install");
    return ok;
}

/* ── Batch script generation ─────────────────────────────────────────── */

static void writeBatchScript(std::ofstream& bat, const std::string& sourceExe,
                              const std::string& sourceDir,
                              const std::string& exePath, const std::string& exeDir,
                              const std::string& tempDir, DWORD pid)
{
    bat << "@echo off\r\n";
    bat << "echo Updating CrossPad...\r\n";

    // Wait for the process to exit by PID (up to 30 seconds)
    bat << "echo Waiting for CrossPad (PID " << pid << ") to exit...\r\n";
    bat << "set WAIT_RETRIES=0\r\n";
    bat << ":wait_exit\r\n";
    bat << "tasklist /FI \"PID eq " << pid << "\" 2>nul | find \"" << pid << "\" >nul\r\n";
    bat << "if not errorlevel 1 (\r\n";
    bat << "    set /a WAIT_RETRIES+=1\r\n";
    bat << "    if %WAIT_RETRIES% gtr 30 (\r\n";
    bat << "        echo ERROR: CrossPad did not exit after 30 seconds\r\n";
    bat << "        pause\r\n";
    bat << "        exit /b 1\r\n";
    bat << "    )\r\n";
    bat << "    timeout /t 1 /nobreak >nul\r\n";
    bat << "    goto wait_exit\r\n";
    bat << ")\r\n";
    bat << "echo Process exited.\r\n";
    bat << "\r\n";

    bat << "copy /y \"" << sourceExe << "\" \"" << exePath << "\" >nul 2>&1\r\n";
    bat << "if errorlevel 1 (\r\n";
    bat << "    echo ERROR: Failed to copy update\r\n";
    bat << "    pause\r\n";
    bat << "    exit /b 1\r\n";
    bat << ")\r\n";
    bat << "\r\n";

    // Copy SDL2.dll if available alongside source
    if (!sourceDir.empty()) {
        bat << "if exist \"" << sourceDir << "\\SDL2.dll\" (\r\n";
        bat << "    copy /y \"" << sourceDir << "\\SDL2.dll\" \"" << exeDir << "\\SDL2.dll\"\r\n";
        bat << ")\r\n";
        bat << "if exist \"" << sourceDir << "\\assets\" (\r\n";
        bat << "    xcopy /e /y /q \"" << sourceDir << "\\assets\" \"" << exeDir << "\\assets\\\"\r\n";
        bat << ")\r\n";
        bat << "\r\n";
    }

    bat << "echo Update complete! Restarting...\r\n";
    bat << "start \"\" \"" << exePath << "\"\r\n";
    bat << "\r\n";
    bat << "timeout /t 2 /nobreak >nul\r\n";
    if (!tempDir.empty()) {
        bat << "rmdir /s /q \"" << tempDir << "\" 2>nul\r\n";
    }
    bat << "exit\r\n";
}

std::string PcUpdater::prepareInstall()
{
    std::string exePath = getCurrentExePath();
    std::string exeDir = fs::path(exePath).parent_path().string();
    std::string batPath = tempDir_ + "\\update.bat";

    std::ofstream bat(batPath);
    if (!bat.is_open()) return "";

    writeBatchScript(bat, extractDir_ + "\\CrossPad.exe", extractDir_,
                     exePath, exeDir, tempDir_, GetCurrentProcessId());
    bat.close();
    printf("[Updater] Batch script written to %s\n", batPath.c_str());
    return batPath;
}

std::string PcUpdater::prepareInstallFromCache(const std::string& version)
{
    std::string cachedExe = (fs::path(getCacheDir()) / ("CrossPad_v" + version + ".exe")).string();
    if (!fs::exists(cachedExe)) {
        printf("[Updater] Cached exe not found: %s\n", cachedExe.c_str());
        return "";
    }

    std::string exePath = getCurrentExePath();
    std::string exeDir = fs::path(exePath).parent_path().string();
    std::string tempDir = getTempUpdateDir();
    fs::create_directories(tempDir);
    std::string batPath = tempDir + "\\update.bat";

    std::ofstream bat(batPath);
    if (!bat.is_open()) return "";

    // For cache install, source is just the cached exe (no SDL2/assets)
    writeBatchScript(bat, cachedExe, "", exePath, exeDir, tempDir, GetCurrentProcessId());
    bat.close();
    printf("[Updater] Cache install script written to %s\n", batPath.c_str());
    return batPath;
}

/* ── Cache current exe before switching ──────────────────────────────── */

static void cacheCurrentExe()
{
    std::string curVersion = CROSSPAD_PC_VERSION;
    std::string cacheDir = PcUpdater::getCacheDir();
    fs::create_directories(cacheDir);
    std::string cachedExe = (fs::path(cacheDir) / ("CrossPad_v" + curVersion + ".exe")).string();

    if (fs::exists(cachedExe)) return; // already cached

    std::string curExe = PcUpdater::getCurrentExePath();
    std::error_code ec;
    fs::copy_file(curExe, cachedExe, fs::copy_options::none, ec);
    if (!ec) {
        printf("[Updater] Cached current exe (v%s) before switching\n", curVersion.c_str());
    } else {
        printf("[Updater] Warning: failed to cache current exe: %s\n", ec.message().c_str());
    }
}

/* ── Launch batch and exit ───────────────────────────────────────────── */

static void launchBatAndExit(const std::string& batPath)
{
    std::wstring wBatPath = to_wide(batPath);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"cmd.exe /c \"" + wBatPath + L"\"";

    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
        FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        printf("[Updater] Update script launched, exiting...\n");
        fflush(stdout);
    } else {
        printf("[Updater] ERROR: Failed to launch script (error %lu)\n", GetLastError());
        return;
    }
    // Terminate immediately — _exit() works from any thread and skips CRT cleanup
    _exit(0);
}

void PcUpdater::installAndRestart()
{
    std::string batPath = prepareInstall();
    if (batPath.empty()) return;
    cacheCurrentExe();
    launchBatAndExit(batPath);
}

void PcUpdater::installCachedAndRestart(const std::string& version)
{
    std::string batPath = prepareInstallFromCache(version);
    if (batPath.empty()) return;
    cacheCurrentExe();
    launchBatAndExit(batPath);
}

std::string PcUpdater::getCachedReleaseNotes(const std::string& version)
{
    auto path = fs::path(getCacheDir()) / ("CrossPad_v" + version + ".md");
    if (!fs::exists(path)) return "";
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

std::string PcUpdater::getCachedMetadataJson(const std::string& version)
{
    auto path = fs::path(getCacheDir()) / ("CrossPad_v" + version + ".json");
    if (!fs::exists(path)) return "";
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

#elif defined(__linux__) // ── Linux implementation (AppImage) ────────── */

#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

/* ── Linux helpers ──────────────────────────────────────────────────── */

static std::string getTempUpdateDir()
{
    return "/tmp/crosspad_update";
}

std::string PcUpdater::getCurrentExePath()
{
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) return "";
    buf[len] = '\0';
    return std::string(buf);
}

std::string PcUpdater::getCacheDir()
{
    const char* home = getenv("HOME");
    if (!home) return "/tmp/crosspad/versions";
    return std::string(home) + "/.local/share/crosspad/versions";
}

bool PcUpdater::isCached(const std::string& version) const
{
    return fs::exists(fs::path(getCacheDir()) / ("CrossPad_v" + version + ".AppImage"));
}

std::vector<std::string> PcUpdater::getCachedVersions() const
{
    std::vector<std::string> versions;
    std::string cacheDir = getCacheDir();
    if (!fs::exists(cacheDir)) return versions;
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        std::string name = entry.path().filename().string();
        // Pattern: CrossPad_v0.3.1.AppImage
        if (name.find("CrossPad_v") == 0 && name.size() > 19 &&
            name.substr(name.size() - 9) == ".AppImage") {
            versions.push_back(name.substr(10, name.size() - 19));
        }
    }
    return versions;
}

/* ── curl-based download with progress ──────────────────────────────── */

static bool curlDownload(const std::string& url, const std::string& outputPath,
                          uint64_t expectedSize, UpdateProgressCallback progressCb)
{
    if (progressCb) progressCb(UpdateState::Downloading, 0, "Connecting...");

    pid_t pid = fork();
    if (pid == 0) {
        execlp("curl", "curl", "-L", "-f", "-s",
               "-o", outputPath.c_str(), url.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (pid < 0) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Failed to start download");
        return false;
    }

    // Poll file size for progress while curl downloads
    int status;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (expectedSize > 0 && progressCb) {
            std::error_code ec;
            if (fs::exists(outputPath, ec)) {
                auto size = fs::file_size(outputPath, ec);
                if (!ec) {
                    int pct = (int)((size * 100) / expectedSize);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "%.1f / %.1f MB",
                             size / 1048576.0, expectedSize / 1048576.0);
                    progressCb(UpdateState::Downloading, pct, msg);
                }
            }
        }
        usleep(500000);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Download failed");
        std::error_code ec;
        fs::remove(outputPath, ec);
        return false;
    }

    if (progressCb) progressCb(UpdateState::Downloading, 100, "Download complete");
    return true;
}

/* ── Download and cache AppImage ────────────────────────────────────── */

static bool downloadAndCacheAppImage(const ReleaseInfo& release,
                                      UpdateProgressCallback progressCb,
                                      std::string& outCachedPath)
{
    std::string cacheDir = PcUpdater::getCacheDir();
    fs::create_directories(cacheDir);

    std::string cachedPath = (fs::path(cacheDir) /
        ("CrossPad_v" + release.version + ".AppImage")).string();

    if (!curlDownload(release.downloadUrl, cachedPath, release.assetSize, progressCb))
        return false;

    // Make executable
    chmod(cachedPath.c_str(), 0755);

    // Save release notes
    std::string prefix = (fs::path(cacheDir) / ("CrossPad_v" + release.version)).string();
    if (!release.releaseNotes.empty()) {
        std::ofstream nf(prefix + ".md");
        if (nf.is_open()) nf << release.releaseNotes;
    }

    // Save release metadata JSON
    {
        JsonDocument meta;
        meta["version"]       = release.version;
        meta["tag_name"]      = release.tagName;
        meta["release_name"]  = release.releaseName;
        meta["asset_size"]    = release.assetSize;
        meta["cached_at"]     = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ofstream mf(prefix + ".json");
        if (mf.is_open()) serializeJsonPretty(meta, mf);
    }

    outCachedPath = cachedPath;
    printf("[Updater] Cached AppImage: %s\n", cachedPath.c_str());
    return true;
}

/* ── Public download methods ────────────────────────────────────────── */

bool PcUpdater::downloadUpdate(const UpdateInfo& info, UpdateProgressCallback progressCb)
{
    if (info.downloadUrl.empty()) {
        if (progressCb) progressCb(UpdateState::Error, 0, "No download URL");
        return false;
    }

    ReleaseInfo rel;
    rel.version      = info.latestVersion;
    rel.tagName      = "v" + info.latestVersion;
    rel.releaseName  = "";
    rel.downloadUrl  = info.downloadUrl;
    rel.releaseNotes = info.releaseNotes;
    rel.assetSize    = info.assetSize;

    std::string cachedPath;
    bool ok = downloadAndCacheAppImage(rel, progressCb, cachedPath);
    if (ok) {
        extractDir_ = cachedPath;
        if (progressCb) progressCb(UpdateState::ReadyToInstall, 100, "Ready to install");
    }
    return ok;
}

bool PcUpdater::downloadAndCache(const ReleaseInfo& release, UpdateProgressCallback progressCb)
{
    if (release.downloadUrl.empty()) {
        if (progressCb) progressCb(UpdateState::Error, 0, "No download URL");
        return false;
    }

    std::string cachedPath;
    bool ok = downloadAndCacheAppImage(release, progressCb, cachedPath);
    if (ok && progressCb)
        progressCb(UpdateState::ReadyToInstall, 100, "Ready to install");
    return ok;
}

/* ── Shell script generation ────────────────────────────────────────── */

std::string PcUpdater::prepareInstall()
{
    if (extractDir_.empty()) return "";

    std::string exePath = getCurrentExePath();
    std::string tempDir = getTempUpdateDir();
    fs::create_directories(tempDir);
    std::string scriptPath = tempDir + "/update.sh";

    std::ofstream script(scriptPath);
    if (!script.is_open()) return "";

    script << "#!/bin/bash\n";
    script << "echo 'Updating CrossPad...'\n";
    script << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 0.5; done\n";
    script << "cp '" << extractDir_ << "' '" << exePath << "'\n";
    script << "chmod +x '" << exePath << "'\n";
    script << "echo 'Update complete! Restarting...'\n";
    script << "'" << exePath << "' &\n";
    script << "sleep 2\n";
    script << "rm -rf '" << tempDir << "'\n";
    script.close();

    chmod(scriptPath.c_str(), 0755);
    printf("[Updater] Update script written to %s\n", scriptPath.c_str());
    return scriptPath;
}

std::string PcUpdater::prepareInstallFromCache(const std::string& version)
{
    std::string cachedAppImage = (fs::path(getCacheDir()) /
        ("CrossPad_v" + version + ".AppImage")).string();
    if (!fs::exists(cachedAppImage)) {
        printf("[Updater] Cached AppImage not found: %s\n", cachedAppImage.c_str());
        return "";
    }

    std::string exePath = getCurrentExePath();
    std::string tempDir = getTempUpdateDir();
    fs::create_directories(tempDir);
    std::string scriptPath = tempDir + "/update.sh";

    std::ofstream script(scriptPath);
    if (!script.is_open()) return "";

    script << "#!/bin/bash\n";
    script << "echo 'Updating CrossPad...'\n";
    script << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 0.5; done\n";
    script << "cp '" << cachedAppImage << "' '" << exePath << "'\n";
    script << "chmod +x '" << exePath << "'\n";
    script << "echo 'Update complete! Restarting...'\n";
    script << "'" << exePath << "' &\n";
    script << "sleep 2\n";
    script << "rm -rf '" << tempDir << "'\n";
    script.close();

    chmod(scriptPath.c_str(), 0755);
    printf("[Updater] Cache install script written to %s\n", scriptPath.c_str());
    return scriptPath;
}

/* ── Cache current AppImage before switching ────────────────────────── */

static void cacheCurrentExe()
{
    std::string curVersion = CROSSPAD_PC_VERSION;
    std::string cacheDir = PcUpdater::getCacheDir();
    fs::create_directories(cacheDir);
    std::string cachedPath = (fs::path(cacheDir) /
        ("CrossPad_v" + curVersion + ".AppImage")).string();

    if (fs::exists(cachedPath)) return;

    std::string curExe = PcUpdater::getCurrentExePath();
    std::error_code ec;
    fs::copy_file(curExe, cachedPath, fs::copy_options::none, ec);
    if (!ec) {
        printf("[Updater] Cached current AppImage (v%s)\n", curVersion.c_str());
    } else {
        printf("[Updater] Warning: failed to cache current AppImage: %s\n", ec.message().c_str());
    }
}

/* ── Launch script and exit ─────────────────────────────────────────── */

static void launchScriptAndExit(const std::string& scriptPath)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/bash", "bash", scriptPath.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (pid > 0) {
        printf("[Updater] Update script launched (PID %d), exiting...\n", pid);
        fflush(stdout);
        _exit(0);
    }
    printf("[Updater] ERROR: Failed to fork for update script\n");
}

void PcUpdater::installAndRestart()
{
    std::string scriptPath = prepareInstall();
    if (scriptPath.empty()) return;
    cacheCurrentExe();
    launchScriptAndExit(scriptPath);
}

void PcUpdater::installCachedAndRestart(const std::string& version)
{
    std::string scriptPath = prepareInstallFromCache(version);
    if (scriptPath.empty()) return;
    cacheCurrentExe();
    launchScriptAndExit(scriptPath);
}

std::string PcUpdater::getCachedReleaseNotes(const std::string& version)
{
    auto path = fs::path(getCacheDir()) / ("CrossPad_v" + version + ".md");
    if (!fs::exists(path)) return "";
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

std::string PcUpdater::getCachedMetadataJson(const std::string& version)
{
    auto path = fs::path(getCacheDir()) / ("CrossPad_v" + version + ".json");
    if (!fs::exists(path)) return "";
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

#else // ── Stubs for unsupported platforms ───────────────────────────── */

bool PcUpdater::downloadUpdate(const UpdateInfo&, UpdateProgressCallback cb)
{
    if (cb) cb(UpdateState::Error, 0, "Not available on this platform");
    return false;
}

bool PcUpdater::downloadAndCache(const ReleaseInfo&, UpdateProgressCallback cb)
{
    if (cb) cb(UpdateState::Error, 0, "Not available on this platform");
    return false;
}

std::string PcUpdater::getCurrentExePath() { return ""; }
std::string PcUpdater::getCacheDir() { return ""; }
bool PcUpdater::isCached(const std::string&) const { return false; }
std::vector<std::string> PcUpdater::getCachedVersions() const { return {}; }
std::string PcUpdater::prepareInstall() { return ""; }
std::string PcUpdater::prepareInstallFromCache(const std::string&) { return ""; }
void PcUpdater::installAndRestart() {}
void PcUpdater::installCachedAndRestart(const std::string&) {}
std::string PcUpdater::getCachedReleaseNotes(const std::string&) { return ""; }
std::string PcUpdater::getCachedMetadataJson(const std::string&) { return ""; }

#endif // _WIN32
