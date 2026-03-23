/**
 * @file PcUpdater.cpp
 * @brief Auto-update implementation for Windows (WinHTTP download, Shell COM zip, batch replace)
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

int compareSemver(const std::string& a, const std::string& b)
{
    int aMaj = 0, aMin = 0, aPat = 0;
    int bMaj = 0, bMin = 0, bPat = 0;
    sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPat);
    sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPat);

    if (aMaj != bMaj) return aMaj - bMaj;
    if (aMin != bMin) return aMin - bMin;
    return aPat - bPat;
}

#ifdef _WIN32

#include <Windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shldisp.h>

#include <fstream>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        return false;
    }

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
    std::wstring dir = std::wstring(tempPath) + L"crosspad_update";
    return to_utf8(dir);
}

/* ── Version Check ───────────────────────────────────────────────────── */

UpdateInfo PcUpdater::checkForUpdate()
{
    UpdateInfo info;
    info.currentVersion = CROSSPAD_PC_VERSION;

    auto& http = crosspad::getHttpClient();
    if (!http.isAvailable()) {
        info.errorMessage = "HTTP client not available";
        return info;
    }

    crosspad::HttpRequest req;
    req.url = "https://api.github.com/repos/CrossPad/crosspad-pc/releases/latest";
    req.timeoutMs = 10000;
    req.headers.push_back({"Accept", "application/vnd.github.v3+json"});
    req.headers.push_back({"User-Agent", "CrossPad/" CROSSPAD_PC_VERSION});

    printf("[Updater] Checking for updates...\n");
    auto resp = http.get(req);

    if (!resp.success()) {
        if (resp.statusCode == 0) {
            info.errorMessage = resp.errorMessage.empty() ? "Network error" : resp.errorMessage;
        } else if (resp.statusCode == 404) {
            info.errorMessage = "No releases found";
        } else {
            info.errorMessage = "GitHub API error: HTTP " + std::to_string(resp.statusCode);
        }
        return info;
    }

    // Parse JSON response
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp.body);
    if (err) {
        info.errorMessage = std::string("JSON parse error: ") + err.c_str();
        return info;
    }

    // Extract tag_name (e.g. "v1.2.0")
    const char* tagName = doc["tag_name"] | "";
    if (strlen(tagName) == 0) {
        info.errorMessage = "No tag_name in release";
        return info;
    }

    // Strip leading 'v'
    std::string version = tagName;
    if (version[0] == 'v' || version[0] == 'V') {
        version = version.substr(1);
    }
    info.latestVersion = version;

    // Release notes (truncate for display)
    const char* body = doc["body"] | "";
    info.releaseNotes = body;
    if (info.releaseNotes.size() > 500) {
        info.releaseNotes = info.releaseNotes.substr(0, 500) + "...";
    }

    // Find the Windows zip asset
    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
        const char* name = asset["name"] | "";
        if (strstr(name, "Windows") && strstr(name, ".zip")) {
            info.downloadUrl = asset["browser_download_url"] | "";
            info.assetSize = asset["size"] | (uint64_t)0;
            break;
        }
    }

    if (info.downloadUrl.empty()) {
        info.errorMessage = "No Windows asset in release";
        return info;
    }

    // Compare versions
    info.updateAvailable = compareSemver(info.latestVersion, info.currentVersion) > 0;

    printf("[Updater] Current: %s, Latest: %s, Update: %s\n",
           info.currentVersion.c_str(), info.latestVersion.c_str(),
           info.updateAvailable ? "YES" : "no");

    return info;
}

/* ── Binary Download (WinHTTP streaming) ─────────────────────────────── */

bool PcUpdater::downloadUpdate(const UpdateInfo& info, UpdateProgressCallback progressCb)
{
    if (info.downloadUrl.empty()) {
        if (progressCb) progressCb(UpdateState::Error, 0, "No download URL");
        return false;
    }

    // Create temp directory
    tempDir_ = getTempUpdateDir();
    fs::create_directories(tempDir_);

    downloadPath_ = tempDir_ + "\\CrossPad-Windows-x64.zip";
    extractDir_ = tempDir_ + "\\CrossPad";

    if (progressCb) progressCb(UpdateState::Downloading, 0, "Connecting...");

    UrlParts parts;
    if (!parse_url(info.downloadUrl, parts)) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Failed to parse download URL");
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"CrossPad-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (progressCb) progressCb(UpdateState::Error, 0, "WinHttpOpen failed");
        return false;
    }

    // Enable automatic redirect following (GitHub redirects to CDN)
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    // Set timeouts (30s connect, 60s for data)
    WinHttpSetTimeouts(hSession, 30000, 30000, 60000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Connection failed");
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Request creation failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD err = GetLastError();
        if (progressCb) progressCb(UpdateState::Error, 0,
            "Send failed (error " + std::to_string(err) + ")");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD err = GetLastError();
        if (progressCb) progressCb(UpdateState::Error, 0,
            "Receive failed (error " + std::to_string(err) + ")");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Check HTTP status
    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        if (progressCb) progressCb(UpdateState::Error, 0,
            "Download HTTP " + std::to_string(statusCode));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Get content length if available
    uint64_t totalSize = info.assetSize;
    if (totalSize == 0) {
        wchar_t contentLen[64] = {};
        DWORD clSize = sizeof(contentLen);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, contentLen, &clSize,
                WINHTTP_NO_HEADER_INDEX)) {
            totalSize = _wtoi64(contentLen);
        }
    }

    // Stream to file
    std::ofstream outFile(downloadPath_, std::ios::binary);
    if (!outFile.is_open()) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Cannot create temp file");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
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
        if (!outFile.good()) {
            success = false;
            if (progressCb) progressCb(UpdateState::Error, 0, "Disk write error");
            break;
        }

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

    if (!success) {
        fs::remove(downloadPath_);
        return false;
    }

    printf("[Updater] Downloaded %llu bytes to %s\n", bytesTotal, downloadPath_.c_str());

    // ── Extract zip ──
    if (progressCb) progressCb(UpdateState::Extracting, 0, "Extracting...");

    fs::create_directories(extractDir_);

    // Use PowerShell to extract (simpler and more reliable than Shell COM)
    std::string cmd = "powershell -NoProfile -Command \"Expand-Archive -Path '"
        + downloadPath_ + "' -DestinationPath '" + extractDir_ + "' -Force\"";

    printf("[Updater] Extracting: %s\n", cmd.c_str());
    int ret = system(cmd.c_str());

    if (ret != 0) {
        if (progressCb) progressCb(UpdateState::Error, 0, "Extraction failed");
        return false;
    }

    // Verify extraction — look for CrossPad.exe in extract dir (may be nested)
    bool foundExe = false;
    for (auto& entry : fs::recursive_directory_iterator(extractDir_)) {
        if (entry.path().filename() == "CrossPad.exe") {
            // If exe is in a subdirectory, adjust extractDir_ to that subdirectory
            if (entry.path().parent_path() != fs::path(extractDir_)) {
                extractDir_ = entry.path().parent_path().string();
            }
            foundExe = true;
            break;
        }
    }

    if (!foundExe) {
        if (progressCb) progressCb(UpdateState::Error, 0, "CrossPad.exe not found in archive");
        return false;
    }

    if (progressCb) progressCb(UpdateState::ReadyToInstall, 100, "Ready to install");
    printf("[Updater] Extraction complete, exe found in %s\n", extractDir_.c_str());
    return true;
}

/* ── Self-Replace via Batch Script ───────────────────────────────────── */

std::string PcUpdater::getCurrentExePath()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return to_utf8(path);
}

std::string PcUpdater::prepareInstall()
{
    std::string exePath = getCurrentExePath();
    std::string exeDir = fs::path(exePath).parent_path().string();
    std::string batPath = tempDir_ + "\\update.bat";

    std::ofstream bat(batPath);
    if (!bat.is_open()) return "";

    bat << "@echo off\r\n";
    bat << "echo Updating CrossPad...\r\n";
    bat << "echo Waiting for CrossPad to exit...\r\n";
    bat << "\r\n";

    // Retry loop — wait up to 30 seconds for the exe to become writable
    bat << "set RETRIES=0\r\n";
    bat << ":retry_copy\r\n";
    bat << "timeout /t 2 /nobreak >nul\r\n";
    bat << "copy /y \"" << extractDir_ << "\\CrossPad.exe\" \"" << exePath << "\" >nul 2>&1\r\n";
    bat << "if errorlevel 1 (\r\n";
    bat << "    set /a RETRIES+=1\r\n";
    bat << "    if %RETRIES% lss 15 (\r\n";
    bat << "        echo Retrying... (%RETRIES%/15)\r\n";
    bat << "        goto retry_copy\r\n";
    bat << "    )\r\n";
    bat << "    echo ERROR: Failed to copy CrossPad.exe after 15 attempts\r\n";
    bat << "    pause\r\n";
    bat << "    exit /b 1\r\n";
    bat << ")\r\n";
    bat << "\r\n";

    // Copy SDL2.dll if present
    bat << "if exist \"" << extractDir_ << "\\SDL2.dll\" (\r\n";
    bat << "    copy /y \"" << extractDir_ << "\\SDL2.dll\" \"" << exeDir << "\\SDL2.dll\"\r\n";
    bat << ")\r\n";
    bat << "\r\n";

    // Copy assets if present
    bat << "if exist \"" << extractDir_ << "\\assets\" (\r\n";
    bat << "    xcopy /e /y /q \"" << extractDir_ << "\\assets\" \"" << exeDir << "\\assets\\\"\r\n";
    bat << ")\r\n";
    bat << "\r\n";

    // Restart first, then cleanup (bat is inside tempDir so rmdir must be last)
    bat << "echo Update complete! Restarting...\r\n";
    bat << "start \"\" \"" << exePath << "\"\r\n";
    bat << "\r\n";
    bat << "timeout /t 2 /nobreak >nul\r\n";
    bat << "rmdir /s /q \"" << tempDir_ << "\" 2>nul\r\n";
    bat << "exit\r\n";

    bat.close();
    printf("[Updater] Batch script written to %s\n", batPath.c_str());
    return batPath;
}

void PcUpdater::installAndRestart()
{
    std::string batPath = prepareInstall();
    if (batPath.empty()) {
        printf("[Updater] ERROR: Failed to write update script\n");
        return;
    }

    // Launch the batch script as a detached process
    std::wstring wBatPath = to_wide(batPath);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Use cmd.exe /c to run the batch file
    std::wstring cmdLine = L"cmd.exe /c \"" + wBatPath + L"\"";

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr, nullptr,
        &si, &pi
    );

    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        printf("[Updater] Update script launched, exiting...\n");
    } else {
        printf("[Updater] ERROR: Failed to launch update script (error %lu)\n", GetLastError());
        return;
    }

    // Graceful shutdown — same path as the Power OFF button
    crosspad_gui::getGuiPlatform().sendPowerOff();
}

#else // Non-Windows stub

UpdateInfo PcUpdater::checkForUpdate()
{
    UpdateInfo info;
    info.currentVersion = CROSSPAD_PC_VERSION;
    info.errorMessage = "Auto-update not available on this platform";
    return info;
}

bool PcUpdater::downloadUpdate(const UpdateInfo&, UpdateProgressCallback cb)
{
    if (cb) cb(UpdateState::Error, 0, "Not available on this platform");
    return false;
}

std::string PcUpdater::getCurrentExePath() { return ""; }
std::string PcUpdater::prepareInstall() { return ""; }
void PcUpdater::installAndRestart() {}

#endif // _WIN32
