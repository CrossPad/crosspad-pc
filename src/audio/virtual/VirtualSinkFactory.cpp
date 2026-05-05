/**
 * @file VirtualSinkFactory.cpp
 * @brief Platform-selecting factory for IVirtualSinkManager.
 *
 * Linux:   real implementation (pactl module-null-sink)
 * Win/Mac: no-op stub for Phase 1; real detection will be added per-OS later.
 */

#include "IVirtualSinkManager.hpp"

namespace crosspad_pc {

#ifdef __linux__
std::unique_ptr<IVirtualSinkManager> makeLinuxPipewireSinks();
#endif

namespace {

class NullSinkManager : public IVirtualSinkManager {
public:
    bool setup(uint32_t) override { return false; }
    void teardown() override {}
    std::vector<VirtualSink> list() const override { return {}; }
    bool isAvailable() const override { return false; }
    std::string errorHint() const override {
        return "Virtual audio sinks are not implemented for this platform yet.";
    }
};

} // namespace

std::unique_ptr<IVirtualSinkManager> makeVirtualSinkManager() {
#ifdef __linux__
    return makeLinuxPipewireSinks();
#else
    return std::make_unique<NullSinkManager>();
#endif
}

#ifndef __linux__
std::vector<PulseSinkInfo> enumeratePulseSinks(bool /*includeUnavailable*/) { return {}; }
bool movePulseOutputToSink(int /*slot*/, const std::string& /*name*/) { return false; }
#endif

} // namespace crosspad_pc
