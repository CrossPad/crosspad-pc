/*
 * Copyright (c) 2025 Marcel Licence
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file    ml_fm.cpp
 * @author  Marcel Licence
 * @date    15.09.2024
 *
 * @brief This is a simple implementation of an FM Synthesizer module
 *
 * Vendored from ML_SynthTools with PC/MSVC portability fixes:
 *   - Added ml_compat.h include (handles __attribute__ and _USE_MATH_DEFINES)
 *   - Changed <ml_utils.h>/<ml_status.h> to local "..." includes
 *   - Replaced #define fabs/fmax macros with <cmath> std::fabs/std::fmax
 *   - Added (float) casts to suppress MSVC double-to-float warnings
 *
 * @see https://youtu.be/rGTw05GKwvU
 */

#include "ml_compat.h"

#include "ml_utils.h"
#include "ml_status.h"

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>

static float sample_rate = 0;

#define SINE_BIT    12UL
#define SINE_CNT    (1<<SINE_BIT)
#define SINE_MSK    ((1<<SINE_BIT)-1)
#define SINE_I(i)   ((i) >> (32 - SINE_BIT))

static float *sine = NULL;

static void Sine_Init(void)
{
    uint32_t memSize = sizeof(float) * SINE_CNT;
    sine = (float *)malloc(memSize);
    if (sine == NULL)
    {
        Status_LogMessage("not enough heap memory for sine buffer!\n");
    }
    for (int i = 0; i < SINE_CNT; i++)
    {
        sine[i] = (float)sin(i * 2.0 * M_PI / SINE_CNT);
    }
}

static float SineNorm(float alpha_div2pi)
{
    uint32_t index = ((uint32_t)(alpha_div2pi * ((float)SINE_CNT))) % SINE_CNT;
    return sine[index];
}

static float SineNormU32(uint32_t pos)
{
    return sine[SINE_I(pos)];
}

#ifndef ARDUINO
float millis()
{
    return 0.0f;
}
#endif

#define OP1 3
#define OP2 2
#define OP3 1
#define OP4 0

#define MIDI_CH_CNT 16

#define FM_VOICE_CNT    6

/* Replaced original #define fabs/fmax macros with proper std:: calls.
 * The original macros lacked parentheses and conflicted with <cmath>. */

enum ssg_eg_e
{
    ssgeg_off,
    ssgeg_repeat,
    ssgeg_once,
    ssgeg_mirror,
    ssgeg_once_high,
    ssgeg_loop_rev,
    ssgeg_once_inv,
    ssgeg_mirro_inv,
    ssgeg_once_high_inv
};

struct op_properties_s
{
    float mw;
    float am;

    uint32_t ar;
    uint32_t d1r;
    uint32_t d2r;
    float d2l;
    uint32_t rr;
    uint32_t rs;

    float ssgeg;
    float mul;

    float mul_fine;
    float mul_coarse;

    float fixed;
    float dt;
    float vel;
    float tl;

    float vel_to_tl;
};

struct custom_properties_s
{
    float feedback;

    float lfo_enable;
    float lfo_speed;
    float fms;
    float fmsmw;
    float ams;

    float legato_retrig;
    float pitchbend_range;
    float volume;
    float alg;
};

typedef uint8_t envState_t;

#define ENV_ATTACK  (envState_t)0
#define ENV_DECAY1  (envState_t)1
#define ENV_DECAY2  (envState_t)2
#define ENV_RELEASE (envState_t)3
#define ENV_OFF     (envState_t)4

struct synthTone_s
{
    float pitch;
    float pitchCalc;

    float out;
    float in;

    uint32_t pos;
    float sine_preoout;

    float lvl_env;
    float lvl_add;
    envState_t state;
    int stateLen;

    float vel;

    struct op_properties_s *op_prop;
};

struct synthVoice_s
{
    struct synthTone_s op[4];
    bool active;

    uint8_t midiNote;
    uint8_t midiCh;
    float out;
    float outSlow;

    struct channelSettingParam_s *settings;
};

struct synthVoice_s fmVoice[FM_VOICE_CNT];

struct channelSettingParam_s
{
    struct custom_properties_s props;
    struct op_properties_s op_prop[4];

    float fmFeedback;

    int algo;

    bool mono;
    bool legato;

    uint8_t notes[16];
    uint8_t noteStackCnt;
};

static uint32_t milliCnt = 0;

static float modulationDepth = 0.0f;
static float modulationSpeed = 5.0f;
static float modulationPitch = 1.0f;
static float pitchBendValue = 0.0f;
static float pitchMultiplier = 1.0f;

static bool initChannelSetting = false;
static uint32_t initChannelSettingCnt = 0;

static struct channelSettingParam_s channelSettings[MIDI_CH_CNT];

static struct channelSettingParam_s *currentChSetting = &channelSettings[0];

static uint8_t selectedOp = OP4;

static struct op_properties_s op_props_silent;

static float multiplierPitchToAddValue = 0;


static void FmSynth_ProcessOperator(struct synthTone_s *osc);
static void FmSynth_EnvStateProcess(struct synthTone_s *osc);
static void FmSynth_AlgMixProcess(float *out, struct synthVoice_s *voice);


void FmSynth_ToneEnvUpdate(struct synthTone_s *tone)
{
    uint32_t attack = tone->op_prop->ar * tone->op_prop->rs;
    attack = attack > 0 ? attack : 1;

    uint32_t decay1 = tone->op_prop->d1r * tone->op_prop->rs;
    decay1 = decay1 > 0 ? decay1 : 1;

    uint32_t decay2 = tone->op_prop->d2r * tone->op_prop->rs;
    decay2 = decay2 > 0 ? decay2 : 1;

    uint32_t release = tone->op_prop->rr * tone->op_prop->rs;
    release = release > 0 ? release : 1;

    switch (tone->state)
    {
    case ENV_ATTACK:
        tone->lvl_add = 1.0f / (((float)attack));
        tone->stateLen = attack;
        break;
    case ENV_DECAY1:
        tone->lvl_add = (tone->op_prop->d2l - tone->lvl_env) / (((float)decay1));
        tone->stateLen = decay1;
        break;
    case ENV_DECAY2:
        tone->lvl_add = (0.0f - tone->lvl_env) / (((float)decay2));
        tone->stateLen = decay2;
        break;
    case ENV_RELEASE:
        tone->lvl_add = (0.0f - tone->lvl_env) / (((float)release));
        tone->stateLen = release;
        break;
    case ENV_OFF:
        tone->lvl_add = 0;
        tone->stateLen = 0;
        tone->lvl_env = 0.0f;
        break;
    default:
        tone->lvl_add = 0;
        tone->stateLen = 1;
        tone->state = ENV_OFF;
        break;
    }
}

void FmSynth_ToneInit(struct synthTone_s *tone, struct op_properties_s *op_props)
{
    tone->pos = 0;
    tone->sine_preoout = 0.0f;

    tone->in = 0.0f;
    tone->out = 0.0f;

    tone->vel = 1.0f;

    tone->lvl_env = 0.0f;
    tone->lvl_add = 0.0f;

    tone->state = ENV_ATTACK;
    tone->op_prop = op_props;

    FmSynth_ToneEnvUpdate(tone);
}

void FmSynth_InitOpProps(struct op_properties_s *op_props)
{
    op_props->ar = 1;
    op_props->d1r = 32767;
    op_props->d2l = 1;
    op_props->d2r = 32767;
    op_props->rr = 1;
    op_props->rs = 50;

    op_props->am = 0;

    op_props->mul_coarse = 1.0f;
    op_props->mul_fine = 0.0f;
    op_props->mul = 1.0f;

    op_props->dt = 0.0f;

    op_props->vel = 0;
    op_props->tl = 0;

    op_props->vel_to_tl = 0.0f;
}

void FmSynth_IntiVoice(struct synthVoice_s *voice)
{
    voice->out = 0.0f;
    voice->midiCh = 0xff;
    voice->midiNote = 0xff;
    voice->settings = currentChSetting;

    for (int i = 0; i < 4; i++)
    {
        FmSynth_ToneInit(&voice->op[i], &op_props_silent);
    }
}

void FmSynth_InitChannelSettings(struct channelSettingParam_s *chSettings)
{
    chSettings->fmFeedback = 0;
    chSettings->algo = 0;
    chSettings->mono = false;
    chSettings->legato = false;
    chSettings->noteStackCnt = 0;

    for (int i = 0; i < 4; i++)
    {
        FmSynth_InitOpProps(&chSettings->op_prop[i]);
    }
    chSettings->op_prop[OP4].tl = 1.0f;
}

void FmSynth_Init(float sample_rate_in)
{
    Sine_Init();

    sample_rate = sample_rate_in;
    multiplierPitchToAddValue = ((float)(1ULL << 32ULL) / ((float)sample_rate));

    FmSynth_InitOpProps(&op_props_silent);
    op_props_silent.tl = 0.0f;
    op_props_silent.d1r = 1;
    op_props_silent.d2r = 1;

    for (int ch = 0; ch < MIDI_CH_CNT; ch++)
    {
        FmSynth_InitChannelSettings(&channelSettings[ch]);
    }

    for (int j = 0; j < FM_VOICE_CNT; j++)
    {
        FmSynth_IntiVoice(&fmVoice[j]);
    }

    struct channelSettingParam_s *setting = &channelSettings[0];

    setting->algo = 0;
    setting->fmFeedback = 0;
    setting->op_prop[0].ar = 1; setting->op_prop[0].d1r = 1; setting->op_prop[0].d2l = 1.000000f; setting->op_prop[0].d2r = 32767; setting->op_prop[0].rr = 1; setting->op_prop[0].rs = 50; setting->op_prop[0].tl = 1.000000f; setting->op_prop[0].mul = 2.500000f; setting->op_prop[0].vel_to_tl = 0.000000f; setting->op_prop[0].am = 0.000000f; setting->op_prop[0].mw = 0.000000f; setting->op_prop[0].vel = 0.000000f;
    setting->op_prop[1].ar = 1; setting->op_prop[1].d1r = 1; setting->op_prop[1].d2l = 0.346456f; setting->op_prop[1].d2r = 32767; setting->op_prop[1].rr = 1; setting->op_prop[1].rs = 50; setting->op_prop[1].tl = 0.322834f; setting->op_prop[1].mul = 3.500000f; setting->op_prop[1].vel_to_tl = 0.614172f; setting->op_prop[1].am = 0.000000f; setting->op_prop[1].mw = 0.000000f; setting->op_prop[1].vel = 0.000000f;
    setting->op_prop[2].ar = 1; setting->op_prop[2].d1r = 1; setting->op_prop[2].d2l = 0.401574f; setting->op_prop[2].d2r = 32767; setting->op_prop[2].rr = 1; setting->op_prop[2].rs = 50; setting->op_prop[2].tl = 0.047244f; setting->op_prop[2].mul = 11.000000f; setting->op_prop[2].vel_to_tl = 0.999998f; setting->op_prop[2].am = 0.000000f; setting->op_prop[2].mw = 0.000000f; setting->op_prop[2].vel = 0.000000f;
    setting->op_prop[3].ar = 1; setting->op_prop[3].d1r = 1; setting->op_prop[3].d2l = 1.000000f; setting->op_prop[3].d2r = 32767; setting->op_prop[3].rr = 1; setting->op_prop[3].rs = 50; setting->op_prop[3].tl = 0.000000f; setting->op_prop[3].mul = 1.000000f; setting->op_prop[3].vel_to_tl = 0.000000f; setting->op_prop[3].am = 0.000000f; setting->op_prop[3].mw = 0.000000f; setting->op_prop[3].vel = 0.000000f;

    setting = &channelSettings[1];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0].ar = 1; setting->op_prop[0].d1r = 1; setting->op_prop[0].d2l = 1.000000f; setting->op_prop[0].d2r = 32767; setting->op_prop[0].rr = 1023; setting->op_prop[0].rs = 50; setting->op_prop[0].tl = 1.000000f; setting->op_prop[0].mul = 2.500000f; setting->op_prop[0].vel_to_tl = 0.000000f; setting->op_prop[0].am = 0.000000f; setting->op_prop[0].mw = 0.000000f; setting->op_prop[0].vel = 0.000000f;
    setting->op_prop[1].ar = 1; setting->op_prop[1].d1r = 1; setting->op_prop[1].d2l = 0.346456f; setting->op_prop[1].d2r = 32767; setting->op_prop[1].rr = 1023; setting->op_prop[1].rs = 50; setting->op_prop[1].tl = 0.322834f; setting->op_prop[1].mul = 3.500000f; setting->op_prop[1].vel_to_tl = 0.614172f; setting->op_prop[1].am = 0.000000f; setting->op_prop[1].mw = 0.000000f; setting->op_prop[1].vel = 0.000000f;
    setting->op_prop[2].ar = 1; setting->op_prop[2].d1r = 1; setting->op_prop[2].d2l = 0.401574f; setting->op_prop[2].d2r = 32767; setting->op_prop[2].rr = 1023; setting->op_prop[2].rs = 50; setting->op_prop[2].tl = 0.047244f; setting->op_prop[2].mul = 11.000000f; setting->op_prop[2].vel_to_tl = 0.999998f; setting->op_prop[2].am = 0.000000f; setting->op_prop[2].mw = 0.000000f; setting->op_prop[2].vel = 0.000000f;
    setting->op_prop[3].ar = 1; setting->op_prop[3].d1r = 1; setting->op_prop[3].d2l = 1.000000f; setting->op_prop[3].d2r = 32767; setting->op_prop[3].rr = 1; setting->op_prop[3].rs = 50; setting->op_prop[3].tl = 0.000000f; setting->op_prop[3].mul = 1.000000f; setting->op_prop[3].vel_to_tl = 0.000000f; setting->op_prop[3].am = 0.000000f; setting->op_prop[3].mw = 0.000000f; setting->op_prop[3].vel = 0.000000f;

    /* guitar */
    setting = &channelSettings[2];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,1.0f, 0};
    setting->op_prop[1] = {0,0, 1,147,2386,0.637794f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.354330f, 0};
    setting->op_prop[2] = {0,0, 1,1239,32767,0.999998f,1,50, 0,1.250000f,0,1.250000f, 0,0,0,0.181102f, 0};
    setting->op_prop[3] = {0,0, 1,26,32767,0.000000f,1,50, 0,0.630000f,0,0.630000f, 0,0,0,0.291338f, 0};

    /* hard voice */
    setting = &channelSettings[3];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.118110f, 0};
    setting->op_prop[2] = {0,0, 1,32767,32767,1.0f,1,50, 0,3.250000f,0,3.250000f, 0,0,0,0.094488f, 0};
    setting->op_prop[3] = {0,0, 1,1460,32767,0.307086f,1,50, 0,0.750000f,0,0.750000f, 0,0,0,0.999998f, 0};

    /* bassy base */
    setting = &channelSettings[4];
    setting->algo = 0; setting->fmFeedback = 0.999998f;
    setting->op_prop[0] = {0,0, 1,1,3050,0.999998f,1,50, 0,1.0f,0,1.0f, 0,0,0,1.0f, 0};
    setting->op_prop[1] = {0,0, 1,1345,32767,0.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.251968f, 0};
    setting->op_prop[2] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.039370f, 0};
    setting->op_prop[3] = {0,0, 1,115,32767,0.622046f,1,50, 0,0.5f,0,0.5f, 0,0,0,0.094488f, 0.141732f};

    /* organ */
    setting = &channelSettings[5];
    setting->algo = 7; setting->fmFeedback = 0.267716f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.881888f, 0};
    setting->op_prop[1] = {0,0, 1,135,32767,0.448818f,1,50, 0,3.0f,0,3.0f, 0,0,0,0.653542f, 0};
    setting->op_prop[2] = {0,0, 1,14,32767,0.267716f,1,50, 0,8.0f,0,8.0f, 0,0,0,0.401574f, 0};
    setting->op_prop[3] = {0,0, 1,5,32767,0.496062f,1,50, 0,4.0f,0,4.0f, 0,0,0,0.236220f, 0};

    /* some bass */
    setting = &channelSettings[6];
    setting->algo = 3; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,0.5f,0,0.5f, 0,0,0,0.047244f, 0};
    setting->op_prop[2] = {0,0, 1,427,32767,0.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.196850f, 0};
    setting->op_prop[3] = {0,0, 1,50,32767,0.0f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.062992f, 0};

    /* schnatter bass */
    setting = &channelSettings[7];
    setting->algo = 2; setting->fmFeedback = 0.055118f;
    setting->op_prop[0] = {0,0, 1,363,32767,0.614172f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.937006f, 0};
    setting->op_prop[1] = {0,0, 1,204,32767,0.196850f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.314960f, 0};
    setting->op_prop[2] = {0,0, 1,90,32767,0.543306f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.716534f, 0};
    setting->op_prop[3] = {0,0, 1,10,32767,0.299212f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.724408f, 0};

    /* harpsichord */
    setting = &channelSettings[8];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,1.0f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,3.0f,0,3.0f, 0,0,0,0.102362f, 0};
    setting->op_prop[2] = {0,0, 1,1239,32767,0.385826f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.094488f, 0};
    setting->op_prop[3] = {0,0, 1,28,4593,0.283464f,1,50, 0,15.0f,0,15.0f, 0,0,0,0.173228f, 0};

    /* some harsh */
    setting = &channelSettings[9];
    setting->algo = 3; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,2.8f,0,2.8f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,2.2f,0,2.2f, 0,0,0,0.236220f, 0};
    setting->op_prop[2] = {0,0, 1,32767,32767,1.0f,1,50, 0,2.4f,0,2.4f, 0,0,0,0.173228f, 0};
    setting->op_prop[3] = {0,0, 1,393,32767,0.307086f,1,50, 0,2.4f,0,2.4f, 0,0,0,0.157480f, 0};

    /* string bass */
    setting = &channelSettings[10];
    setting->algo = 3; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,0.5f,0,0.5f, 0,0,0,0.999998f, 0.078740f};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,0.5f,0,0.5f, 0,0,0,0.385826f, 0};
    setting->op_prop[2] = {0,0, 1,334,32767,0.251968f,1,50, 0,1.01f,0,1.01f, 0,0,0,0.330708f, 0.803148f};
    setting->op_prop[3] = {0,0, 1,27818,32767,0.181102f,1,50, 0,0.5f,0,0.5f, 0,0,0,0.700786f, 0};

    /* kick bass */
    setting = &channelSettings[11];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.078740f, 0};
    setting->op_prop[2] = {0,0, 1,46,32767,0.078740f,1,50, 0,11.0f,0,11.0f, 0,0,0,0.094488f, 0.425196f};
    setting->op_prop[3] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.0f, 0};

    /* wood bass */
    setting = &channelSettings[12];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.133858f, 0};
    setting->op_prop[2] = {0,0, 1,46,32767,0.078740f,1,50, 0,3.0f,0,3.0f, 0,0,0,0.236220f, 0.425196f};
    setting->op_prop[3] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.0f, 0};

    /* saw bass */
    setting = &channelSettings[13];
    setting->algo = 5; setting->fmFeedback = 0.503936f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.01f,0,1.01f, 0,0,0,0.251968f, 0};
    setting->op_prop[2] = {0,0, 1,546,32767,0.0f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.330708f, 0};
    setting->op_prop[3] = {0,0, 1,308,32767,0.251968f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.141732f, 0};

    /* plug sound */
    setting = &channelSettings[14];
    setting->algo = 2; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.999998f, 0};
    setting->op_prop[1] = {0,0, 1,76,32767,0.267716f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.212598f, 0};
    setting->op_prop[2] = {0,0, 1,36,32767,0.393700f,1,50, 0,4.0f,0,4.0f, 0,0,0,0.110236f, 0.527558f};
    setting->op_prop[3] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.055118f, 0};

    /* fady stuff */
    setting = &channelSettings[15];
    setting->algo = 0; setting->fmFeedback = 0.000000f;
    setting->op_prop[0] = {0,0, 70,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,1.0f, 0};
    setting->op_prop[1] = {0,0, 1,32767,32767,1.0f,1,50, 0,1.0f,0,1.0f, 0,0,0,0.023622f, 0};
    setting->op_prop[2] = {0,0, 2198,32767,32767,1.0f,1,50, 0,2.0f,0,2.0f, 0,0,0,0.496062f, 0};
    setting->op_prop[3] = {0,0, 30191,32767,32767,1.0f,1,50, 0,4.0f,0,4.0f, 0,0,0,0.0f, 0};
}

static void FmSynth_ProcessOperator(struct synthTone_s *osc)
{
    float pitch = osc->pitchCalc;

    int32_t phaseShift = (int32_t)(100000.0f * osc->in * multiplierPitchToAddValue);
    osc->in = 0;

    int32_t add = (int32_t)(pitch * multiplierPitchToAddValue);
    osc->pos += add;

    osc->sine_preoout = SineNormU32(osc->pos + phaseShift);
    osc->lvl_env += osc->lvl_add;
}

static void FmSynth_EnvStateProcess(struct synthTone_s *osc)
{
    osc->stateLen--;
    if (osc->stateLen <= 0)
    {
        if (osc->state != ENV_OFF)
        {
            osc->state++;
            FmSynth_ToneEnvUpdate(osc);
        }
    }
}

static void FmSynth_AlgMixProcess(float *out, struct synthVoice_s *voice)
{
    voice->out = 0.0f;

    voice->op[OP1].in += (voice->settings->fmFeedback * voice->op[OP1].out);

    switch (voice->settings->algo)
    {
    case 0:
        voice->out += voice->op[OP4].out;
        voice->op[OP4].in += voice->op[OP3].out;
        voice->op[OP3].in += voice->op[OP2].out;
        voice->op[OP2].in += voice->op[OP1].out;
        break;
    case 1:
        voice->out += voice->op[OP4].out;
        voice->op[OP4].in += voice->op[OP3].out;
        voice->op[OP3].in += voice->op[OP2].out;
        voice->op[OP3].in += voice->op[OP1].out;
        break;
    case 2:
        voice->out += voice->op[OP4].out;
        voice->op[OP4].in += voice->op[OP3].out;
        voice->op[OP4].in += voice->op[OP2].out;
        voice->op[OP3].in += voice->op[OP1].out;
        break;
    case 3:
        voice->out += voice->op[OP4].out;
        voice->op[OP4].in += voice->op[OP3].out;
        voice->op[OP4].in += voice->op[OP2].out;
        voice->op[OP2].in += voice->op[OP1].out;
        break;
    case 4:
        voice->out += voice->op[OP4].out;
        voice->op[OP4].in += voice->op[OP3].out;
        voice->out += voice->op[OP2].out;
        voice->op[OP2].in += voice->op[OP1].out;
        break;
    case 5:
        voice->out += voice->op[OP4].out;
        voice->out += voice->op[OP3].out;
        voice->out += voice->op[OP2].out;
        voice->op[OP4].in += voice->op[OP1].out;
        voice->op[OP3].in += voice->op[OP1].out;
        voice->op[OP2].in += voice->op[OP1].out;
        break;
    case 6:
        voice->out += voice->op[OP4].out;
        voice->out += voice->op[OP3].out;
        voice->out += voice->op[OP2].out;
        voice->op[OP2].in += voice->op[OP1].out;
        break;
    case 7:
        voice->out += voice->op[OP4].out;
        voice->out += voice->op[OP3].out;
        voice->out += voice->op[OP2].out;
        voice->out += voice->op[OP1].out;
        break;
    }

    *out += voice->out;

    voice->outSlow = std::fmax(std::fabs(voice->out), voice->outSlow);
}

inline
float FmSynth_GetModulationPitchMultiplier(void)
{
    float modSpeed = modulationSpeed;
    return modulationDepth * modulationPitch * (SineNorm((modSpeed * ((float)milliCnt) / 1000.0f)));
}

void FmSynth_Process(const float * /*in*/, float *out, int bufLen)
{
    milliCnt += (uint32_t)((bufLen * 1000) / sample_rate);

    float pitchVar = pitchBendValue + FmSynth_GetModulationPitchMultiplier();
    pitchMultiplier = std::pow(2.0f, pitchVar / 12.0f);

    for (int n = 0; n < bufLen; n++)
    {
        float sample = 0.0f;

        for (int j = 0; j < FM_VOICE_CNT; j++)
        {
            struct synthVoice_s *voice = &fmVoice[j];

            for (int i = 0; i < 4; i++)
            {
                struct synthTone_s *osc = &voice->op[i];
                osc->pitchCalc = osc->op_prop->mul * osc->pitch * pitchMultiplier;
                FmSynth_ProcessOperator(osc);
                osc->out = osc->sine_preoout * osc->op_prop->tl * osc->lvl_env * osc->vel;
                FmSynth_EnvStateProcess(osc);
            }

            FmSynth_AlgMixProcess(&sample, voice);
        }

        out[n] = sample / 8;
    }

    for (int j = 0; j < FM_VOICE_CNT; j++)
    {
        fmVoice[j].outSlow *= 0.99f;
    }

    if (initChannelSetting)
    {
        initChannelSettingCnt++;
        if (initChannelSettingCnt > (uint32_t)(3 * (sample_rate / (float)bufLen)))
        {
            initChannelSetting = false;
            initChannelSettingCnt = 0;
            FmSynth_InitChannelSettings(currentChSetting);
        }
    }
    else
    {
        initChannelSettingCnt = 0;
    }
}

struct synthVoice_s *FmSynth_GetQuietestVoice(void)
{
    static uint8_t roundCnt = 0;
    roundCnt++;
    if (roundCnt >= FM_VOICE_CNT)
    {
        roundCnt = 0;
    }

    struct synthVoice_s *foundVoice = &fmVoice[roundCnt];
    float quietestVoice = 10.0f;

    for (int i = 0; i < FM_VOICE_CNT; i++)
    {
        struct synthVoice_s *voice = &fmVoice[i];
        if (voice->outSlow < quietestVoice)
        {
            quietestVoice = voice->outSlow;
            foundVoice = voice;
        }
    }

    return foundVoice;
}

void FmSynth_NoteOn(uint8_t ch, uint8_t note, float vel)
{
    if (ch < MIDI_CH_CNT)
    {
        currentChSetting = &channelSettings[ch];
    }

    struct synthVoice_s *newVoice = FmSynth_GetQuietestVoice();

    synthTone_s *tones[4];
    tones[0] = &newVoice->op[OP4];
    tones[1] = &newVoice->op[OP3];
    tones[2] = &newVoice->op[OP2];
    tones[3] = &newVoice->op[OP1];

    newVoice->midiCh = ch;
    newVoice->midiNote = note;
    newVoice->settings = currentChSetting;
    newVoice->outSlow = 5.0f;

    if (currentChSetting->mono)
    {
        currentChSetting->notes[currentChSetting->noteStackCnt++] = note;
    }
    float tonePitch = ((std::pow(2.0f, (float)(note - 69) / 12.0f) * 440.0f));
    struct synthTone_s *tone;

    tone = tones[OP4];
    FmSynth_ToneInit(tone, &currentChSetting->op_prop[OP4]);
    tone->pitch = tonePitch;
    tone->vel *= (1.0f - tone->op_prop->vel_to_tl) + tone->op_prop->vel_to_tl * vel;

    tone = tones[OP3];
    FmSynth_ToneInit(tone, &currentChSetting->op_prop[OP3]);
    tone->pitch = tonePitch;
    tone->vel *= (1.0f - tone->op_prop->vel_to_tl) + tone->op_prop->vel_to_tl * vel;

    tone = tones[OP2];
    FmSynth_ToneInit(tone, &currentChSetting->op_prop[OP2]);
    tone->pitch = tonePitch;
    tone->vel *= (1.0f - tone->op_prop->vel_to_tl) + tone->op_prop->vel_to_tl * vel;

    tone = tones[OP1];
    FmSynth_ToneInit(tone, &currentChSetting->op_prop[OP1]);
    tone->pitch = tonePitch;
    tone->vel *= (1.0f - tone->op_prop->vel_to_tl) + tone->op_prop->vel_to_tl * vel;
}

void FmSynth_NoteOff(uint8_t ch, uint8_t note)
{
    for (int i = 0; i < FM_VOICE_CNT; i++)
    {
        if ((fmVoice[i].midiNote == note) && (fmVoice[i].midiCh == ch))
        {
            for (int j = 0; j < 4; j++)
            {
                fmVoice[i].op[j].state = ENV_RELEASE;
                FmSynth_ToneEnvUpdate(&fmVoice[i].op[j]);
            }
        }
    }
}

void FmSynth_ChannelSettingDump(uint8_t /*ch*/, float value)
{
    if (value > 0)
    {
        printf("setting->algo = %d;\n", currentChSetting->algo);
        printf("setting->fmFeedback =  %0.6f;\n", currentChSetting->fmFeedback);
        for (int i = 0; i < 4; i++)
        {
            printf("setting->op_prop[%d].ar = %u;\n", i, currentChSetting->op_prop[i].ar);
            printf("setting->op_prop[%d].d1r = %u;\n", i, currentChSetting->op_prop[i].d1r);
            printf("setting->op_prop[%d].d2l = %0.6f;\n", i, currentChSetting->op_prop[i].d2l);
            printf("setting->op_prop[%d].d2r = %u;\n", i, currentChSetting->op_prop[i].d2r);
            printf("setting->op_prop[%d].rr = %u;\n", i, currentChSetting->op_prop[i].rr);
            printf("setting->op_prop[%d].rs = %u;\n", i, currentChSetting->op_prop[i].rs);
            printf("setting->op_prop[%d].tl = %0.6f;\n", i, currentChSetting->op_prop[i].tl);
            printf("setting->op_prop[%d].mul = %0.6f;\n", i, currentChSetting->op_prop[i].mul);
            printf("setting->op_prop[%d].vel_to_tl = %0.6f;\n", i, currentChSetting->op_prop[i].vel_to_tl);
            printf("setting->op_prop[%d].am = %0.6f;\n", i, currentChSetting->op_prop[i].am);
            printf("setting->op_prop[%d].mw = %0.6f;\n", i, currentChSetting->op_prop[i].mw);
            printf("setting->op_prop[%d].vel = %0.6f;\n", i, currentChSetting->op_prop[i].vel);
        }
    }
}

void FmSynth_ChannelSettingInit(uint8_t /*ch*/, float value)
{
    if (value > 0)
        initChannelSetting = true;
    else
        initChannelSetting = false;
}

void FmSynth_PitchBend(uint8_t /*ch*/, float bend)
{
    pitchBendValue = bend;
}

void FmSynth_ModulationWheel(uint8_t /*ch*/, float value)
{
    modulationDepth = value;
}

void FmSynth_ToggleMono(uint8_t /*param*/, float value)
{
    if (value > 0)
        currentChSetting->mono = !currentChSetting->mono;
}

void FmSynth_ToggleLegato(uint8_t /*param*/, float value)
{
    if (value > 0)
        currentChSetting->legato = !currentChSetting->legato;
}

void FmSynth_SelectOp(uint8_t param, float value)
{
    if (value > 0)
    {
        if (param < 4)
            selectedOp = param;
        Status_ValueChangedInt("selectedOp", 4 - selectedOp);
    }
}

void FmSynth_SetAlgorithm(uint8_t param, float value)
{
    if (value > 0)
    {
        currentChSetting->algo = param;
        Status_ValueChangedInt("Algorithm", currentChSetting->algo);
    }
}

void FmSynth_ChangeParam(uint8_t param, float value)
{
    switch (param)
    {
    case 0:
        currentChSetting->op_prop[selectedOp].tl = value;
        Status_ValueChangedFloatArr("op_tl", currentChSetting->op_prop[selectedOp].tl, 4 - selectedOp);
        break;
    case 1:
    {
        uint32_t u32 = (uint32_t)(value * 31);
        float mul_c = (u32 > 0) ? (float)u32 : 0.5f;
        currentChSetting->op_prop[selectedOp].mul_coarse = mul_c;
        currentChSetting->op_prop[selectedOp].mul = currentChSetting->op_prop[selectedOp].mul_coarse + currentChSetting->op_prop[selectedOp].mul_fine;
        Status_ValueChangedFloatArr("op_mul", currentChSetting->op_prop[selectedOp].mul, 4 - selectedOp);
    }
    break;
    case 2:
    {
        uint32_t u32 = (uint32_t)(value * 100);
        currentChSetting->op_prop[selectedOp].mul_fine = (float)u32 * 0.01f;
        currentChSetting->op_prop[selectedOp].mul = currentChSetting->op_prop[selectedOp].mul_coarse + currentChSetting->op_prop[selectedOp].mul_fine;
        Status_ValueChangedFloatArr("op_mul", currentChSetting->op_prop[selectedOp].mul, 4 - selectedOp);
    }
    break;
    }
}

void FmSynth_VelToLev(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].vel_to_tl = value;
    Status_ValueChangedFloatArr("vel_to_tl", currentChSetting->op_prop[selectedOp].vel_to_tl, 4 - selectedOp);
}

void FmSynth_LfoAM(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].am = value;
    Status_ValueChangedFloatArr("op_lfo_am", currentChSetting->op_prop[selectedOp].am, 4 - selectedOp);
}

void FmSynth_LfoFM(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].mw = value;
    Status_ValueChangedFloatArr("op_lfo_mw", currentChSetting->op_prop[selectedOp].mw, 4 - selectedOp);
}

void FmSynth_Attack(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].ar = (uint32_t)std::pow(2.0f, value * 15.0f);
    Status_ValueChangedFloatArr("op_attackRate", (float)currentChSetting->op_prop[selectedOp].ar, 4 - selectedOp);
}

void FmSynth_Decay1(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].d1r = (uint32_t)std::pow(2.0f, value * 15.0f);
    Status_ValueChangedFloatArr("op_decay1rate", (float)currentChSetting->op_prop[selectedOp].d1r, 4 - selectedOp);
}

void FmSynth_DecayL(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].d2l = value;
    Status_ValueChangedFloatArr("op_decay2level", currentChSetting->op_prop[selectedOp].d2l, 4 - selectedOp);
}

void FmSynth_Decay2(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].d2r = (uint32_t)std::pow(2.0f, value * 15.0f);
    Status_ValueChangedFloatArr("op_decay2rate", (float)currentChSetting->op_prop[selectedOp].d2r, 4 - selectedOp);
}

void FmSynth_Release(uint8_t /*unused*/, float value)
{
    currentChSetting->op_prop[selectedOp].rr = (uint32_t)std::pow(2.0f, value * 12.0f);
    Status_ValueChangedFloatArr("op_releaseRate", (float)currentChSetting->op_prop[selectedOp].rr, 4 - selectedOp);
}

void FmSynth_Feedback(uint8_t /*unused*/, float value)
{
    currentChSetting->fmFeedback = value;
    Status_ValueChangedFloat("feedback", currentChSetting->fmFeedback);
}
