// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SampleStreamPlayer.hpp
 * @brief The embedded sample player's API, backed by SampleStreamEngine.
 *
 * Names, argument order and return types match Marcel's ml_smpl_stream_ply.h
 * so the platform glue that drives the sampler on the board reads the same
 * here — a `SamplerPlatformCallbacks` implementation can be moved between the
 * two with the includes changed and nothing else. Parameters that are
 * `char*` on the board are `const char*` here, which accepts both.
 *
 * Two things the board's player cannot do are added rather than substituted:
 * SampleStreamPly_ProcessAdd() renders straight into the float bus the PC
 * audio module runs on, and SampleStreamPlayer_SetRootPrefix() maps the
 * device-absolute paths a kit stores onto whatever folder is mounted as the
 * virtual SD card.
 */

#include <cstdint>

/// Note slots the player exposes: 0..15 are pads, 16 is the audition slot.
inline constexpr uint8_t SAMPLE_STREAM_PLY_CH_CNT = 17;

/// Note slot the GUI auditions through — the one above the pad range, exactly
/// as on the board, where anything higher is refused by the player.
inline constexpr uint8_t SAMPLE_STREAM_PLY_PREVIEW_NOTE = 16;

/// "To the end of the sample". Constants, not the `#define SAMPLE_END_MAX` the
/// board's header uses: crosspad-sampler declares its own
/// `crosspad_sampler::SAMPLE_END_MAX`, and a macro of that name turns that
/// declaration into a syntax error in whichever translation unit happens to
/// include the two headers the wrong way round. On the board it only compiles
/// because one file's include order says so.
inline constexpr uint32_t SAMPLE_STREAM_PLY_END_MAX = 0xFFFFFFFFu;

struct sample_info_from_file {
    uint16_t numberOfChannels = 0;
    uint32_t sampleRate       = 0;
    uint32_t byteRate         = 0;
    uint16_t bytesPerSample   = 0;
    uint16_t bitsPerSample    = 0;
    uint32_t dataLength       = 0;
    uint32_t sampleCount      = 0;
};

// ── Lifecycle ────────────────────────────────────────────────────────────

bool SampleStreamPlayer_Init(void);
void SampleStreamPlayer_DeInit(void);
bool SampleStreamPlayer_IsReady(void);

/// Prefix that device-absolute sample paths ("/crosspad/kits/…") resolve
/// against. Set it whenever the virtual SD card is mounted or unmounted.
void SampleStreamPlayer_SetRootPrefix(const char* prefix);

/// Buffer size and sample rate of the audio module. Call before the first
/// render, from the thread that owns the pipeline — not from the audio thread.
void SampleStreamPlayer_Prepare(uint32_t maxFrames, uint32_t sampleRate);

// ── Sample assignment ────────────────────────────────────────────────────

bool SampleStreamPlayer_GetInfo(struct sample_info_from_file* ptr, const char* filename);

bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename, uint8_t group);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename, uint8_t group,
                                    uint8_t wavIdx);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char* filename, uint8_t group,
                                    uint8_t wavIdx, uint32_t skipSamples, uint32_t lenSamples);

void    SampleStreamPlayer_WipeOut(void);
void    SampleStreamPlayer_WipeOut(uint8_t note);
uint8_t SampleStreamPlayer_GetFreeWavCnt(void);
void    SampleStreamPlayer_SetMaxPolyphony(uint8_t maxPoly);

// ── Per-slot parameters ──────────────────────────────────────────────────

void SampleStreamPlayer_SetVolume(uint8_t note, uint8_t volume);   ///< 0..127
void SampleStreamPlayer_SetPan(uint8_t note, uint8_t pan);         ///< 0..127
void SampleStreamPlayer_SetLoopEnd(uint8_t note, uint32_t loop_end);
void SampleStreamPlayer_SetLoopClear(uint8_t note);
void SampleStreamPlayer_SetLoopClear(void);

// ── Playback ─────────────────────────────────────────────────────────────

void SampleStreamPlayer_NoteOn(uint8_t note, uint8_t vel);
void SampleStreamPlayer_NoteOff(uint8_t note);

/// Audition a file through the spare slot: wipe, set up, play. Matches what
/// the board's platform glue does, because the board's own Preview() renders
/// nothing audible there.
void SampleStreamPlayer_Preview(const char* filename);
void SampleStreamPlayer_Preview(const char* filename, uint32_t start, uint32_t end);

/// Callbacks fired when a voice starts and when it is retired. The start one
/// runs on the caller's thread, the end one on the streamer.
void SampleStreamPlayer_SetNoteCallbacks(void (*onNoteOn)(uint8_t note, uint8_t wavIdx),
                                         void (*onNoteOff)(uint8_t note, uint8_t wavIdx));

// ── Waveform ─────────────────────────────────────────────────────────────

/// Peak-per-bin over the whole file / over [start, end). `buffer` receives
/// `count` values.
bool SampleStreamPlayer_GetWaveform(const char* filename, int16_t* buffer, uint32_t count);
bool SampleStreamPlayer_GetWaveform(const char* filename, int16_t* buffer, uint32_t count,
                                    uint32_t start, uint32_t end);

// ── Rendering ────────────────────────────────────────────────────────────

/// Planar int16, the board's signature. Overwrites both buffers.
void SampleStreamPly_Process(int16_t* chL, int16_t* chR, uint32_t count);

/// Interleaved stereo float, added into the bus. What PcSampleNode uses.
void SampleStreamPly_ProcessAdd(float* interleaved, uint32_t frames);

/// Refill pass. A no-op while the engine runs its own streamer, which it does
/// by default — kept so code written against the board's loop still builds.
void SampleStreamPlayer_BkgProc(void);

// ── Telemetry ────────────────────────────────────────────────────────────

float    SampleStreamPly_GetLoadMain(void);
uint32_t SampleStreamPly_GetActiveVoices(void);
uint32_t SampleStreamPly_GetUnderrunCount(void);
uint32_t SampleStreamPly_GetDroppedNoteCount(void);
float    SampleStreamPly_GetPeak(void);
