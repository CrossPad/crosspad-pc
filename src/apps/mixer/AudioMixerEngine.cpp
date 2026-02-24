/**
 * @file AudioMixerEngine.cpp
 * @brief Real-time audio mixer thread implementation.
 */

#include "AudioMixerEngine.hpp"

#include "pc_stubs/pc_platform.h"
#include "audio/PcAudio.hpp"
#include "audio/PcAudioInput.hpp"
#include "synth/MlPianoSynth.hpp"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <vector>
#include <fstream>

AudioMixerEngine::~AudioMixerEngine()
{
    stop();
}

void AudioMixerEngine::start()
{
    if (running_.load()) return;

    running_.store(true);
    std::thread(&AudioMixerEngine::mixerThreadFunc, this).detach();
}

void AudioMixerEngine::stop()
{
    if (!running_.load()) return;
    running_.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// ── Route matrix ──

void AudioMixerEngine::setRouteEnabled(MixerInput in, MixerOutput out, bool enabled)
{
    routes_[(int)in][(int)out].enabled.store(enabled, std::memory_order_relaxed);
}

bool AudioMixerEngine::isRouteEnabled(MixerInput in, MixerOutput out) const
{
    return routes_[(int)in][(int)out].enabled.load(std::memory_order_relaxed);
}

void AudioMixerEngine::setRouteVolume(MixerInput in, MixerOutput out, float vol)
{
    routes_[(int)in][(int)out].volume.store(vol, std::memory_order_relaxed);
}

float AudioMixerEngine::getRouteVolume(MixerInput in, MixerOutput out) const
{
    return routes_[(int)in][(int)out].volume.load(std::memory_order_relaxed);
}

// ── Channel control ──

void AudioMixerEngine::setChannelVolume(MixerInput in, float vol)
{
    channels_[(int)in].volume.store(vol, std::memory_order_relaxed);
}

float AudioMixerEngine::getChannelVolume(MixerInput in) const
{
    return channels_[(int)in].volume.load(std::memory_order_relaxed);
}

void AudioMixerEngine::setChannelMute(MixerInput in, bool muted)
{
    channels_[(int)in].muted.store(muted, std::memory_order_relaxed);
}

bool AudioMixerEngine::isChannelMuted(MixerInput in) const
{
    return channels_[(int)in].muted.load(std::memory_order_relaxed);
}

void AudioMixerEngine::setChannelSolo(MixerInput in, bool soloed)
{
    channels_[(int)in].soloed.store(soloed, std::memory_order_relaxed);
}

bool AudioMixerEngine::isChannelSoloed(MixerInput in) const
{
    return channels_[(int)in].soloed.load(std::memory_order_relaxed);
}

// ── Output bus ──

void AudioMixerEngine::setOutputVolume(MixerOutput out, float vol)
{
    outputs_[(int)out].volume.store(vol, std::memory_order_relaxed);
}

float AudioMixerEngine::getOutputVolume(MixerOutput out) const
{
    return outputs_[(int)out].volume.load(std::memory_order_relaxed);
}

void AudioMixerEngine::setOutputMute(MixerOutput out, bool muted)
{
    outputs_[(int)out].muted.store(muted, std::memory_order_relaxed);
}

bool AudioMixerEngine::isOutputMuted(MixerOutput out) const
{
    return outputs_[(int)out].muted.load(std::memory_order_relaxed);
}

// ── Metering ──

void AudioMixerEngine::getChannelLevel(MixerInput in, int16_t& left, int16_t& right) const
{
    left  = channels_[(int)in].peakL.load(std::memory_order_relaxed);
    right = channels_[(int)in].peakR.load(std::memory_order_relaxed);
}

void AudioMixerEngine::getOutputLevel(MixerOutput out, int16_t& left, int16_t& right) const
{
    left  = outputs_[(int)out].peakL.load(std::memory_order_relaxed);
    right = outputs_[(int)out].peakR.load(std::memory_order_relaxed);
}

bool AudioMixerEngine::isAnySoloed() const
{
    for (int i = 0; i < MIXER_NUM_INPUTS; i++) {
        if (channels_[i].soloed.load(std::memory_order_relaxed))
            return true;
    }
    return false;
}

// ── Helper: compute peak from interleaved stereo buffer ──

static void computePeak(const int16_t* buf, uint32_t frames,
                        std::atomic<int16_t>& peakL, std::atomic<int16_t>& peakR)
{
    int16_t maxL = 0, maxR = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t absL = buf[i * 2]     < 0 ? (int16_t)-buf[i * 2]     : buf[i * 2];
        int16_t absR = buf[i * 2 + 1] < 0 ? (int16_t)-buf[i * 2 + 1] : buf[i * 2 + 1];
        if (absL > maxL) maxL = absL;
        if (absR > maxR) maxR = absR;
    }
    peakL.store(maxL, std::memory_order_relaxed);
    peakR.store(maxR, std::memory_order_relaxed);
}

// ── Mixer thread ──

void AudioMixerEngine::mixerThreadFunc()
{
    constexpr uint32_t CHUNK = MIXER_CHUNK_FRAMES;
    constexpr uint32_t STEREO_SAMPLES = CHUNK * 2;

    // Input buffers (interleaved int16 stereo)
    std::vector<int16_t> inBuf[MIXER_NUM_INPUTS];
    for (auto& b : inBuf) b.resize(STEREO_SAMPLES, 0);

    // Output accumulation buffers (int32 to avoid clipping during sum)
    std::vector<int32_t> outAccum[MIXER_NUM_OUTPUTS];
    for (auto& b : outAccum) b.resize(STEREO_SAMPLES, 0);

    // Final output buffer
    std::vector<int16_t> outBuf(STEREO_SAMPLES, 0);

    printf("[Mixer] Audio mixer thread started\n");
    fflush(stdout);

    // Determine chunk duration for pacing.
    // Default to 48000 Hz if no output is open yet.
    uint32_t sampleRate = 48000;
    auto* pOut = pc_platform_get_audio_output(0);
    if (pOut && pOut->isOpen() && pOut->getSampleRate() > 0)
        sampleRate = pOut->getSampleRate();
    const auto chunkDuration = std::chrono::microseconds(
        (uint64_t)CHUNK * 1000000 / sampleRate);

    while (running_.load()) {
        auto iterStart = std::chrono::steady_clock::now();

        // ── 1. Read inputs ──

        // IN1
        auto* audioIn1 = pc_platform_get_audio_input(0);
        if (audioIn1) {
            auto* pcIn = static_cast<PcAudioInput*>(audioIn1);
            uint32_t got = pcIn->read(inBuf[0].data(), CHUNK);
            if (got < CHUNK) {
                std::memset(inBuf[0].data() + got * 2, 0,
                            (CHUNK - got) * 2 * sizeof(int16_t));
            }
        } else {
            std::memset(inBuf[0].data(), 0, STEREO_SAMPLES * sizeof(int16_t));
        }

        // IN2
        auto* audioIn2 = pc_platform_get_audio_input(1);
        if (audioIn2) {
            auto* pcIn = static_cast<PcAudioInput*>(audioIn2);
            uint32_t got = pcIn->read(inBuf[1].data(), CHUNK);
            if (got < CHUNK) {
                std::memset(inBuf[1].data() + got * 2, 0,
                            (CHUNK - got) * 2 * sizeof(int16_t));
            }
        } else {
            std::memset(inBuf[1].data(), 0, STEREO_SAMPLES * sizeof(int16_t));
        }

        // SYNTH
        auto* synthEngine = pc_platform_get_synth_engine();
        if (synthEngine) {
            auto* synth = static_cast<MlPianoSynth*>(synthEngine);
            synth->process(inBuf[2].data(), CHUNK);
        } else {
            std::memset(inBuf[2].data(), 0, STEREO_SAMPLES * sizeof(int16_t));
        }

        // ── 2. Compute per-channel peaks ──
        for (int ch = 0; ch < MIXER_NUM_INPUTS; ch++) {
            computePeak(inBuf[ch].data(), CHUNK,
                        channels_[ch].peakL, channels_[ch].peakR);
        }

        // ── 3. Solo logic ──
        bool anySoloed = isAnySoloed();

        // ── 4. Clear output accumulators ──
        for (auto& b : outAccum) {
            std::memset(b.data(), 0, STEREO_SAMPLES * sizeof(int32_t));
        }

        // ── 5. Route and mix ──
        for (int ch = 0; ch < MIXER_NUM_INPUTS; ch++) {
            bool muted = channels_[ch].muted.load(std::memory_order_relaxed);
            bool solo  = channels_[ch].soloed.load(std::memory_order_relaxed);

            // Skip this channel if muted, or if solo mode is active and this isn't soloed
            if (muted) continue;
            if (anySoloed && !solo) continue;

            float chVol = channels_[ch].volume.load(std::memory_order_relaxed);

            for (int out = 0; out < MIXER_NUM_OUTPUTS; out++) {
                if (!routes_[ch][out].enabled.load(std::memory_order_relaxed))
                    continue;

                float routeVol = routes_[ch][out].volume.load(std::memory_order_relaxed);
                float gain = chVol * routeVol;

                // Fixed-point gain: gain * 256
                int32_t gainFP = static_cast<int32_t>(gain * 256.0f);

                for (uint32_t s = 0; s < STEREO_SAMPLES; s++) {
                    outAccum[out][s] += (static_cast<int32_t>(inBuf[ch][s]) * gainFP) >> 8;
                }
            }
        }

        // ── 6. Apply output volume, clamp, write ──
        for (int out = 0; out < MIXER_NUM_OUTPUTS; out++) {
            bool outMuted = outputs_[out].muted.load(std::memory_order_relaxed);
            float outVol  = outputs_[out].volume.load(std::memory_order_relaxed);
            int32_t outGainFP = static_cast<int32_t>(outVol * 256.0f);

            int16_t maxL = 0, maxR = 0;

            for (uint32_t i = 0; i < CHUNK; i++) {
                int32_t sL = outMuted ? 0 : (outAccum[out][i * 2]     * outGainFP) >> 8;
                int32_t sR = outMuted ? 0 : (outAccum[out][i * 2 + 1] * outGainFP) >> 8;

                // Clamp to int16 range
                if (sL > 32767)  sL = 32767;
                if (sL < -32768) sL = -32768;
                if (sR > 32767)  sR = 32767;
                if (sR < -32768) sR = -32768;

                outBuf[i * 2]     = static_cast<int16_t>(sL);
                outBuf[i * 2 + 1] = static_cast<int16_t>(sR);

                int16_t absL = sL < 0 ? (int16_t)-sL : (int16_t)sL;
                int16_t absR = sR < 0 ? (int16_t)-sR : (int16_t)sR;
                if (absL > maxL) maxL = absL;
                if (absR > maxR) maxR = absR;
            }

            outputs_[out].peakL.store(maxL, std::memory_order_relaxed);
            outputs_[out].peakR.store(maxR, std::memory_order_relaxed);

            // Write to audio output
            auto* pcOut = pc_platform_get_audio_output(out);
            if (pcOut && pcOut->isOpen()) {
                uint32_t offset = 0;
                uint32_t remaining = CHUNK;
                while (remaining > 0 && running_.load()) {
                    uint32_t written = pcOut->write(outBuf.data() + offset * 2, remaining);
                    offset += written;
                    remaining -= written;
                    if (remaining > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
        }

        // ── 7. Pace to real-time ──
        // Sleep for the remainder of the chunk period so we don't spin the CPU.
        auto elapsed = std::chrono::steady_clock::now() - iterStart;
        if (elapsed < chunkDuration) {
            std::this_thread::sleep_for(chunkDuration - elapsed);
        }
    }

    printf("[Mixer] Audio mixer thread exited\n");
}

// ── Defaults ──

void AudioMixerEngine::setDefaults()
{
    // Default: SYNTH -> OUT1 enabled at full volume (matches legacy behavior)
    for (int i = 0; i < MIXER_NUM_INPUTS; i++) {
        for (int o = 0; o < MIXER_NUM_OUTPUTS; o++) {
            routes_[i][o].enabled.store(false, std::memory_order_relaxed);
            routes_[i][o].volume.store(1.0f, std::memory_order_relaxed);
        }
        channels_[i].volume.store(1.0f, std::memory_order_relaxed);
        channels_[i].muted.store(false, std::memory_order_relaxed);
        channels_[i].soloed.store(false, std::memory_order_relaxed);
    }
    for (int o = 0; o < MIXER_NUM_OUTPUTS; o++) {
        outputs_[o].volume.store(1.0f, std::memory_order_relaxed);
        outputs_[o].muted.store(false, std::memory_order_relaxed);
    }

    routes_[(int)MixerInput::SYNTH][(int)MixerOutput::OUT1].enabled.store(true, std::memory_order_relaxed);
}

// ── State persistence ──

void AudioMixerEngine::saveState(const std::string& path) const
{
    JsonDocument doc;

    // Route matrix
    JsonArray routesArr = doc["routes"].to<JsonArray>();
    for (int i = 0; i < MIXER_NUM_INPUTS; i++) {
        for (int o = 0; o < MIXER_NUM_OUTPUTS; o++) {
            JsonObject r = routesArr.add<JsonObject>();
            r["in"] = i;
            r["out"] = o;
            r["enabled"] = routes_[i][o].enabled.load(std::memory_order_relaxed);
            r["volume"] = routes_[i][o].volume.load(std::memory_order_relaxed);
        }
    }

    // Channel state
    JsonArray chArr = doc["channels"].to<JsonArray>();
    for (int i = 0; i < MIXER_NUM_INPUTS; i++) {
        JsonObject ch = chArr.add<JsonObject>();
        ch["volume"] = channels_[i].volume.load(std::memory_order_relaxed);
        ch["muted"] = channels_[i].muted.load(std::memory_order_relaxed);
        ch["soloed"] = channels_[i].soloed.load(std::memory_order_relaxed);
    }

    // Output bus state
    JsonArray outArr = doc["outputs"].to<JsonArray>();
    for (int o = 0; o < MIXER_NUM_OUTPUTS; o++) {
        JsonObject ob = outArr.add<JsonObject>();
        ob["volume"] = outputs_[o].volume.load(std::memory_order_relaxed);
        ob["muted"] = outputs_[o].muted.load(std::memory_order_relaxed);
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        printf("[Mixer] Failed to save state to %s\n", path.c_str());
        return;
    }
    serializeJsonPretty(doc, f);
    printf("[Mixer] State saved to %s\n", path.c_str());
}

void AudioMixerEngine::loadState(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        printf("[Mixer] No saved state at %s, using defaults\n", path.c_str());
        setDefaults();
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    if (err) {
        printf("[Mixer] State parse error: %s, using defaults\n", err.c_str());
        setDefaults();
        return;
    }

    // Route matrix
    JsonArray routesArr = doc["routes"];
    if (routesArr) {
        for (JsonObject r : routesArr) {
            int i = r["in"] | 0;
            int o = r["out"] | 0;
            if (i >= 0 && i < MIXER_NUM_INPUTS && o >= 0 && o < MIXER_NUM_OUTPUTS) {
                routes_[i][o].enabled.store(r["enabled"] | false, std::memory_order_relaxed);
                routes_[i][o].volume.store(r["volume"] | 1.0f, std::memory_order_relaxed);
            }
        }
    }

    // Channel state
    JsonArray chArr = doc["channels"];
    if (chArr) {
        int idx = 0;
        for (JsonObject ch : chArr) {
            if (idx >= MIXER_NUM_INPUTS) break;
            channels_[idx].volume.store(ch["volume"] | 1.0f, std::memory_order_relaxed);
            channels_[idx].muted.store(ch["muted"] | false, std::memory_order_relaxed);
            channels_[idx].soloed.store(ch["soloed"] | false, std::memory_order_relaxed);
            idx++;
        }
    }

    // Output bus state
    JsonArray outArr = doc["outputs"];
    if (outArr) {
        int idx = 0;
        for (JsonObject ob : outArr) {
            if (idx >= MIXER_NUM_OUTPUTS) break;
            outputs_[idx].volume.store(ob["volume"] | 1.0f, std::memory_order_relaxed);
            outputs_[idx].muted.store(ob["muted"] | false, std::memory_order_relaxed);
            idx++;
        }
    }

    printf("[Mixer] State loaded from %s\n", path.c_str());
}
