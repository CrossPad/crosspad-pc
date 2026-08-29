// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SampleStreamEngine.hpp
 * @brief Streaming, polyphonic sample player for the PC simulator.
 *
 * The desktop counterpart of Marcel's ml_sample_stream_player on the board:
 * same shape (note slots, layers, choke groups, per-slot volume/pan/loop,
 * background refill separate from rendering), standard library only.
 *
 * ── Why streaming, and how the attack survives it ─────────────────────────
 * Samples are not loaded whole. Each layer keeps a short head in RAM — the
 * first kHeadFrames frames — and the rest arrives through a per-voice ring
 * that a background thread refills from the layer's own file handle. A hit
 * therefore sounds on the render call it arrived in (the head is already
 * resident) while the streamer has the whole length of that head, ~170 ms at
 * 48 kHz, to open the file and get ahead of the playhead. That is what makes
 * a 200 MB kit cost the same to trigger as a 200 kB one.
 *
 * ── Threads ───────────────────────────────────────────────────────────────
 * Three touch a voice and none of them blocks the audio thread:
 *
 *   render  — mixes Playing voices. Reads the head (immutable) and pops the
 *             ring. Takes no lock and allocates nothing. When the ring runs
 *             dry it *stalls* the voice rather than skipping ahead: a dropout
 *             is audible, but a jump in the playhead is audible *and* leaves
 *             the streamer chasing a position it never asked for.
 *   stream  — the background refill. Owns every WavReader and every ring
 *             write, so those need no atomics beyond the ring indices.
 *   control — note on/off, slot setup, wipe. Allocates and retires voices.
 *
 * control and stream share streamMutex_; render never takes it. Voice state
 * moves Free → Playing → Stopping → Free and each transition has exactly one
 * writer, so a stolen voice cannot be handed out twice.
 *
 * A voice holds a shared_ptr to its layer, so re-loading a kit under a
 * ringing pad frees nothing the renderer is still reading — on the board that
 * same situation needs an explicit render gate.
 */

#include "WavReader.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace crosspad_pc {

// ── Fixed sizes ──────────────────────────────────────────────────────────

/// Note slots. 0..15 are the pads; 16 is the spare the GUI auditions through,
/// which is exactly the layout the embedded player exposes.
inline constexpr uint8_t  kSampleSlotCount = 17;
inline constexpr uint8_t  kLayersPerSlot   = 8;
inline constexpr uint8_t  kMaxVoices       = 24;
inline constexpr uint8_t  kDefaultPolyphony = 16;

/// RAM-resident attack per layer, in frames. Also the streamer's head start.
inline constexpr uint32_t kHeadFrames = 8192;
/// Per-voice stream ring, in frames. Power of two — the index maths wraps by
/// masking, so a torn read cannot land outside the buffer.
inline constexpr uint32_t kRingFrames = 16384;

/// Loop-end sentinel, matching SamplerConfig.hpp and the embedded player.
inline constexpr uint32_t kSampleEndMax = 0xFFFFFFFFu;

// ── A layer: one WAV, cropped ────────────────────────────────────────────

struct SampleSource {
    std::string path;             ///< resolved filesystem path
    WavInfo     info{};
    uint32_t    startFrame = 0;   ///< crop start, frames into the data chunk
    uint32_t    length     = 0;   ///< frames from startFrame that may play
    uint32_t    headFrames = 0;   ///< how much of `head` is valid
    std::vector<int16_t> head;    ///< interleaved stereo, headFrames * 2
};
using SampleSourcePtr = std::shared_ptr<SampleSource>;

// ── Single-producer / single-consumer frame ring ─────────────────────────

class StreamRing {
public:
    void reset() {
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
    }
    /// Consumer's view. It owns r_, so only w_ needs acquire.
    uint32_t readable() const {
        return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed);
    }
    /// Producer's view — and the acquire is on r_, not w_, which is the whole
    /// point. Reading the consumer's index relaxed leaves no happens-before
    /// between its last read and this write, so the producer may legally
    /// overwrite a region the consumer is still copying out of. It is a real
    /// race, not a formality: ThreadSanitizer reports it as one memcpy against
    /// the other on the first churn run.
    uint32_t writable() const {
        return kRingFrames - (w_.load(std::memory_order_relaxed) -
                              r_.load(std::memory_order_acquire));
    }

    /// Producer side (stream thread). Writes min(frames, writable()).
    uint32_t write(const int16_t* src, uint32_t frames);
    /// Consumer side (render thread). Reads min(frames, readable()).
    uint32_t read(int16_t* dst, uint32_t frames);

private:
    std::vector<int16_t>  buf_ = std::vector<int16_t>(kRingFrames * 2, 0);
    std::atomic<uint32_t> w_{0};
    std::atomic<uint32_t> r_{0};
};

// ── Voice ────────────────────────────────────────────────────────────────

enum class VoiceState : uint8_t { Free = 0, Playing = 1, Stopping = 2 };

struct Voice {
    std::atomic<uint8_t> state{static_cast<uint8_t>(VoiceState::Free)};

    SampleSourcePtr src;              ///< written only while Free
    uint8_t  note    = 0;
    uint8_t  wavIdx  = 0;
    uint8_t  group   = 0;
    float    gainL   = 0.0f;
    float    gainR   = 0.0f;
    bool     loop    = false;
    uint32_t loopEnd = 0;             ///< frames; loop region is [0, loopEnd)
    uint64_t seq     = 0;             ///< allocation order, for voice stealing

    /// Frames the renderer has consumed since note-on. Monotonic: with a loop
    /// the position inside the sample is played % loopEnd, so nothing has to
    /// be rewound when the loop point moves under a ringing voice.
    std::atomic<uint64_t> played{0};

    StreamRing ring;

    // Stream-thread only, under streamMutex_.
    WavReader reader;
    uint64_t  fetched = 0;            ///< frames handed to the ring so far

    std::atomic<uint32_t> underruns{0};

    /// Set while the renderer is inside this voice. A thread about to free the
    /// voice's layer publishes a non-Playing state and then waits here, so it
    /// can never pull the sample out from under a render call in flight. The
    /// renderer only ever *stores* to it — it never waits on anything.
    std::atomic<bool> rendering{false};
};

// ── The engine ───────────────────────────────────────────────────────────

class SampleStreamEngine {
public:
    SampleStreamEngine() = default;
    ~SampleStreamEngine();

    SampleStreamEngine(const SampleStreamEngine&)            = delete;
    SampleStreamEngine& operator=(const SampleStreamEngine&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────

    /// @param ownThread  start the background streamer. Pass false to drive
    ///                   refills yourself with pump(), which is what a test
    ///                   that wants a deterministic frame count does.
    bool init(bool ownThread = true);
    void deinit();
    bool isReady() const { return ready_.load(std::memory_order_acquire); }

    /// Size the render scratch. Called before the first render(); calling it
    /// again with a larger block is safe, calling it from the audio thread is
    /// not.
    void prepare(uint32_t maxFrames, uint32_t sampleRate);

    /// Everything under `path` that does not already start with it is
    /// resolved against this. The kit JSON stores device-absolute paths
    /// ("/crosspad/kits/..."), which on the desktop live under the folder
    /// mounted as the virtual SD card.
    void setRootPrefix(const std::string& prefix);
    std::string resolvePath(const char* path) const;

    // ── Slot setup (control thread) ──────────────────────────────

    /// Attach a WAV to a slot layer. `lenFrames` of 0 (or past the end) means
    /// "to the end of the file".
    bool setupSample(uint8_t note, const char* path, uint8_t group, uint8_t wavIdx,
                     uint32_t skipFrames, uint32_t lenFrames);
    void wipeSlot(uint8_t note);
    void wipeAll();

    void setVolume(uint8_t note, uint8_t volume);   ///< 0..127
    void setPan(uint8_t note, uint8_t pan);         ///< 0..127, 64 = centre
    void setLoopEnd(uint8_t note, uint32_t loopEnd);
    void clearLoop(uint8_t note);
    void setMaxPolyphony(uint8_t voices);

    uint8_t freeLayerCount() const;
    bool    slotInfo(uint8_t note, uint8_t wavIdx, SampleSourcePtr& out) const;

    // ── Playback (control thread) ────────────────────────────────

    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void allNotesOff();

    /// Callbacks fired when a voice actually starts and ends. The start one
    /// runs on the caller's thread; the end one runs on the streamer, so a
    /// GUI handler must hop to its own thread.
    using NoteCallback = void (*)(uint8_t note, uint8_t wavIdx);
    void setNoteCallbacks(NoteCallback onOn, NoteCallback onOff);

    // ── Render (audio thread) ────────────────────────────────────

    /// Add into an interleaved stereo float buffer. RT-safe: no allocation,
    /// no lock, no I/O.
    void renderAdd(float* out, uint32_t frames);

    /// Planar int16, the signature the embedded node renders through. Writes
    /// (does not add), so the caller does not have to clear first.
    void renderPlanar(int16_t* chL, int16_t* chR, uint32_t frames);

    /// One refill pass. Called by the background thread; call it yourself
    /// when init(false) was used.
    void pump();

    // ── Telemetry ────────────────────────────────────────────────

    uint32_t activeVoices() const;
    uint32_t underrunCount() const { return underruns_.load(std::memory_order_relaxed); }
    uint32_t droppedNotes() const  { return dropped_.load(std::memory_order_relaxed); }
    float    peak() const          { return peak_.load(std::memory_order_relaxed); }
    float    renderLoad() const    { return renderLoad_.load(std::memory_order_relaxed); }
    uint32_t sampleRate() const    { return sampleRate_; }

private:
    struct Slot {
        SampleSourcePtr layers[kLayersPerSlot];
        uint8_t layerCount = 0;
        uint8_t nextLayer  = 0;       ///< round-robin cursor
        uint8_t group      = 0;
        uint8_t volume     = 100;
        uint8_t pan        = 64;
        bool     loop      = false;
        uint32_t loopEnd   = 0;
    };

    SampleSourcePtr loadSource(const std::string& fsPath, uint32_t skipFrames,
                               uint32_t lenFrames) const;

    /// Reclaim every Stopping voice. Caller holds streamMutex_.
    void reclaimStoppedLocked();
    void releaseVoiceLocked(Voice& v);
    int  allocVoiceLocked();
    void fillVoiceLocked(Voice& v);

    void streamLoop();
    void flushPendingOff();

    mutable std::mutex ctrlMutex_;    ///< serialises control-thread mutations
    std::mutex         streamMutex_;  ///< guards readers and ring writes

    Slot  slots_[kSampleSlotCount];
    std::vector<Voice> voices_ = std::vector<Voice>(kMaxVoices);

    std::string rootPrefix_;

    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    std::thread       streamThread_;

    uint32_t sampleRate_ = 48000;
    uint32_t maxFrames_  = 0;
    std::vector<int16_t> scratch_;    ///< render-side ring drain, stereo
    std::vector<float>   scratchF_;   ///< planar render staging, interleaved

    /// Voices retired under a lock; their note-off callbacks fire after it.
    std::vector<uint16_t> pendingOff_;

    uint8_t  maxPolyphony_ = kDefaultPolyphony;
    uint64_t seqCounter_   = 0;

    NoteCallback onNoteOn_  = nullptr;
    NoteCallback onNoteOff_ = nullptr;

    std::atomic<uint32_t> underruns_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<float>    peak_{0.0f};
    std::atomic<float>    renderLoad_{0.0f};
};

/// The engine the SampleStreamPlayer_* API and PcSampleNode share.
SampleStreamEngine& getSampleStreamEngine();

} // namespace crosspad_pc
