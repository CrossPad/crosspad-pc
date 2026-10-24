// SPDX-License-Identifier: GPL-3.0-or-later

#include "PcKitLoad.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace crosspad_pc {
namespace {

std::mutex  s_rootMu;
std::string s_root;

} // namespace

void native_sampler_set_root(const std::string& root) {
    std::lock_guard<std::mutex> lk(s_rootMu);
    s_root = root;
}

std::string native_sampler_fs_path(const char* path) {
    if (!path || !path[0]) return {};
    std::string p(path);
    for (char& c : p) { if (c == '\\') c = '/'; }
    std::lock_guard<std::mutex> lk(s_rootMu);
    if (s_root.empty()) return p;
    if (p.compare(0, s_root.size(), s_root) == 0) return p;
    if (p[0] != '/') return s_root + "/" + p;
    return s_root + p;
}

namespace kit_load {

using crosspad::HeadPlan;
using crosspad::KitPad;
using crosspad::PadMode;
using crosspad::SampleDesc;
using crosspad::SamplerKit;
using crosspad::SampleStore;
using crosspad::TakeLayer;
using crosspad::TakePad;

void* heap_alloc(size_t bytes, void*) { return std::malloc(bytes); }
void  heap_free(void* p, void*)       { std::free(p); }

namespace {

/* Pitch the engine does itself; filters and reverse it does not, and the
 * sampler renders a copy with them (crosspad::renderedSamplePath) -- that
 * copy is what plays when it is there, with the step baked in. Reversed, the
 * trim's end is where the copy starts. The board's layer_file(), on stdio. */
struct LayerFile {
    std::string path;
    uint32_t    start = 0;
    uint32_t    end   = 0;
    bool        pitchInFile = false;
};

LayerFile layer_file(PcSampleSource& source, const char* kitPath, const TakePad& pad, const TakeLayer& layer) {
    LayerFile f;
    const std::string logical = crosspad::takeLayerLogical(kitPath, layer);
    f.path  = native_sampler_fs_path(logical.c_str());
    f.start = layer.start;
    f.end   = layer.end;

    const std::string copyLogical = crosspad::takeLayerCopy(pad, logical);
    if (copyLogical.empty()) return f;
    const std::string copy = native_sampler_fs_path(copyLogical.c_str());
    struct stat sb;
    if (::stat(copy.c_str(), &sb) != 0) return f;

    if (pad.reverse) {
        SampleDesc d;
        if (!source.describe(f.path.c_str(), d) || d.frames == 0) return f;
        const uint32_t n = d.frames;
        const uint32_t e = (f.end && f.end < n) ? f.end : n;
        const uint32_t b = f.start < e ? f.start : e;
        f.start = n - e;
        f.end   = b ? n - b : 0;
    }
    f.path = copy;
    f.pitchInFile = true;
    return f;
}

} // namespace

uint16_t build_pad(PcSampleSource& source, SampleStore& store, const char* kitPath, const TakePad& tp,
                   const TakeLayer* layers, uint8_t p, uint32_t headBytes, KitPad& kp) {
    kp = KitPad{};
    kp.mode  = static_cast<PadMode>(tp.mode <= static_cast<uint8_t>(PadMode::Loop) ? tp.mode : 0);
    kp.choke = static_cast<uint8_t>(tp.choke & 0x07);
    kp.gain  = SamplerKit::volumeToGain(tp.volume);
    kp.pan   = SamplerKit::panToBalance(tp.pan + kPanCentre);
    kp.semitones = tp.pitch;
    kp.roundRobin = tp.cycle;

    uint16_t loaded = 0;
    for (uint8_t i = 0; i < tp.layerCount && i < KitPad::kMaxLayers; ++i) {
        const LayerFile lf = layer_file(source, kitPath, tp, layers[tp.firstLayer + i]);
        SampleDesc d;
        if (!source.describe(lf.path.c_str(), d)) {
            std::printf("[sampler] pad %u layer %u: cannot use '%s'\n", (unsigned)p, (unsigned)i,
                        lf.path.c_str());
            continue;
        }
        const uint32_t frameBytes = d.channels * sizeof(int16_t);
        const uint16_t slot = store.load(lf.path.c_str(), headBytes / frameBytes, lf.start, lf.end);
        if (slot == SampleStore::kNoSlot) {
            std::printf("[sampler] pad %u layer %u: no slot for '%s'\n", (unsigned)p, (unsigned)i,
                        lf.path.c_str());
            continue;
        }
        kp.layers[kp.layerCount].slot  = slot;
        kp.layers[kp.layerCount].start = lf.start;
        kp.layers[kp.layerCount].end   = lf.end;
        ++kp.layerCount;
        ++loaded;
        if (lf.pitchInFile) kp.semitones = 0;
    }
    return loaded;
}

/* Head size for a kit, from every sample's length: what HeadPlan makes of
 * kHeadBytes a sample within kHeadsBudget. */
uint32_t plan_heads(PcSampleSource& source, const char* kitPath, const TakePad* pads, const TakeLayer* layers) {
    HeadPlan plan;
    plan.headBytes   = kHeadBytes;
    plan.budgetBytes = kHeadsBudget;
    uint32_t sizes[SampleStore::kMaxSlots];
    uint16_t n = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads && n < SampleStore::kMaxSlots; ++p) {
        const TakePad& tp = pads[p];
        for (uint8_t i = 0; i < tp.layerCount && n < SampleStore::kMaxSlots; ++i) {
            const LayerFile lf = layer_file(source, kitPath, tp, layers[tp.firstLayer + i]);
            SampleDesc d;
            if (!source.describe(lf.path.c_str(), d)) continue;
            const uint32_t from = SampleStore::trimStart(lf.start, lf.end, d.frames);
            sizes[n++] = (d.frames - from) * d.channels * sizeof(int16_t);
        }
    }
    return plan.resolve(sizes, n);
}

} // namespace kit_load
} // namespace crosspad_pc
