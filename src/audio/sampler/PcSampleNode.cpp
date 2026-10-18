// SPDX-License-Identifier: GPL-3.0-or-later

#include "PcSampleNode.hpp"
#include "SampleStreamEngine.hpp"

#include <algorithm>

namespace crosspad_pc {

void PcSampleNode::onPrepare(uint32_t maxFrames, uint32_t sampleRate) {
    getSampleStreamEngine().prepare(maxFrames, sampleRate);
    preparedMaxFrames_ = maxFrames;
    // Sized here, unconditionally, and never again: setScopeEnabled() runs on
    // the UI thread and mix() reads these same vectors on the audio thread
    // with no lock between them. A resize() toggled from the UI thread while
    // mix() was mid-block raced its reallocation against a concurrent read —
    // that's the "the scope stopped working" bug, not a logic error in the
    // scope itself. Same reasoning as ScopeTap's own buffer.
    renderScratch_.resize(static_cast<size_t>(maxFrames) * 2);
    scopeMono_.resize(maxFrames);
    scopeTap_.setSampleRate(sampleRate);
}

void PcSampleNode::setScopeEnabled(bool enabled) {
    if (enabled && !scopeTap_.enabled()) {
        scopeTap_.clear();
        scopeTap_.setEnabled(true);
    } else if (!enabled) {
        scopeTap_.setEnabled(false);
    }
}

void PcSampleNode::mix(float* out, uint32_t frames) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (!scopeTap_.enabled()) {
        getSampleStreamEngine().renderAdd(out, frames);
        return;
    }
    if (scopeMono_.size() < frames || renderScratch_.size() < static_cast<size_t>(frames) * 2) {
        // onPrepare() normally guarantees this. Preserve audio if a host ever
        // violates its advertised maximum instead of truncating the block.
        getSampleStreamEngine().renderAdd(out, frames);
        return;
    }
    const uint32_t n = frames;

    std::fill_n(renderScratch_.data(), static_cast<size_t>(n) * 2, 0.0f);
    getSampleStreamEngine().renderAdd(renderScratch_.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
        const float l = renderScratch_[i * 2];
        const float r = renderScratch_[i * 2 + 1];
        out[i * 2]     += l;
        out[i * 2 + 1] += r;
        scopeMono_[i]   = (l + r) * 0.5f;
    }
    scopeTap_.write(scopeMono_.data(), n);
}

} // namespace crosspad_pc
