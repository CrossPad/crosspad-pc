// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PwVirtualSinkManager.hpp
 * @brief IVirtualSinkManager backed by native pw_stream Audio/Sink nodes
 *        (PwVirtualSinkCapture), one per slot. Preferred over the pactl
 *        null-sink path when a PipeWire daemon is reachable — see
 *        VirtualSinkFactory.cpp for the runtime fallback decision.
 */

#ifdef __linux__

#include "../virtual/IVirtualSinkManager.hpp"
#include "PwVirtualSinkCapture.hpp"

#include <array>

namespace crosspad_pc {

class PwVirtualSinkManager : public IVirtualSinkManager {
public:
    static constexpr uint32_t kMaxSinks = 2;

    bool setup(uint32_t sinkCount) override;
    void teardown() override;
    std::vector<VirtualSink> list() const override;
    crosspad::IAudioInput* input(uint32_t index) override;
    bool isAvailable() const override;
    std::string errorHint() const override;

private:
    std::array<PwVirtualSinkCapture, kMaxSinks> caps_;
    uint32_t activeCount_ = 0;
};

} // namespace crosspad_pc

#endif // __linux__
