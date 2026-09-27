// SPDX-License-Identifier: MIT

#include "PcSampleNode.hpp"
#include "SampleStreamEngine.hpp"

namespace crosspad_pc {

void PcSampleNode::onPrepare(uint32_t maxFrames, uint32_t sampleRate) {
    getSampleStreamEngine().prepare(maxFrames, sampleRate);
}

void PcSampleNode::mix(float* out, uint32_t frames) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    getSampleStreamEngine().renderAdd(out, frames);
}

} // namespace crosspad_pc
