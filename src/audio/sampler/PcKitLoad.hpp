// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcKitLoad.hpp
 * @brief What the simulator's sampler engines share: where a kit's bytes come
 *        from and how a kit's pads are put into a store.
 *
 * The pads' engine (PcSamplerEngine) and a song's second kit (PcSecondKit)
 * each own a store and a source; the reading of WAVs, the resolving of a
 * layer's file and the building of a pad are the same code for both.
 * LVGL-free, so the second kit builds into the unit tests.
 */

#include <crosspad/audio/WavLayout.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/sampler/BlockPool.hpp>
#include <crosspad/sampler/KitTake.hpp>
#include <crosspad/sampler/SampleStore.hpp>
#include <crosspad/sampler/SamplerKit.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace crosspad_pc {

/// Where logical paths ("/crosspad/kits/…") resolve: the folder mounted as
/// the virtual SD card, empty when none is.
void native_sampler_set_root(const std::string& root);
/// A logical path, or one already rooted at the mount, as a filesystem path.
std::string native_sampler_fs_path(const char* path);

namespace kit_load {

using crosspad::SampleDesc;

/* The board's numbers, kept: 16 kB blocks, six to a voice's ring, sixteen
 * rings at most, so a kit streams here the way it does on the card. */
constexpr size_t   kBlockBytes   = 16384;
constexpr uint8_t  kStreamBlocks = 6;
constexpr size_t   kPageBytes    = kStreamBlocks * kBlockBytes;
constexpr uint32_t kMaxPages     = 16;
/* What a sample asks to keep resident, as on the board -- halved by HeadPlan
 * until the kit fits the budget. The board's budget is what its arena has
 * left; the heap here has no such wall, so it is a plain cap that still
 * leaves a long sample streaming. */
constexpr uint32_t kHeadBytes   = 256u * 1024;
constexpr uint32_t kHeadsBudget = 32u * 1024 * 1024;


/* The 0..127 pan the engine's helper takes has its centre here; kits store a
 * signed offset from it. */
constexpr int kPanCentre = 64;

/* A WAV is parsed once per path, when the store loads its head, and kept
 * here: a read only gets a path and a frame number. Written by the loader,
 * read by the streaming thread -- one mutex, held for the copy only. */
class PcSampleSource : public crosspad::ISampleSource {
public:
    struct Entry {
        uint32_t dataOffset = 0;
        uint32_t frames = 0;
        uint32_t rate = 0;
        uint8_t  channels = 0;
    };

    static bool parse(const char* path, Entry& e) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        crosspad::WavLayout lay;
        const bool ok = lay.parse(f) && lay.pcm16();
        std::fclose(f);
        if (!ok) return false;
        e.dataOffset = lay.dataOffset;
        e.frames = lay.frames;
        e.rate = lay.rate;
        e.channels = static_cast<uint8_t>(lay.channels);
        return true;
    }

    static uint32_t readEntry(const char* path, const Entry& e, uint32_t first, uint32_t frames,
                              int16_t* dst) {
        if (frames == 0 || first >= e.frames) return 0;
        if (first + frames > e.frames) frames = e.frames - first;
        FILE* f = std::fopen(path, "rb");
        if (!f) return 0;
        const uint32_t frameBytes = e.channels * sizeof(int16_t);
        uint32_t got = 0;
        if (std::fseek(f, static_cast<long>(e.dataOffset + first * frameBytes), SEEK_SET) == 0) {
            got = static_cast<uint32_t>(std::fread(dst, frameBytes, frames, f));
        }
        std::fclose(f);
        return got;
    }

    bool describe(const char* path, SampleDesc& out) override {
        Entry e;
        if (!parse(path, e)) return false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            table_[path] = e;
        }
        out.frames = e.frames;
        out.sampleRate = e.rate;
        out.channels = e.channels;
        out.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t frames, int16_t* dst) override {
        Entry e;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto it = table_.find(path);
            if (it == table_.end()) return 0;
            e = it->second;
        }
        return readEntry(path, e, first, frames, dst);
    }

    void forget() {
        std::lock_guard<std::mutex> lk(mu_);
        table_.clear();
    }

private:
    std::mutex mu_;
    std::map<std::string, Entry> table_;
};

void* heap_alloc(size_t bytes, void*);
void  heap_free(void* p, void*);

/// Pad @p p from a copy of the kit model into @p kp, its samples loaded into
/// @p store through @p source. @return layers that found their file.
uint16_t build_pad(PcSampleSource& source, crosspad::SampleStore& store, const char* kitPath,
                   const crosspad::TakePad& tp, const crosspad::TakeLayer* layers, uint8_t p,
                   uint32_t headBytes, crosspad::KitPad& kp);

/// Head size for a kit, from every sample's length: what HeadPlan makes of
/// kHeadBytes a sample within kHeadsBudget.
uint32_t plan_heads(PcSampleSource& source, const char* kitPath, const crosspad::TakePad* pads,
                    const crosspad::TakeLayer* layers);

/// Poll @p done every @p poll until it holds or @p bound is up.
template <class Pred>
bool wait_for(Pred done, std::chrono::milliseconds bound, std::chrono::milliseconds poll) {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(poll);
    }
    return true;
}

} // namespace kit_load
} // namespace crosspad_pc
