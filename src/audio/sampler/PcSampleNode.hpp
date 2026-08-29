// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PcSampleNode.hpp
 * @brief IAudioNode generator wrapping SampleStreamEngine.
 *
 * The PC counterpart of MarcelSampleNode on the board, and deliberately
 * thinner than it: the engine renders float directly, so there is no planar
 * int16 staging and no mono→stereo mirroring here, and it stops a voice at the
 * end of its sample instead of parking on the last value, so the DC blocker
 * the board needs has nothing to remove.
 *
 * Register it the same way the other generators are registered — as a mixer
 * channel where AudioMixerEngine is present, otherwise as a node on the audio
 * module.
 */

#include <crosspad/audio/IAudioNode.hpp>

#include <atomic>
#include <cstdint>

namespace crosspad_pc {

class PcSampleNode : public crosspad::IAudioNode {
public:
    const char* name() const override { return "Sampler"; }
    bool isGenerator() const override { return true; }

    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }

    /// Sizes the engine's render scratch. Called by the host before the first
    /// mix() and on any restart-time change.
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override;

    void mix(float* out, uint32_t frames) override;

private:
    std::atomic<bool> enabled_{true};
};

} // namespace crosspad_pc
