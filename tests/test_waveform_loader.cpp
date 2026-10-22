// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sampler's waveform loader: every layer of a kit bigger than its queue
// gets cached. The queue holds LOADER_QUEUE_SIZE requests; the rest of a big
// kit is fetched by the loader's idle sweeps, which walk the kit again.
#include <catch2/catch_test_macros.hpp>

#include "FreeRTOS.h"
#include "task.h"

#include <crosspad-sampler/SamplerConfig.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/waveform/waveform_cache.hpp>
#include <crosspad-sampler/waveform/waveform_loader.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* The kit loader is the sampler's LVGL side; the waveform loader only asks
 * whether it is busy. */
namespace crosspad_sampler {
bool kit_load_task_is_loading() { return false; }
}

namespace {

/// More layers than the loader's queue holds, as DREAMSCAPE has (45).
constexpr int kLayers = crosspad_sampler::LOADER_QUEUE_SIZE + 13;
/// Room for the queue to drain, an idle sweep and the layers it fetches.
constexpr int kCacheWaitMs = 10000;
constexpr int kPollMs = 50;

class OneKitManager : public crosspad::IKitManager {
public:
    crosspad::KitInfo kit;
    /// Set: the next getCurrentKit() first runs a whole preload of its own --
    /// the platform's top-up filling the queue while a sweep is under way.
    std::atomic<bool> preloadInside{false};

    int getKitCount() const override { return 1; }
    crosspad::KitInfo* getKit(int id) override { return id == 0 ? &kit : nullptr; }
    crosspad::KitInfo* getCurrentKit() override {
        if (preloadInside.exchange(false)) crosspad_sampler::waveform_loader_preload_kit();
        return &kit;
    }
    int getCurrentKitId() const override { return 0; }
    void setCurrentKitId(int) override {}
    bool isLoading() const override { return false; }
    std::vector<crosspad::KitInfo> listKits(const std::string&) override { return {}; }
    crosspad::KitLoadResult loadKit(crosspad::KitInfo&) override { return {}; }
    bool saveKit(const crosspad::KitInfo&) override { return false; }
};

bool fakeInfo(const char*, crosspad_sampler::AudioFileInfo* out) {
    out->sampleCount = 4410;
    out->sampleRate = 44100;
    out->channels = 1;
    out->bitDepth = 16;
    return true;
}

bool fakeSamples(const char*, int16_t* buf, uint32_t n, uint32_t, uint32_t) {
    std::memset(buf, 0, n * sizeof(int16_t));
    return true;
}

void* fakeAlloc(uint32_t bytes) { return std::malloc(bytes); }
void fakeFree(void* p) { std::free(p); }

int cachedLayers(const crosspad::KitInfo& kit) {
    int n = 0;
    for (const auto& pad : kit.pads)
        for (const auto& s : pad.samples)
            if (crosspad_sampler::waveform_cache_contains(s.sample.c_str())) ++n;
    return n;
}

int waitForLayers(const crosspad::KitInfo& kit, int want) {
    int n = 0;
    for (int waited = 0; waited < kCacheWaitMs; waited += kPollMs) {
        n = cachedLayers(kit);
        if (n >= want) break;
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
    return n;
}

} // namespace

TEST_CASE("waveform loader: a sweep that meets a full queue keeps sweeping",
          "[sampler][waveform]") {
    static OneKitManager mgr;
    mgr.kit.path = "/hil/big/kit.json";
    for (int i = 0; i < kLayers; ++i) {
        // Absolute: resolveKitSamplePath() takes it as it is, no stat().
        mgr.kit.pads[i % KIT_PADS].AddSample("/hil/big/layer" + std::to_string(i) + ".wav");
    }

    crosspad_sampler::SamplerPlatformCallbacks cb{};
    cb.waveform.getFileInfo = fakeInfo;
    cb.waveform.getWaveformSamples = fakeSamples;
    cb.waveform.allocWaveformMem = fakeAlloc;
    cb.waveform.freeWaveformMem = fakeFree;
    crosspad_sampler::sampler_set_platform_callbacks(cb);

    auto& svc = crosspad::getPlatformServices();
    crosspad::IKitManager* const previous = svc.kitManager;
    svc.kitManager = &mgr;

    // A new kit: empty cache, empty queue, a fresh sweep budget.
    crosspad_sampler::waveform_cache_clear_if_kit_changed(&mgr.kit, mgr.kit.path.c_str());
    REQUIRE(crosspad_sampler::waveform_loader_start());

    /* The loader's first idle sweep walks the kit, and before it queues
     * anything the top-up fills the queue. Every request of that sweep is
     * dropped, though most of the kit is still to fetch. */
    mgr.preloadInside = true;

    CHECK(waitForLayers(mgr.kit, kLayers) == kLayers);
    CHECK_FALSE(mgr.preloadInside.load());

    crosspad_sampler::waveform_loader_stop();
    crosspad_sampler::waveform_cache_clear();
    svc.kitManager = previous;
    crosspad_sampler::sampler_set_platform_callbacks({});
}
