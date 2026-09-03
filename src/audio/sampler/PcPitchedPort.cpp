// SPDX-License-Identifier: MIT

#include "PcPitchedPort.hpp"

#include "pc_stubs/pc_platform.h"

#include <crosspad/dsp/FftFactory.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/instrument/PitchedKitLoader.hpp>
#include <crosspad/instrument/SampleBank.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>

namespace crosspad_pc {
namespace {

crosspad::PitchedInstrument s_instrument;
crosspad::SampleBank*       s_bank = nullptr;
uint32_t                    s_engineRate = 44100;

// Desktop memory is plain malloc — there is no arena to hand back to another
// engine here, so unlike the board the sampler and the pitched bank simply
// coexist in the process heap.
void* bankAlloc(size_t bytes) { return std::malloc(bytes); }
void  bankFree(void* p) { std::free(p); }

// The FFT tables. PortableFft is what the factory settles on off the board, and
// it asks for nothing special; the alignment argument is only meaningful to
// esp-dsp's assembly kernels.
void* fftAlloc(size_t bytes, size_t /*align*/) { return std::malloc(bytes); }
void  fftFree(void* p) { std::free(p); }

void pitchedLog(const char* line) {
    std::printf("[pitched] %s\n", line ? line : "");
}

/// kit.json paths are device-logical ("/crosspad/kits/X/…"); stdio here needs
/// the virtual card's real location, the same mapping the sampler's file
/// helpers use.
std::string resolvePath(const std::string& kitPath, const std::string& sample) {
    const std::string logical = crosspad::resolveKitSamplePath(kitPath, sample);
    if (logical.empty()) return logical;
    return pc_platform_resolve_sdcard_path(logical);
}

/// The render gate spins on the audio thread's own progress, so a sleep is the
/// right wait: one 128-frame block is under 3 ms at 44.1 kHz.
void gateYield() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

constexpr uint32_t kGateSpins = 50;

crosspad::IFft& fft() {
    crosspad::FftMemory mem;
    mem.alloc = fftAlloc;
    mem.free  = fftFree;
    mem.log   = pitchedLog;
    return crosspad::createFft(mem);
}

/// Publish @p bank and retire whatever it replaces. `nullptr` unloads.
void install(crosspad::SampleBank* bank, const crosspad::KitInfo* kit) {
    // Default-constructed already carries InstrumentParams' own maxVoices
    // default, so a kit that leaves it unset (0) keeps that instead of
    // repeating the number here.
    crosspad::InstrumentParams params;
    if (kit) {
        if (kit->instrument.maxVoices) params.maxVoices = kit->instrument.maxVoices;
        params.attackMs  = static_cast<float>(kit->instrument.attackMs);
        params.releaseMs = static_cast<float>(kit->instrument.releaseMs);
    }

    bool safeToFree = false;
    crosspad::SampleBank* old =
        s_instrument.installBank(bank, params, kit, kGateSpins, gateYield, &safeToFree);
    s_bank = bank;
    if (bank) s_instrument.resetStats();

    if (!old) return;
    if (safeToFree) {
        delete old;
    } else {
        // The audio thread never acknowledged the gate, so it may still be
        // reading the old PCM. A leak beats a use-after-free under a live
        // render — the same trade the board makes.
        std::printf("[pitched] render gate timed out — leaking the previous bank (%u kB)\n",
                    static_cast<unsigned>(old->bytes() / 1024));
    }
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────

crosspad::IAudioNode* pitched_port_init(uint32_t engineRate) {
    if (engineRate) s_engineRate = engineRate;
    crosspad::getPlatformServices().setPitchedInstrument(&s_instrument);
    return &s_instrument;
}

crosspad::PitchedInstrument& pitched_port_instrument() { return s_instrument; }

bool pitched_port_load(const crosspad::KitInfo& kit) {
    crosspad::PitchedLoadEnv env;
    env.alloc       = bankAlloc;
    env.free        = bankFree;
    env.engineRate  = s_engineRate;
    env.resolvePath = resolvePath;
    env.log         = pitchedLog;
    env.maxBankBytes = 0;   // desktop heap; the board's arena budget has no analogue

    crosspad::SampleBank* bank = nullptr;
    char err[128] = {0};
    if (!crosspad::pitchedBuildBank(kit, env, fft(), bank, err, sizeof(err))) {
        std::printf("[pitched] kit '%s' failed to load: %s\n", kit.name.c_str(), err);
        // Nothing was swapped in, so the previous kit's bank is still installed
        // — and the caller has already moved on to this kit's pad_notes. A
        // pitched kit that failed to load has to be silent.
        pitched_port_unload();
        return false;
    }

    install(bank, &kit);
    // Same fallback install() applies: an unset kit value reports the
    // instrument's own default rather than a misleading 0.
    const uint8_t effectiveMaxVoices =
        kit.instrument.maxVoices ? kit.instrument.maxVoices : crosspad::InstrumentParams{}.maxVoices;
    std::printf("[pitched] kit '%s': %u zones, %u kB, maxVoices %u\n",
                kit.name.c_str(), static_cast<unsigned>(bank->zoneCount()),
                static_cast<unsigned>(bank->bytes() / 1024),
                static_cast<unsigned>(effectiveMaxVoices));
    return true;
}

void pitched_port_unload() {
    if (!s_bank) return;
    install(nullptr, nullptr);
    std::printf("[pitched] unloaded\n");
}

bool pitched_port_active() { return s_bank != nullptr; }

} // namespace crosspad_pc
