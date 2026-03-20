#pragma once

/**
 * @file    test_helpers.hpp
 * @brief   Reusable test fixtures and mocks for crosspad-core unit tests.
 */

#include "crosspad/platform/CrosspadPlatformInit.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/event/FreeRtosEventBus.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/midi/NullMidiOutput.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

#include <chrono>
#include <cstring>

namespace test {

// ── PcClock (std::chrono, same as PcPlatformStubs) ──

class TestClock : public crosspad::IClock {
public:
    int64_t getTimeUs() override {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }
};

// ── In-memory LED strip (same as PcLedStrip) ──

class TestLedStrip : public crosspad::ILedStrip {
public:
    void begin() override {}
    void setPixel(uint16_t idx, crosspad::RgbColor color) override {
        if (idx < 16) pixels[idx] = color;
    }
    void refresh() override { refreshCount++; }
    uint16_t getPixelCount() const override { return 16; }

    crosspad::RgbColor pixels[16] = {};
    int refreshCount = 0;
};

// ── In-memory key-value store for settings tests ──

class MemoryKVStore : public crosspad::IKeyValueStore {
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

    size_t size() const { return store_.size(); }

private:
    std::string makeKey(const char* ns, const char* key) {
        return std::string(ns) + "/" + key;
    }
    std::map<std::string, int32_t> store_;
};

// ── Full platform fixture ──

struct TestPlatform {
    crosspad::FreeRtosEventBus eventBus;
    TestClock clock;
    TestLedStrip ledStrip;
    crosspad::NullMidiOutput midiOutput;
    crosspad::PadManager padManager;
    crosspad::PadLedController padLedController;
    crosspad::PadAnimator padAnimator;
    crosspad::CrosspadStatus status;
    MemoryKVStore kvStore;

    ~TestPlatform() {
        crosspad::crosspad_platform_reset();
    }

    bool init() {
        crosspad::CrosspadPlatformConfig config;
        config.eventBus        = &eventBus;
        config.clock           = &clock;
        config.ledStrip        = &ledStrip;
        config.midiOutput      = &midiOutput;
        config.padManager      = &padManager;
        config.padLedController = &padLedController;
        config.padAnimator     = &padAnimator;
        config.status          = &status;
        config.kvStore         = &kvStore;
        config.padCount        = 16;

        return crosspad::crosspad_platform_init(config);
    }
};

} // namespace test
