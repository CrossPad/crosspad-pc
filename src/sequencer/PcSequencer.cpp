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
#include "pc_stubs/pc_platform.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string>

namespace {

crosspad::MetronomeNode s_metronome;
crosspad::PatternSequencer s_sequencer;
crosspad::SequencerController s_controller;

constexpr const char* kScenesFile = "/crosspad/sequences.json";
/// While a pattern or a count-in runs the poll is the jitter a fired hit
/// inherits; idle it only decides how soon a transport start is noticed.
constexpr uint32_t kPollLiveMs = 2;
constexpr uint32_t kPollIdleMs = 20;
constexpr uint32_t kStoreSweepMs = 500;
constexpr uint32_t kTaskStack = 4096;
/// The poll fires pads, which post on the event bus: a FreeRTOS task, above
/// the LVGL task (1) so a busy screen cannot drag the beat. The store writes
/// files and waits its turn with the UI.
constexpr UBaseType_t kPollPriority = 2;
constexpr UBaseType_t kStorePriority = 1;

void poll_task(void*)
{
    for (;;) {
        s_sequencer.poll();
        const auto st = s_sequencer.state();
        const bool live = st == crosspad::SequencerState::Running ||
                          st == crosspad::SequencerState::PreBeat;
        vTaskDelay(pdMS_TO_TICKS(live ? kPollLiveMs : kPollIdleMs));
    }
}

/// Read the scenes once a card is there, then write them back whenever they
/// change -- never mid-take, and never over a file that did not read.
void store_task(void*)
{
    std::string file;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kStoreSweepMs));
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
    xTaskCreate(poll_task, "seq_poll", kTaskStack, nullptr, kPollPriority, nullptr);
    xTaskCreate(store_task, "seq_store", kTaskStack, nullptr, kStorePriority, nullptr);
}

} // namespace crosspad_pc
