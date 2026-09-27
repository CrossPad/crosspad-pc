// SPDX-License-Identifier: MIT

#include "SampleStreamEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace crosspad_pc {

namespace {

constexpr uint32_t kRingMask   = kRingFrames - 1;
constexpr uint32_t kFillChunk  = 1024;    ///< frames per read on the streamer
constexpr uint32_t kStreamTickMs = 4;

static_assert((kRingFrames & kRingMask) == 0, "kRingFrames must be a power of two");

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/// Pack a retired voice's identity into one value so the pending list stays a
/// POD vector that can be drained without touching voice state again.
inline uint16_t packNote(uint8_t note, uint8_t wavIdx) {
    return static_cast<uint16_t>((static_cast<uint16_t>(note) << 8) | wavIdx);
}

} // namespace

// ── StreamRing ───────────────────────────────────────────────────────────

uint32_t StreamRing::write(const int16_t* src, uint32_t frames) {
    const uint32_t space = writable();
    if (frames > space) frames = space;
    if (frames == 0) return 0;

    const uint32_t w = w_.load(std::memory_order_relaxed) & kRingMask;
    const uint32_t first = std::min(frames, kRingFrames - w);
    std::memcpy(&buf_[w * 2], src, first * 2 * sizeof(int16_t));
    if (frames > first) {
        std::memcpy(&buf_[0], src + first * 2, (frames - first) * 2 * sizeof(int16_t));
    }
    w_.fetch_add(frames, std::memory_order_release);
    return frames;
}

uint32_t StreamRing::read(int16_t* dst, uint32_t frames) {
    const uint32_t avail = readable();
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    const uint32_t r = r_.load(std::memory_order_relaxed) & kRingMask;
    const uint32_t first = std::min(frames, kRingFrames - r);
    std::memcpy(dst, &buf_[r * 2], first * 2 * sizeof(int16_t));
    if (frames > first) {
        std::memcpy(dst + first * 2, &buf_[0], (frames - first) * 2 * sizeof(int16_t));
    }
    r_.fetch_add(frames, std::memory_order_release);
    return frames;
}

// ── Lifecycle ────────────────────────────────────────────────────────────

SampleStreamEngine::~SampleStreamEngine() { deinit(); }

bool SampleStreamEngine::init(bool ownThread) {
    if (ready_.load(std::memory_order_acquire)) return true;

    pendingOff_.reserve(kMaxVoices);
    prepare(1024, sampleRate_);
    ready_.store(true, std::memory_order_release);

    if (ownThread) {
        running_.store(true, std::memory_order_release);
        streamThread_ = std::thread(&SampleStreamEngine::streamLoop, this);
    }
    return true;
}

void SampleStreamEngine::deinit() {
    if (running_.exchange(false, std::memory_order_acq_rel) && streamThread_.joinable()) {
        streamThread_.join();
    }
    ready_.store(false, std::memory_order_release);
    wipeAll();
}

void SampleStreamEngine::prepare(uint32_t maxFrames, uint32_t sampleRate) {
    if (sampleRate) sampleRate_ = sampleRate;
    if (maxFrames > maxFrames_) {
        maxFrames_ = maxFrames;
        scratch_.assign(static_cast<size_t>(maxFrames_) * 2, 0);
        scratchF_.assign(static_cast<size_t>(maxFrames_) * 2, 0.0f);
    }
}

void SampleStreamEngine::setRootPrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> g(ctrlMutex_);
    rootPrefix_ = prefix;
    while (!rootPrefix_.empty() &&
           (rootPrefix_.back() == '/' || rootPrefix_.back() == '\\')) {
        rootPrefix_.pop_back();
    }
}

std::string SampleStreamEngine::resolvePath(const char* path) const {
    if (!path || !path[0]) return {};
    std::string p(path);
    for (char& c : p) { if (c == '\\') c = '/'; }
    if (rootPrefix_.empty()) return p;
    // Already rooted at the mount point — the kit browser hands out both
    // shapes depending on which side produced the string.
    if (p.compare(0, rootPrefix_.size(), rootPrefix_) == 0) return p;
    if (p.empty() || p[0] != '/') return rootPrefix_ + "/" + p;
    return rootPrefix_ + p;
}

// ── Slot setup ───────────────────────────────────────────────────────────

SampleSourcePtr SampleStreamEngine::loadSource(const std::string& fsPath,
                                               uint32_t skipFrames,
                                               uint32_t lenFrames) const {
    WavReader reader;
    if (!reader.open(fsPath.c_str())) return nullptr;

    const WavInfo& info = reader.info();
    if (skipFrames >= info.frameCount) skipFrames = 0;

    uint32_t avail = info.frameCount - skipFrames;
    if (lenFrames == 0 || lenFrames == kSampleEndMax || lenFrames > avail) lenFrames = avail;
    if (lenFrames == 0) return nullptr;

    auto src = std::make_shared<SampleSource>();
    src->path       = fsPath;
    src->info       = info;
    src->startFrame = skipFrames;
    src->length     = lenFrames;
    src->headFrames = std::min(kHeadFrames, lenFrames);

    src->head.assign(static_cast<size_t>(src->headFrames) * 2, 0);
    if (!reader.seekFrame(skipFrames)) return nullptr;
    const uint32_t got = reader.readStereo(src->head.data(), src->headFrames);
    if (got == 0) return nullptr;
    // A short read is not fatal: the head simply covers less than asked, and
    // the streamer picks up from wherever it actually ends.
    src->headFrames = got;
    src->head.resize(static_cast<size_t>(got) * 2);
    return src;
}

bool SampleStreamEngine::setupSample(uint8_t note, const char* path, uint8_t group,
                                     uint8_t wavIdx, uint32_t skipFrames,
                                     uint32_t lenFrames) {
    if (note >= kSampleSlotCount || wavIdx >= kLayersPerSlot) return false;

    // Parsed outside the locks: this opens a file and reads the attack, which
    // is exactly the kind of work that must not sit in front of the streamer.
    const std::string fsPath = resolvePath(path);
    SampleSourcePtr src = loadSource(fsPath, skipFrames, lenFrames);
    if (!src) {
        std::printf("[sampler] setupSample: cannot use '%s'\n", fsPath.c_str());
        return false;
    }

    std::lock_guard<std::mutex> gc(ctrlMutex_);
    Slot& slot = slots_[note];
    slot.layers[wavIdx] = std::move(src);
    slot.group = group;
    if (wavIdx + 1 > slot.layerCount) slot.layerCount = static_cast<uint8_t>(wavIdx + 1);
    if (slot.nextLayer >= slot.layerCount) slot.nextLayer = 0;
    return true;
}

void SampleStreamEngine::wipeSlot(uint8_t note) {
    if (note >= kSampleSlotCount) return;
    {
        std::lock_guard<std::mutex> gc(ctrlMutex_);
        std::lock_guard<std::mutex> gs(streamMutex_);
        for (auto& v : voices_) {
            if (v.state.load(std::memory_order_acquire) ==
                static_cast<uint8_t>(VoiceState::Playing) && v.note == note) {
                releaseVoiceLocked(v);
            }
        }
        slots_[note] = Slot{};
    }
    flushPendingOff();
}

void SampleStreamEngine::wipeAll() {
    {
        std::lock_guard<std::mutex> gc(ctrlMutex_);
        std::lock_guard<std::mutex> gs(streamMutex_);
        for (auto& v : voices_) releaseVoiceLocked(v);
        for (auto& s : slots_) s = Slot{};
    }
    flushPendingOff();
}

void SampleStreamEngine::setVolume(uint8_t note, uint8_t volume) {
    if (note >= kSampleSlotCount) return;
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    slots_[note].volume = std::min<uint8_t>(volume, 127);
}

void SampleStreamEngine::setPan(uint8_t note, uint8_t pan) {
    if (note >= kSampleSlotCount) return;
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    slots_[note].pan = std::min<uint8_t>(pan, 127);
}

void SampleStreamEngine::setLoopEnd(uint8_t note, uint32_t loopEnd) {
    if (note >= kSampleSlotCount) return;
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    slots_[note].loop    = true;
    slots_[note].loopEnd = loopEnd;
}

void SampleStreamEngine::clearLoop(uint8_t note) {
    if (note >= kSampleSlotCount) return;
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    slots_[note].loop    = false;
    slots_[note].loopEnd = 0;
}

void SampleStreamEngine::setMaxPolyphony(uint8_t voices) {
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    maxPolyphony_ = std::max<uint8_t>(1, std::min<uint8_t>(voices, kMaxVoices));
}

uint8_t SampleStreamEngine::freeLayerCount() const {
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    int used = 0;
    for (const auto& s : slots_) {
        for (uint8_t i = 0; i < kLayersPerSlot; ++i) {
            if (s.layers[i]) ++used;
        }
    }
    const int total = kSampleSlotCount * kLayersPerSlot;
    const int freeSlots = total - used;
    return static_cast<uint8_t>(std::min(freeSlots, 255));
}

bool SampleStreamEngine::slotInfo(uint8_t note, uint8_t wavIdx, SampleSourcePtr& out) const {
    if (note >= kSampleSlotCount || wavIdx >= kLayersPerSlot) return false;
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    out = slots_[note].layers[wavIdx];
    return static_cast<bool>(out);
}

// ── Voice bookkeeping ────────────────────────────────────────────────────

void SampleStreamEngine::releaseVoiceLocked(Voice& v) {
    const uint8_t st = v.state.load(std::memory_order_acquire);
    if (st == static_cast<uint8_t>(VoiceState::Free)) return;

    // Publish "not playing" first, then wait out a render call that entered
    // before it. Both sides are seq_cst, which is what makes the pair a
    // handshake rather than two independent flags: the renderer sets
    // `rendering` and then re-reads the state, so exactly one of the two sees
    // the other. Bounded by one audio block, and never waited on by the
    // renderer itself.
    v.state.store(static_cast<uint8_t>(VoiceState::Stopping), std::memory_order_seq_cst);
    while (v.rendering.load(std::memory_order_seq_cst)) std::this_thread::yield();

    pendingOff_.push_back(packNote(v.note, v.wavIdx));
    v.reader.close();
    v.ring.reset();
    v.src.reset();
    v.fetched = 0;
    v.played.store(0, std::memory_order_relaxed);
    v.state.store(static_cast<uint8_t>(VoiceState::Free), std::memory_order_release);
}

void SampleStreamEngine::reclaimStoppedLocked() {
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) ==
            static_cast<uint8_t>(VoiceState::Stopping)) {
            releaseVoiceLocked(v);
        }
    }
}

int SampleStreamEngine::allocVoiceLocked() {
    // Polyphony is a cap on *sounding* voices, not on the pool: the pool is
    // larger so a steal always has somewhere to land.
    //
    // The loop matters. Stealing exactly one voice per note-on only caps the
    // *growth* — lower the cap from 16 to 4 with sixteen voices already
    // ringing and every subsequent hit retires one and starts one, so the
    // count sits at sixteen forever and the setting looks ignored. Retiring
    // oldest-first until there is room brings a running voice count down to
    // the new cap on the next hit, which is what someone lowering it is
    // asking for.
    for (;;) {
        int freeIdx = -1, playing = 0, oldestIdx = -1;
        uint64_t oldestSeq = UINT64_MAX;

        for (size_t i = 0; i < voices_.size(); ++i) {
            Voice& v = voices_[i];
            const uint8_t st = v.state.load(std::memory_order_acquire);
            if (st == static_cast<uint8_t>(VoiceState::Free)) {
                if (freeIdx < 0) freeIdx = static_cast<int>(i);
            } else if (st == static_cast<uint8_t>(VoiceState::Playing)) {
                ++playing;
                if (v.seq < oldestSeq) { oldestSeq = v.seq; oldestIdx = static_cast<int>(i); }
            }
        }

        if (playing < maxPolyphony_ && freeIdx >= 0) return freeIdx;
        if (oldestIdx < 0) return -1;      // nothing playing and nothing free
        releaseVoiceLocked(voices_[oldestIdx]);
    }
}

void SampleStreamEngine::flushPendingOff() {
    std::vector<uint16_t> drained;
    {
        std::lock_guard<std::mutex> gs(streamMutex_);
        drained.swap(pendingOff_);
        pendingOff_.reserve(kMaxVoices);
    }
    if (!onNoteOff_) return;
    for (uint16_t packed : drained) {
        onNoteOff_(static_cast<uint8_t>(packed >> 8), static_cast<uint8_t>(packed & 0xFF));
    }
}

void SampleStreamEngine::setNoteCallbacks(NoteCallback onOn, NoteCallback onOff) {
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    onNoteOn_  = onOn;
    onNoteOff_ = onOff;
}

// ── Playback ─────────────────────────────────────────────────────────────

void SampleStreamEngine::noteOn(uint8_t note, uint8_t velocity) {
    if (note >= kSampleSlotCount || !ready_.load(std::memory_order_acquire)) return;
    if (velocity == 0) { noteOff(note); return; }

    uint8_t firedNote = 0, firedWav = 0;
    bool fired = false;

    {
        std::lock_guard<std::mutex> gc(ctrlMutex_);
        Slot& slot = slots_[note];
        if (slot.layerCount == 0) return;

        std::lock_guard<std::mutex> gs(streamMutex_);
        reclaimStoppedLocked();

        // Choke group: a closed hat silences the open one. Group 0 means "no
        // group", which is why it is not treated as a group of its own.
        if (slot.group != 0) {
            for (auto& v : voices_) {
                if (v.state.load(std::memory_order_acquire) ==
                        static_cast<uint8_t>(VoiceState::Playing) &&
                    v.group == slot.group) {
                    v.state.store(static_cast<uint8_t>(VoiceState::Stopping),
                                  std::memory_order_release);
                }
            }
        }

        // Round-robin across the layers that are actually loaded.
        uint8_t layer = slot.nextLayer;
        for (uint8_t tries = 0; tries < slot.layerCount && !slot.layers[layer]; ++tries) {
            layer = static_cast<uint8_t>((layer + 1) % slot.layerCount);
        }
        if (!slot.layers[layer]) return;
        slot.nextLayer = static_cast<uint8_t>((layer + 1) % slot.layerCount);

        const int idx = allocVoiceLocked();
        if (idx < 0) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Voice& v = voices_[idx];
        v.src    = slot.layers[layer];
        v.note   = note;
        v.wavIdx = layer;
        v.group  = slot.group;

        const float vel = clamp01(velocity / 127.0f);
        const float vol = clamp01(slot.volume / 127.0f);
        // Equal power across the sweep, so a pad panned hard is as loud as the
        // same pad centred. 64 is dead centre by construction — dividing the
        // raw 0..127 by 127 puts it 0.4 % to the right, which is inaudible and
        // still enough to make a sample-exact comparison against the source
        // file fail.
        const float pos   = (static_cast<float>(slot.pan) - 64.0f) / 63.0f;   // -1..+1
        const float theta = (std::max(-1.0f, std::min(1.0f, pos)) + 1.0f) * 0.7853982f;
        v.gainL = vel * vol * std::cos(theta);
        v.gainR = vel * vol * std::sin(theta);

        v.loop    = slot.loop;
        const uint32_t len = v.src->length;
        v.loopEnd = (!slot.loopEnd || slot.loopEnd == kSampleEndMax)
                      ? len : std::min(slot.loopEnd, len);
        if (v.loopEnd == 0) v.loopEnd = len;

        v.played.store(0, std::memory_order_relaxed);
        v.fetched = 0;
        v.ring.reset();
        v.reader.close();
        v.underruns.store(0, std::memory_order_relaxed);
        v.seq = ++seqCounter_;

        v.state.store(static_cast<uint8_t>(VoiceState::Playing), std::memory_order_release);

        firedNote = note;
        firedWav  = layer;
        fired     = true;
    }

    flushPendingOff();
    if (fired && onNoteOn_) onNoteOn_(firedNote, firedWav);
}

void SampleStreamEngine::noteOff(uint8_t note) {
    if (note >= kSampleSlotCount) return;
    std::lock_guard<std::mutex> gc(ctrlMutex_);
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) !=
            static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.note != note) continue;
        // A one-shot ignores note-off — that is what makes a drum pad a drum
        // pad. Only a sustaining (looped) voice is gated by the key.
        if (!v.loop) continue;
        v.state.store(static_cast<uint8_t>(VoiceState::Stopping), std::memory_order_release);
    }
}

void SampleStreamEngine::allNotesOff() {
    {
        std::lock_guard<std::mutex> gc(ctrlMutex_);
        std::lock_guard<std::mutex> gs(streamMutex_);
        for (auto& v : voices_) releaseVoiceLocked(v);
    }
    flushPendingOff();
}

// ── Render ───────────────────────────────────────────────────────────────

void SampleStreamEngine::renderAdd(float* out, uint32_t frames) {
    if (!out || frames == 0 || !ready_.load(std::memory_order_acquire)) return;
    if (frames > maxFrames_) frames = maxFrames_;   // never write past the scratch

    const auto t0 = std::chrono::steady_clock::now();
    float peak = 0.0f;

    for (auto& v : voices_) {
        // Claim the voice before testing it — see releaseVoiceLocked().
        v.rendering.store(true, std::memory_order_seq_cst);
        if (v.state.load(std::memory_order_seq_cst) !=
            static_cast<uint8_t>(VoiceState::Playing)) {
            v.rendering.store(false, std::memory_order_seq_cst);
            continue;
        }

        const SampleSource* s = v.src.get();
        if (!s) {
            v.rendering.store(false, std::memory_order_seq_cst);
            continue;
        }

        const uint32_t region     = v.loop ? v.loopEnd : s->length;
        const uint32_t headUsable = std::min(s->headFrames, region);
        if (region == 0) {
            v.state.store(static_cast<uint8_t>(VoiceState::Stopping), std::memory_order_release);
            v.rendering.store(false, std::memory_order_seq_cst);
            continue;
        }

        uint64_t played    = v.played.load(std::memory_order_relaxed);
        uint32_t remaining = frames;
        float*   dst       = out;
        const float gL = v.gainL, gR = v.gainR;

        while (remaining > 0) {
            if (!v.loop && played >= s->length) {
                v.state.store(static_cast<uint8_t>(VoiceState::Stopping),
                              std::memory_order_release);
                break;
            }

            const int16_t* src16 = nullptr;
            uint32_t n = 0;

            if (played < headUsable) {
                // First pass through the attack: no I/O has to have happened
                // yet for this to sound.
                n = static_cast<uint32_t>(std::min<uint64_t>(remaining, headUsable - played));
                src16 = s->head.data() + played * 2;
            } else {
                n = v.ring.read(scratch_.data(), remaining);
                if (n == 0) {
                    // Stall, do not skip: the playhead stays put so the
                    // streamer resumes at the sample the listener last heard.
                    v.underruns.fetch_add(1, std::memory_order_relaxed);
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                src16 = scratch_.data();
            }
            if (!v.loop && played + n > s->length) {
                n = static_cast<uint32_t>(s->length - played);
                if (n == 0) break;
            }

            for (uint32_t i = 0; i < n; ++i) {
                const float l = src16[i * 2]     * (1.0f / 32768.0f) * gL;
                const float r = src16[i * 2 + 1] * (1.0f / 32768.0f) * gR;
                dst[i * 2]     += l;
                dst[i * 2 + 1] += r;
                const float a = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
                if (a > peak) peak = a;
            }

            played    += n;
            remaining -= n;
            dst       += static_cast<size_t>(n) * 2;
        }

        v.played.store(played, std::memory_order_relaxed);
        v.rendering.store(false, std::memory_order_seq_cst);
    }

    if (peak > peak_.load(std::memory_order_relaxed)) {
        peak_.store(peak, std::memory_order_relaxed);
    }

    const auto  dt      = std::chrono::steady_clock::now() - t0;
    const double us     = std::chrono::duration<double, std::micro>(dt).count();
    const double budget = (1e6 * frames) / static_cast<double>(sampleRate_);
    if (budget > 0.0) renderLoad_.store(static_cast<float>(us / budget), std::memory_order_relaxed);
}

void SampleStreamEngine::renderPlanar(int16_t* chL, int16_t* chR, uint32_t frames) {
    if (frames > maxFrames_) frames = maxFrames_;
    std::fill_n(scratchF_.data(), static_cast<size_t>(frames) * 2, 0.0f);
    renderAdd(scratchF_.data(), frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = std::max(-1.0f, std::min(1.0f, scratchF_[i * 2]));
        const float r = std::max(-1.0f, std::min(1.0f, scratchF_[i * 2 + 1]));
        if (chL) chL[i] = static_cast<int16_t>(l * 32767.0f);
        if (chR) chR[i] = static_cast<int16_t>(r * 32767.0f);
    }
}

// ── Streaming ────────────────────────────────────────────────────────────

void SampleStreamEngine::fillVoiceLocked(Voice& v) {
    SampleSource* s = v.src.get();
    if (!s) return;

    const uint32_t region     = v.loop ? v.loopEnd : s->length;
    const uint32_t headUsable = std::min(s->headFrames, region);
    if (region == 0) return;
    // Wholly resident and not looping: there is nothing to stream.
    if (!v.loop && region <= headUsable) return;

    if (!v.reader.isOpen()) {
        if (!v.reader.open(s->path.c_str())) {
            // The file went away between setup and the hit — retire the voice
            // rather than letting it stall forever on an empty ring.
            v.state.store(static_cast<uint8_t>(VoiceState::Stopping), std::memory_order_release);
            return;
        }
        v.fetched = headUsable;
        if (!v.reader.seekFrame(s->startFrame + (headUsable % region))) {
            v.state.store(static_cast<uint8_t>(VoiceState::Stopping), std::memory_order_release);
            return;
        }
    }

    int16_t tmp[kFillChunk * 2];
    while (v.ring.writable() >= kFillChunk) {
        if (!v.loop && v.fetched >= region) break;

        const uint32_t pos    = static_cast<uint32_t>(v.fetched % region);
        uint32_t       want   = std::min(kFillChunk, region - pos);
        if (!v.loop) {
            want = std::min<uint32_t>(want, static_cast<uint32_t>(region - v.fetched));
        }
        if (want == 0) break;

        const uint32_t got = v.reader.readStereo(tmp, want);
        if (got == 0) {
            if (!v.loop) break;
            if (!v.reader.seekFrame(s->startFrame)) break;
            continue;
        }
        v.ring.write(tmp, got);
        v.fetched += got;

        // Wrap exactly at the loop point, not at end of file: a loop end set
        // inside the sample must not read past it.
        if (v.loop && (v.fetched % region) == 0) {
            if (!v.reader.seekFrame(s->startFrame)) break;
        }
    }
}

void SampleStreamEngine::pump() {
    {
        std::lock_guard<std::mutex> gs(streamMutex_);
        for (auto& v : voices_) {
            const uint8_t st = v.state.load(std::memory_order_acquire);
            if (st == static_cast<uint8_t>(VoiceState::Stopping)) {
                releaseVoiceLocked(v);
            } else if (st == static_cast<uint8_t>(VoiceState::Playing)) {
                fillVoiceLocked(v);
            }
        }
    }
    flushPendingOff();
}

void SampleStreamEngine::streamLoop() {
    while (running_.load(std::memory_order_acquire)) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(kStreamTickMs));
    }
}

uint32_t SampleStreamEngine::activeVoices() const {
    uint32_t n = 0;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) ==
            static_cast<uint8_t>(VoiceState::Playing)) ++n;
    }
    return n;
}

// ── Singleton ────────────────────────────────────────────────────────────

SampleStreamEngine& getSampleStreamEngine() {
    static SampleStreamEngine engine;
    return engine;
}

} // namespace crosspad_pc
