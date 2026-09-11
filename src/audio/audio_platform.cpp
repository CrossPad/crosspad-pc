// SPDX-License-Identifier: MIT
#include "audio_platform.hpp"

#ifdef USE_VIRTUAL_AUDIO
#include "virtual/IVirtualSinkManager.hpp"   // enumeratePulseSinks (portable stub off-Linux)
#endif
#ifdef USE_PIPEWIRE
#include "pipewire/PwSinkEnumerator.hpp"     // pwEnumerateSinks / pwEnumerateSources
#endif
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
#include "virtual/PulseMonitorCapture.hpp"   // libpulse monitor capture (Linux)
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

/* ── Virtual-sink monitor capture ─────────────────────────────────────── */
#if defined(USE_VIRTUAL_AUDIO) && defined(__linux__)
// Two dedicated libpulse-simple capturers for the virtual sink monitors —
// decoupled from RtAudio's PULSE quirks (it aggregates sinks-per-card and
// hides per-sink sources). When active these register with PlatformServices
// instead of PcAudioInput.
static PulseMonitorCapture s_cap[2];

crosspad::IAudioInput* startVirtualCapture(int slot, const std::string& captureName,
                                           uint32_t sampleRate) {
    if (slot < 0 || slot > 1) return nullptr;
    if (s_cap[slot].start(captureName, sampleRate)) return &s_cap[slot];
    return nullptr;
}
void stopVirtualCapture(int slot) {
    if (slot < 0 || slot > 1) return;
    s_cap[slot].stop();
}
crosspad::IAudioInput* virtualCaptureInput(int slot) {
    if (slot < 0 || slot > 1) return nullptr;
    return s_cap[slot].isOpen() ? &s_cap[slot] : nullptr;
}
bool virtualCaptureOpen(int slot) {
    return slot >= 0 && slot <= 1 && s_cap[slot].isOpen();
}
void detachVirtualCaptureForShutdown() {
    s_cap[0].detachForShutdown();
    s_cap[1].detachForShutdown();
}
#else
crosspad::IAudioInput* startVirtualCapture(int, const std::string&, uint32_t) { return nullptr; }
void stopVirtualCapture(int) {}
crosspad::IAudioInput* virtualCaptureInput(int) { return nullptr; }
bool virtualCaptureOpen(int) { return false; }
void detachVirtualCaptureForShutdown() {}
#endif

} // namespace audio_platform
} // namespace crosspad_pc
