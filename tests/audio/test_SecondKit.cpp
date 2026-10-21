// SPDX-License-Identifier: GPL-3.0-or-later
//
// The simulator's second kit (core ISecondKit, PcSecondKit): a song's second
// kit track plays a kit of its own in an engine of its own. A generated kit
// goes through the kit manager's readKitCopy into the engine, a note goes in
// through the interface, and what comes out of the "Kit 2" node's mix() is
// measured. The release tests run the render on a thread of its own, as the
// audio callback does, while kits are held and let go underneath it.

#include <catch2/catch_test_macros.hpp>

#include "wav_io.hpp"
#include "audio/sampler/PcSecondKit.hpp"
#include "crosspad/dsp/DspMath.hpp"

#include <crosspad/audio/IAudioNode.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/sampler/ISecondKit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace crosspad_test;
using State = crosspad::ISecondKit::State;

namespace {

constexpr uint32_t kRate  = 44100;
constexpr uint32_t kBlock = 256;
constexpr double   kAmplitude = 0.5 * 32767.0;
constexpr double   kToneSeconds = 0.5;
constexpr auto     kSettleBound = std::chrono::seconds(10);
constexpr int      kSwapRounds = 12;

/// Kits by path: what readKitCopy hands back, as the card's reader would.
class FakeKitManager : public crosspad::IKitManager {
public:
    std::map<std::string, crosspad::KitInfo> kits;

    int getKitCount() const override { return static_cast<int>(kits.size()); }
    crosspad::KitInfo* getKit(int) override { return nullptr; }
    crosspad::KitInfo* getCurrentKit() override { return nullptr; }
    int getCurrentKitId() const override { return -1; }
    void setCurrentKitId(int) override {}
    bool isLoading() const override { return false; }
    std::vector<crosspad::KitInfo> listKits(const std::string&) override { return {}; }
    crosspad::KitLoadResult loadKit(crosspad::KitInfo&) override { return {}; }
    bool saveKit(const crosspad::KitInfo&) override { return false; }
    bool readKitCopy(const std::string& path, crosspad::KitInfo& out) override {
        const auto it = kits.find(path);
        if (it == kits.end()) return false;
        out = it->second;
        return true;
    }
};

void writeTone(const std::filesystem::path& path, double hz) {
    WavFile w;
    w.sampleRate = kRate;
    w.channels = 1;
    w.samples.resize(static_cast<size_t>(kRate * kToneSeconds));
    for (size_t i = 0; i < w.samples.size(); ++i) {
        w.samples[i] = static_cast<int16_t>(kAmplitude * std::sin(2.0 * crosspad::kPi * hz * i / kRate));
    }
    REQUIRE(wavWrite(path.string(), w));
}

/// Two kits on disk, pad 0 of each a tone, and the manager that reads them.
struct Fixture {
    std::filesystem::path dir;
    FakeKitManager        mgr;
    std::string           kitA, kitB;
    crosspad::IKitManager* previous = nullptr;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("crosspad_kit2_" + std::to_string(
                   static_cast<unsigned long long>(
                       std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(dir);
        kitA = make("a", 220.0);
        kitB = make("b", 880.0);
        auto& svc = crosspad::getPlatformServices();
        previous = svc.kitManager;
        svc.kitManager = &mgr;
    }
    ~Fixture() {
        crosspad::getPlatformServices().kitManager = previous;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string make(const std::string& name, double hz) {
        const auto wav = dir / (name + ".wav");
        writeTone(wav, hz);
        crosspad::KitInfo k;
        k.path = (dir / (name + "_kit.json")).string();
        k.name = "KIT " + name;
        k.parsed = true;
        // Absolute: the kit's own layout is not what is under test.
        k.pads[0].AddSample(wav.string());
        mgr.kits[k.path] = k;
        return k.path;
    }
};

bool settle(crosspad::ISecondKit& kit) {
    const auto until = std::chrono::steady_clock::now() + kSettleBound;
    while (kit.state() == State::Loading) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// Loudest sample of @p blocks blocks rendered from the node.
float renderPeak(crosspad::IAudioNode& node, int blocks) {
    std::vector<float> buf(kBlock * 2);
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        node.mix(buf.data(), kBlock);
        for (float v : buf) peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

crosspad::IAudioNode& preparedNode() {
    crosspad::IAudioNode& node = *crosspad_pc::second_kit_node();
    node.onPrepare(kBlock, kRate);
    return node;
}

} // namespace

TEST_CASE("second kit: it holds a kit, and a note sounds on its own node", "[sampler][kit2]") {
    Fixture fx;
    crosspad::ISecondKit& kit = *crosspad_pc::second_kit();
    crosspad::IAudioNode& node = preparedNode();

    kit.hold(fx.kitA);
    REQUIRE(settle(kit));
    CHECK(kit.state() == State::Ready);
    CHECK(kit.kit() == fx.kitA);

    CHECK(renderPeak(node, 4) == 0.0f);   // held, not played
    float l = 1.0f, r = 1.0f;
    kit.takeLevel(l, r);
    CHECK(l == 0.0f);
    CHECK(r == 0.0f);

    kit.noteOn(0, 127);
    CHECK(renderPeak(node, 8) > 0.05f);
    kit.takeLevel(l, r);
    CHECK(l > 0.05f);
    CHECK(r > 0.05f);
    kit.takeLevel(l, r);                  // read and cleared
    CHECK(l == 0.0f);

    kit.hold("");
    REQUIRE(settle(kit));
}

TEST_CASE("second kit: asking for what it holds does nothing, another kit replaces it", "[sampler][kit2]") {
    Fixture fx;
    crosspad::ISecondKit& kit = *crosspad_pc::second_kit();
    crosspad::IAudioNode& node = preparedNode();

    kit.hold(fx.kitA);
    kit.hold(fx.kitA);
    REQUIRE(settle(kit));
    REQUIRE(kit.state() == State::Ready);

    kit.hold(fx.kitB);
    REQUIRE(settle(kit));
    CHECK(kit.state() == State::Ready);
    CHECK(kit.kit() == fx.kitB);
    kit.noteOn(0, 127);
    CHECK(renderPeak(node, 8) > 0.05f);

    kit.hold("");
    REQUIRE(settle(kit));
}

TEST_CASE("second kit: an empty ask gives the kit back and the node is silent", "[sampler][kit2]") {
    Fixture fx;
    crosspad::ISecondKit& kit = *crosspad_pc::second_kit();
    crosspad::IAudioNode& node = preparedNode();

    kit.hold(fx.kitA);
    REQUIRE(settle(kit));
    kit.noteOn(0, 127);
    REQUIRE(renderPeak(node, 4) > 0.05f);

    kit.hold("");
    REQUIRE(settle(kit));
    CHECK(kit.state() == State::Idle);
    CHECK(kit.kit().empty());
    kit.noteOn(0, 127);                   // no engine to receive it
    CHECK(renderPeak(node, 8) == 0.0f);
}

TEST_CASE("second kit: a kit that cannot be read is Failed and is not asked again", "[sampler][kit2]") {
    Fixture fx;
    crosspad::ISecondKit& kit = *crosspad_pc::second_kit();
    crosspad::IAudioNode& node = preparedNode();

    kit.hold("/nowhere/kit.json");
    REQUIRE(settle(kit));
    CHECK(kit.state() == State::Failed);
    CHECK(renderPeak(node, 4) == 0.0f);

    kit.hold(fx.kitA);                    // a good kit afterwards loads
    REQUIRE(settle(kit));
    CHECK(kit.state() == State::Ready);

    kit.hold("");
    REQUIRE(settle(kit));
}

TEST_CASE("second kit: held and released while the render runs on another thread", "[sampler][kit2]") {
    Fixture fx;
    crosspad::ISecondKit& kit = *crosspad_pc::second_kit();
    crosspad::IAudioNode& node = preparedNode();

    std::atomic<bool> stop{false};
    std::atomic<bool> finite{true};
    std::atomic<uint64_t> blocks{0};
    // The audio callback: renders back to back, as RtAudio's does.
    std::thread render([&] {
        std::vector<float> buf(kBlock * 2);
        while (!stop.load(std::memory_order_acquire)) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            node.mix(buf.data(), kBlock);
            for (float v : buf) {
                if (!std::isfinite(v) || std::fabs(v) > 8.0f) finite.store(false);
            }
            blocks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    // The sequencer's sink: notes from a thread of its own.
    std::thread player([&] {
        while (!stop.load(std::memory_order_acquire)) {
            kit.noteOn(0, 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            kit.noteOff(0);
        }
    });

    bool settled = true;
    for (int i = 0; i < kSwapRounds && settled; ++i) {
        kit.hold(i % 2 ? fx.kitB : fx.kitA);
        settled = settle(kit);
        if (!settled) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        kit.hold("");
        settled = settle(kit);
    }
    stop.store(true, std::memory_order_release);
    render.join();
    player.join();

    REQUIRE(settled);
    CHECK(finite.load());
    CHECK(blocks.load() > 0);
    CHECK(kit.state() == State::Idle);

    // And it still works after all of that.
    kit.hold(fx.kitA);
    REQUIRE(settle(kit));
    kit.noteOn(0, 127);
    CHECK(renderPeak(node, 8) > 0.05f);
    kit.hold("");
    REQUIRE(settle(kit));
}
