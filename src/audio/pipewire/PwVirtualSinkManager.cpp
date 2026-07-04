// SPDX-License-Identifier: MIT
#ifdef __linux__
#include "PwVirtualSinkManager.hpp"
#include "PwContext.hpp"

#include <cstdio>

namespace crosspad_pc {

namespace {
const char* kDescriptions[PwVirtualSinkManager::kMaxSinks] = {
    "CrossPad IN#1", "CrossPad IN#2",
};
} // namespace

bool PwVirtualSinkManager::setup(uint32_t sinkCount)
{
    if (!PwContext::instance().init()) return false;

    if (sinkCount > kMaxSinks) sinkCount = kMaxSinks;
    activeCount_ = sinkCount;

    uint32_t started = 0;
    for (uint32_t i = 0; i < sinkCount; ++i) {
        std::string nodeName = "crosspad_vin" + std::to_string(i + 1);
        if (caps_[i].start(nodeName.c_str(), kDescriptions[i], 48000))
            ++started;
        else
            printf("[PwVirtualSinkManager] failed to start '%s'\n", nodeName.c_str());
    }
    return started >= 1;
}

void PwVirtualSinkManager::teardown()
{
    for (auto& cap : caps_) cap.stop();
    activeCount_ = 0;
}

std::vector<IVirtualSinkManager::VirtualSink> PwVirtualSinkManager::list() const
{
    std::vector<VirtualSink> out;
    for (uint32_t i = 0; i < activeCount_ && i < kMaxSinks; ++i) {
        if (!caps_[i].isOpen()) continue;
        VirtualSink sink;
        sink.displayName       = kDescriptions[i];
        sink.captureDeviceName = "crosspad_vin" + std::to_string(i + 1) + ".monitor";
        sink.channelCount      = 2;
        out.push_back(std::move(sink));
    }
    return out;
}

crosspad::IAudioInput* PwVirtualSinkManager::input(uint32_t index)
{
    if (index >= kMaxSinks) return nullptr;
    return caps_[index].isOpen() ? static_cast<crosspad::IAudioInput*>(&caps_[index]) : nullptr;
}

bool PwVirtualSinkManager::isAvailable() const
{
    return PwContext::instance().init();
}

std::string PwVirtualSinkManager::errorHint() const
{
    return "PipeWire daemon not reachable";
}

} // namespace crosspad_pc

#endif // __linux__
