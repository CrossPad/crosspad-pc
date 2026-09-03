// SPDX-License-Identifier: MIT
//
// The pitched sample engine, end to end on the host: a generated two-zone kit
// goes through core's loader into a PitchedInstrument, and what comes back out
// of mix() is measured in Hz.
//
// The zones are pure sines, so every number here is exact — a wrong zone
// choice or a wrong varispeed ratio shows up as a frequency error, not as
// taste. The 48 kHz zone additionally exercises the load-time rate conversion
// and the root detector, neither of which the engine itself can fake.

#include <catch2/catch_test_macros.hpp>

#include "wav_io.hpp"

#include <cmath>

#include <crosspad/dsp/FftFactory.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/instrument/PitchedKitLoader.hpp>
#include <crosspad/instrument/SampleBank.hpp>
#include <crosspad/kit/KitInfo.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace crosspad_test;

namespace {

constexpr uint32_t kEngineRate     = 44100;
constexpr uint32_t kBlock          = 256;
constexpr double   kToleranceRatio = 0.005;             // 0.5 %
constexpr double   kFixtureAmplitude = 0.5 * 32767.0;   // half-scale int16

void* hostAlloc(size_t bytes) { return std::malloc(bytes); }
void  hostFree(void* p) { std::free(p); }
void* hostFftAlloc(size_t bytes, size_t) { return std::malloc(bytes); }

/// The fixture names its zones by absolute path, so there is no mount prefix
/// and no kit-relative lookup to do — the platform hook is the identity here.
std::string testResolve(const std::string& /*kitPath*/, const std::string& sample) {
    return sample;
}

crosspad::IFft& hostFft() {
    crosspad::FftMemory mem;
    mem.alloc = hostFftAlloc;
    mem.free  = hostFree;
    return crosspad::createFft(mem);
}

/// One second of a mono 16-bit sine at @p hz, written as a WAV at @p rate.
void writeSine(const std::filesystem::path& path, double hz, uint32_t rate,
               double seconds = 1.0) {
    WavFile w;
    w.sampleRate = rate;
    w.channels   = 1;
    const uint32_t frames = static_cast<uint32_t>(rate * seconds);
    w.samples.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        w.samples[i] = static_cast<int16_t>(kFixtureAmplitude * std::sin(2.0 * M_PI * hz * t));
    }
    REQUIRE(wavWrite(path.string(), w));
}

/// Fundamental of a steady tone, by interpolated zero crossings.
///
/// The signal under test is a single sine of known amplitude, so counting
/// upward crossings beats an FFT bin here: no window, no interpolation of a
/// spectrum, and the precision grows with the length of the take rather than
/// with the transform size.
double dominantHz(const std::vector<float>& mono, uint32_t rate) {
    double firstCross = -1.0, lastCross = -1.0;
    int crossings = 0;
    for (size_t i = 1; i < mono.size(); ++i) {
        if (!(mono[i - 1] <= 0.0f && mono[i] > 0.0f)) continue;
        const double denom = static_cast<double>(mono[i]) - mono[i - 1];
        const double frac  = denom != 0.0 ? -static_cast<double>(mono[i - 1]) / denom : 0.0;
        const double pos   = static_cast<double>(i - 1) + frac;
        if (firstCross < 0.0) { firstCross = pos; }
        else { lastCross = pos; ++crossings; }
    }
    if (crossings < 2 || lastCross <= firstCross) return 0.0;
    return static_cast<double>(rate) * crossings / (lastCross - firstCross);
}

double noteHz(int midi) {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

/// Render @p seconds through mix() and return the left channel.
std::vector<float> render(crosspad::PitchedInstrument& inst, double seconds) {
    const uint32_t total = static_cast<uint32_t>(kEngineRate * seconds);
    std::vector<float> left;
    left.reserve(total);
    std::vector<float> block(kBlock * 2);
    for (uint32_t done = 0; done < total; done += kBlock) {
        std::fill(block.begin(), block.end(), 0.0f);
        inst.mix(block.data(), kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) left.push_back(block[2 * i]);
    }
    return left;
}

/// A two-zone pitched kit on disk plus the KitInfo that describes it.
struct PitchedFixture {
    std::filesystem::path dir;
    crosspad::KitInfo     kit;

    PitchedFixture() {
        dir = std::filesystem::temp_directory_path() /
              ("crosspad_pitched_" + std::to_string(
                   static_cast<unsigned long long>(
                       std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(dir);

        // A3 at the engine rate, root declared; A4 at 48 kHz, root detected.
        writeSine(dir / "z57.wav", 220.0, kEngineRate);
        writeSine(dir / "z69.wav", 440.0, 48000);

        kit.path   = (dir / "kit.json").string();
        kit.name   = "PITCHED TEST";
        kit.engine = crosspad::KitEngine::Pitched;

        crosspad::InstrumentZoneSpec z0;
        z0.sample = (dir / "z57.wav").string();
        z0.root   = 57.0f;
        crosspad::InstrumentZoneSpec z1;
        z1.sample = (dir / "z69.wav").string();
        z1.root   = -1.0f;   // "auto"
        kit.instrument.zones = {z0, z1};

        for (uint8_t p = 0; p < KIT_PADS; ++p) kit.instrument.padNotes[p] = 60 + p;
        kit.instrument.padNotesSet = true;
        kit.instrument.maxVoices   = 24;
        kit.instrument.attackMs    = 2;
        kit.instrument.releaseMs   = 100;

        for (uint8_t p = 0; p < KIT_PADS; ++p) {
            kit.pads[p].volume = 100;
            kit.pads[p].pan    = 0;
        }
    }

    ~PitchedFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    crosspad::PitchedLoadEnv env() const {
        crosspad::PitchedLoadEnv e;
        e.alloc       = hostAlloc;
        e.free        = hostFree;
        e.engineRate  = kEngineRate;
        e.resolvePath = testResolve;
        return e;
    }
};

} // namespace

TEST_CASE("Pitched kit loads both zones and detects the auto root", "[pitched]") {
    PitchedFixture fx;
    crosspad::SampleBank* bank = nullptr;
    char err[128] = {0};

    REQUIRE(crosspad::pitchedBuildBank(fx.kit, fx.env(), hostFft(), bank, err, sizeof(err)));
    REQUIRE(bank != nullptr);
    CHECK(bank->zoneCount() == 2);

    CHECK(bank->zone(0).rootMidi == 57.0f);
    // The 48 kHz zone was converted to 44.1 k at load; the root the detector
    // found has to survive that conversion.
    CHECK(std::fabs(bank->zone(1).rootMidi - 69.0f) < 0.05f);

    delete bank;
}

TEST_CASE("Every pad sounds its own note", "[pitched]") {
    PitchedFixture fx;
    crosspad::SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE(crosspad::pitchedBuildBank(fx.kit, fx.env(), hostFft(), bank, err, sizeof(err)));

    crosspad::PitchedInstrument inst;
    inst.onPrepare(kBlock, kEngineRate);

    crosspad::InstrumentParams params;
    params.maxVoices = fx.kit.instrument.maxVoices;
    params.attackMs  = static_cast<float>(fx.kit.instrument.attackMs);
    params.releaseMs = static_cast<float>(fx.kit.instrument.releaseMs);

    bool safeToFree = false;
    crosspad::SampleBank* old =
        inst.installBank(bank, params, &fx.kit, 10, nullptr, &safeToFree);
    CHECK(old == nullptr);

    for (uint8_t pad : {uint8_t(0), uint8_t(9), uint8_t(15)}) {
        const uint8_t note = fx.kit.instrument.noteForPad(pad);
        inst.noteOn(note, 100, pad);
        // Skip the attack ramp before measuring; 0.3 s of steady tone follows.
        render(inst, 0.02);
        const std::vector<float> pcm = render(inst, 0.3);
        inst.allNotesOff();
        render(inst, 0.05);

        const double measured = dominantHz(pcm, kEngineRate);
        const double expected = noteHz(note);
        INFO("pad " << int(pad) << " note " << int(note)
             << " expected " << expected << " Hz, measured " << measured << " Hz");
        CHECK(std::fabs(measured - expected) <= expected * kToleranceRatio);
    }

    inst.installBank(nullptr, params, nullptr, 10, nullptr, &safeToFree);
    delete bank;
}

TEST_CASE("A sixteen-note chord plays without stealing", "[pitched]") {
    PitchedFixture fx;
    crosspad::SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE(crosspad::pitchedBuildBank(fx.kit, fx.env(), hostFft(), bank, err, sizeof(err)));

    crosspad::PitchedInstrument inst;
    inst.onPrepare(kBlock, kEngineRate);

    crosspad::InstrumentParams params;
    params.maxVoices = fx.kit.instrument.maxVoices;   // 24 > 16, so nothing is stolen
    params.attackMs  = static_cast<float>(fx.kit.instrument.attackMs);
    params.releaseMs = static_cast<float>(fx.kit.instrument.releaseMs);

    bool safeToFree = false;
    inst.installBank(bank, params, &fx.kit, 10, nullptr, &safeToFree);

    for (uint8_t pad = 0; pad < 16; ++pad) {
        inst.noteOn(fx.kit.instrument.noteForPad(pad), 100, pad);
    }
    const std::vector<float> pcm = render(inst, 0.3);

    CHECK(inst.stealCount() == 0u);
    CHECK(inst.activeVoices() == 16);

    double peak = 0.0;
    for (float s : pcm) peak = std::max(peak, std::fabs(static_cast<double>(s)));
    CHECK(peak > 0.0);

    inst.allNotesOff();
    inst.installBank(nullptr, params, nullptr, 10, nullptr, &safeToFree);
    delete bank;
}
