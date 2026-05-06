/**
 * @file    test_gui_harness.cpp
 * @brief   GUI integration tests — launches the full simulator and interacts
 *          via the TCP remote control protocol (localhost:19840).
 *
 * These tests exercise the LVGL interface end-to-end: launch app, click on
 * launcher icons, navigate settings, verify screenshot changes, pad input
 * reflected in stats, etc.
 *
 * Requires: bin/CrossPad.exe built and accessible. The test launches it as a
 * subprocess, waits for the TCP server, runs test scenarios, then kills it.
 *
 * Protocol: newline-delimited JSON over TCP.
 *   Request:  {"cmd":"screenshot"}\n
 *   Response: {"ok":true,"width":490,"height":660,...}\n
 */

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET socket_t;
#  define CLOSE_SOCKET closesocket
#  define SOCKET_INVALID INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <signal.h>
   typedef int socket_t;
#  define CLOSE_SOCKET close
#  define SOCKET_INVALID (-1)
#endif

// ── Minimal JSON helpers ──

static std::string json_get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static int json_get_int(const std::string& json, const std::string& key, int dflt = 0) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return dflt;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    try { return std::stoi(json.substr(pos)); } catch (...) { return dflt; }
}

static double json_get_double(const std::string& json, const std::string& key, double dflt = 0.0) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return dflt;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    try { return std::stod(json.substr(pos)); } catch (...) { return dflt; }
}

static bool json_get_bool(const std::string& json, const std::string& key, bool dflt = false) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return dflt;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return dflt;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos < json.size() && json[pos] == 't') return true;
    if (pos < json.size() && json[pos] == 'f') return false;
    return dflt;
}

// ── TCP client for simulator remote control ──

class SimulatorClient {
public:
    bool connect(int timeoutMs = 10000) {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == SOCKET_INVALID) return false;

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(19840);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        int waited = 0;
        while (waited < timeoutMs) {
            if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            waited += 200;
            // Recreate socket after failed connect
            CLOSE_SOCKET(sock_);
            sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }
        return false;
    }

    void disconnect() {
        if (sock_ != SOCKET_INVALID) {
            CLOSE_SOCKET(sock_);
            sock_ = SOCKET_INVALID;
        }
    }

    std::string sendCommand(const std::string& json) {
        std::string msg = json + "\n";
        send(sock_, msg.c_str(), (int)msg.size(), 0);

        // Read response (newline-delimited)
        std::string buf;
        char chunk[8192];
        while (true) {
            int n = recv(sock_, chunk, sizeof(chunk) - 1, 0);
            if (n <= 0) break;
            chunk[n] = '\0';
            buf.append(chunk, n);
            if (buf.find('\n') != std::string::npos) break;
        }
        // Strip trailing newline
        if (!buf.empty() && buf.back() == '\n') buf.pop_back();
        return buf;
    }

    // Convenience wrappers
    std::string ping() {
        return sendCommand(R"({"cmd":"ping"})");
    }

    std::string screenshot() {
        return sendCommand(R"({"cmd":"screenshot"})");
    }

    std::string click(int x, int y) {
        return sendCommand("{\"cmd\":\"click\",\"x\":" + std::to_string(x) +
                          ",\"y\":" + std::to_string(y) + "}");
    }

    std::string padPress(int pad, int velocity = 127) {
        return sendCommand("{\"cmd\":\"pad_press\",\"pad\":" + std::to_string(pad) +
                          ",\"velocity\":" + std::to_string(velocity) + "}");
    }

    std::string padRelease(int pad) {
        return sendCommand("{\"cmd\":\"pad_release\",\"pad\":" + std::to_string(pad) + "}");
    }

    std::string encoderRotate(int delta) {
        return sendCommand("{\"cmd\":\"encoder_rotate\",\"delta\":" + std::to_string(delta) + "}");
    }

    std::string encoderPress() {
        return sendCommand(R"({"cmd":"encoder_press"})");
    }

    std::string encoderRelease() {
        return sendCommand(R"({"cmd":"encoder_release"})");
    }

    std::string stats() {
        return sendCommand(R"({"cmd":"stats"})");
    }

    std::string settingsGet(const std::string& category = "all") {
        return sendCommand("{\"cmd\":\"settings_get\",\"category\":\"" + category + "\"}");
    }

    std::string settingsSet(const std::string& key, int value) {
        return sendCommand("{\"cmd\":\"settings_set\",\"key\":\"" + key +
                          "\",\"value\":" + std::to_string(value) + "}");
    }

    std::string midiNoteOn(int note, int velocity = 100, int channel = 0) {
        return sendCommand("{\"cmd\":\"midi_note_on\",\"channel\":" + std::to_string(channel) +
                          ",\"note\":" + std::to_string(note) +
                          ",\"velocity\":" + std::to_string(velocity) + "}");
    }

    std::string midiNoteOff(int note, int channel = 0) {
        return sendCommand("{\"cmd\":\"midi_note_off\",\"channel\":" + std::to_string(channel) +
                          ",\"note\":" + std::to_string(note) + "}");
    }

    std::string audioLevel(int stream = 0) {
        return sendCommand("{\"cmd\":\"audio_level\",\"stream\":" + std::to_string(stream) + "}");
    }

    std::string citestRun() {
        return sendCommand(R"({"cmd":"citest_run"})");
    }

    std::string citestStatus() {
        return sendCommand(R"({"cmd":"citest_status"})");
    }

    void waitFrames(int n = 3) {
        // Wait for LVGL to process N frames (~5ms each)
        std::this_thread::sleep_for(std::chrono::milliseconds(n * 20));
    }

private:
    socket_t sock_ = SOCKET_INVALID;
};

// ── Process management ──

#ifdef _WIN32
static HANDLE s_simProcess = NULL;

static bool launchSimulator() {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Try multiple paths — test may run from project root or from bin/
    const char* paths[] = { "bin\\CrossPad.exe", "CrossPad.exe", "..\\bin\\CrossPad.exe" };
    bool launched = false;
    for (auto path : paths) {
        char cmd[MAX_PATH];
        strncpy(cmd, path, MAX_PATH - 1);
        cmd[MAX_PATH - 1] = '\0';
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                           CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
            launched = true;
            break;
        }
    }
    if (!launched) {
        printf("[GUI Test] Failed to launch simulator (error %lu)\n", GetLastError());
        return false;
    }
    s_simProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    printf("[GUI Test] Simulator launched (PID %lu)\n", pi.dwProcessId);
    return true;
}

static void killSimulator() {
    if (s_simProcess) {
        TerminateProcess(s_simProcess, 0);
        WaitForSingleObject(s_simProcess, 3000);
        CloseHandle(s_simProcess);
        s_simProcess = NULL;
        printf("[GUI Test] Simulator killed\n");
    }
}
#else
static pid_t s_simPid = 0;

static bool launchSimulator() {
    s_simPid = fork();
    if (s_simPid == 0) {
        // Try multiple paths — Linux build drops "bin/CrossPad" (no .exe);
        // legacy ".exe" suffix kept for cross-platform parity.
        const char* paths[] = {
            "bin/CrossPad",
            "./CrossPad",
            "bin/CrossPad.exe",
            "../bin/CrossPad",
        };
        for (auto p : paths) execl(p, "CrossPad", static_cast<char*>(nullptr));
        _exit(1);
    }
    if (s_simPid < 0) return false;
    printf("[GUI Test] Simulator launched (PID %d)\n", s_simPid);
    return true;
}

static void killSimulator() {
    if (s_simPid > 0) {
        kill(s_simPid, SIGTERM);
        s_simPid = 0;
        printf("[GUI Test] Simulator killed\n");
    }
}
#endif

// ── Shared fixtures ──

static SimulatorClient g_client;
static bool g_simulatorRunning = false;

// ── Main — launch simulator, run tests, cleanup ──

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    // Launch simulator
    if (!launchSimulator()) {
        printf("[GUI Test] Cannot start simulator — is bin/CrossPad.exe built?\n");
        return 1;
    }

    // Connect to remote control
    printf("[GUI Test] Waiting for simulator TCP server...\n");
    if (!g_client.connect(15000)) {
        printf("[GUI Test] Failed to connect to simulator on port 19840\n");
        killSimulator();
        return 1;
    }
    g_simulatorRunning = true;
    printf("[GUI Test] Connected to simulator\n");

    // Let LVGL fully initialize (launcher, status bar, etc.)
    std::this_thread::sleep_for(std::chrono::seconds(2));

    int result = Catch::Session().run(argc, argv);

    g_client.disconnect();
    killSimulator();

#ifdef _WIN32
    WSACleanup();
#endif

    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// GUI Test Scenarios
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("GUI: Simulator responds to ping", "[gui]") {
    auto resp = g_client.ping();
    REQUIRE(json_get_bool(resp, "ok") == true);
}

TEST_CASE("GUI: Screenshot returns valid image data", "[gui]") {
    auto resp = g_client.screenshot();
    REQUIRE(json_get_bool(resp, "ok") == true);

    int width = json_get_int(resp, "width");
    int height = json_get_int(resp, "height");

    // Simulator window is 490x680 (LCD 320x240 + emulator body)
    REQUIRE(width == 490);
    REQUIRE(height == 680);

    std::string format = json_get_string(resp, "format");
    REQUIRE(format == "bmp");

    // base64 data should be non-empty
    std::string data = json_get_string(resp, "data");
    REQUIRE(data.size() > 1000); // BMP header + pixel data
}

TEST_CASE("GUI: Stats show registered apps", "[gui]") {
    auto resp = g_client.stats();
    REQUIRE(json_get_bool(resp, "ok") == true);

    int appCount = json_get_int(resp, "app_count");
    REQUIRE(appCount >= 3); // Mixer, MLPiano, Settings at minimum

    // Capabilities should include Pads and Display
    REQUIRE(resp.find("\"Pads\"") != std::string::npos);
    REQUIRE(resp.find("\"Display\"") != std::string::npos);
}

TEST_CASE("GUI: Pad press via remote changes pad state", "[gui]") {
    // Press pad 0
    auto pressResp = g_client.padPress(0, 100);
    REQUIRE(json_get_bool(pressResp, "ok") == true);

    g_client.waitFrames(2);

    // Verify in stats that pad 0 is pressed
    auto stats = g_client.stats();
    // The stats response includes a pads array — check pad 0 is pressed
    // Simple string search for pad 0 pressed state
    // (pads array is [{...},{...},...] — first entry is pad 0)
    auto padsPos = stats.find("\"pads\":[");
    REQUIRE(padsPos != std::string::npos);
    // First pad entry should show pressed:true
    auto firstPad = stats.substr(padsPos, 200);
    REQUIRE(firstPad.find("\"pressed\":true") != std::string::npos);

    // Release pad 0
    auto releaseResp = g_client.padRelease(0);
    REQUIRE(json_get_bool(releaseResp, "ok") == true);

    g_client.waitFrames(2);

    // Verify pad 0 is now released
    stats = g_client.stats();
    padsPos = stats.find("\"pads\":[");
    firstPad = stats.substr(padsPos, 200);
    REQUIRE(firstPad.find("\"pressed\":false") != std::string::npos);
}

TEST_CASE("GUI: Click on screen doesn't crash", "[gui]") {
    // Click in the middle of the LCD area (emulator LCD starts around x=85, y=39)
    auto resp = g_client.click(245, 160);
    REQUIRE(json_get_bool(resp, "ok") == true);

    g_client.waitFrames(5);

    // Simulator should still be alive
    auto ping = g_client.ping();
    REQUIRE(json_get_bool(ping, "ok") == true);
}

TEST_CASE("GUI: Encoder rotation doesn't crash", "[gui]") {
    // Rotate encoder (scroll wheel)
    auto resp = g_client.encoderRotate(3);
    REQUIRE(json_get_bool(resp, "ok") == true);

    g_client.waitFrames(3);

    resp = g_client.encoderRotate(-3);
    REQUIRE(json_get_bool(resp, "ok") == true);

    // Simulator still alive
    auto ping = g_client.ping();
    REQUIRE(json_get_bool(ping, "ok") == true);
}

TEST_CASE("GUI: Settings read/write via remote control", "[gui]") {
    // Read current brightness
    auto getResp = g_client.settingsGet("display");
    REQUIRE(json_get_bool(getResp, "ok") == true);
    int origBri = json_get_int(getResp, "rgb_brightness");

    // Change brightness
    auto setResp = g_client.settingsSet("rgb_brightness", 42);
    REQUIRE(json_get_bool(setResp, "ok") == true);

    // Read back
    getResp = g_client.settingsGet("display");
    int newBri = json_get_int(getResp, "rgb_brightness");
    REQUIRE(newBri == 42);

    // Restore
    g_client.settingsSet("rgb_brightness", origBri);
}

TEST_CASE("GUI: Screenshot changes after pad press", "[gui]") {
    // Take baseline screenshot
    auto shot1 = g_client.screenshot();
    REQUIRE(json_get_bool(shot1, "ok") == true);
    std::string data1 = json_get_string(shot1, "data");

    // Press pad (should change LED colors in the emulator body area)
    g_client.padPress(0, 127);
    g_client.waitFrames(5);

    // Take second screenshot
    auto shot2 = g_client.screenshot();
    REQUIRE(json_get_bool(shot2, "ok") == true);
    std::string data2 = json_get_string(shot2, "data");

    // The screenshots should differ (pad LED lit up)
    REQUIRE(data1 != data2);

    g_client.padRelease(0);
    g_client.waitFrames(3);
}

TEST_CASE("GUI: Launch app by clicking its launcher icon", "[gui]") {
    // The launcher shows app icons in the LCD area (320x240, offset in emulator window)
    // Exact positions depend on theme/layout — we click roughly where the first app tile is
    // LCD area in emulator: x=85..405, y=39..279 (320x240 LCD)
    // First app tile is roughly at LCD center-ish

    // Take screenshot before click
    auto shot1 = g_client.screenshot();
    std::string data1 = json_get_string(shot1, "data");

    // Click on approximate first app tile position in the launcher grid
    // Launcher grid typically starts below status bar (~y=70 in LCD, +39 offset = ~109 in window)
    g_client.click(170, 150);
    g_client.waitFrames(10); // Give app time to load

    // Take screenshot after click
    auto shot2 = g_client.screenshot();
    std::string data2 = json_get_string(shot2, "data");

    // Screen should have changed (app opened or at least UI responded)
    REQUIRE(data1 != data2);

    // Go back to launcher — press Escape key (SDL keycode 27)
    g_client.sendCommand(R"({"cmd":"key","keycode":27})");
    g_client.waitFrames(10);
}

// ── Tier 3 in-app integration: drive the existing CITest app over TCP ────
//
// CITestApp.cpp owns the canonical audio-pipeline test sequence (synth →
// mixer → output → tap capture, 7 stages). We orchestrate it via the
// citest_run / citest_status remote cmds and assert all stages PASS.

TEST_CASE("Audio in-app: smoke primitives respond", "[gui][audio]") {
    SECTION("audio_level returns ok with bounded values") {
        auto resp = g_client.audioLevel(0);
        REQUIRE(json_get_bool(resp, "ok") == true);
        REQUIRE(json_get_int(resp, "stream") == 0);
        REQUIRE(json_get_double(resp, "left")  >= 0.0);
        REQUIRE(json_get_double(resp, "left")  <= 1.0);
    }
    SECTION("midi_note_on rejects out-of-range note") {
        auto resp = g_client.midiNoteOn(200, 100);
        REQUIRE(json_get_bool(resp, "ok") == false);
    }
    SECTION("midi_note_on accepts valid note") {
        auto on = g_client.midiNoteOn(60, 100);
        REQUIRE(json_get_bool(on, "ok") == true);
        g_client.midiNoteOff(60);
    }
}

TEST_CASE("Audio in-app: CITest end-to-end (7 stages)", "[gui][audio][citest]") {
    auto run = g_client.citestRun();
    REQUIRE(json_get_bool(run, "ok") == true);

    // Run does ~5–6s of mixer/synth gymnastics. Poll up to 15s.
    bool finished = false;
    std::string finalStatus;
    for (int i = 0; i < 75 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        finalStatus = g_client.citestStatus();
        if (!json_get_bool(finalStatus, "ok")) continue;
        if (!json_get_bool(finalStatus, "running")) finished = true;
    }

    INFO("final status = " << finalStatus);
    REQUIRE(finished);

    // Naive json_get_int gets confused by "result":"pass" appearing earlier
    // than the top-level "pass":N. Count stage results directly via substring
    // matching instead.
    auto countOccurrences = [&](const std::string& needle) {
        size_t pos = 0;
        int n = 0;
        while ((pos = finalStatus.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    };
    const int passCount = countOccurrences("\"result\":\"pass\"");
    const int failCount = countOccurrences("\"result\":\"fail\"");
    INFO("pass=" << passCount << " fail=" << failCount);
    REQUIRE(failCount == 0);
    REQUIRE(passCount == 7);
}

TEST_CASE("GUI: Multiple rapid pad presses don't crash", "[gui]") {
    // Stress test: rapid-fire all 16 pads
    for (int i = 0; i < 16; i++) {
        g_client.padPress(i, 100);
    }
    g_client.waitFrames(5);

    for (int i = 0; i < 16; i++) {
        g_client.padRelease(i);
    }
    g_client.waitFrames(5);

    // Simulator should still be alive and responsive
    auto ping = g_client.ping();
    REQUIRE(json_get_bool(ping, "ok") == true);

    // Verify all pads are released
    auto stats = g_client.stats();
    REQUIRE(json_get_bool(stats, "ok") == true);
    // None of the 16 pads should show pressed:true
    // Count occurrences of "pressed":true — should be 0
    size_t pos = 0;
    int pressedCount = 0;
    while ((pos = stats.find("\"pressed\":true", pos)) != std::string::npos) {
        pressedCount++;
        pos++;
    }
    REQUIRE(pressedCount == 0);
}
