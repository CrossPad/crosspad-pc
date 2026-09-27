// The sequencer on PC, wired the way the board wires it (CrosspadPlatform.cpp
// in platform-idf): the metronome is a mixer channel and the pattern's clock,
// the pattern plays through handlePadPress() as the sequencer, and the
// portable controller is what the apps see.

#include "PcSequencer.hpp"

#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/sequencer/SequenceStore.hpp"
#include "crosspad/sequencer/SequencerController.hpp"

#include <chrono>
#include <string>
#include <thread>

std::string pc_platform_resolve_sdcard_path(const std::string& virtualPath);
const std::string& pc_platform_get_sdcard_path();

namespace {

crosspad::MetronomeNode s_metronome;
crosspad::PatternSequencer s_sequencer;
crosspad::SequencerController s_controller;

constexpr const char* kScenesFile = "/crosspad/sequences.json";
/// While a pattern or a count-in runs the poll is the jitter a fired hit
/// inherits; idle it only decides how soon a transport start is noticed.
constexpr auto kPollLive = std::chrono::milliseconds(2);
constexpr auto kPollIdle = std::chrono::milliseconds(20);
constexpr auto kStoreSweep = std::chrono::milliseconds(500);

void poll_loop()
{
    for (;;) {
        s_sequencer.poll();
        const auto st = s_sequencer.state();
        const bool live = st == crosspad::SequencerState::Running ||
                          st == crosspad::SequencerState::PreBeat;
        std::this_thread::sleep_for(live ? kPollLive : kPollIdle);
    }
}

/// Read the scenes once a card is there, then write them back whenever they
/// change -- never mid-take, and never over a file that did not read.
void store_loop()
{
    std::string file;
    for (;;) {
        std::this_thread::sleep_for(kStoreSweep);
        if (pc_platform_get_sdcard_path().empty()) continue;
        if (file.empty()) {
            const std::string path = pc_platform_resolve_sdcard_path(kScenesFile);
            if (crosspad::SequenceStore::open(s_sequencer, path) == crosspad::SequenceLoad::Unreadable) {
                continue;
            }
            file = path;
            s_sequencer.clearDirty();
            continue;
        }
        if (!s_sequencer.dirty() || s_sequencer.isRecording()) continue;
        if (crosspad::SequenceStore::save(s_sequencer, file)) s_sequencer.clearDirty();
    }
}

} // namespace

crosspad::MetronomeNode& getMetronome() { return s_metronome; }
crosspad::PatternSequencer& getSequencer() { return s_sequencer; }

namespace crosspad_pc {

void sequencer_init(AudioMixerEngine& mixer, uint8_t audibleOutputs)
{
    /* A reference for playing, not part of a take: never the capture bus. */
    const auto ch = mixer.addChannel(&s_metronome, "Click");
    if (ch != kMixerInvalidChannelId) {
        for (uint8_t o = 0; o < audibleOutputs; ++o) mixer.setRouteEnabled(ch, o, true);
    }
    s_sequencer.attach(&s_metronome);
    s_sequencer.setSink([](void*, uint8_t pad, uint8_t vel, bool on) {
        auto& pads = crosspad::getPadManager();
        if (on) pads.handlePadPress(pad, vel, crosspad::EventSource::SequencerCallback);
        else    pads.handlePadRelease(pad, crosspad::EventSource::SequencerCallback);
    }, nullptr);
    /* A played pad is recorded; the pattern's own playback is not, or every
     * loop would re-record itself. The board stamps at each input; here the
     * pads have more ways in (mouse grid, keyboard, MIDI, remote), so the
     * pad manager's input tap stamps them all. */
    crosspad::getPadManager().setInputTap([](void*, uint8_t pad, uint8_t vel) {
        s_sequencer.noteIn(pad, vel);
    }, nullptr);
    s_controller.setSeqLength(crosspad::SequencerLimits::MAX_SEQ_LENGTH);
    s_controller.init(s_sequencer);
    s_sequencer.setController(&s_controller);
    crosspad::getPlatformServices().sequencer = &s_controller;
    crosspad::getPlatformServices().sequencerEngine = &s_sequencer;
    /* The process ends with _Exit(); these run until then. */
    std::thread(poll_loop).detach();
    std::thread(store_loop).detach();
}

} // namespace crosspad_pc
