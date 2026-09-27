// SPDX-License-Identifier: MIT

#include "SampleStreamPlayer.hpp"
#include "SampleStreamEngine.hpp"
#include "WavReader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using crosspad_pc::getSampleStreamEngine;

// ── Lifecycle ────────────────────────────────────────────────────────────

bool SampleStreamPlayer_Init(void) { return getSampleStreamEngine().init(true); }
void SampleStreamPlayer_DeInit(void) { getSampleStreamEngine().deinit(); }
bool SampleStreamPlayer_IsReady(void) { return getSampleStreamEngine().isReady(); }

void SampleStreamPlayer_SetRootPrefix(const char* prefix) {
    getSampleStreamEngine().setRootPrefix(prefix ? prefix : "");
}

void SampleStreamPlayer_Prepare(uint32_t maxFrames, uint32_t sampleRate) {
    getSampleStreamEngine().prepare(maxFrames, sampleRate);
}

// ── Sample assignment ────────────────────────────────────────────────────

bool SampleStreamPlayer_GetInfo(struct sample_info_from_file* ptr, const char* filename) {
    if (!ptr || !filename) return false;
    crosspad_pc::WavInfo info;
    const std::string resolved = getSampleStreamEngine().resolvePath(filename);
    if (!crosspad_pc::wav_read_info(resolved.c_str(), info)) return false;

    ptr->numberOfChannels = info.channels;
    ptr->sampleRate       = info.sampleRate;
    ptr->bitsPerSample    = info.bitsPerSample;
    ptr->bytesPerSample   = info.blockAlign;
    ptr->byteRate         = info.sampleRate * info.blockAlign;
    ptr->dataLength       = info.dataBytes;
    ptr->sampleCount      = info.frameCount;
    return true;
}

bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename) {
    return SampleStreamPlayer_SetupSample(note, filename, 0, 0, 0, 0);
}
bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename, uint8_t group) {
    return SampleStreamPlayer_SetupSample(note, filename, group, 0, 0, 0);
}
bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename, uint8_t group,
                                    uint8_t wavIdx) {
    return SampleStreamPlayer_SetupSample(note, filename, group, wavIdx, 0, 0);
}
bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename, uint8_t group,
                                    uint8_t wavIdx, uint32_t skipSamples,
                                    uint32_t lenSamples) {
    // The kit stores an absolute end position, the engine wants a length —
    // the same translation the board's glue does before calling in.
    uint32_t len = lenSamples;
    if (len != 0 && len != SAMPLE_STREAM_PLY_END_MAX && len > skipSamples) len -= skipSamples;
    else if (len == SAMPLE_STREAM_PLY_END_MAX) len = 0;
    else if (len != 0 && len <= skipSamples) len = 0;
    return getSampleStreamEngine().setupSample(note, filename, group, wavIdx, skipSamples, len);
}

void SampleStreamPlayer_WipeOut(void) { getSampleStreamEngine().wipeAll(); }
void SampleStreamPlayer_WipeOut(uint8_t note) { getSampleStreamEngine().wipeSlot(note); }

uint8_t SampleStreamPlayer_GetFreeWavCnt(void) {
    return getSampleStreamEngine().freeLayerCount();
}

void SampleStreamPlayer_SetMaxPolyphony(uint8_t maxPoly) {
    getSampleStreamEngine().setMaxPolyphony(maxPoly);
}

// ── Per-slot parameters ──────────────────────────────────────────────────

void SampleStreamPlayer_SetVolume(uint8_t note, uint8_t volume) {
    getSampleStreamEngine().setVolume(note, volume);
}
void SampleStreamPlayer_SetPan(uint8_t note, uint8_t pan) {
    getSampleStreamEngine().setPan(note, pan);
}
void SampleStreamPlayer_SetLoopEnd(uint8_t note, uint32_t loop_end) {
    getSampleStreamEngine().setLoopEnd(note, loop_end);
}
void SampleStreamPlayer_SetLoopClear(uint8_t note) {
    getSampleStreamEngine().clearLoop(note);
}
void SampleStreamPlayer_SetLoopClear(void) {
    for (uint8_t n = 0; n < SAMPLE_STREAM_PLY_CH_CNT; ++n) {
        getSampleStreamEngine().clearLoop(n);
    }
}

// ── Playback ─────────────────────────────────────────────────────────────

void SampleStreamPlayer_NoteOn(uint8_t note, uint8_t vel) {
    getSampleStreamEngine().noteOn(note, vel);
}
void SampleStreamPlayer_NoteOff(uint8_t note) {
    getSampleStreamEngine().noteOff(note);
}

void SampleStreamPlayer_Preview(const char* filename) {
    SampleStreamPlayer_Preview(filename, 0, SAMPLE_STREAM_PLY_END_MAX);
}

void SampleStreamPlayer_Preview(const char* filename, uint32_t start, uint32_t end) {
    if (!filename || !filename[0]) return;
    constexpr uint8_t kSlot = SAMPLE_STREAM_PLY_PREVIEW_NOTE;

    // Wipe first so repeated auditions reuse one layer instead of piling up.
    getSampleStreamEngine().wipeSlot(kSlot);

    uint32_t len = 0;
    if (end != 0 && end != SAMPLE_STREAM_PLY_END_MAX && end > start) len = end - start;

    if (!getSampleStreamEngine().setupSample(kSlot, filename, /*group=*/0, /*wavIdx=*/0,
                                             start, len)) {
        return;
    }
    getSampleStreamEngine().setVolume(kSlot, 100);
    getSampleStreamEngine().setPan(kSlot, 64);
    getSampleStreamEngine().clearLoop(kSlot);
    getSampleStreamEngine().noteOn(kSlot, 127);
}

void SampleStreamPlayer_SetNoteCallbacks(void (*onNoteOn)(uint8_t, uint8_t),
                                         void (*onNoteOff)(uint8_t, uint8_t)) {
    getSampleStreamEngine().setNoteCallbacks(onNoteOn, onNoteOff);
}

// ── Waveform ─────────────────────────────────────────────────────────────

bool SampleStreamPlayer_GetWaveform(const char* filename, int16_t* buffer, uint32_t count) {
    return SampleStreamPlayer_GetWaveform(filename, buffer, count, 0, 0);
}

bool SampleStreamPlayer_GetWaveform(const char* filename, int16_t* buffer, uint32_t count,
                                    uint32_t start, uint32_t end) {
    if (!filename || !buffer || count == 0) return false;

    crosspad_pc::WavReader reader;
    const std::string resolved = getSampleStreamEngine().resolvePath(filename);
    if (!reader.open(resolved.c_str())) return false;

    const uint32_t total = reader.info().frameCount;
    if (end == 0 || end == SAMPLE_STREAM_PLY_END_MAX || end > total) end = total;
    if (start >= end) start = 0;
    const uint32_t range = end - start;
    if (range == 0) return false;

    // Peak per bin, which is what a waveform view wants: an averaged bin makes
    // a transient look like a ramp.
    constexpr uint32_t kChunk = 512;
    std::vector<int16_t> tmp(static_cast<size_t>(kChunk) * 2);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t binStart = start + static_cast<uint32_t>(
            (static_cast<uint64_t>(i) * range) / count);
        uint32_t binEnd = start + static_cast<uint32_t>(
            (static_cast<uint64_t>(i + 1) * range) / count);
        if (binEnd > end) binEnd = end;
        if (binEnd <= binStart) { buffer[i] = 0; continue; }

        if (!reader.seekFrame(binStart)) { buffer[i] = 0; continue; }

        int16_t peak = 0;
        uint32_t left = binEnd - binStart;
        while (left > 0) {
            const uint32_t want = std::min(left, kChunk);
            const uint32_t got  = reader.readStereo(tmp.data(), want);
            if (got == 0) break;
            for (uint32_t f = 0; f < got; ++f) {
                const int32_t v = tmp[f * 2];
                if (std::abs(v) > std::abs(static_cast<int32_t>(peak))) {
                    peak = static_cast<int16_t>(v);
                }
            }
            left -= got;
        }
        buffer[i] = peak;
    }
    return true;
}

// ── Rendering ────────────────────────────────────────────────────────────

void SampleStreamPly_Process(int16_t* chL, int16_t* chR, uint32_t count) {
    getSampleStreamEngine().renderPlanar(chL, chR, count);
}

void SampleStreamPly_ProcessAdd(float* interleaved, uint32_t frames) {
    getSampleStreamEngine().renderAdd(interleaved, frames);
}

void SampleStreamPlayer_BkgProc(void) {
    // The engine's own streamer does this. Left callable so a port written
    // against the board's main loop still links.
}

// ── Telemetry ────────────────────────────────────────────────────────────

float    SampleStreamPly_GetLoadMain(void)       { return getSampleStreamEngine().renderLoad(); }
uint32_t SampleStreamPly_GetActiveVoices(void)   { return getSampleStreamEngine().activeVoices(); }
uint32_t SampleStreamPly_GetUnderrunCount(void)  { return getSampleStreamEngine().underrunCount(); }
uint32_t SampleStreamPly_GetDroppedNoteCount(void) { return getSampleStreamEngine().droppedNotes(); }
float    SampleStreamPly_GetPeak(void)           { return getSampleStreamEngine().peak(); }
