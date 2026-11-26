// SPDX-License-Identifier: GPL-3.0-or-later
//
// The effects bus in the simulator, the board's FxBusPort.cpp without its
// input pairs: the sources are mixer channels (the mics = IN1, Line 1/2 =
// IN2, USB = the host's playback, silent in a simulator), the returns are
// Out 1/2, Out 3/4 and the USB rec bus. Attached, the sources' own routes
// are put aside so the processor is their only way out, and put back on
// detach. Every call comes from the GUI thread; the render reads only the
// mixer's atomics.

#include "PcFxBus.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/IFxBus.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include <atomic>
#include <cstdio>

namespace crosspad_pc {
namespace {

using crosspad::FxOutput;
using crosspad::FxSource;
using crosspad::IAudioNode;

constexpr int kSources = (int)FxSource::Count;
constexpr int kOutputs = (int)FxOutput::Count;

AudioMixerEngine* s_mixer = nullptr;
uint8_t s_ch[kSources] = {kMixerInvalidChannelId, kMixerInvalidChannelId, kMixerInvalidChannelId,
                          kMixerInvalidChannelId};
uint8_t s_out[kOutputs] = {0, 1, 0};

class PcFxBus final : public crosspad::IFxBus {
public:
    bool hasSource(FxSource s) const override { return idx(s) >= 0 && s_ch[idx(s)] != kMixerInvalidChannelId; }

    /* Both inputs are always heard; nothing plays into a simulator over USB. */
    bool sourceAvailable(FxSource s) const override { return s != FxSource::Usb && hasSource(s); }

    void setSend(FxSource s, float level) override
    {
        const int i = idx(s);
        if (i < 0) return;
        send_[i].store(level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level));
        if (attached_) apply();
    }

    float send(FxSource s) const override { return idx(s) >= 0 ? send_[idx(s)].load() : 0.0f; }

    void setPreFader(bool pre) override
    {
        pre_ = pre;
        if (attached_) s_mixer->setFxPreFader(pre);
    }

    bool hasOutput(FxOutput o) const override { return (int)o < kOutputs && s_out[(int)o] < s_mixer->numOutputs(); }

    void setReturn(FxOutput o, bool on) override
    {
        if ((int)o >= kOutputs) return;
        ret_[(int)o] = on;
        if (attached_) s_mixer->setFxReturn(s_out[(int)o], on ? 1.0f : 0.0f);
    }

    bool returnOn(FxOutput o) const override { return (int)o < kOutputs && ret_[(int)o]; }

    /* The processor and its sends go in before the sources' own routes come
     * out, and the routes come back before it goes: setFxProcessor() prepares
     * the processor first, and with the routes already off that is a gap. */
    void attach(IAudioNode* processor) override
    {
        if (!s_mixer || !processor) return;
        s_mixer->setFxProcessor(processor);
        const bool first = !attached_;
        attached_ = true;
        apply();
        if (first) {
            for (int i = 0; i < kSources; ++i) {
                if (s_ch[i] == kMixerInvalidChannelId) continue;
                for (uint8_t o = 0; o < kMixerMaxOutputs; ++o) {
                    saved_[i][o] = s_mixer->isRouteEnabled(s_ch[i], o);
                    s_mixer->setRouteEnabled(s_ch[i], o, false);
                }
            }
        }
        std::printf("[FxBus] processor %s attached\n", processor->name());
    }

    void detach() override
    {
        if (!s_mixer || !attached_) return;
        for (int i = 0; i < kSources; ++i) {
            if (s_ch[i] == kMixerInvalidChannelId) continue;
            for (uint8_t o = 0; o < kMixerMaxOutputs; ++o) s_mixer->setRouteEnabled(s_ch[i], o, saved_[i][o]);
        }
        s_mixer->setFxProcessor(nullptr);
        for (int i = 0; i < kSources; ++i) {
            if (s_ch[i] != kMixerInvalidChannelId) s_mixer->setFxSend(s_ch[i], 0.0f);
        }
        for (int o = 0; o < kOutputs; ++o) s_mixer->setFxReturn(s_out[o], 0.0f);
        attached_ = false;
        std::printf("[FxBus] processor detached, routes back\n");
    }

    void level(float& left, float& right) const override
    {
        if (s_mixer) s_mixer->getFxLevel(left, right);
        else left = right = 0.0f;
    }

private:
    static int idx(FxSource s) { return (int)s < kSources ? (int)s : -1; }

    void apply()
    {
        for (int i = 0; i < kSources; ++i) {
            if (s_ch[i] != kMixerInvalidChannelId) s_mixer->setFxSend(s_ch[i], send_[i].load());
        }
        for (int o = 0; o < kOutputs; ++o) s_mixer->setFxReturn(s_out[o], ret_[o] ? 1.0f : 0.0f);
        s_mixer->setFxPreFader(pre_);
    }

    std::atomic<float> send_[kSources] = {};
    bool ret_[kOutputs] = {true, false, false};
    bool pre_ = false;
    bool attached_ = false;
    bool saved_[kSources][kMixerMaxOutputs] = {};
};

PcFxBus s_bus;

} // namespace

void fx_bus_init(AudioMixerEngine& mixer, uint8_t chMic, uint8_t chLine12, uint8_t chUsb, uint8_t captureBus)
{
    s_mixer = &mixer;
    s_ch[(int)FxSource::Mic]    = chMic;
    s_ch[(int)FxSource::Line12] = chLine12;
    s_ch[(int)FxSource::Usb]    = chUsb;
    s_out[(int)FxOutput::Usb]   = captureBus;
    crosspad::getPlatformServices().fxBus = &s_bus;
}

} // namespace crosspad_pc
