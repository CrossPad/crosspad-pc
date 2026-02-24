/**
 * @file PcPlatformStubs.cpp
 * @brief PC platform stubs: interface implementations + singleton getters + globals
 *
 * Provides all platform-specific implementations required by crosspad-core
 * and crosspad-gui for the desktop (PC) simulator.
 */

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>

#include "lvgl.h"

#include <ArduinoJson.h>

// crosspad-core interfaces (BEFORE Windows.h to avoid ERROR macro conflict)
#include "crosspad/platform/CrosspadPlatformInit.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/synth/IAudioOutput.hpp"
#include "crosspad/synth/IAudioInput.hpp"
#include "crosspad/synth/ISynthEngine.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"

// Windows.h AFTER crosspad headers (its ERROR macro conflicts with AnimType::ERROR)
#ifdef _MSC_VER
#include <Windows.h>
#else
#include <unistd.h>
#endif

// crosspad-gui interfaces
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/platform/IFileSystem.h"

#include "crosspad/event/FreeRtosEventBus.hpp"

using namespace crosspad;

// =============================================================================
// Global variables required by crosspad-gui (extern in status_bar.cpp etc.)
// =============================================================================

CrosspadSettings* settings = nullptr;
CrosspadStatus status;
lv_obj_t* status_c = nullptr;

// =============================================================================
// PcClock — IClock via std::chrono
// =============================================================================

namespace {

class PcClock : public IClock {
public:
    PcClock() : start_(std::chrono::steady_clock::now()) {}

    int64_t getTimeUs() override {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

// =============================================================================
// PcLedStrip — ILedStrip no-op stub (16 virtual LEDs)
// =============================================================================

class PcLedStrip : public ILedStrip {
public:
    void begin() override {}

    void setPixel(uint16_t idx, RgbColor color) override {
        if (idx < PIXEL_COUNT) pixels_[idx] = color;
    }

    void refresh() override {}

    uint16_t getPixelCount() const override { return PIXEL_COUNT; }

    RgbColor getPixelColor(uint16_t idx) const {
        return (idx < PIXEL_COUNT) ? pixels_[idx] : RgbColor(0, 0, 0);
    }

private:
    static constexpr uint16_t PIXEL_COUNT = 16;
    RgbColor pixels_[PIXEL_COUNT]{};
};

// =============================================================================
// NullMidiOutput — fallback IMidiOutput when PcMidi is not available
// =============================================================================

class NullMidiOutput : public IMidiOutput {
public:
    void sendNoteOn(uint8_t note, uint8_t channel) override { (void)note; (void)channel; }
    void sendNoteOff(uint8_t note, uint8_t channel) override { (void)note; (void)channel; }
};

// =============================================================================
// NullAudioOutput — fallback IAudioOutput when PcAudio is not available
// =============================================================================

class NullAudioOutput : public IAudioOutput {
public:
    uint32_t write(const int16_t*, uint32_t frameCount) override { return frameCount; }
    uint32_t getSampleRate() const override { return 44100; }
    uint32_t getBufferSize() const override { return 256; }
};

// =============================================================================
// PcKeyValueStore — IKeyValueStore backed by ~/.crosspad/preferences.json
// =============================================================================

class PcKeyValueStore : public IKeyValueStore {
public:
    bool init() override {
        profileDir_ = resolveProfileDir();
        filePath_ = profileDir_ + "/preferences.json";

        // Ensure ~/.crosspad/ and ~/.crosspad/cache/ exist
        std::filesystem::create_directories(profileDir_);
        std::filesystem::create_directories(profileDir_ + "/cache");

        load();
        printf("[KVStore] Profile dir: %s\n", profileDir_.c_str());
        return true;
    }

    void saveBool(const char* ns, const char* key, bool value) override {
        store_[makeKey(ns, key)] = value ? 1 : 0;
        flush();
    }

    void saveU8(const char* ns, const char* key, uint8_t value) override {
        store_[makeKey(ns, key)] = value;
        flush();
    }

    void saveI32(const char* ns, const char* key, int32_t value) override {
        store_[makeKey(ns, key)] = value;
        flush();
    }

    bool readBool(const char* ns, const char* key, bool defaultVal) override {
        auto it = store_.find(makeKey(ns, key));
        return it != store_.end() ? (it->second != 0) : defaultVal;
    }

    uint8_t readU8(const char* ns, const char* key, uint8_t defaultVal) override {
        auto it = store_.find(makeKey(ns, key));
        return it != store_.end() ? static_cast<uint8_t>(it->second) : defaultVal;
    }

    int32_t readI32(const char* ns, const char* key, int32_t defaultVal) override {
        auto it = store_.find(makeKey(ns, key));
        return it != store_.end() ? it->second : defaultVal;
    }

    void eraseAll() override {
        store_.clear();
        flush();
    }

    const std::string& getProfileDir() const { return profileDir_; }

private:
    std::map<std::string, int32_t> store_;
    std::string profileDir_;
    std::string filePath_;

    std::string makeKey(const char* ns, const char* key) {
        return std::string(ns) + "/" + key;
    }

    static std::string resolveProfileDir() {
#ifdef _MSC_VER
        const char* userProfile = std::getenv("USERPROFILE");
        if (userProfile) return std::string(userProfile) + "/.crosspad";
#endif
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + "/.crosspad";
        return ".crosspad";
    }

    void load() {
        std::ifstream f(filePath_);
        if (!f.is_open()) return;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        if (err) {
            printf("[KVStore] Failed to parse %s: %s\n", filePath_.c_str(), err.c_str());
            return;
        }

        for (JsonPair kv : doc.as<JsonObject>()) {
            store_[kv.key().c_str()] = kv.value().as<int32_t>();
        }
        printf("[KVStore] Loaded %zu keys from %s\n", store_.size(), filePath_.c_str());
    }

    void flush() {
        JsonDocument doc;
        for (auto it = store_.begin(); it != store_.end(); ++it) {
            doc[it->first] = it->second;
        }

        std::ofstream f(filePath_);
        if (!f.is_open()) {
            printf("[KVStore] Failed to write %s\n", filePath_.c_str());
            return;
        }
        serializeJsonPretty(doc, f);
    }
};

// =============================================================================
// PcGuiPlatform — IGuiPlatform for desktop
// =============================================================================

class PcGuiPlatform : public crosspad_gui::IGuiPlatform {
public:
    PcGuiPlatform() : start_(std::chrono::steady_clock::now()) {
        initAssetPath();
    }

    crosspad_gui::HeapStats getHeapStats() override {
        // Simulated values for PC
        return {512 * 1024 * 1024, 512 * 1024 * 1024, 0, 0};
    }

    uint32_t millis() override {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count());
    }

    const char* assetPathPrefix() override {
        return assetPrefix_.c_str();
    }

    void delayMs(uint32_t ms) override {
#ifdef _MSC_VER
        Sleep(ms);
#else
        usleep(ms * 1000);
#endif
    }

    void sendPowerOff() override {
        printf("[PC] Power off requested (exiting)\n");
        exit(0);
    }

private:
    std::chrono::steady_clock::time_point start_;
    std::string assetPrefix_ = "C:/";

    void initAssetPath() {
        namespace fs = std::filesystem;
        fs::path exeDir;

#ifdef _MSC_VER
        char buf[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            exeDir = fs::path(buf).parent_path();
        }
#elif defined(__linux__)
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            exeDir = fs::path(buf).parent_path();
        }
#elif defined(__APPLE__)
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            exeDir = fs::path(buf).parent_path();
        }
#endif

        // Try exe-relative: exe_dir/../crosspad-gui/assets/
        if (!exeDir.empty()) {
            fs::path candidate = exeDir / ".." / "crosspad-gui" / "assets";
            std::error_code ec;
            if (fs::exists(candidate, ec)) {
                std::string resolved = fs::canonical(candidate, ec).string();
                for (char& c : resolved) { if (c == '\\') c = '/'; }
                assetPrefix_ = resolved + "/";
                return;
            }
        }

        // Fallback: CROSSPAD_ASSETS env var
        const char* envPath = std::getenv("CROSSPAD_ASSETS");
        if (envPath && fs::exists(envPath)) {
            std::string p(envPath);
            for (char& c : p) { if (c == '\\') c = '/'; }
            if (!p.empty() && p.back() != '/') p += '/';
            assetPrefix_ = p;
            return;
        }

        // Fallback: cwd-relative
        {
            std::error_code ec;
            fs::path candidate = fs::current_path(ec) / "crosspad-gui" / "assets";
            if (fs::exists(candidate, ec)) {
                std::string resolved = fs::canonical(candidate, ec).string();
                for (char& c : resolved) { if (c == '\\') c = '/'; }
                assetPrefix_ = resolved + "/";
                return;
            }
        }

        // Last resort
        assetPrefix_ = "C:/";
    }
};

// =============================================================================
// PcFileSystem — IFileSystem using Win32 / POSIX
// =============================================================================

class PcFileSystem : public crosspad_gui::IFileSystem {
public:
    bool listDirectory(const std::string& path, std::vector<crosspad_gui::FileItem>& outEntries) override {
        outEntries.clear();
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (ec) break;
            crosspad_gui::FileItem item;
            item.path = entry.path().string();
            item.name = entry.path().filename().string();
            item.isFolder = entry.is_directory(ec);
            // Normalize backslashes to forward slashes for LVGL
            for (char& c : item.path) {
                if (c == '\\') c = '/';
            }
            outEntries.push_back(std::move(item));
        }
        return !outEntries.empty();
    }
};

// =============================================================================
// Static singletons
// =============================================================================

static PcClock         s_clock;
static PcLedStrip      s_ledStrip;
static NullMidiOutput  s_nullMidi;
static NullAudioOutput s_nullAudio;
static FreeRtosEventBus s_eventBus;
static PcKeyValueStore s_kvStore;
static PcGuiPlatform   s_guiPlatform;
static PcFileSystem    s_fileSystem;

static PadLedController s_padLedController;
static PadAnimator      s_padAnimator;
static PadManager       s_padManager;

static IAudioOutput* s_audioOutput  = &s_nullAudio;
static IAudioOutput* s_audioOutput2 = &s_nullAudio;
static IAudioInput*  s_audioInputs[2] = { nullptr, nullptr };

static bool s_initialized = false;

} // anonymous namespace

// Synth engine singleton (outside anonymous namespace for external access)
static crosspad::ISynthEngine* s_synthEngine = nullptr;

// =============================================================================
// LED color accessor for STM32 emulator window
// =============================================================================

crosspad::RgbColor pc_get_led_color(uint16_t idx) {
    return s_ledStrip.getPixelColor(idx);
}

// =============================================================================
// crosspad-core singleton getters
// =============================================================================

namespace crosspad {

IClock& getClock() {
    return s_clock;
}

IMidiOutput& getMidiOutput() {
    return s_nullMidi;
}

PadManager& getPadManager() {
    return s_padManager;
}

PadLedController& getPadLedController() {
    return s_padLedController;
}

PadAnimator& getPadAnimator() {
    return s_padAnimator;
}

IAudioOutput& getAudioOutput() {
    return *s_audioOutput;
}

} // namespace crosspad

// =============================================================================
// PC platform initialization — call from main() before GUI
// =============================================================================

extern "C" void pc_platform_init() {
    if (s_initialized) return;
    s_initialized = true;

    // Settings pointer for global access
    settings = CrosspadSettings::getInstance();

    // Common crosspad-core initialization
    crosspad::CrosspadPlatformConfig config;
    config.eventBus        = &s_eventBus;
    config.clock           = &s_clock;
    config.ledStrip        = &s_ledStrip;
    config.midiOutput      = &s_nullMidi;
    config.padManager      = &s_padManager;
    config.padLedController = &s_padLedController;
    config.padAnimator     = &s_padAnimator;
    config.kvStore         = &s_kvStore;
    config.settings        = settings;
    config.status          = &status;

    crosspad::crosspad_platform_init(config);

    // Register GUI platform (crosspad-gui specific, not in crosspad-core)
    crosspad_gui::setGuiPlatform(&s_guiPlatform);
    crosspad_gui::setFileSystem(&s_fileSystem);

    printf("[PC] Platform stubs initialized\n");
}

// Allow main.cpp to swap in PcMidi as the MIDI output
void pc_platform_set_midi_output(IMidiOutput* midi) {
    if (midi) {
        crosspad::crosspad_platform_set_midi(*midi);
    }
}

// Allow main.cpp to swap in PcAudioOutput as the audio output
void pc_platform_set_audio_output(IAudioOutput* audio) {
    if (audio) {
        s_audioOutput = audio;
    }
}

void pc_platform_set_audio_output_2(IAudioOutput* audio) {
    if (audio) {
        s_audioOutput2 = audio;
    }
}

void pc_platform_set_audio_input(int index, IAudioInput* input) {
    if (index >= 0 && index < 2) {
        s_audioInputs[index] = input;
    }
}

crosspad::IAudioInput* pc_platform_get_audio_input(int index) {
    if (index >= 0 && index < 2) return s_audioInputs[index];
    return nullptr;
}

// Synth engine getter/setter
void pc_platform_set_synth_engine(crosspad::ISynthEngine* synth) {
    s_synthEngine = synth;
}

crosspad::ISynthEngine* pc_platform_get_synth_engine() {
    return s_synthEngine;
}

// Save current settings to ~/.crosspad/preferences.json
void pc_platform_save_settings() {
    if (settings) {
        settings->saveTo(s_kvStore);
    }
}

// Get user profile directory (~/.crosspad)
const char* pc_platform_get_profile_dir() {
    return s_kvStore.getProfileDir().c_str();
}
