// SPDX-License-Identifier: MIT
#include "audio_platform.hpp"

#ifdef USE_VIRTUAL_AUDIO
#include "virtual/IVirtualSinkManager.hpp"   // enumeratePulseSinks (portable stub off-Linux)
#endif
#ifdef USE_PIPEWIRE
#include "pipewire/PwSinkEnumerator.hpp"     // pwEnumerateSinks / pwEnumerateSources
#endif

#include <sstream>

namespace crosspad_pc {
namespace audio_platform {

bool supportsPwTakeover() {
#ifdef USE_PIPEWIRE
    return true;
#else
    return false;
#endif
}

std::vector<std::string> prettyInputLabels(const std::vector<std::string>& rawNames) {
    std::vector<std::string> out;
    out.reserve(rawNames.size());
#ifdef USE_PIPEWIRE
    // Match each RtAudio name to a PipeWire source description by a shared long
    // token (same idiom the OUT list uses); fall back to the raw name.
    auto sources = pwEnumerateSources();
    auto pretty = [&](const std::string& raw) -> std::string {
        std::istringstream tok(raw);
        std::string w;
        while (tok >> w) {
            if (w.size() < 6) continue;
            for (auto& s : sources)
                if (s.description.find(w) != std::string::npos) return s.description;
        }
        return raw;
    };
    for (auto& r : rawNames) out.push_back(pretty(r));
#else
    out = rawNames;
#endif
    return out;
}

std::vector<SinkCandidate> outputSinkCandidates() {
    std::vector<SinkCandidate> out;
#ifdef USE_VIRTUAL_AUDIO
#ifdef USE_PIPEWIRE
    // Native PipeWire registry first — node.description is always present.
    for (auto& s : pwEnumerateSinks()) out.push_back({s.name, s.description});
#endif
    if (out.empty()) {
        // pactl fallback (portable stub returns {} off-Linux).
        for (auto& s : enumeratePulseSinks()) out.push_back({s.name, s.description});
    }
#endif
    return out;
}

} // namespace audio_platform
} // namespace crosspad_pc
