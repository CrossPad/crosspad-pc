// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcSamplerEngine.hpp
 * @brief The simulator's side of crosspad::SamplerEngine.
 *
 * Core owns the engine -- voices, residency, the stream window, the pitch,
 * Loop pads that launch on the bar. This supplies what it will not know
 * about, as platform-idf's SamplerEnginePort does on the board: where bytes
 * come from (the virtual SD card, through stdio), where memory comes from
 * (the heap) and when the streaming pass runs (a thread of its own beside
 * RtAudio's, the board's audio_bg).
 *
 * Every path these take is a kit's logical one ("/crosspad/kits/…") or one
 * already rooted at the mount, as the kit browser hands out both.
 *
 * Unlike the board there is no arena to share with the pitched engine, so a
 * kit is read on the thread that asks for it -- the kit loader's worker, or
 * the pad editor -- under one loader lock, and an audition plays with no kit
 * at all.
 */

#include <cstdint>
#include <string>

namespace crosspad { class IAudioNode; }

namespace crosspad_pc {

/// Where logical paths ("/crosspad/kits/…") resolve: the folder mounted as
/// the virtual SD card, empty when none is.
void native_sampler_set_root(const std::string& root);
/// A logical path, or one already rooted at the mount, as a filesystem path.
std::string native_sampler_fs_path(const char* path);

/// The "Sampler" channel: the kit's voices and the audition. Binds the pool
/// and the store and starts the streaming thread the first time it is asked.
crosspad::IAudioNode* native_sampler_node();

/// True while the engine holds a kit.
bool native_sampler_active();

/**
 * @brief Put pad @p pad of the current sampler kit into the engine.
 *
 * Not holding the kit yet: takes it whole first (every pad). Holding another
 * kit: heads are sized for the incoming one and this pad swaps -- a voice
 * already sounding plays its old sample out, and the old sample goes back
 * once nothing reads it. Holding this kit: the pad is rebuilt in place (the
 * pad editor). Reads the card. False when the current kit is not a sampler
 * kit or nothing of it could be read. @p fromLoader: the kit loader's worker,
 * which does not try a kit again pad after pad once none of it could be read.
 */
bool native_sampler_swap_pad(uint8_t pad, bool fromLoader);

/// Every voice ended and every sample given back; the audition stays
/// possible. Nothing to do while the engine holds no kit.
void native_sampler_release();

/// Whether a hit on @p pad sounds now: the engine holds the current kit and
/// the pad has a sample in it.
bool native_sampler_pad_ready(uint8_t pad);

void native_sampler_note_on(uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key);
void native_sampler_note_off(uint8_t pad, uint8_t key);
bool native_sampler_arm(uint8_t touch, uint8_t pad, int8_t transpose, uint8_t key);
void native_sampler_play(uint8_t touch, uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key);
void native_sampler_disarm(uint8_t touch);
bool native_sampler_pad_sounding(uint8_t pad);

/// Level and pan as the kit stores them (volume 100 unity, pan a signed
/// offset from centre), applied to the voices already sounding too.
void native_sampler_set_pad_volume(uint8_t pad, int volume);
void native_sampler_set_pad_pan(uint8_t pad, int pan);
bool native_sampler_set_pad_pitch(uint8_t pad, int semitones);

/// Silence a pad now, whatever its playbackMode says.
void native_sampler_stop_pad(uint8_t pad);
/// Take the pad's playbackMode from the kit model (the editor's loop button).
bool native_sampler_sync_pad_mode(uint8_t pad);
/// Samples the store could still hold -- all of them with no kit, so a pad
/// of an empty kit can be given its first sample.
int native_sampler_free_slots();

/// A WAV's header -- from the store when the kit holds it, else the card.
bool native_sampler_describe(const char* path, uint32_t* frames, uint8_t* channels,
                             uint32_t* rate, uint8_t* bits);

/// Peak per bin of [start, end) of a WAV (end 0 or past it: to the end),
/// first channel.
bool native_sampler_waveform(const char* path, int16_t* out, uint32_t count,
                             uint32_t start, uint32_t end);

/// Audition @p path (end 0 or 0xFFFFFFFF: to the end). An empty path stops it.
bool native_sampler_preview(const char* path, uint32_t start, uint32_t end, int volume, int pan);

void native_sampler_scope_enable(bool on);
uint32_t native_sampler_scope_snapshot(float* out, uint32_t maxSamples, uint32_t* sampleRate);

/// What the control port's smpl_peak reports.
struct NativeSamplerStats {
    float    peak = 0.0f;          ///< loudest sample since the last read, 0..1, read and cleared
    int      freeSlots = -1;
    uint32_t activeVoices = 0;
    uint32_t underruns = 0;
    uint32_t droppedNotes = 0;
    float    renderLoad = 0.0f;    ///< longest render since the last read, as a share of its block
};
NativeSamplerStats native_sampler_stats();

/// smpl_loops: each pad's Loop state ('.' idle, 'q' waiting for the bar,
/// 'P' playing, 'm' muted), the launch counters and the bar clock.
struct NativeSamplerLoops {
    char     states[17] = {};
    uint32_t queued = 0;
    uint32_t launched = 0;
    bool     clockRunning = false;
    uint32_t framesToBar = 0;
};
NativeSamplerLoops native_sampler_loops();

} // namespace crosspad_pc
