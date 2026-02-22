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

#include "lvgl.h"

// crosspad-core interfaces (BEFORE Windows.h to avoid ERROR macro conflict)
#include "crosspad/platform/IClock.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/synth/IAudioOutput.hpp"
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

// PC event bus
#include "PcEventBus.hpp"

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
// PcKeyValueStore — IKeyValueStore in-memory stub (returns defaults)
// =============================================================================

class PcKeyValueStore : public IKeyValueStore {
public:
    bool init() override { return true; }

    void saveBool(const char* ns, const char* key, bool value) override {
        store_[makeKey(ns, key)] = value ? 1 : 0;
    }

    void saveU8(const char* ns, const char* key, uint8_t value) override {
        store_[makeKey(ns, key)] = value;
    }

    void saveI32(const char* ns, const char* key, int32_t value) override {
        store_[makeKey(ns, key)] = value;
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

    void eraseAll() override { store_.clear(); }

private:
    std::string makeKey(const char* ns, const char* key) {
        return std::string(ns) + "/" + key;
    }

    std::map<std::string, int32_t> store_;
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
#ifdef _MSC_VER
        char exePath[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;

        std::string path(exePath, len);
        // Remove exe filename → bin directory
        size_t pos = path.find_last_of("\\/");
        if (pos == std::string::npos) return;
        path = path.substr(0, pos);
        // Go up one level → project root
        pos = path.find_last_of("\\/");
        if (pos == std::string::npos) return;
        path = path.substr(0, pos);
        // Convert backslashes to forward slashes (LVGL convention)
        for (char& c : path) {
            if (c == '\\') c = '/';
        }
        assetPrefix_ = path + "/crosspad-gui/assets/";
#endif
    }
};

// =============================================================================
// PcFileSystem — IFileSystem using Win32 / POSIX
// =============================================================================

class PcFileSystem : public crosspad_gui::IFileSystem {
public:
    bool listDirectory(const std::string& path, std::vector<crosspad_gui::FileItem>& outEntries) override {
        (void)path;
        outEntries.clear();
        // Stub: return empty listing. Can be extended to use std::filesystem.
        return false;
    }
};

// =============================================================================
// Static singletons
// =============================================================================

static PcClock         s_clock;
static PcLedStrip      s_ledStrip;
static NullMidiOutput  s_nullMidi;
static NullAudioOutput s_nullAudio;
static PcEventBus      s_eventBus;
static PcKeyValueStore s_kvStore;
static PcGuiPlatform   s_guiPlatform;
static PcFileSystem    s_fileSystem;

static PadLedController s_padLedController;
static PadAnimator      s_padAnimator;
static PadManager       s_padManager;

static IAudioOutput* s_audioOutput = &s_nullAudio;

static bool s_initialized = false;

} // anonymous namespace

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

    // Event bus
    s_eventBus.init();

    // Settings singleton
    settings = CrosspadSettings::getInstance();

    // Initialize pad subsystem
    s_padAnimator.init(s_ledStrip, s_clock);
    s_padLedController.init(s_ledStrip, s_eventBus, settings);
    s_padManager.init(s_padLedController, s_padAnimator, s_nullMidi, s_eventBus, settings, &status);
    s_padManager.begin();
    s_padLedController.begin();

    // Register GUI platform
    crosspad_gui::setGuiPlatform(&s_guiPlatform);
    crosspad_gui::setFileSystem(&s_fileSystem);

    printf("[PC] Platform stubs initialized\n");
}

// Allow main.cpp to swap in PcMidi as the MIDI output
void pc_platform_set_midi_output(IMidiOutput* midi) {
    if (midi) {
        s_padManager.init(s_padLedController, s_padAnimator, *midi, s_eventBus, settings, &status);
        s_padManager.begin();
    }
}

// Allow main.cpp to swap in PcAudioOutput as the audio output
void pc_platform_set_audio_output(IAudioOutput* audio) {
    if (audio) {
        s_audioOutput = audio;
    }
}
