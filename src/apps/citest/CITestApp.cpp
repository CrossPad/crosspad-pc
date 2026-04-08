/**
 * @file    CITestApp.cpp
 * @brief   CI Test app — automated audio pipeline integration tests.
 *
 * Runs 7 test stages that exercise the full audio chain:
 * synth → mixer → audio output → tap capture → analysis.
 * Shows pass/fail results on an LVGL status screen.
 */

#include "pc_stubs/PcApp.hpp"
#include "pc_stubs/pc_platform.h"
#include "crosspad-mixer/AudioMixerEngine.hpp"
#include "synth/MlPianoSynth.hpp"

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad/audio/AudioRingBuffer.hpp"
#include "crosspad/synth/ISynthEngine.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include "lvgl.h"
#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <atomic>
#include <vector>

// ── Audio analysis utilities ──

struct AudioStats {
    float   rms;            // RMS level (0.0 - 1.0 relative to int16 max)
    int16_t peak;           // Max absolute sample
    int     zeroCrossings;  // Sign changes in left channel
    int     sampleCount;    // Total stereo frames analyzed
};

static AudioStats analyzeAudio(const int16_t* stereo, int frames) {
    AudioStats s{};
    s.sampleCount = frames;
    if (frames <= 0) return s;

    double sumSq = 0.0;
    int16_t maxAbs = 0;
    int zc = 0;
    int16_t prevL = stereo[0];

    for (int i = 0; i < frames; i++) {
        int16_t L = stereo[i * 2];
        int16_t R = stereo[i * 2 + 1];

        sumSq += (double)L * L + (double)R * R;

        int16_t absL = L < 0 ? (int16_t)-L : L;
        int16_t absR = R < 0 ? (int16_t)-R : R;
        if (absL > maxAbs) maxAbs = absL;
        if (absR > maxAbs) maxAbs = absR;

        if (i > 0 && ((prevL >= 0 && L < 0) || (prevL < 0 && L >= 0)))
            zc++;
        prevL = L;
    }

    s.rms = (float)std::sqrt(sumSq / (frames * 2)) / 32767.0f;
    s.peak = maxAbs;
    s.zeroCrossings = zc;
    return s;
}

// ── Test stage definitions ──

enum class StageResult { PENDING, RUNNING, PASS, FAIL };

struct TestStage {
    const char*  name;
    StageResult  result;
    char         detail[128];
};

static constexpr int NUM_STAGES = 7;

static TestStage s_stages[NUM_STAGES] = {
    {"Synth → Audio",       StageResult::PENDING, ""},
    {"NoteOff → Silence",   StageResult::PENDING, ""},
    {"Mixer Routing",       StageResult::PENDING, ""},
    {"Mixer Mute",          StageResult::PENDING, ""},
    {"Mixer Volume",        StageResult::PENDING, ""},
    {"Audio Capture",       StageResult::PENDING, ""},
    {"Restore Defaults",    StageResult::PENDING, ""},
};

// ── LVGL UI ──

static lv_obj_t* s_labels[NUM_STAGES] = {};
static lv_obj_t* s_titleLabel = nullptr;
static lv_obj_t* s_summaryLabel = nullptr;
static std::atomic<bool> s_testRunning{false};
static std::atomic<bool> s_uiDirty{false};
static lv_timer_t* s_uiTimer = nullptr;

static const char* resultStr(StageResult r) {
    switch (r) {
        case StageResult::PENDING: return "#808080 [ .... ]#";
        case StageResult::RUNNING: return "#FFFF00 [ >>>> ]#";
        case StageResult::PASS:    return "#00FF00 [ PASS ]#";
        case StageResult::FAIL:    return "#FF0000 [ FAIL ]#";
    }
    return "";
}

static void updateUI() {
    if (!s_uiDirty.load()) return;
    s_uiDirty.store(false);

    int pass = 0, fail = 0, total = 0;
    for (int i = 0; i < NUM_STAGES; i++) {
        if (s_labels[i]) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s  %s  %s",
                     resultStr(s_stages[i].result),
                     s_stages[i].name,
                     s_stages[i].detail);
            lv_label_set_text(s_labels[i], buf);
        }
        if (s_stages[i].result == StageResult::PASS) { pass++; total++; }
        if (s_stages[i].result == StageResult::FAIL) { fail++; total++; }
    }

    if (s_summaryLabel && !s_testRunning.load()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d/%d passed%s",
                 pass, total, fail > 0 ? " — FAILURES" : " — ALL PASS");
        lv_label_set_text(s_summaryLabel, buf);
        lv_obj_set_style_text_color(s_summaryLabel,
            fail > 0 ? lv_color_hex(0xFF4444) : lv_color_hex(0x44FF44), 0);
    }
}

static void uiTimerCb(lv_timer_t*) { updateUI(); }

static void setStage(int idx, StageResult result, const char* detail = "") {
    s_stages[idx].result = result;
    snprintf(s_stages[idx].detail, sizeof(s_stages[idx].detail), "%s", detail);
    s_uiDirty.store(true);
}

// ── Helpers ──

static void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void drainTap(crosspad::AudioRingBuffer<int16_t>& tap) {
    int16_t junk[512];
    while (tap.available() > 0) tap.read(junk, 512);
}

// ── Test runner (runs in FreeRTOS task) ──

static void testRunnerTask(void* pvParam) {
    (void)pvParam;

    auto& mixer = getMixerEngine();
    auto* synth = pc_platform_get_synth_engine();

    // Tap buffer: 2 seconds stereo at 48kHz
    crosspad::AudioRingBuffer<int16_t> tapBuf(48000 * 2 * 2);

    if (!synth) {
        for (int i = 0; i < NUM_STAGES; i++)
            setStage(i, StageResult::FAIL, "no synth engine");
        s_testRunning.store(false);
        vTaskDelete(nullptr);
        return;
    }

    // Save original mixer state
    float origSynthVol = mixer.getChannelVolume(MixerInput::SYNTH);
    bool origSynthMuted = mixer.isChannelMuted(MixerInput::SYNTH);
    bool origRouteOut1 = mixer.isRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1);
    bool origRouteOut2 = mixer.isRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT2);

    // Ensure clean starting state
    mixer.setChannelMute(MixerInput::SYNTH, false);
    mixer.setChannelVolume(MixerInput::SYNTH, 1.0f);
    mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
    mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT2, false);
    mixer.setOutputMute(MixerOutput::OUT1, false);
    mixer.setOutputMute(MixerOutput::OUT2, false);
    mixer.setOutputVolume(MixerOutput::OUT1, 1.0f);
    mixer.setOutputVolume(MixerOutput::OUT2, 1.0f);
    delayMs(100);

    // ── Stage 1: Synth → Audio Pipeline ──
    {
        setStage(0, StageResult::RUNNING);
        synth->noteOn(60, 100);
        delayMs(300);  // let mixer process

        int16_t peakL, peakR;
        mixer.getOutputLevel(MixerOutput::OUT1, peakL, peakR);
        int16_t peak = peakL > peakR ? peakL : peakR;

        char detail[128];
        snprintf(detail, sizeof(detail), "peak=%d", peak);

        if (peak > 50) {
            setStage(0, StageResult::PASS, detail);
        } else {
            setStage(0, StageResult::FAIL, detail);
        }
        synth->noteOff(60);
    }

    // ── Stage 2: NoteOff → Silence ──
    {
        setStage(1, StageResult::RUNNING);
        delayMs(500);  // wait for decay

        int16_t peakL, peakR;
        mixer.getOutputLevel(MixerOutput::OUT1, peakL, peakR);
        int16_t peak = peakL > peakR ? peakL : peakR;

        char detail[128];
        snprintf(detail, sizeof(detail), "peak=%d", peak);

        if (peak < 100) {
            setStage(1, StageResult::PASS, detail);
        } else {
            setStage(1, StageResult::FAIL, detail);
        }
    }

    // ── Stage 3: Mixer Routing (SYNTH→OUT2 only) ──
    {
        setStage(2, StageResult::RUNNING);

        // Route SYNTH to OUT2 only
        mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, false);
        mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT2, true);
        delayMs(50);

        synth->noteOn(60, 100);
        delayMs(300);

        int16_t out1L, out1R, out2L, out2R;
        mixer.getOutputLevel(MixerOutput::OUT1, out1L, out1R);
        mixer.getOutputLevel(MixerOutput::OUT2, out2L, out2R);
        int16_t peakOut1 = out1L > out1R ? out1L : out1R;
        int16_t peakOut2 = out2L > out2R ? out2L : out2R;

        synth->noteOff(60);

        char detail[128];
        snprintf(detail, sizeof(detail), "out1=%d out2=%d", peakOut1, peakOut2);

        // OUT1 should be silent, OUT2 should have audio
        if (peakOut1 < 50 && peakOut2 > 50) {
            setStage(2, StageResult::PASS, detail);
        } else {
            setStage(2, StageResult::FAIL, detail);
        }

        // Restore routing
        mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, true);
        mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT2, false);
        delayMs(100);
    }

    // ── Stage 4: Mixer Mute ──
    {
        setStage(3, StageResult::RUNNING);

        mixer.setChannelMute(MixerInput::SYNTH, true);
        delayMs(50);

        synth->noteOn(60, 100);
        delayMs(300);

        int16_t peakL, peakR;
        mixer.getOutputLevel(MixerOutput::OUT1, peakL, peakR);
        int16_t peak = peakL > peakR ? peakL : peakR;

        synth->noteOff(60);
        mixer.setChannelMute(MixerInput::SYNTH, false);

        char detail[128];
        snprintf(detail, sizeof(detail), "muted_peak=%d", peak);

        if (peak < 50) {
            setStage(3, StageResult::PASS, detail);
        } else {
            setStage(3, StageResult::FAIL, detail);
        }
        delayMs(300);
    }

    // ── Stage 5: Mixer Volume attenuation ──
    {
        setStage(4, StageResult::RUNNING);

        // Full volume
        mixer.setChannelVolume(MixerInput::SYNTH, 1.0f);
        synth->noteOn(60, 100);
        delayMs(300);
        int16_t fullL, fullR;
        mixer.getOutputLevel(MixerOutput::OUT1, fullL, fullR);
        int16_t fullPeak = fullL > fullR ? fullL : fullR;
        synth->noteOff(60);
        delayMs(500);

        // Half volume
        mixer.setChannelVolume(MixerInput::SYNTH, 0.5f);
        synth->noteOn(60, 100);
        delayMs(300);
        int16_t halfL, halfR;
        mixer.getOutputLevel(MixerOutput::OUT1, halfL, halfR);
        int16_t halfPeak = halfL > halfR ? halfL : halfR;
        synth->noteOff(60);

        mixer.setChannelVolume(MixerInput::SYNTH, 1.0f);

        char detail[128];
        snprintf(detail, sizeof(detail), "full=%d half=%d", fullPeak, halfPeak);

        // Half volume should be significantly less (allow some tolerance for FM synth variability)
        if (fullPeak > 100 && halfPeak < fullPeak && halfPeak < fullPeak * 0.8f) {
            setStage(4, StageResult::PASS, detail);
        } else {
            setStage(4, StageResult::FAIL, detail);
        }
        delayMs(300);
    }

    // ── Stage 6: Audio Capture + Analysis ──
    {
        setStage(5, StageResult::RUNNING);

        // Enable tap on OUT1
        mixer.setTapOutput(MixerOutput::OUT1);
        mixer.setTapBuffer(&tapBuf);
        drainTap(tapBuf);  // clear any stale data

        synth->noteOn(60, 100);
        delayMs(500);  // capture ~0.5s of audio
        synth->noteOff(60);
        delayMs(100);

        mixer.setTapBuffer(nullptr);  // disable tap

        // Read captured audio
        size_t avail = tapBuf.available();
        int frames = (int)(avail / 2);  // stereo pairs

        if (frames < 100) {
            char detail[128];
            snprintf(detail, sizeof(detail), "only %d frames captured", frames);
            setStage(5, StageResult::FAIL, detail);
        } else {
            std::vector<int16_t> captured(avail);
            tapBuf.read(captured.data(), avail);

            auto stats = analyzeAudio(captured.data(), frames);

            char detail[128];
            snprintf(detail, sizeof(detail),
                     "rms=%.4f peak=%d zc=%d frames=%d",
                     stats.rms, stats.peak, stats.zeroCrossings, stats.sampleCount);

            // Verify: non-trivial audio (RMS > 0.001, peak > 50, zero crossings > 10)
            bool hasAudio = stats.rms > 0.001f && stats.peak > 50 && stats.zeroCrossings > 10;

            // Verify: not just noise (should have structured zero crossings)
            // A 261Hz note (C4) at 48kHz → ~522 ZC/sec → ~261 in 0.5s
            // FM synth is complex, so we just check it's in a reasonable range
            bool structured = stats.zeroCrossings > 50 && stats.zeroCrossings < 5000;

            if (hasAudio && structured) {
                setStage(5, StageResult::PASS, detail);
            } else {
                setStage(5, StageResult::FAIL, detail);
            }
        }
    }

    // ── Stage 7: Restore Defaults ──
    {
        setStage(6, StageResult::RUNNING);

        mixer.setChannelVolume(MixerInput::SYNTH, origSynthVol);
        mixer.setChannelMute(MixerInput::SYNTH, origSynthMuted);
        mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT1, origRouteOut1);
        mixer.setRouteEnabled(MixerInput::SYNTH, MixerOutput::OUT2, origRouteOut2);

        setStage(6, StageResult::PASS, "mixer restored");
    }

    // Count results
    int pass = 0, fail = 0;
    for (auto& st : s_stages) {
        if (st.result == StageResult::PASS) pass++;
        if (st.result == StageResult::FAIL) fail++;
    }
    printf("[CITest] Complete: %d/%d passed\n", pass, pass + fail);

    s_testRunning.store(false);
    s_uiDirty.store(true);
    vTaskDelete(nullptr);
}

// ── App lifecycle ──

static lv_obj_t* CITest_create(lv_obj_t* parent, App*) {
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 2, 0);

    // Title
    s_titleLabel = lv_label_create(cont);
    lv_label_set_text(s_titleLabel, "CI Test — Audio Pipeline");
    lv_obj_set_style_text_color(s_titleLabel, lv_color_hex(0x00BFFF), 0);
    lv_obj_set_style_text_font(s_titleLabel, &lv_font_montserrat_14, 0);

    // Stage labels
    for (int i = 0; i < NUM_STAGES; i++) {
        s_stages[i].result = StageResult::PENDING;
        s_stages[i].detail[0] = '\0';

        s_labels[i] = lv_label_create(cont);
        lv_label_set_recolor(s_labels[i], true);
        lv_obj_set_style_text_font(s_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_labels[i], lv_color_hex(0xCCCCCC), 0);

        char buf[256];
        snprintf(buf, sizeof(buf), "%s  %s", resultStr(StageResult::PENDING), s_stages[i].name);
        lv_label_set_text(s_labels[i], buf);
    }

    // Summary
    s_summaryLabel = lv_label_create(cont);
    lv_label_set_text(s_summaryLabel, "Running...");
    lv_obj_set_style_text_color(s_summaryLabel, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(s_summaryLabel, &lv_font_montserrat_12, 0);

    // UI refresh timer
    s_uiTimer = lv_timer_create(uiTimerCb, 100, nullptr);

    // Start test runner
    s_testRunning.store(true);
    xTaskCreate(testRunnerTask, "CITest", 8192, nullptr, 1, nullptr);

    printf("[CITest] App started\n");
    return cont;
}

static void CITest_destroy(lv_obj_t* obj) {
    if (s_uiTimer) {
        lv_timer_delete(s_uiTimer);
        s_uiTimer = nullptr;
    }
    for (auto& l : s_labels) l = nullptr;
    s_titleLabel = nullptr;
    s_summaryLabel = nullptr;

    lv_obj_delete_async(obj);
    printf("[CITest] App destroyed\n");
}

void _register_CITest_app() {
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%stest.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());
    static const crosspad::AppEntry entry = {
        "CITest", icon_path,
        CITest_create, CITest_destroy,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}
