// SPDX-License-Identifier: MIT
//
// The sample tools on this board: a take from the mics, the line-in or the
// pads to a WAV on the card, a region of a WAV played back for audition, and
// the file jobs behind the Recorder and the Slicer -- listing takes, drawing
// a file, saving a loop, writing a sliced loop out as a kit.
//
// Threads, as in the song engine:
//   audio_rt   meters the selected input and hands it to the recording ring,
//              and plays the preview ring out. Never blocks, never the card.
//   tools_io   this file's worker: drains a take to the card, keeps the
//              preview ring filled, and runs one file job at a time -- and
//              between a job's chunks it does the first two, so a long job
//              never starves a take or an audition.
//   LVGL/CDC   asks, and polls.
//
// A take with the song (startRecordingWithSong) is armed first, then the
// song engine is asked to play. The render marks where each block began in
// the take (sample_tools_rt_block_begin); after the mixer it asks the engine
// how much it has played, and the first block that is not nothing is the one
// the song started in -- that block's first input frame, moved on by the
// codecs' delay, is the take's frame 0. Until then the worker throws the
// lead-in away and keeps only the newest part of it in the ring.

#include "SampleToolsPort.hpp"
#include "AudioFileIo.hpp"   // PC: src/sampletools/AudioFileIo.hpp

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/sampletools/SliceMath.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

/* main/audio_route_control.cpp: hold a codec's ADC on its own input (the
 * mics, the jack) while the tools listen to it; the capture preset's choice
 * comes back when the last claim on it goes. */
/* PC: the host inputs are already their own streams -- nothing to claim. */
static bool sample_tools_adc_claim(uint8_t, bool) { return true; }

namespace crosspad {
namespace {

constexpr const char* TAG = "SampleTools";
/* PC: the card is a host folder (Settings -> SD card); set in sample_tools_init. */
std::string s_cardRoot, s_recDir, s_kitsFs;
const char* kCardRoot = "/sdcard";
const char* kRecDir   = "/sdcard/crosspad/recordings";
const char* kKitsFs   = "/sdcard/crosspad/kits";
constexpr const char* kKitsLogical = "/crosspad/kits";
constexpr uint32_t kRecRingFrames  = 65536;   // 256 kB, ~1.4 s of card stall
constexpr uint32_t kRecMinFrames   = 16384;
constexpr uint32_t kPrevRingFrames = 8192;    // ~170 ms ahead
constexpr uint32_t kPrevPrime      = 4096;
constexpr uint32_t kChunk          = 1024;    // 4 kB per card access
constexpr uint32_t kMaxTakeSeconds = 600;
constexpr uint32_t kEnvHop         = 256;     // ~5 ms: fine enough to place a cut by ear
constexpr uint32_t kFadeIn         = 32;      // a slice's edges, against clicks
constexpr uint32_t kFadeOut        = 256;
constexpr size_t   kMaxNameLen     = 32;
constexpr uint32_t kIdlePollMs     = 3;
/// Frames from the block the song goes out to the codecs in to the block the
/// ADC hands back what was heard with it: DMA out, the codecs' filters, DMA
/// in. Not measured yet, so 0: a take sits that much late, estimated 5-12 ms
/// from the 4 x 64-frame DMA each way (sample-tools.md says how to measure).
constexpr uint32_t kCodecDelayFrames = 0;
constexpr uint32_t kSyncKeep       = 16384;   // lead-in kept while the song primes, ~0.35 s
constexpr int64_t  kSongStartMs    = 3000;    // a song not playing by then is not coming

bool make_dir(const std::string& p)
{
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(p.c_str(), 0775) == 0;
}

bool exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

/// A kit name that is one folder FAT will take.
bool valid_name(const std::string& n)
{
    if (n.empty() || n.size() > kMaxNameLen || n == "." || n == ".." || n.back() == ' ' || n.back() == '.') return false;
    for (char c : n) if ((unsigned char)c < 32) return false;
    return n.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

/// The first free "<dir>/<stem>_NNN.wav".
std::string next_free(const std::string& dir, const char* stem)
{
    char buf[96];
    for (int n = 1; n < 1000; ++n) {
        std::snprintf(buf, sizeof(buf), "%s/%s_%03d.wav", dir.c_str(), stem, n);
        if (!exists(buf)) return buf;
    }
    return std::string();
}

std::string base_of(const std::string& path)
{
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

int codec_of(RecInput in) { return in == RecInput::Mic ? 0 : (in == RecInput::Line ? 1 : -1); }

/// Sixteen hues round the wheel, one per slice, so pad and slice match.
uint32_t slice_color(int i)
{
    const float h = (float)(i % 16) / 16.0f * 6.0f;
    const int   k = (int)h;
    const float f = h - (float)k;
    float r = 0, g = 0, b = 0;
    switch (k) {
        case 0: r = 1; g = f; break;
        case 1: r = 1 - f; g = 1; break;
        case 2: g = 1; b = f; break;
        case 3: g = 1 - f; b = 1; break;
        case 4: r = f; b = 1; break;
        default: r = 1; b = 1 - f; break;
    }
    return ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
}

enum class Prev : uint8_t { Idle, Priming, Running, Ended };
enum class JobKind : uint8_t { None, List, Folder, Overview, SaveRegion, Export };

bool is_wav(const char* n)
{
    const size_t len = std::strlen(n);
    return len > 4 && strcasecmp(n + len - 4, ".wav") == 0;
}

class SampleTools final : public ISampleTools, public IAudioNode {
public:
    // ── IAudioNode: the preview, played into the mixer (audio_rt) ─────────
    const char* name() const override { return "Preview"; }
    bool isGenerator() const override { return true; }
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override {
        sampleRate_ = sampleRate ? sampleRate : 44100;
        scratch_.assign(maxFrames * 2, 0);
        capture_.assign(maxFrames * 2, 0);
    }
    void onSampleRateChange(uint32_t sr) override { sampleRate_ = sr ? sr : 44100; }

    void mix(float* out, uint32_t frames) override {
        if (prev_.load(std::memory_order_acquire) != Prev::Running) return;
        if (frames * 2 > scratch_.size()) return;
        /* Silence when the card falls behind, and the position does not move
         * for it: what previewFrame() says is what was heard, which is what a
         * cut made by ear has to land on. */
        const uint32_t got = pRing_.pop(scratch_.data(), frames);
        const int16_t* s = scratch_.data();
        for (uint32_t i = 0; i < got * 2; ++i) out[i] += (float)s[i] * (1.0f / 32768.0f);
        const uint32_t done = pDone_.load(std::memory_order_relaxed) + got;
        pDone_.store(done, std::memory_order_relaxed);
        if (!pLoop_ && done >= pLen_) prev_.store(Prev::Ended, std::memory_order_release);
    }

    // ── RT taps ───────────────────────────────────────────────────────────
    bool wantsCodec(int codec) const {
        return listening_.load(std::memory_order_relaxed) &&
               codec_of(input_.load(std::memory_order_relaxed)) == codec;
    }
    void rtInput(int codec, const int16_t* block, uint32_t frames) {
        if (wantsCodec(codec)) take(block, frames);
    }
    void rtBlockBegin() { blockBegin_ = recFrames_.load(std::memory_order_relaxed); }

    void rtBus(const float* bus, uint32_t frames) {
        if (syncWait_.load(std::memory_order_acquire)) rtSync(frames);
        if (!listening_.load(std::memory_order_relaxed) ||
            input_.load(std::memory_order_relaxed) != RecInput::Pads) return;
        if (frames * 2 > capture_.size()) return;
        int16_t* d = capture_.data();
        for (uint32_t i = 0; i < frames * 2; ++i) {
            const float v = bus[i] * 32767.0f;
            d[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
        take(d, frames);
    }

    void start() {
        wake_ = xSemaphoreCreateBinary();
    }

    // ── Input and recording ───────────────────────────────────────────────
    bool selectInput(RecInput in) override {
        std::lock_guard<std::mutex> l(inputMutex_);
        if (recArmed_.load() || recStartReq_.load()) return false;   // not mid-take
        releaseAdc();
        input_.store(in);
        const int c = codec_of(in);
        if (c >= 0 && sample_tools_adc_claim((uint8_t)c, true)) claimed_ = c;
        listening_.store(true);
        return true;
    }
    RecInput input() const override { return input_.load(); }

    void peaks(float& left, float& right) override {
        left  = (float)pkL_.exchange(0) / 32768.0f;
        right = (float)pkR_.exchange(0) / 32768.0f;
    }

    bool startRecording() override { return requestTake(false, 0, 0); }

    bool startRecordingWithSong(uint32_t fromBar) override { return startRecordingWithSong(fromBar, 0); }
    bool startRecordingWithSong(uint32_t fromBar, uint8_t countInBeats) override {
        return getPlatformServices().songEngine && requestTake(true, fromBar, countInBeats);
    }

    bool requestTake(bool withSong, uint32_t fromBar, uint8_t countInBeats) {
        if (!listening_.load() || recArmed_.load() || recStartReq_.load() || !ensureWorker()) return false;
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            rec_ = RecStatus{};
            rec_.recording = true;
            rec_.sampleRate = sampleRate_;
            rec_.withSong = withSong;
            rec_.fromBar = fromBar;
        }
        recWithSong_ = withSong;      // read by the worker after recStartReq_
        recFromBar_ = fromBar;
        recCountIn_ = countInBeats;
        /* Waiting from the request on, not from when the worker arms it: the
         * screen read "playing" for a moment in between. */
        skip_.store(withSong ? UINT32_MAX : 0);
        recStopReq_.store(false);     // a stop from before this take is not for it
        recFrames_.store(0);
        recStartReq_.store(true);
        xSemaphoreGive(wake_);
        return true;
    }

    void stopRecording() override {
        recStopReq_.store(true);
        if (task_) xSemaphoreGive(wake_);
    }

    RecStatus recStatus() override {
        std::lock_guard<std::mutex> l(statusMutex_);
        RecStatus s = rec_;
        if (s.recording) {
            const uint32_t f = recFrames_.load(), skip = skip_.load();
            s.waiting = s.withSong && skip == UINT32_MAX;
            s.frames = !s.withSong ? f : (s.waiting || f < skip ? 0 : f - skip);
        }
        return s;
    }

    void releaseInput() override {
        stopRecording();
        stopPreview();
        std::lock_guard<std::mutex> l(inputMutex_);
        listening_.store(false);
        releaseAdc();
    }

    // ── Files ─────────────────────────────────────────────────────────────
    bool requestRecordings() override { return request(JobKind::List); }

    bool requestFolder(const std::string& dir) override {
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = dir;
        }
        return request(JobKind::Folder);
    }
    CardFolder folder() override {
        std::lock_guard<std::mutex> l(resultMutex_);
        return folder_;
    }
    std::string cardRoot() const override { return kCardRoot; }
    std::vector<std::string> recordings() override {
        std::lock_guard<std::mutex> l(resultMutex_);
        return recordings_;
    }

    bool requestOverview(const std::string& file, uint32_t start, uint32_t end, uint16_t columns) override {
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = file; pA_ = start; pB_ = end; pCols_ = std::max<uint16_t>(1, columns);
        }
        return request(JobKind::Overview);
    }
    WaveOverview overview() override {
        std::lock_guard<std::mutex> l(resultMutex_);
        return overview_;
    }

    bool requestSaveRegion(const std::string& file, uint32_t start, uint32_t end) override {
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = file; pA_ = start; pB_ = end;
        }
        return request(JobKind::SaveRegion);
    }

    bool requestExportKit(const SliceKit& kit) override {
        if (!valid_name(kit.name) || kit.starts.empty() || kit.starts.size() > SliceMath::kMaxSlices) return false;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pKit_ = kit;
        }
        return request(JobKind::Export);
    }

    ToolJob job() const override { return job_.load(); }
    std::string jobResult() override {
        std::lock_guard<std::mutex> l(resultMutex_);
        return result_;
    }
    uint8_t jobProgress() const override { return progress_.load(); }

    // ── Preview ───────────────────────────────────────────────────────────
    bool preview(const std::string& file, uint32_t start, uint32_t end, bool loop) override {
        if (file.empty() || !ensureWorker()) return false;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            prevFile_ = file; prevA_ = start; prevB_ = end; prevLoop_ = loop;
        }
        prevStopReq_.store(false);
        prevGen_.fetch_add(1);
        xSemaphoreGive(wake_);
        return true;
    }
    void stopPreview() override {
        prevDone_.store(prevGen_.load());          // a request not started yet is dropped
        if (prev_.load() == Prev::Idle) return;
        prevStopReq_.store(true);
        if (task_) xSemaphoreGive(wake_);
    }
    /// From the request on, not from the first sound: between the two the
    /// worker is still opening the file, and a caller that reads "not
    /// playing" there takes the audition for over (the Slicer's FREE did).
    bool previewing() const override {
        const Prev p = prev_.load();
        return p == Prev::Priming || p == Prev::Running || prevGen_.load() != prevDone_.load();
    }
    uint32_t previewFrame() const override {
        if (prev_.load() == Prev::Idle || !pLen_) return pStart_;
        const uint32_t done = pDone_.load(std::memory_order_relaxed);
        return pStart_ + (pLoop_ ? done % pLen_ : std::min(done, pLen_));
    }

    // ── Handoff ───────────────────────────────────────────────────────────
    void setHandoff(const std::string& file, uint32_t start, uint32_t end) override {
        std::lock_guard<std::mutex> l(paramMutex_);
        hFile_ = file; hA_ = start; hB_ = end;
    }
    bool takeHandoff(std::string& file, uint32_t& start, uint32_t& end) override {
        std::lock_guard<std::mutex> l(paramMutex_);
        if (hFile_.empty()) return false;
        file = hFile_; start = hA_; end = hB_;
        hFile_.clear();
        return true;
    }

private:
    /// RT, after the mixer: the song's first block, while a take waits for it.
    void rtSync(uint32_t frames) {
        ISongEngine* se = getPlatformServices().songEngine;
        const uint32_t played = se ? se->framesPlayed() : 0;
        if (!played) { syncSeenIdle_.store(true, std::memory_order_relaxed); return; }
        if (!syncSeenIdle_.load(std::memory_order_relaxed)) return;   // the playback from before this take
        /* It sounded first in this block, or `played - frames` earlier: its
         * first frame came in with that block's first input frame. */
        const uint32_t end = blockBegin_ + frames;
        syncFrame_.store(played < end ? end - played : 0, std::memory_order_relaxed);
        syncWait_.store(false, std::memory_order_release);
    }

    /// Meter, and into the take when one is being written.
    void take(const int16_t* b, uint32_t n) {
        int32_t pl = 0, pr = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const int32_t l = b[i * 2] < 0 ? -(int32_t)b[i * 2] : b[i * 2];
            const int32_t r = b[i * 2 + 1] < 0 ? -(int32_t)b[i * 2 + 1] : b[i * 2 + 1];
            if (l > pl) pl = l;
            if (r > pr) pr = r;
        }
        if (pl > pkL_.load(std::memory_order_relaxed)) pkL_.store(pl, std::memory_order_relaxed);
        if (pr > pkR_.load(std::memory_order_relaxed)) pkR_.store(pr, std::memory_order_relaxed);
        if (!recArmed_.load(std::memory_order_acquire)) return;
        if (rRing_.push(b, n) < n) recOverflow_.store(true, std::memory_order_relaxed);
        recFrames_.fetch_add(n, std::memory_order_relaxed);
    }

    void releaseAdc() {
        if (claimed_ >= 0) sample_tools_adc_claim((uint8_t)claimed_, false);
        claimed_ = -1;
    }

    bool request(JobKind kind) {
        if (!ensureWorker()) return false;
        ToolJob expected = job_.load();
        if (expected == ToolJob::Busy) return false;
        if (!job_.compare_exchange_strong(expected, ToolJob::Busy)) return false;
        progress_.store(0);
        jobKind_.store(kind);
        xSemaphoreGive(wake_);
        return true;
    }

    bool ensureWorker() {
        if (task_) return true;
        std::lock_guard<std::mutex> l(workerMutex_);
        if (task_) return true;
        if (!io_) io_ = static_cast<int16_t*>(heap_caps_malloc(kChunk * 4, MALLOC_CAP_SPIRAM));
        if (!io_) return false;
        /* Above LVGL (4), as the song engine's worker: a busy screen must not
         * starve a take's writes. It waits on the card nearly all the time. */
        return xTaskCreatePinnedToCoreWithCaps(worker_entry, "tools_io", 8192, this, 5, &task_, 0,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }

    static void worker_entry(void* arg) { static_cast<SampleTools*>(arg)->worker(); }

    void worker() {
        for (;;) {
            const bool busy = recArmed_.load() || prev_.load() != Prev::Idle;
            xSemaphoreTake(wake_, busy ? pdMS_TO_TICKS(kIdlePollMs) : portMAX_DELAY);
            service();
            const JobKind k = jobKind_.exchange(JobKind::None);
            if (k != JobKind::None) runJob(k);
        }
    }

    /// The realtime duties, between everything else.
    void service() {
        if (recStartReq_.exchange(false)) beginTake();
        if (recArmed_.load()) drainTake(false);
        if (prevStopReq_.exchange(false)) endPreview();
        const uint32_t gen = prevGen_.load();
        if (gen != prevDone_.load()) {
            beginPreview();                         // reads the newest request's parameters
            prevDone_.store(gen);
        }
        const Prev p = prev_.load();
        if (p == Prev::Ended) endPreview();
        else if (p != Prev::Idle) fillPreview();
    }

    // ── Take ──────────────────────────────────────────────────────────────
    void failTake(const char* why) {
        endSync();
        ESP_LOGW(TAG, "take failed: %s", why);
        std::lock_guard<std::mutex> l(statusMutex_);
        rec_.recording = false;
        rec_.error = why;
    }

    void beginTake() {
        if (!card_can_open() || !make_dir(std::string(kCardRoot) + "/crosspad") || !make_dir(kRecDir)) { failTake("no card"); return; }
        const std::string path = next_free(kRecDir, "rec");
        if (path.empty()) { failTake("no free name"); return; }
        if (!writer_.open(path.c_str(), sampleRate_, 2)) { failTake("card write failed"); return; }
        if (!rRing_.alloc(kRecRingFrames, kRecMinFrames)) {
            writer_.close();
            std::remove(path.c_str());
            failTake("not enough memory");
            return;
        }
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            rec_.file = path;
        }
        recFrames_.store(0);
        recOverflow_.store(false);
        syncTake_ = recWithSong_;
        dropped_ = 0;
        skip_.store(syncTake_ ? UINT32_MAX : 0);
        if (syncTake_) {
            syncSeenIdle_.store(false);
            syncWait_.store(true, std::memory_order_release);
            armedUs_ = esp_timer_get_time();
        }
        recArmed_.store(true, std::memory_order_release);
        /* Armed first: the take is listening before the song can sound. */
        ISongEngine* se = getPlatformServices().songEngine;
        /* Only a SYNC take starts the song (the device's port starts it for every take,
         * and a plain MIC take then leaves the song playing: endSync() only stops a synced one). */
        const bool played = syncTake_ && se && (recCountIn_ ? se->playCountedIn(recFromBar_, SyncTake::None, recCountIn_)
                                                            : se->play(recFromBar_));
        if (syncTake_ && !played) {
            recArmed_.store(false, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(10));    // RT may be inside take()
            rRing_.release();
            writer_.close();
            std::remove(path.c_str());
            failTake("the song cannot play: no blocks, or a bounce running");
            return;
        }
        ESP_LOGI(TAG, "recording %s%s", path.c_str(), syncTake_ ? " with the song" : "");
    }

    /// The song stops with a take that started it.
    void endSync() {
        if (!syncTake_) return;
        syncTake_ = false;
        syncWait_.store(false, std::memory_order_release);
        if (ISongEngine* se = getPlatformServices().songEngine) se->stop();
    }

    void drop(uint32_t n) {
        while (n) {
            const uint32_t got = rRing_.pop(io_, std::min(kChunk, n));
            if (!got) break;
            dropped_ += got;
            n -= got;
        }
    }

    /// A take with the song: what came in before its frame 0 is thrown away.
    /// Until the song sounds, the newest kSyncKeep frames stay in the ring --
    /// frame 0 is among them once it does. Returns why the take cannot go
    /// on, or nullptr.
    const char* leadIn(bool stopping) {
        uint32_t skip = skip_.load();
        if (skip == UINT32_MAX) {
            if (syncWait_.load(std::memory_order_acquire)) {
                if (stopping) return "stopped before the song started";
                ISongEngine* se = getPlatformServices().songEngine;
                /* A count-in is the song being on its way: timed from its end. */
                if (se && se->countInBeatsLeft()) armedUs_ = esp_timer_get_time();
                const int64_t waited = (esp_timer_get_time() - armedUs_) / 1000;
                if ((waited > kSongStartMs && !(se && se->playing())) || waited > 4 * kSongStartMs) {
                    return "the song did not start";
                }
                const uint32_t used = rRing_.used();
                drop(used > kSyncKeep ? used - kSyncKeep : 0);
                return nullptr;
            }
            const uint32_t delay = codec_of(input_.load()) >= 0 ? kCodecDelayFrames : 0;
            skip = syncFrame_.load() + delay;
            if (skip < dropped_) return "the song's first frame was lost";
            skip_.store(skip);
        }
        if (dropped_ < skip) drop(skip - dropped_);
        return nullptr;
    }

    /// Ring to card. @p finishing: the take is over, write what is left.
    void drainTake(bool finishing) {
        const uint32_t maxFrames = kMaxTakeSeconds * sampleRate_;
        bool stop = recStopReq_.exchange(false) || finishing || recFrames_.load() >= maxFrames;
        const bool overflow = recOverflow_.load();
        const char* syncError = syncTake_ ? leadIn(stop || overflow) : nullptr;
        if (stop || overflow || syncError) {
            recArmed_.store(false, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(10));    // RT may be inside take()
        }
        /* With the song, nothing is written before frame 0. */
        const bool beforeZero = syncTake_ && (skip_.load() == UINT32_MAX || dropped_ < skip_.load());
        for (;;) {
            if (syncError || beforeZero) break;
            const uint32_t n = rRing_.pop(io_, kChunk);
            if (!n) break;
            if (!writer_.write(io_, n)) { overflowWrite_ = true; break; }
            if (!stop && !overflow) break;     // one chunk a pass while the take runs
        }
        if (!stop && !overflow && !overflowWrite_ && !syncError) return;
        rRing_.release();
        const uint32_t frames = writer_.frames();
        const bool closed = writer_.close();
        std::string file;
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            file = rec_.file;
        }
        if (!syncError && syncTake_ && !frames) syncError = "stopped before the song started";
        if (syncError || overflow || overflowWrite_ || !closed) {
            overflowWrite_ = false;
            std::remove(file.c_str());
            failTake(syncError ? syncError : (overflow ? "card too slow, audio lost" : "card write failed"));
            return;
        }
        endSync();
        ESP_LOGI(TAG, "took %s (%u frames)", file.c_str(), (unsigned)frames);
        std::lock_guard<std::mutex> l(statusMutex_);
        rec_.recording = false;
        rec_.frames = frames;
    }

    // ── Preview ───────────────────────────────────────────────────────────
    void beginPreview() {
        endPreview();
        std::string file;
        uint32_t a, b;
        bool loop;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = prevFile_; a = prevA_; b = prevB_; loop = prevLoop_;
        }
        if (pReader_.path != file && !pReader_.open(file)) return;
        if (!b || b > pReader_.frames) b = pReader_.frames;
        if (a >= b) return;
        if (!pRing_.alloc(kPrevRingFrames, kPrevPrime)) return;
        pStart_ = a;
        pLen_ = b - a;
        pLoop_ = loop;
        pWrite_ = 0;
        pDone_.store(0);
        prev_.store(Prev::Priming, std::memory_order_release);
        fillPreview();
    }

    void fillPreview() {
        while (pRing_.space() >= kChunk) {
            if (!pLoop_ && pWrite_ >= pLen_) break;
            const uint32_t at = pWrite_ % pLen_;
            const uint32_t n = std::min(kChunk, pLen_ - at);
            pReader_.read(pStart_ + at, io_, n);
            pRing_.push(io_, n);
            pWrite_ += n;
        }
        if (prev_.load() == Prev::Priming &&
            (pRing_.used() >= kPrevPrime || (!pLoop_ && pWrite_ >= pLen_))) {
            prev_.store(Prev::Running, std::memory_order_release);
        }
    }

    void endPreview() {
        if (prev_.load() == Prev::Idle) return;
        prev_.store(Prev::Idle, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(10));        // RT may be inside mix()
        pRing_.release();
    }

    // ── Jobs ──────────────────────────────────────────────────────────────
    void finish(bool ok, const std::string& result) {
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            result_ = result;
        }
        progress_.store(100);
        job_.store(ok ? ToolJob::Ready : ToolJob::Failed);
    }

    void runJob(JobKind k) {
        switch (k) {
            case JobKind::List:       doList();       break;
            case JobKind::Folder:     doFolder();     break;
            case JobKind::Overview:   doOverview();   break;
            case JobKind::SaveRegion: doSaveRegion(); break;
            case JobKind::Export:     doExport();     break;
            default: finish(false, "nothing asked"); break;
        }
    }

    void doList() {
        std::vector<std::string> out;
        if (card_can_open()) {
            if (DIR* d = opendir(kRecDir)) {
                while (dirent* e = readdir(d)) {
                    const std::string n = e->d_name;
                    if (n.size() < 5 || n[0] == '.') continue;
                    std::string ext = n.substr(n.size() - 4);
                    for (auto& c : ext) c = (char)tolower((unsigned char)c);
                    if (ext == ".wav") out.push_back(std::string(kRecDir) + "/" + n);
                }
                closedir(d);
            }
        }
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a > b; });
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            recordings_ = std::move(out);
        }
        finish(true, "");
    }

    /// A folder on the card: its folders, then its WAVs, each by name, never
    /// above the card and never more than CardFolder::kMaxEntries. Names go
    /// into one block: small allocations come from internal RAM here.
    void doFolder() {
        std::string dir;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            dir = pFile_;
        }
        const size_t rootLen = std::strlen(kCardRoot);
        if (dir.empty()) dir = kRecDir;
        while (dir.size() > rootLen && dir.back() == '/') dir.pop_back();
        if (dir.compare(0, rootLen, kCardRoot) != 0 || (dir.size() > rootLen && dir[rootLen] != '/') ||
            dir.find("/..") != std::string::npos) {
            dir = kCardRoot;
        }
        if (!card_can_open()) { finish(false, "no card"); return; }
        DIR* d = opendir(dir.c_str());
        if (!d && dir == kRecDir) d = opendir((dir = kCardRoot).c_str());   // no take recorded yet
        if (!d) { finish(false, "cannot open " + dir); return; }
        std::string blob;
        std::vector<uint32_t> offs;
        std::vector<uint8_t> isDir;
        blob.reserve(4096);
        offs.reserve(256);
        isDir.reserve(256);
        bool truncated = false;
        uint32_t seen = 0;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            bool folder = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                struct stat st;
                folder = stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (!folder && !is_wav(e->d_name)) continue;
            if (offs.size() >= CardFolder::kMaxEntries) { truncated = true; break; }
            offs.push_back((uint32_t)blob.size());
            isDir.push_back(folder ? 1 : 0);
            blob.append(e->d_name);
            blob.push_back('\0');
            if (++seen % 32 == 0) { give_way(); service(); }
        }
        closedir(d);
        std::vector<uint32_t> order(offs.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            if (isDir[a] != isDir[b]) return isDir[a] > isDir[b];
            return strcasecmp(blob.c_str() + offs[a], blob.c_str() + offs[b]) < 0;
        });
        CardFolder f;
        f.path = dir;
        f.truncated = truncated;
        f.names.reserve(blob.size());
        f.offsets.reserve(order.size());
        for (uint32_t i : order) {
            f.offsets.push_back((uint32_t)f.names.size());
            f.names.append(blob.c_str() + offs[i]);
            f.names.push_back('\0');
            if (isDir[i]) ++f.folders;
        }
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            folder_ = std::move(f);
        }
        finish(true, dir);
    }

    void doOverview() {
        std::string file;
        uint32_t a, b;
        uint16_t cols;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = pFile_; a = pA_; b = pB_; cols = pCols_;
        }
        ClipReader r;
        if (!r.open(file)) { finish(false, "cannot read " + base_of(file)); return; }
        if (!b || b > r.frames) b = r.frames;
        if (a >= b) { r.close(); finish(false, "empty range"); return; }
        const uint32_t len = b - a;
        WaveOverview ov;
        ov.file = file;
        ov.frames = r.frames;
        ov.sampleRate = sampleRate_;
        ov.start = a;
        ov.end = b;
        ov.hop = kEnvHop;
        std::vector<int16_t> lo(cols, INT16_MAX), hi(cols, INT16_MIN);
        const size_t hops = (len + kEnvHop - 1) / kEnvHop;
        ov.envelope.assign(hops, 0);
        float acc = 0.0f;
        uint32_t accN = 0;
        size_t hopIdx = 0;
        for (uint32_t f = a; f < b;) {
            const uint32_t n = std::min(kChunk, b - f);
            r.read(f, io_, n);
            for (uint32_t k = 0; k < n; ++k) {
                const int32_t m = ((int32_t)io_[k * 2] + (int32_t)io_[k * 2 + 1]) / 2;
                const uint32_t rel = f + k - a;
                const uint32_t c = (uint32_t)((uint64_t)rel * cols / len);
                if (m < lo[c]) lo[c] = (int16_t)m;
                if (m > hi[c]) hi[c] = (int16_t)m;
                const float v = (float)m * (1.0f / 32768.0f);
                acc += v * v;
                if (++accN == kEnvHop) {
                    ov.envelope[hopIdx++] = SliceMath::envelopeStep(acc / (float)accN);
                    acc = 0.0f;
                    accN = 0;
                }
            }
            f += n;
            progress_.store((uint8_t)((uint64_t)(f - a) * 99 / len));
            give_way();
            service();
        }
        if (accN && hopIdx < hops) ov.envelope[hopIdx] = SliceMath::envelopeStep(acc / (float)accN);
        r.close();
        ov.lo.resize(cols);
        ov.hi.resize(cols);
        for (uint16_t c = 0; c < cols; ++c) {
            ov.lo[c] = lo[c] == INT16_MAX ? 0 : (int8_t)(lo[c] / 258);
            ov.hi[c] = hi[c] == INT16_MIN ? 0 : (int8_t)(hi[c] / 258);
        }
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            overview_ = std::move(ov);
        }
        finish(true, file);
    }

    /// Copy [a, b) of @p r into @p path; fades at the edges when asked.
    bool copyRegion(ClipReader& r, uint32_t a, uint32_t b, const std::string& path, bool fades,
                    uint32_t doneBefore, uint32_t total) {
        WavWriter w;
        if (!w.open(path.c_str(), sampleRate_, 2)) return false;
        const uint32_t len = b - a;
        for (uint32_t f = a; f < b;) {
            const uint32_t n = std::min(kChunk, b - f);
            r.read(f, io_, n);
            if (fades) {
                for (uint32_t k = 0; k < n; ++k) {
                    const uint32_t rel = f + k - a;
                    float g = 1.0f;
                    if (rel < kFadeIn) g = (float)rel / (float)kFadeIn;
                    if (len - rel <= kFadeOut) g = std::min(g, (float)(len - rel - 1) / (float)kFadeOut);
                    if (g < 1.0f) {
                        io_[k * 2]     = (int16_t)((float)io_[k * 2] * g);
                        io_[k * 2 + 1] = (int16_t)((float)io_[k * 2 + 1] * g);
                    }
                }
            }
            if (!w.write(io_, n)) { w.close(); return false; }
            f += n;
            if (total) progress_.store((uint8_t)((uint64_t)(doneBefore + f - a) * 99 / total));
            give_way();
            service();
        }
        return w.close();
    }

    void doSaveRegion() {
        std::string file;
        uint32_t a, b;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = pFile_; a = pA_; b = pB_;
        }
        ClipReader r;
        if (!r.open(file)) { finish(false, "cannot read " + base_of(file)); return; }
        if (!b || b > r.frames) b = r.frames;
        if (a >= b) { r.close(); finish(false, "empty range"); return; }
        /* Into the recordings, wherever the source is: a loop cut from a kit's
         * sample must not land in that kit's folder. */
        if (!make_dir(std::string(kCardRoot) + "/crosspad") || !make_dir(kRecDir)) { r.close(); finish(false, "no card"); return; }
        const std::string out = next_free(kRecDir, "loop");
        if (out.empty()) { r.close(); finish(false, "no free name"); return; }
        const bool ok = copyRegion(r, a, b, out, false, 0, b - a);
        r.close();
        if (!ok) { std::remove(out.c_str()); finish(false, "card write failed"); return; }
        finish(true, out);
    }

    void doExport() {
        SliceKit kit;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            kit = pKit_;
        }
        const std::string dir = std::string(kKitsFs) + "/" + kit.name;
        if (!card_can_open()) { finish(false, "no card"); return; }
        if (exists(dir)) { finish(false, "a kit called " + kit.name + " is already on the card"); return; }
        ClipReader r;
        if (!r.open(kit.source)) { finish(false, "cannot read " + base_of(kit.source)); return; }
        uint32_t end = kit.end && kit.end <= r.frames ? kit.end : r.frames;
        std::vector<uint32_t> starts = kit.starts;
        std::sort(starts.begin(), starts.end());
        starts.erase(std::remove_if(starts.begin(), starts.end(), [end](uint32_t s) { return s >= end; }), starts.end());
        if (starts.empty()) { r.close(); finish(false, "no slices"); return; }
        if (!make_dir(kKitsFs) || !make_dir(dir) || !make_dir(dir + "/SAMPLES")) {
            r.close();
            finish(false, "cannot make the kit's folder");
            return;
        }
        const uint32_t total = end - starts.front();
        uint32_t done = 0;
        for (size_t i = 0; i < starts.size(); ++i) {
            const uint32_t a = starts[i];
            const uint32_t b = i + 1 < starts.size() ? starts[i + 1] : end;
            char name[24];
            std::snprintf(name, sizeof(name), "/SAMPLES/%u.wav", (unsigned)(i + 1));
            if (b <= a || !copyRegion(r, a, b, dir + name, true, done, total)) {
                r.close();
                finish(false, "card write failed");
                return;
            }
            done += b - a;
        }
        r.close();

        KitInfo info;
        info.name = kit.name;
        info.author = "CrossPad Slicer";
        info.description = "Sliced from " + base_of(kit.source);
        info.version = "1.0";
        info.bpm = kit.bpm;
        info.path = std::string(kKitsLogical) + "/" + kit.name + "/kit.json";
        for (int p = 0; p < KIT_PADS; ++p) {
            Pad& pad = info.pads[p];
            pad.color = pad.color_pressed = slice_color(p);
            /* Chops of one loop cut each other off, as on an MPC: a hit
             * replaces the one before instead of piling up on it. */
            pad.choke_group = 1;
            if (p < (int)starts.size()) pad.AddSample(std::to_string(p + 1) + ".wav", false);
        }
        auto* loader = static_cast<PortableKitLoader*>(getKitManager());
        if (!loader || !loader->saveKit(info)) {                 // to info.path, under the mount
            finish(false, "kit.json not written");
            return;
        }
        const int id = loader->addKit(info.path);
        ESP_LOGI(TAG, "kit %s: %u slices, list id %d", kit.name.c_str(), (unsigned)starts.size(), id);
        finish(true, id >= 0 ? kit.name : kit.name + " (in the kit list after a restart)");
    }

    // Input and take.
    std::mutex            inputMutex_;
    std::atomic<RecInput> input_{RecInput::Mic};
    std::atomic<bool>     listening_{false};
    int                   claimed_ = -1;                      // codec whose ADC is held; under inputMutex_
    std::atomic<int32_t>  pkL_{0}, pkR_{0};
    std::atomic<bool>     recArmed_{false}, recStartReq_{false}, recStopReq_{false}, recOverflow_{false};
    std::atomic<uint32_t> recFrames_{0};
    bool                  overflowWrite_ = false;             // worker
    bool                  recWithSong_ = false;               // set before recStartReq_
    uint32_t              recFromBar_ = 0;
    uint8_t               recCountIn_ = 0;

    // A take with the song.
    std::atomic<bool>     syncWait_{false};                   // armed, the song not heard yet
    std::atomic<bool>     syncSeenIdle_{false};               // RT saw the song silent since arming
    std::atomic<uint32_t> syncFrame_{0};                      // take frame its first frame came in with
    std::atomic<uint32_t> skip_{0};                           // frames before frame 0; UINT32_MAX = not known
    uint32_t              blockBegin_ = 0;                    // RT: take frames when this block began
    bool                  syncTake_ = false;                  // worker
    uint32_t              dropped_ = 0;                       // worker: lead-in thrown away
    int64_t               armedUs_ = 0;                       // worker
    FrameRing             rRing_;
    WavWriter             writer_;                            // worker
    RecStatus             rec_;
    std::mutex            statusMutex_;
    std::vector<int16_t>  capture_;                           // RT: the pads bus as int16

    // Preview.
    std::atomic<Prev>     prev_{Prev::Idle};
    std::atomic<bool>     prevStopReq_{false};
    std::atomic<uint32_t> prevGen_{0}, prevDone_{0};  // requests made, and handled
    std::string           prevFile_;                          // under paramMutex_
    uint32_t              prevA_ = 0, prevB_ = 0;
    bool                  prevLoop_ = false;
    FrameRing             pRing_;
    ClipReader            pReader_;                           // worker
    uint32_t              pStart_ = 0, pLen_ = 0, pWrite_ = 0;
    bool                  pLoop_ = false;
    std::atomic<uint32_t> pDone_{0};
    std::vector<int16_t>  scratch_;

    // Jobs.
    std::atomic<ToolJob>  job_{ToolJob::Idle};
    std::atomic<JobKind>  jobKind_{JobKind::None};
    std::atomic<uint8_t>  progress_{0};
    std::mutex            paramMutex_, resultMutex_;
    std::string           pFile_;
    uint32_t              pA_ = 0, pB_ = 0;
    uint16_t              pCols_ = 1;
    SliceKit              pKit_;
    std::string           hFile_;
    uint32_t              hA_ = 0, hB_ = 0;
    std::string           result_;
    std::vector<std::string> recordings_;
    CardFolder            folder_;
    WaveOverview          overview_;

    uint32_t              sampleRate_ = 44100;
    int16_t*              io_ = nullptr;
    std::mutex            workerMutex_;
    SemaphoreHandle_t     wake_ = nullptr;
    TaskHandle_t          task_ = nullptr;
};

SampleTools* s_tools = nullptr;

} // namespace

void sample_tools_init(AudioMixerEngine& mixer)
{
    s_cardRoot = pc_platform_get_sdcard_path();
    while (s_cardRoot.size() > 1 && s_cardRoot.back() == '/') s_cardRoot.pop_back();
    s_recDir = s_cardRoot + "/crosspad/recordings";
    s_kitsFs = s_cardRoot + "/crosspad/kits";
    kCardRoot = s_cardRoot.c_str(); kRecDir = s_recDir.c_str(); kKitsFs = s_kitsFs.c_str();
    static SampleTools tools;
    s_tools = &tools;
    tools.start();
    const auto ch = mixer.addChannel(&tools, "Preview");
    if (ch != kMixerInvalidChannelId) {
        /* Speakers and phones, not the pads-only bus: a resample of the pads
         * must not record the audition playing beside it. */
        for (uint8_t o = 0; o < 2 /* OUT1, OUT2 */; ++o) mixer.setRouteEnabled(ch, o, true);
    }
    getPlatformServices().sampleTools = &tools;
}

void sample_tools_rt_input(int codec, const int16_t* block, uint32_t frames)
{
    if (s_tools && block) s_tools->rtInput(codec, block, frames);
}

void sample_tools_rt_block_begin()
{
    if (s_tools) s_tools->rtBlockBegin();
}

void sample_tools_rt_bus(const float* padsBus, uint32_t frames)
{
    if (s_tools) s_tools->rtBus(padsBus, frames);
}

bool sample_tools_wants_codec(int codec)
{
    return s_tools && s_tools->wantsCodec(codec);
}

} // namespace crosspad
