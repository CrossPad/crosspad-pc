// Bench for the PC sample engine — the same shape as a crosspad-hil scenario
// run against a board: play something real, capture what came out, count what
// the engine says happened, and let the capture be judged separately.
//
//   ./sampler_hil <scenario> [args]
//
// Exit 0 = pass, 1 = the engine failed, 2 = the bench is wrong (missing file).

#include "SampleStreamPlayer.hpp"
#include "SampleStreamEngine.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using clockt = std::chrono::steady_clock;
static std::string g_sd;
static std::string g_out;
static int g_fail = 0;

static std::string kitPath(const char* name) {
    return std::string("/crosspad/kits/TEST/") + name;
}

static void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) g_fail = 1;
}

// ── WAV capture ──────────────────────────────────────────────────────────

class WavWriter {
public:
    bool open(const std::string& path, uint32_t sr) {
        f_ = fopen(path.c_str(), "wb");
        if (!f_) return false;
        sr_ = sr;
        unsigned char hdr[44] = {0};
        fwrite(hdr, 1, 44, f_);
        return true;
    }
    void write(const float* interleaved, uint32_t frames) {
        buf_.resize(static_cast<size_t>(frames) * 2);
        for (uint32_t i = 0; i < frames * 2; ++i) {
            float v = interleaved[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            buf_[i] = static_cast<int16_t>(v * 32767.0f);
        }
        fwrite(buf_.data(), sizeof(int16_t), buf_.size(), f_);
        frames_ += frames;
    }
    void close() {
        if (!f_) return;
        const uint32_t dataBytes = frames_ * 4;
        auto u32 = [&](long off, uint32_t v) { fseek(f_, off, SEEK_SET); fwrite(&v, 4, 1, f_); };
        auto u16 = [&](long off, uint16_t v) { fseek(f_, off, SEEK_SET); fwrite(&v, 2, 1, f_); };
        fseek(f_, 0, SEEK_SET); fwrite("RIFF", 1, 4, f_);
        u32(4, 36 + dataBytes);
        fseek(f_, 8, SEEK_SET);  fwrite("WAVEfmt ", 1, 8, f_);
        u32(16, 16); u16(20, 1); u16(22, 2); u32(24, sr_);
        u32(28, sr_ * 4); u16(32, 4); u16(34, 16);
        fseek(f_, 36, SEEK_SET); fwrite("data", 1, 4, f_);
        u32(40, dataBytes);
        fclose(f_); f_ = nullptr;
    }
    uint32_t frames() const { return frames_; }
private:
    FILE* f_ = nullptr;
    uint32_t sr_ = 48000, frames_ = 0;
    std::vector<int16_t> buf_;
};

// ── Real-time render thread ──────────────────────────────────────────────
//
// Paced to the wall clock exactly like an audio callback, so a streamer that
// cannot keep up shows as an underrun here the way it would as a dropout on
// the board. Anything measured off a free-running render loop measures the
// disk, not the engine.

class RtRender {
public:
    RtRender(uint32_t sr, uint32_t block) : sr_(sr), block_(block) {}

    bool start(const std::string& wavPath) {
        if (!wavPath.empty() && !wav_.open(wavPath, sr_)) return false;
        capture_ = !wavPath.empty();
        run_.store(true);
        t_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_.store(false);
        if (t_.joinable()) t_.join();
        if (capture_) wav_.close();
    }
    uint64_t frames() const { return frames_.load(); }
    uint32_t lateBlocks() const { return late_.load(); }
    double   worstBlockUs() const { return worstUs_.load(); }

private:
    void loop() {
        std::vector<float> buf(static_cast<size_t>(block_) * 2, 0.0f);
        auto next = clockt::now();
        const auto period = std::chrono::nanoseconds(
            static_cast<long long>(1e9 * block_ / sr_));
        while (run_.load()) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            const auto t0 = clockt::now();
            SampleStreamPly_ProcessAdd(buf.data(), block_);
            const double us = std::chrono::duration<double, std::micro>(clockt::now() - t0).count();
            if (us > worstUs_.load()) worstUs_.store(us);
            if (capture_) wav_.write(buf.data(), block_);
            frames_.fetch_add(block_);

            next += period;
            const auto now = clockt::now();
            if (now < next) std::this_thread::sleep_until(next);
            else { late_.fetch_add(1); next = now; }
        }
    }

    uint32_t sr_, block_;
    std::thread t_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint32_t> late_{0};
    std::atomic<double> worstUs_{0.0};
    WavWriter wav_;
    bool capture_ = false;
};

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ── Scenarios ────────────────────────────────────────────────────────────

static int sc_smoke() {
    std::printf("[smoke] init, one pad, one hit\n");
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_Prepare(1024, 48000);

    sample_info_from_file info{};
    check(SampleStreamPlayer_GetInfo(&info, kitPath("pad00.wav").c_str()), "GetInfo");
    check(info.sampleRate == 48000 && info.numberOfChannels == 2 && info.bitsPerSample == 16,
          "header parsed",
          std::to_string(info.sampleRate) + " Hz, " + std::to_string(info.numberOfChannels) +
          " ch, " + std::to_string(info.sampleCount) + " frames");

    check(SampleStreamPlayer_SetupSample(0, kitPath("pad00.wav").c_str(), 0, 0, 0, 0),
          "SetupSample");
    const uint8_t freeBefore = SampleStreamPlayer_GetFreeWavCnt();

    std::vector<float> buf(512 * 2, 0.0f);
    SampleStreamPly_ProcessAdd(buf.data(), 512);
    double idle = 0; for (float v : buf) idle += std::fabs(v);
    check(idle == 0.0, "silent before the hit");

    SampleStreamPlayer_NoteOn(0, 127);
    check(SampleStreamPly_GetActiveVoices() == 1, "one voice active");

    std::fill(buf.begin(), buf.end(), 0.0f);
    SampleStreamPly_ProcessAdd(buf.data(), 512);
    double peak = 0; for (float v : buf) peak = std::max<double>(peak, std::fabs(v));
    check(peak > 0.05, "first block after note-on is audible",
          "peak " + std::to_string(peak));

    check(freeBefore < SampleStreamPlayer_GetFreeWavCnt() + 1, "layer accounted");
    SampleStreamPlayer_DeInit();
    return g_fail;
}

// Streaming is only correct if what comes out is bit-for-bit what went in.
// Rendered deterministically (the engine's streamer driven by hand) so a slow
// disk cannot be mistaken for a broken ring.
static int sc_stream() {
    std::printf("[stream] 10 s sample, sample-exact readback past the head cache\n");
    auto& eng = crosspad_pc::getSampleStreamEngine();
    check(eng.init(false), "engine init (manual pump)");
    eng.setRootPrefix(g_sd);
    eng.prepare(256, 48000);

    check(eng.setupSample(0, kitPath("sine997_10s.wav").c_str(), 0, 0, 0, 0), "setup");
    eng.setVolume(0, 127);
    eng.setPan(0, 64);
    eng.noteOn(0, 127);

    // Reference: the file itself.
    crosspad_pc::WavReader ref;
    if (!ref.open((g_sd + kitPath("sine997_10s.wav")).c_str())) {
        std::printf("  bench error: reference unreadable\n");
        return 2;
    }
    const uint32_t total = ref.info().frameCount;

    const uint32_t block = 256;
    std::vector<float> out(block * 2, 0.0f);
    std::vector<int16_t> want(block * 2, 0);
    // Centre pan is equal power, so a full-scale sample comes back at 1/sqrt2.
    const float g = std::cos(0.7853982f);

    uint64_t compared = 0, mismatches = 0;
    double worstErr = 0.0;
    while (compared < total) {
        for (int i = 0; i < 8; ++i) eng.pump();      // let the streamer get ahead
        std::fill(out.begin(), out.end(), 0.0f);
        eng.renderAdd(out.data(), block);
        const uint32_t n = ref.readStereo(want.data(), block);
        for (uint32_t i = 0; i < n; ++i) {
            const double expL = want[i * 2] * (1.0 / 32768.0) * g;
            const double err  = std::fabs(out[i * 2] - expL);
            if (err > worstErr) worstErr = err;
            if (err > 1e-4) ++mismatches;
        }
        compared += n;
        if (n < block) break;
    }

    check(compared == total, "whole sample rendered",
          std::to_string(compared) + " of " + std::to_string(total) + " frames");
    check(mismatches == 0, "no dropped or repeated samples",
          std::to_string(mismatches) + " mismatching frames, worst |err| " +
          std::to_string(worstErr));
    check(eng.underrunCount() == 0, "no underruns with the streamer kept ahead",
          std::to_string(eng.underrunCount()));

    // The voice ends on the render call *after* its last frame — that call is
    // where the position check runs — so give it one, then let the streamer
    // reclaim it.
    std::fill(out.begin(), out.end(), 0.0f);
    eng.renderAdd(out.data(), block);
    eng.pump();
    check(eng.activeVoices() == 0, "voice retired at end of sample");
    eng.deinit();
    return g_fail;
}

// Capture a looped tone in real time and hand the WAV to `analyze sine`,
// which is what tells a stream that drops one sample every few seconds apart
// from one that is clean — the spectrum cannot.
static int sc_sine_rt(double seconds) {
    std::printf("[sine_rt] %.0f s of a looped 997 Hz tone through the RT path\n", seconds);
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    check(SampleStreamPlayer_SetupSample(0, kitPath("sine997_10s.wav").c_str(), 0, 0, 0, 0),
          "setup");
    SampleStreamPlayer_SetVolume(0, 127);
    SampleStreamPlayer_SetLoopEnd(0, SAMPLE_STREAM_PLY_END_MAX);

    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/sine_rt.wav")) { std::printf("  bench error: cannot write\n"); return 2; }
    sleepMs(200);
    SampleStreamPlayer_NoteOn(0, 127);
    sleepMs(static_cast<int>(seconds * 1000));
    rt.stop();

    std::printf("  frames=%llu late_blocks=%u worst_render=%.0f us underruns=%u\n",
                (unsigned long long)rt.frames(), rt.lateBlocks(), rt.worstBlockUs(),
                SampleStreamPly_GetUnderrunCount());
    check(SampleStreamPly_GetUnderrunCount() == 0, "no stream underruns in real time",
          std::to_string(SampleStreamPly_GetUnderrunCount()));
    check(rt.worstBlockUs() < 5333.0, "worst render call inside the block budget",
          std::to_string(rt.worstBlockUs()) + " us vs 5333 us");
    SampleStreamPlayer_DeInit();
    std::printf("  wrote %s/sine_rt.wav\n", g_out.c_str());
    return g_fail;
}

// Scheduled hits, captured, so `analyze onset` can say whether every one
// sounded and how much the trigger jittered.
static int sc_onset(int hits, double gap) {
    std::printf("[onset] %d hits every %.0f ms\n", hits, gap * 1000);
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    for (uint8_t i = 0; i < 16; ++i) {
        char name[32]; std::snprintf(name, sizeof(name), "pad%02d.wav", i);
        SampleStreamPlayer_SetupSample(i, kitPath(name).c_str(), 0, 0, 0, 0);
        SampleStreamPlayer_SetVolume(i, 127);
    }
    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/onset.wav")) return 2;

    FILE* sched = fopen((g_out + "/onset_schedule.txt").c_str(), "w");
    const auto t0 = clockt::now() + std::chrono::milliseconds(500);
    std::this_thread::sleep_until(t0);
    for (int i = 0; i < hits; ++i) {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(
            static_cast<long long>(i * gap * 1e6)));
        SampleStreamPlayer_NoteOn(static_cast<uint8_t>(i % 16), 127);
        std::fprintf(sched, "%.4f 127\n", 0.5 + i * gap);
    }
    fclose(sched);
    sleepMs(600);
    rt.stop();
    check(SampleStreamPly_GetDroppedNoteCount() == 0, "no note dropped by the engine",
          std::to_string(SampleStreamPly_GetDroppedNoteCount()));
    SampleStreamPlayer_DeInit();
    std::printf("  wrote %s/onset.wav\n", g_out.c_str());
    return g_fail;
}

static int sc_velocity() {
    std::printf("[velocity] one pad, velocity ramp\n");
    const int vels[] = {16, 32, 48, 64, 80, 96, 112, 127};
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    check(SampleStreamPlayer_SetupSample(0, kitPath("pad04.wav").c_str(), 0, 0, 0, 0), "setup");
    SampleStreamPlayer_SetVolume(0, 127);

    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/velocity.wav")) return 2;
    FILE* sched = fopen((g_out + "/velocity_schedule.txt").c_str(), "w");
    const auto t0 = clockt::now() + std::chrono::milliseconds(500);
    std::this_thread::sleep_until(t0);
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(i * 500));
        SampleStreamPlayer_NoteOn(0, static_cast<uint8_t>(vels[i]));
        std::fprintf(sched, "%.4f %d\n", 0.5 + i * 0.5, vels[i]);
    }
    fclose(sched);
    sleepMs(700);
    rt.stop();
    SampleStreamPlayer_DeInit();
    std::printf("  wrote %s/velocity.wav\n", g_out.c_str());
    return g_fail;
}

static int sc_poly() {
    std::printf("[poly] all 16 pads at once, then past the polyphony cap\n");
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(512, 48000);
    for (uint8_t i = 0; i < 16; ++i) {
        char name[32]; std::snprintf(name, sizeof(name), "pad%02d.wav", i);
        SampleStreamPlayer_SetupSample(i, kitPath(name).c_str(), 0, 0, 0, 0);
        SampleStreamPlayer_SetVolume(i, 100);
    }
    SampleStreamPlayer_SetMaxPolyphony(16);
    for (uint8_t i = 0; i < 16; ++i) SampleStreamPlayer_NoteOn(i, 127);
    check(SampleStreamPly_GetActiveVoices() == 16, "16 voices sounding",
          std::to_string(SampleStreamPly_GetActiveVoices()));

    std::vector<float> buf(512 * 2, 0.0f);
    SampleStreamPly_ProcessAdd(buf.data(), 512);
    double peak = 0; for (float v : buf) peak = std::max<double>(peak, std::fabs(v));
    check(peak > 0.5, "16 voices sum to something loud", "peak " + std::to_string(peak));

    // 17th hit: the cap must steal, not drop, and never exceed itself.
    for (uint8_t i = 0; i < 16; ++i) SampleStreamPlayer_NoteOn(i, 127);
    check(SampleStreamPly_GetActiveVoices() <= 16, "polyphony cap respected",
          std::to_string(SampleStreamPly_GetActiveVoices()));
    check(SampleStreamPly_GetDroppedNoteCount() == 0, "steal rather than drop",
          std::to_string(SampleStreamPly_GetDroppedNoteCount()));

    SampleStreamPlayer_SetMaxPolyphony(4);
    for (uint8_t i = 0; i < 16; ++i) SampleStreamPlayer_NoteOn(i, 127);
    check(SampleStreamPly_GetActiveVoices() <= 4, "lowered cap takes effect",
          std::to_string(SampleStreamPly_GetActiveVoices()));
    SampleStreamPlayer_DeInit();
    return g_fail;
}

static int sc_choke() {
    std::printf("[choke] two pads in one choke group\n");
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    SampleStreamPlayer_SetupSample(0, kitPath("pad00.wav").c_str(), /*group=*/3, 0, 0, 0);
    SampleStreamPlayer_SetupSample(1, kitPath("pad08.wav").c_str(), /*group=*/3, 0, 0, 0);
    SampleStreamPlayer_SetupSample(2, kitPath("pad12.wav").c_str(), /*group=*/0, 0, 0, 0);

    SampleStreamPlayer_NoteOn(0, 127);
    SampleStreamPlayer_NoteOn(2, 127);
    check(SampleStreamPly_GetActiveVoices() == 2, "two voices before the choke");
    SampleStreamPlayer_NoteOn(1, 127);
    sleepMs(40);   // the streamer retires the choked voice
    check(SampleStreamPly_GetActiveVoices() == 2,
          "choke silenced the group sibling, left the ungrouped pad",
          std::to_string(SampleStreamPly_GetActiveVoices()));
    SampleStreamPlayer_DeInit();
    return g_fail;
}

static int sc_loop() {
    std::printf("[loop] a 1 s sample looped for 4 s, then gated off\n");
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    check(SampleStreamPlayer_SetupSample(0, kitPath("loop_1s.wav").c_str(), 0, 0, 0, 0), "setup");
    SampleStreamPlayer_SetVolume(0, 127);
    SampleStreamPlayer_SetLoopEnd(0, SAMPLE_STREAM_PLY_END_MAX);

    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/loop.wav")) return 2;
    sleepMs(200);
    SampleStreamPlayer_NoteOn(0, 127);
    sleepMs(4000);
    check(SampleStreamPly_GetActiveVoices() == 1, "still sounding after 4x its length",
          std::to_string(SampleStreamPly_GetActiveVoices()));
    check(SampleStreamPly_GetUnderrunCount() == 0, "loop wrap did not starve the ring",
          std::to_string(SampleStreamPly_GetUnderrunCount()));
    SampleStreamPlayer_NoteOff(0);
    sleepMs(200);
    check(SampleStreamPly_GetActiveVoices() == 0, "note-off gates a looped voice",
          std::to_string(SampleStreamPly_GetActiveVoices()));

    // A one-shot must ignore note-off — that is what makes a drum pad a drum pad.
    SampleStreamPlayer_SetLoopClear(0);
    SampleStreamPlayer_NoteOn(0, 127);
    SampleStreamPlayer_NoteOff(0);
    check(SampleStreamPly_GetActiveVoices() == 1, "one-shot ignores note-off");
    rt.stop();
    SampleStreamPlayer_DeInit();
    return g_fail;
}

// The one scenario that plays and restructures at the same time — a kit swap
// that only works from silence is not the thing being tested.
static int sc_churn(int rounds, double hitsPerSec) {
    std::printf("[churn] %d kit swaps with pads firing at %.0f/s\n", rounds, hitsPerSec);
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    SampleStreamPlayer_SetMaxPolyphony(16);

    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/churn.wav")) return 2;

    std::atomic<bool> run{true};
    std::atomic<uint64_t> hits{0}, hitsInSwap{0};
    std::atomic<bool> swapping{false};

    std::thread stim([&] {
        std::mt19937 rng(1234);
        std::uniform_int_distribution<int> pad(0, 15);
        const auto period = std::chrono::microseconds(
            static_cast<long long>(1e6 / hitsPerSec));
        auto next = clockt::now();
        while (run.load()) {
            SampleStreamPlayer_NoteOn(static_cast<uint8_t>(pad(rng)), 100);
            hits.fetch_add(1);
            if (swapping.load()) hitsInSwap.fetch_add(1);
            next += period;
            std::this_thread::sleep_until(next);
        }
    });

    double swapMsTotal = 0.0, swapMsWorst = 0.0;
    for (int r = 0; r < rounds; ++r) {
        const auto s0 = clockt::now();
        swapping.store(true);
        for (uint8_t i = 0; i < 16; ++i) {
            char name[32];
            // Four of the sixteen are the 10 s tone, so a swap reads a real
            // attack cache off disk rather than a file that fits in one page —
            // otherwise the swap window is too short for a hit to land in it.
            if (i < 4) std::snprintf(name, sizeof(name), "sine997_10s.wav");
            else       std::snprintf(name, sizeof(name), "pad%02d.wav", (i + r) % 16);
            SampleStreamPlayer_WipeOut(i);
            if (!SampleStreamPlayer_SetupSample(i, kitPath(name).c_str(), 0, 0, 0, 0)) {
                check(false, "SetupSample during churn", name);
            }
            SampleStreamPlayer_SetVolume(i, 100);
        }
        swapping.store(false);
        const double ms = std::chrono::duration<double, std::milli>(clockt::now() - s0).count();
        swapMsTotal += ms;
        if (ms > swapMsWorst) swapMsWorst = ms;
        sleepMs(60);
    }
    std::printf("  swap_total=%.0f ms swap_worst=%.1f ms swap_avg=%.1f ms\n",
                swapMsTotal, swapMsWorst, swapMsTotal / rounds);
    run.store(false);
    stim.join();
    sleepMs(300);
    rt.stop();

    std::printf("  hits=%llu inside_swap=%llu dropped=%u underruns=%u worst_render=%.0f us "
                "late_blocks=%u\n",
                (unsigned long long)hits.load(), (unsigned long long)hitsInSwap.load(),
                SampleStreamPly_GetDroppedNoteCount(), SampleStreamPly_GetUnderrunCount(),
                rt.worstBlockUs(), rt.lateBlocks());
    // A green run with no stimulus inside the swap window is a false negative.
    check(hitsInSwap.load() > 0, "pads actually fired inside the swap windows",
          std::to_string(hitsInSwap.load()));
    check(rt.worstBlockUs() < 5333.0, "render stayed inside the block budget through swaps",
          std::to_string(rt.worstBlockUs()) + " us");
    SampleStreamPlayer_DeInit();
    return g_fail;
}

static int sc_storm(double hitsPerSec, double seconds) {
    std::printf("[storm] %.0f hits/s for %.0f s across 16 pads\n", hitsPerSec, seconds);
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    SampleStreamPlayer_SetMaxPolyphony(16);
    for (uint8_t i = 0; i < 16; ++i) {
        char name[32]; std::snprintf(name, sizeof(name), "pad%02d.wav", i);
        if (!SampleStreamPlayer_SetupSample(i, kitPath(name).c_str(), 0, 0, 0, 0)) return 2;
        SampleStreamPlayer_SetVolume(i, 90);
    }
    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/storm.wav")) return 2;

    std::mt19937 rng(99);
    std::uniform_int_distribution<int> pad(0, 15);
    std::uniform_int_distribution<int> vel(40, 127);
    const auto period = std::chrono::microseconds(static_cast<long long>(1e6 / hitsPerSec));
    const auto end = clockt::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    auto next = clockt::now();
    uint64_t sent = 0;
    double worstNoteUs = 0.0;
    while (clockt::now() < end) {
        const auto t0 = clockt::now();
        SampleStreamPlayer_NoteOn(static_cast<uint8_t>(pad(rng)), static_cast<uint8_t>(vel(rng)));
        const double us = std::chrono::duration<double, std::micro>(clockt::now() - t0).count();
        if (us > worstNoteUs) worstNoteUs = us;
        ++sent;
        next += period;
        std::this_thread::sleep_until(next);
    }
    sleepMs(400);
    rt.stop();

    std::printf("  sent=%llu dropped=%u underruns=%u peak=%.3f worst_render=%.0f us "
                "worst_noteon=%.0f us late_blocks=%u\n",
                (unsigned long long)sent, SampleStreamPly_GetDroppedNoteCount(),
                SampleStreamPly_GetUnderrunCount(), SampleStreamPly_GetPeak(),
                rt.worstBlockUs(), worstNoteUs, rt.lateBlocks());
    check(SampleStreamPly_GetDroppedNoteCount() == 0, "no hit refused by the engine",
          std::to_string(SampleStreamPly_GetDroppedNoteCount()));
    check(rt.worstBlockUs() < 5333.0, "render never overran the block budget",
          std::to_string(rt.worstBlockUs()) + " us vs 5333 us");
    // note-on runs on the caller's thread; if it blocks, a player feels it.
    check(worstNoteUs < 2000.0, "note-on never blocked the caller",
          std::to_string(worstNoteUs) + " us");
    SampleStreamPlayer_DeInit();
    return g_fail;
}

static int sc_silence() {
    std::printf("[silence] 3 s of an idle engine\n");
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);
    SampleStreamPlayer_SetupSample(0, kitPath("pad00.wav").c_str(), 0, 0, 0, 0);
    RtRender rt(48000, 256);
    if (!rt.start(g_out + "/silence.wav")) return 2;
    // One hit, then let it finish: the tail must reach true zero and stay
    // there. On the board this is where a finished voice parks on its last
    // sample and holds a DC offset forever.
    sleepMs(100);
    SampleStreamPlayer_NoteOn(0, 127);
    sleepMs(2900);
    rt.stop();
    SampleStreamPlayer_DeInit();
    std::printf("  wrote %s/silence.wav\n", g_out.c_str());
    return g_fail;
}

static int sc_edge() {
    std::printf("[edge] files and calls that must fail without taking the engine down\n");
    check(SampleStreamPlayer_Init(), "engine init");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());
    SampleStreamPlayer_Prepare(256, 48000);

    struct { const char* file; const char* why; } bad[] = {
        {"does_not_exist.wav", "missing file"},
        {"notawav.txt",        "not a RIFF file"},
        {"bad_8bit.wav",       "8-bit PCM"},
        {"bad_24bit.wav",      "24-bit PCM"},
        {"empty_data.wav",     "empty data chunk"},
    };
    for (auto& b : bad) {
        check(!SampleStreamPlayer_SetupSample(0, kitPath(b.file).c_str(), 0, 0, 0, 0),
              "rejected", b.why);
    }
    // …and files that are odd but legal must still load.
    check(SampleStreamPlayer_SetupSample(1, kitPath("odd_chunk.wav").c_str(), 0, 0, 0, 0),
          "accepted", "odd-length LIST chunk before data");
    check(SampleStreamPlayer_SetupSample(2, kitPath("lies_about_size.wav").c_str(), 0, 0, 0, 0),
          "accepted", "data size larger than the file (clamped)");
    check(SampleStreamPlayer_SetupSample(3, kitPath("truncated.wav").c_str(), 0, 0, 0, 0),
          "accepted", "file cut short mid-data");
    check(SampleStreamPlayer_SetupSample(4, kitPath("mono44k.wav").c_str(), 0, 0, 0, 0),
          "accepted", "mono 44.1 kHz (plays sharp, as on the board)");
    check(SampleStreamPlayer_SetupSample(5, kitPath("tiny32.wav").c_str(), 0, 0, 0, 0),
          "accepted", "32 frames, shorter than a render block");

    // API abuse.
    check(!SampleStreamPlayer_SetupSample(200, kitPath("pad00.wav").c_str(), 0, 0, 0, 0),
          "rejected", "note past the slot count");
    check(!SampleStreamPlayer_SetupSample(0, kitPath("pad00.wav").c_str(), 0, 99, 0, 0),
          "rejected", "layer past the layer count");
    check(SampleStreamPlayer_SetupSample(6, kitPath("pad00.wav").c_str(), 0, 0,
                                         1u << 30, 0),
          "accepted", "crop start past EOF (falls back to the whole file)");

    SampleStreamPlayer_NoteOn(200, 127);           // must not touch memory
    SampleStreamPlayer_NoteOff(200);
    SampleStreamPlayer_NoteOn(9, 127);             // empty slot
    check(SampleStreamPly_GetActiveVoices() == 0, "note-on on an empty slot is a no-op");

    // Velocity 0 is a note-off in MIDI; it must never sound.
    SampleStreamPlayer_NoteOn(1, 0);
    check(SampleStreamPly_GetActiveVoices() == 0, "velocity 0 does not start a voice");

    // A sample shorter than one block must still sound and then retire.
    std::vector<float> buf(512 * 2, 0.0f);
    SampleStreamPlayer_NoteOn(5, 127);
    SampleStreamPly_ProcessAdd(buf.data(), 512);
    double peak = 0; for (float v : buf) peak = std::max<double>(peak, std::fabs(v));
    check(peak > 0.0, "32-frame sample produced output", "peak " + std::to_string(peak));
    sleepMs(30);
    check(SampleStreamPly_GetActiveVoices() == 0, "and retired inside one block");

    // Wiping a slot under a ringing voice: the render must not read freed data.
    SampleStreamPlayer_NoteOn(4, 127);
    SampleStreamPlayer_WipeOut(4);
    std::fill(buf.begin(), buf.end(), 0.0f);
    SampleStreamPly_ProcessAdd(buf.data(), 512);
    check(SampleStreamPly_GetActiveVoices() == 0, "wipe under a ringing voice stops it");

    // Unmounted card: every path becomes unresolvable, nothing may crash.
    SampleStreamPlayer_SetRootPrefix("");
    check(!SampleStreamPlayer_SetupSample(7, kitPath("pad00.wav").c_str(), 0, 0, 0, 0),
          "rejected", "sample path with the card unmounted");
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());

    // Render with no scratch prepared for the size asked: must clamp, not
    // overrun. (prepare() was called with 256; ask for 4096.)
    std::vector<float> big(4096 * 2, 0.0f);
    SampleStreamPlayer_NoteOn(1, 127);
    SampleStreamPly_ProcessAdd(big.data(), 4096);
    check(true, "oversized render block did not overrun the scratch");

    SampleStreamPlayer_WipeOut();
    check(SampleStreamPly_GetActiveVoices() == 0, "wipe-all clears every voice");
    SampleStreamPlayer_DeInit();
    return g_fail;
}

// ── Entry ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const char* sd  = std::getenv("CP_SD");
    const char* out = std::getenv("CP_OUT");
    if (!sd || !out) { std::printf("bench error: set CP_SD and CP_OUT\n"); return 2; }
    g_sd = sd; g_out = out;

    const std::string sc = (argc > 1) ? argv[1] : "smoke";
    SampleStreamPlayer_SetRootPrefix(g_sd.c_str());

    int rc = 0;
    if      (sc == "smoke")    rc = sc_smoke();
    else if (sc == "stream")   rc = sc_stream();
    else if (sc == "sine_rt")  rc = sc_sine_rt(argc > 2 ? atof(argv[2]) : 12.0);
    else if (sc == "onset")    rc = sc_onset(argc > 2 ? atoi(argv[2]) : 16,
                                             argc > 3 ? atof(argv[3]) : 0.25);
    else if (sc == "velocity") rc = sc_velocity();
    else if (sc == "poly")     rc = sc_poly();
    else if (sc == "choke")    rc = sc_choke();
    else if (sc == "loop")     rc = sc_loop();
    else if (sc == "churn")    rc = sc_churn(argc > 2 ? atoi(argv[2]) : 20,
                                             argc > 3 ? atof(argv[3]) : 8.0);
    else if (sc == "storm")    rc = sc_storm(argc > 2 ? atof(argv[2]) : 40.0,
                                             argc > 3 ? atof(argv[3]) : 20.0);
    else if (sc == "silence")  rc = sc_silence();
    else if (sc == "edge")     rc = sc_edge();
    else { std::printf("unknown scenario '%s'\n", sc.c_str()); return 2; }

    std::printf("[%s] %s\n", sc.c_str(), rc == 0 ? "PASS" : "FAIL");
    return rc;
}
