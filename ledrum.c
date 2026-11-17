/*
 * LEDrum - a drum voice for the Sidecar (Analog Rytm expander), as an RMP user machine.
 *
 * A modified version of the drum voice in the open-source Erica Synths LXR-02 firmware
 * (Copyright (C) 2012-2026 Erica Synths, GPL-3.0-only). Not affiliated with, endorsed by or
 * approved by Erica Synths. "Erica Synths", "LXR" and "LXR-02" are trademarks of Erica Synths.
 *
 * Changes from the original: rewritten for float processing at 48 kHz; new amp and pitch
 * envelopes, a new state-variable filter and a new knob mapping; links the original wavetable
 * and transient sample data unchanged.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License version 3 as published by the Free Software Foundation.
 * It is distributed WITHOUT ANY WARRANTY; see <https://www.gnu.org/licenses/gpl-3.0.html>.
 */

#include "rmp/rmp_api.h"
#include "rmp/rmp_api3.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

/* Self-contained math (no libm): the Sidecar image cannot link the host C library. */
static inline float fm_fabsf(float x) { union { float f; uint32_t u; } v = { x }; v.u &= 0x7fffffffu; return v.f; }
/* 2^x: split into integer and fractional parts, 5th-order polynomial for the fraction (~1e-7 rel. error) */
static inline float fm_exp2f(float x) {
    if (x < -126.0f) return 0.0f;
    if (x > 127.0f) x = 127.0f;
    float fl = (float)(int32_t)x; if (fl > x) fl -= 1.0f;
    float f = x - fl;
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union { float f; uint32_t u; } v; v.u = (uint32_t)((int32_t)fl + 127) << 23;
    return p * v.f;
}

/* ------------------------------------------------------------------ */
/*  Build-time constants                                               */
/* ------------------------------------------------------------------ */

#define FMDRUM_SR        48000.0f   /* Sidecar sample rate             */
#define CTL_RATE_FRAMES  64         /* control-rate boundary (LXR=64)  */
#define SINE_TABLE_SIZE  4096
#define WT_TABLE_SIZE    1024
#define WT_NUM_OCTAVES   11
#define NUM_TRANSIENTS   12
#define TRANSIENT_LEN    2205

/* Phase accumulator constants (matching LXR's 32-bit phase scheme)   */
#define SINE_FRAC_BITS   20         /* 4096 << 20 = full 32-bit       */
#define WT_FRAC_BITS     22         /* 1024 << 22 = full 32-bit       */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/*  Flush-to-zero (AArch64 denormal avoidance)                         */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__)
static inline uint64_t ftz_on(void) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr | (1ULL << 24)));
    return fpcr;
}
static inline void ftz_off(uint64_t prev) {
    __asm__ volatile("msr fpcr, %0" :: "r"(prev));
}
#else
static inline uint64_t ftz_on(void) { return 0; }
static inline void ftz_off(uint64_t prev) { (void)prev; }
#endif

/* ------------------------------------------------------------------ */
/*  Parameter indices (P1..P16)                                        */
/* ------------------------------------------------------------------ */

enum {
    P_COARSE      = 0,   /* P1  main osc coarse tune      0..127 */
    P_FINE        = 1,   /* P2  main osc fine tune         0..127 */
    P_WAVEFORM    = 2,   /* P3  main osc waveform          0..5   */
    P_MOD_COARSE  = 3,   /* P4  mod osc coarse tune        0..127 */
    P_FM_AMOUNT   = 4,   /* P5  FM modulation amount       0..127 */
    P_MIX_MODE    = 5,   /* P6  osc mix mode (FM/add)      0..1   */
    P_FILT_FREQ   = 6,   /* P7  filter frequency           0..127 */
    P_FILT_RESO   = 7,   /* P8  filter resonance           0..127 */
    P_FILT_TYPE   = 8,   /* P9  filter type                0..7   */
    P_EG_ATTACK   = 9,   /* P10 amp EG attack              0..127 */
    P_EG_DECAY    = 10,  /* P11 amp EG decay               0..127 */
    P_EG_SLOPE    = 11,  /* P12 amp EG slope shape         0..127 */
    P_PITCH_DECAY = 12,  /* P13 pitch EG decay             0..127 */
    P_PITCH_AMT   = 13,  /* P14 pitch EG amount            0..127 */
    P_TRANS_WF    = 14,  /* P15 transient waveform         0..13  */
    P_DISTORTION  = 15,  /* P16 distortion amount          0..127 */
};

/* ------------------------------------------------------------------ */
/*  MIDI note to frequency table (128 entries, standard A440 tuning)   */
/* ------------------------------------------------------------------ */

static float MidiNoteFrequencies[128];

static void init_midi_freq_table(void) {
    for (int i = 0; i < 128; i++) {
        MidiNoteFrequencies[i] = 440.0f * fm_exp2f((i - 69) / 12.0f);
    }
}

/* ------------------------------------------------------------------ */
/*  Wavetable data — converted from LXR int16 tables to float at init  */
/*  We store them as static arrays; init_tables() fills them.          */
/*  The LXR originals are extern'd from the LXR source files.         */
/* ------------------------------------------------------------------ */

/* Forward declarations for LXR data (linked from lxr_tables.c) */
extern const int16_t sine_table[SINE_TABLE_SIZE + 1];
extern const int16_t sawTable[WT_NUM_OCTAVES][WT_TABLE_SIZE];
extern const int16_t triTable[WT_NUM_OCTAVES][WT_TABLE_SIZE];
extern const int16_t recTable[WT_NUM_OCTAVES][WT_TABLE_SIZE];
extern const int8_t  transientData[NUM_TRANSIENTS][TRANSIENT_LEN];
extern const float   transientVolumeTable[69];

/* Float versions of the wavetables (normalized +-1.0) */
static float f_sine[SINE_TABLE_SIZE + 1];
static float f_saw[WT_NUM_OCTAVES][WT_TABLE_SIZE + 1]; /* +1 for interp wrap */
static float f_tri[WT_NUM_OCTAVES][WT_TABLE_SIZE + 1];
static float f_rec[WT_NUM_OCTAVES][WT_TABLE_SIZE + 1];
static float f_transient[NUM_TRANSIENTS][TRANSIENT_LEN];

static int tables_initialized = 0;

static void init_tables(void) {
    if (tables_initialized) return;
    const float inv16 = 1.0f / 32768.0f;
    const float inv8  = 1.0f / 128.0f;

    for (int i = 0; i <= SINE_TABLE_SIZE; i++)
        f_sine[i] = sine_table[i] * inv16;

    for (int oct = 0; oct < WT_NUM_OCTAVES; oct++) {
        for (int i = 0; i < WT_TABLE_SIZE; i++) {
            f_saw[oct][i] = sawTable[oct][i] * inv16;
            f_tri[oct][i] = triTable[oct][i] * inv16;
            f_rec[oct][i] = recTable[oct][i] * inv16;
        }
        /* wrap point for interpolation */
        f_saw[oct][WT_TABLE_SIZE] = f_saw[oct][0];
        f_tri[oct][WT_TABLE_SIZE] = f_tri[oct][0];
        f_rec[oct][WT_TABLE_SIZE] = f_rec[oct][0];
    }

    for (int t = 0; t < NUM_TRANSIENTS; t++)
        for (int i = 0; i < TRANSIENT_LEN; i++)
            f_transient[t][i] = transientData[t][i] * inv8;

    init_midi_freq_table();
    tables_initialized = 1;
}

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

static inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* Waveshaper: (1+k)*x / (1+k*|x|) — used throughout LXR */
static inline float waveshape(float x, float k) {
    return (1.0f + k) * x / (1.0f + k * fm_fabsf(x));
}

/* ------------------------------------------------------------------ */
/*  Waveform enum                                                      */
/* ------------------------------------------------------------------ */

enum {
    WF_SINE = 0,
    WF_TRI  = 1,
    WF_SAW  = 2,
    WF_REC  = 3,
    WF_NOISE = 4,
    WF_PWM  = 5,
    NUM_WAVEFORMS = 6
};

/* ------------------------------------------------------------------ */
/*  Oscillator (float port of LXR Oscillator.c)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t phase;        /* 32-bit phase accumulator */
    uint32_t phaseInc;     /* per-sample increment     */
    float    freq;         /* Hz                       */
    uint8_t  waveform;
    int      tableOffset;  /* anti-aliasing octave select for wavetable */
    uint32_t noiseVal;     /* current noise output     */
    uint32_t noiseSeed;    /* LFSR state               */
    float    pwm;          /* PWM duty cycle offset    */
    float    modNodeValue; /* for external mod sources */
} Osc;

static void osc_init(Osc *o) {
    memset(o, 0, sizeof(*o));
    o->noiseSeed = 12345;
    o->waveform = WF_SINE;
}

/* Simple xorshift noise */
static inline uint32_t noise_next(uint32_t *seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

/* Phase increment for 4096-entry sine table at FMDRUM_SR */
static inline uint32_t freq2phaseInc_sine(float f) {
    /* (4096 * f / SR) * 2^20 */
    return (uint32_t)(f * (4096.0f / FMDRUM_SR) * 1048576.0f);
}

/* Phase increment for 1024-entry wavetable at FMDRUM_SR */
static inline uint32_t freq2phaseInc_wt(float f) {
    /* (1024 * f / SR) * 2^22 */
    return (uint32_t)(f * (1024.0f / FMDRUM_SR) * 4194304.0f);
}

/* Select anti-aliasing octave band for wavetable */
static int osc_calcTableOffset(float freq) {
    /* LXR uses hardcoded thresholds matching its wavetable octave bands */
    if (freq < 16.35f)   return 0;
    if (freq < 32.70f)   return 1;
    if (freq < 65.41f)   return 2;
    if (freq < 130.81f)  return 3;
    if (freq < 261.63f)  return 4;
    if (freq < 523.25f)  return 5;
    if (freq < 1046.5f)  return 6;
    if (freq < 2093.0f)  return 7;
    if (freq < 4186.0f)  return 8;
    if (freq < 8372.0f)  return 9;
    return 10;
}

static void osc_setFreq(Osc *o) {
    if (o->waveform == WF_SINE) {
        o->phaseInc = freq2phaseInc_sine(o->freq);
    } else if (o->waveform == WF_NOISE) {
        o->phaseInc = freq2phaseInc_sine(o->freq); /* use sine rate for noise clock */
    } else {
        o->tableOffset = osc_calcTableOffset(o->freq);
        o->phaseInc = freq2phaseInc_wt(o->freq);
    }
}

static void osc_setBaseNote(Osc *o, uint8_t note, float fine) {
    /* fine: 0..1 maps to 0..~2 semitones of detuning */
    float cent = 1.0f + fine * 0.00117f; /* ~0.12 semitone at fine=1.0 */
    if (note > 127) note = 127;
    o->freq = MidiNoteFrequencies[note] * cent;
    osc_setFreq(o);
}

/* Read one sample from the oscillator (no FM) */
static float osc_readSample(Osc *o) {
    float out = 0.0f;

    switch (o->waveform) {
    case WF_SINE: {
        uint32_t idx = o->phase >> SINE_FRAC_BITS;
        float frac = (float)(o->phase & ((1 << SINE_FRAC_BITS) - 1)) / (float)(1 << SINE_FRAC_BITS);
        out = f_sine[idx] + frac * (f_sine[idx + 1] - f_sine[idx]);
        break;
    }
    case WF_TRI: {
        int oct = o->tableOffset;
        uint32_t idx = o->phase >> WT_FRAC_BITS;
        float frac = (float)(o->phase & ((1 << WT_FRAC_BITS) - 1)) / (float)(1 << WT_FRAC_BITS);
        out = f_tri[oct][idx] + frac * (f_tri[oct][idx + 1] - f_tri[oct][idx]);
        break;
    }
    case WF_SAW: {
        int oct = o->tableOffset;
        uint32_t idx = o->phase >> WT_FRAC_BITS;
        float frac = (float)(o->phase & ((1 << WT_FRAC_BITS) - 1)) / (float)(1 << WT_FRAC_BITS);
        out = f_saw[oct][idx] + frac * (f_saw[oct][idx + 1] - f_saw[oct][idx]);
        break;
    }
    case WF_REC: {
        int oct = o->tableOffset;
        uint32_t idx = o->phase >> WT_FRAC_BITS;
        float frac = (float)(o->phase & ((1 << WT_FRAC_BITS) - 1)) / (float)(1 << WT_FRAC_BITS);
        out = f_rec[oct][idx] + frac * (f_rec[oct][idx + 1] - f_rec[oct][idx]);
        break;
    }
    case WF_NOISE: {
        /* Noise: new random value when phase overflows */
        /* the noise clock runs 32x the pitch (capped at one new value per sample): low notes give
           grainy noise, high notes white hiss */
        uint32_t prev = o->phase;
        uint64_t inc = (uint64_t)o->phaseInc * 32u;
        if (inc > 0xffffffffu) { o->noiseVal = noise_next(&o->noiseSeed); return (float)(int32_t)o->noiseVal / 2147483648.0f; }
        o->phase += (uint32_t)inc;
        if (o->phase < prev) { /* overflow */
            o->noiseVal = noise_next(&o->noiseSeed);
        }
        out = (float)(int32_t)(o->noiseVal) / 2147483648.0f;
        return out; /* skip normal phase advance below */
    }
    case WF_PWM: {
        /* Two saws with phase offset, subtracted */
        int oct = o->tableOffset;
        uint32_t pwmOffset = (uint32_t)(o->pwm * 2147483648.0f); /* pwm * UINT32_MAX/2 */
        uint32_t p1 = o->phase;
        uint32_t p2 = o->phase + pwmOffset;
        uint32_t idx1 = p1 >> WT_FRAC_BITS;
        uint32_t idx2 = p2 >> WT_FRAC_BITS;
        float frac1 = (float)(p1 & ((1 << WT_FRAC_BITS) - 1)) / (float)(1 << WT_FRAC_BITS);
        float frac2 = (float)(p2 & ((1 << WT_FRAC_BITS) - 1)) / (float)(1 << WT_FRAC_BITS);
        float s1 = f_saw[oct][idx1] + frac1 * (f_saw[oct][idx1 + 1] - f_saw[oct][idx1]);
        float s2 = f_saw[oct][idx2] + frac2 * (f_saw[oct][idx2 + 1] - f_saw[oct][idx2]);
        out = clampf(s1 - s2, -1.0f, 1.0f);
        break;
    }
    default:
        break;
    }

    o->phase += o->phaseInc;
    return out;
}

/* Read with FM: phase offset from modulator buffer */
static float osc_readSampleFM(Osc *o, float fmMod) {
    float out = 0.0f;
    /* Convert FM mod to phase offset.
     * LXR uses: (modBuf * fmMod) << 17 for sine, << 19 for wavetable.
     * In float domain: fmMod is already in +-1 range from modulator,
     * so the phase offset is proportional to fmMod * fmAmount.
     * We express this as a uint32_t phase offset. */

    if (o->waveform == WF_SINE) {
        /* Phase offset in sine domain: fmMod is +-1.0, map to +-2^31 worth of phase */
        int32_t phaseOff = (int32_t)(fmMod * 2147483648.0f * 0.5f); /* scale down for musicality */
        uint32_t p = o->phase + (uint32_t)phaseOff;
        uint32_t idx = p >> SINE_FRAC_BITS;
        idx &= (SINE_TABLE_SIZE - 1);
        float frac = (float)(p & ((1 << SINE_FRAC_BITS) - 1)) / (float)(1 << SINE_FRAC_BITS);
        out = f_sine[idx] + frac * (f_sine[idx + 1 <= SINE_TABLE_SIZE ? idx + 1 : 0] - f_sine[idx]);
    } else if (o->waveform != WF_NOISE) {
        /* Wavetable FM */
        int oct = o->tableOffset;
        int32_t phaseOff = (int32_t)(fmMod * 2147483648.0f * 0.125f);
        uint32_t p = o->phase + (uint32_t)phaseOff;
        uint32_t idx = p >> WT_FRAC_BITS;
        idx &= (WT_TABLE_SIZE - 1);
        float frac = (float)(p & ((1 << WT_FRAC_BITS) - 1)) / (float)(1 << WT_FRAC_BITS);
        const float *tbl;
        switch (o->waveform) {
            case WF_TRI: tbl = f_tri[oct]; break;
            case WF_SAW: tbl = f_saw[oct]; break;
            case WF_REC: tbl = f_rec[oct]; break;
            case WF_PWM: tbl = f_saw[oct]; break;
            default:     tbl = f_sine;     break;
        }
        out = tbl[idx] + frac * (tbl[idx + 1 <= WT_TABLE_SIZE ? idx + 1 : 0] - tbl[idx]);
    } else {
        out = osc_readSample(o); /* noise ignores FM */
    }

    o->phase += o->phaseInc;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Math helpers (no libm on the Sidecar)                              */
/* ------------------------------------------------------------------ */

/* log2(x) for x > 0: exponent bits, then log2(m) for m in [1,2) from the atanh series in t = (m-1)/(m+1)
   (|t| <= 1/3; error under 2e-6, continuous across powers of two) */
static inline float fm_log2f(float x) {
    union { float f; uint32_t u; } v = { x };
    float e = (float)(int32_t)((v.u >> 23) & 0xff) - 127.0f;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    float t = (v.f - 1.0f) / (v.f + 1.0f), t2 = t * t;
    return e + 2.8853901f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * (0.14285714f + t2 * 0.11111111f))));
}

/* tan(x) for x in [0, 1.45]: half-angle identity over a [3/2] Pade of tan(x/2) */
static inline float fm_tanf(float x) {
    float h = 0.5f * x, h2 = h * h;
    float t = h * (15.0f - h2) / (15.0f - 6.0f * h2);
    return 2.0f * t / (1.0f - t * t);
}

/* knob 0..127 -> lo .. lo*2^oct on an exponential curve */
static inline float knob_exp(float k, float lo, float oct) {
    return lo * fm_exp2f(clampf(k, 0.0f, 127.0f) * (oct / 127.0f));
}

/* per-sample multiplier that decays to 1/1000 (-60 dB) in t seconds */
static inline float decay_coef(float t) {
    return fm_exp2f(-9.9657843f / (t * FMDRUM_SR));
}

/* ------------------------------------------------------------------ */
/*  Amp envelope: linear attack, decay with a shapeable curve          */
/* ------------------------------------------------------------------ */

enum { AMP_IDLE = 0, AMP_ATTACK, AMP_DECAY };

typedef struct {
    int   stage;
    float level;      /* attack: 0..1 */
    float pos;        /* decay: 1 -> 0, linear in time */
    float attInc;     /* per sample, 1 = instant */
    float decInc;     /* per sample */
    float curve;      /* output = pos^curve: >1 snappy, 1 linear, <1 full then drops */
} AmpEnv;

static void ampEnv_trigger(AmpEnv *e) {
    e->stage = (e->attInc >= 1.0f) ? AMP_DECAY : AMP_ATTACK;
    e->level = 0.0f;
    e->pos = 1.0f;
}

static inline float ampEnv_next(AmpEnv *e) {
    switch (e->stage) {
    case AMP_ATTACK:
        e->level += e->attInc;
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = AMP_DECAY; }
        return e->level;
    case AMP_DECAY: {
        float p = e->pos;
        e->pos -= e->decInc;
        if (e->pos <= 0.0f) { e->pos = 0.0f; e->stage = AMP_IDLE; }
        if (p <= 0.0f) return 0.0f;
        if (e->curve == 1.0f) return p;
        return fm_exp2f(e->curve * fm_log2f(p));
    }
    default:
        return 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/*  Transient generator: the LXR's 12 PCM attack samples               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t phase;
    uint32_t inc;       /* 20.12-ish fixed point: 1<<20 = one table step */
    uint8_t  waveform;  /* 0 off, 1 click (pitch snap), 2..13 samples */
    int      active;
} TransGen;

static inline float transGen_next(TransGen *tg) {
    if (!tg->active || tg->waveform < 2) return 0.0f;
    uint32_t idx = tg->phase >> 20;
    if (idx >= TRANSIENT_LEN) { tg->active = 0; return 0.0f; }
    int volIdx = (int)(idx >> 5);
    if (volIdx > 68) volIdx = 68;
    int wf = tg->waveform - 2;
    if (wf >= NUM_TRANSIENTS) wf = NUM_TRANSIENTS - 1;
    float out = f_transient[wf][idx] * transientVolumeTable[volIdx];
    tg->phase += tg->inc;
    return out;
}

/* ------------------------------------------------------------------ */
/*  State-variable filter (TPT / zero-delay feedback)                  */
/* ------------------------------------------------------------------ */

enum { FILT_OFF = 0, FILT_LP, FILT_HP, FILT_BP, FILT_UBP, FILT_NOTCH };

typedef struct {
    float ic1, ic2;     /* integrator states */
    float a1, a2, a3;   /* coefficients */
    float k;            /* damping: 2 = no resonance, small = ringing */
    int   type;
} SVFilter;

static void svf_set(SVFilter *f, float hz, float reso01) {
    float fc = clampf(hz, 20.0f, 0.45f * FMDRUM_SR);
    float g = fm_tanf((float)M_PI * fc / FMDRUM_SR);
    f->k = 2.0f - 1.94f * clampf(reso01, 0.0f, 1.0f);
    f->a1 = 1.0f / (1.0f + g * (g + f->k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

static inline float svf_process(SVFilter *f, float in) {
    if (f->type == FILT_OFF) return in;
    float v3 = in - f->ic2;
    float v1 = f->a1 * f->ic1 + f->a2 * v3;      /* band */
    float v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;  /* low */
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    switch (f->type) {
    case FILT_LP:    return v2;
    case FILT_HP:    return in - f->k * v1 - v2;
    case FILT_BP:    return v1;
    case FILT_UBP:   return f->k * v1;
    case FILT_NOTCH: return in - f->k * v1;
    default:         return v2;
    }
}

/* ------------------------------------------------------------------ */
/*  Distortion (the LXR waveshaper)                                    */
/* ------------------------------------------------------------------ */

static inline float dist_shape_from_knob(float k) {
    float p = clampf(k, 0.0f, 127.0f) / 127.0f - 0.0078125f;
    if (p < 0.0f) p = 0.0f;
    if (p > 0.98f) p = 0.98f;
    return 2.0f * p / (1.0f - p);
}

/* ------------------------------------------------------------------ */
/*  Plugin state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    Osc       mainOsc;
    Osc       modOsc;
    AmpEnv    amp;
    TransGen  trans;
    SVFilter  filter;

    float     fmAmount;      /* 0..1.9 phase-offset depth */
    int       mixOscs;       /* 0 FM, 1 additive */
    float     distShape;

    float     pitchEnv;      /* 1 -> 0 */
    float     pitchCoef;
    float     pitchAmt;      /* semitones at the start of the hit */
    float     snapEnv;       /* the "Offset"/click transient: a fast pitch spike */
    float     snapCoef;

    float     transpose;     /* note - 60, semitones */
    float     baseSemi;      /* coarse + fine, as a MIDI note number */
    float     modSemi;
    float     velocity;
    float     volume;

    float     p[16];         /* knobs, 0..127 */
} FmDrumState;

static const float kDefaults[16] = {
    36.0f,   /* P1  coarse: C2 */
    64.0f,   /* P2  fine: centre */
    0.0f,    /* P3  waveform: sine */
    48.0f,   /* P4  mod osc note */
    20.0f,   /* P5  FM amount */
    0.0f,    /* P6  mix: FM */
    90.0f,   /* P7  filter freq (~4 kHz) */
    10.0f,   /* P8  filter reso */
    1.0f,    /* P9  filter: LP */
    0.0f,    /* P10 attack: instant */
    70.0f,   /* P11 decay (~300 ms) */
    80.0f,   /* P12 slope: a little snappy */
    40.0f,   /* P13 pitch decay (~25 ms) */
    50.0f,   /* P14 pitch amount (~24 semitones) */
    0.0f,    /* P15 transient: off */
    0.0f,    /* P16 distortion: off */
};

/* ------------------------------------------------------------------ */
/*  Knobs -> DSP                                                       */
/* ------------------------------------------------------------------ */

static void osc_setPitch(Osc *o, float semi) {
    o->freq = 440.0f * fm_exp2f((semi - 69.0f) * (1.0f / 12.0f));
    if (o->freq > 0.45f * FMDRUM_SR) o->freq = 0.45f * FMDRUM_SR;
    osc_setFreq(o);
}

static void map_params(FmDrumState *s) {
    const float *p = s->p;

    s->baseSemi = clampf(p[P_COARSE], 0.0f, 127.0f) + (clampf(p[P_FINE], 0.0f, 127.0f) - 64.0f) / 64.0f;
    s->mainOsc.waveform = (uint8_t)clampf(p[P_WAVEFORM], 0.0f, NUM_WAVEFORMS - 1);
    s->mainOsc.pwm = 0.5f;
    s->modSemi = clampf(p[P_MOD_COARSE], 0.0f, 127.0f);
    s->modOsc.waveform = WF_SINE;
    osc_setPitch(&s->modOsc, s->modSemi + s->transpose);

    float fm = clampf(p[P_FM_AMOUNT], 0.0f, 127.0f) / 127.0f;
    s->fmAmount = fm * fm * 1.9f;
    s->mixOscs = (p[P_MIX_MODE] > 0.5f) ? 1 : 0;

    s->filter.type = (int)clampf(p[P_FILT_TYPE] + 0.5f, 0.0f, 5.0f);
    svf_set(&s->filter, knob_exp(p[P_FILT_FREQ], 30.0f, 9.3f), p[P_FILT_RESO] / 127.0f);

    /* attack: 0 = instant, else 0.2 ms .. ~300 ms */
    if (p[P_EG_ATTACK] < 0.5f) s->amp.attInc = 1.0f;
    else s->amp.attInc = 1.0f / (knob_exp(p[P_EG_ATTACK], 0.0002f, 10.5f) * FMDRUM_SR);
    /* decay: 8 ms .. 5 s */
    s->amp.decInc = 1.0f / (knob_exp(p[P_EG_DECAY], 0.008f, 9.29f) * FMDRUM_SR);
    /* slope: 0 = very snappy (^8), 64 = linear, 127 = full body then a drop (^1/8) */
    s->amp.curve = fm_exp2f((clampf(p[P_EG_SLOPE], 0.0f, 127.0f) - 64.0f) * (3.0f / 64.0f));

    /* pitch sweep: 3 ms .. 1.5 s, up to 72 semitones */
    s->pitchCoef = decay_coef(knob_exp(p[P_PITCH_DECAY], 0.003f, 9.0f));
    s->pitchAmt = clampf(p[P_PITCH_AMT], 0.0f, 127.0f) * (72.0f / 127.0f);

    s->trans.waveform = (uint8_t)clampf(p[P_TRANS_WF] + 0.5f, 0.0f, NUM_TRANSIENTS + 1);
    s->distShape = dist_shape_from_knob(p[P_DISTORTION]);
}

/* ------------------------------------------------------------------ */
/*  Audio                                                              */
/* ------------------------------------------------------------------ */

static void render_block(FmDrumState *s, float *out, int n) {
    if (s->amp.stage == AMP_IDLE && !s->trans.active) {
        memset(out, 0, (size_t)n * sizeof(float));
        return;
    }
    for (int i = 0; i < n; i++) {
        /* pitch: base + sweep + click, recomputed every sample */
        float semi = s->baseSemi + s->transpose + s->pitchEnv * s->pitchAmt + s->snapEnv * 24.0f;
        s->pitchEnv *= s->pitchCoef;
        s->snapEnv *= s->snapCoef;
        osc_setPitch(&s->mainOsc, semi);

        float mod = osc_readSample(&s->modOsc);
        float osc;
        if (s->mixOscs == 0) {
            osc = osc_readSampleFM(&s->mainOsc, mod * s->fmAmount);
        } else {
            osc = osc_readSample(&s->mainOsc) + mod * s->fmAmount * 0.5f;
        }

        float x = svf_process(&s->filter, osc) * ampEnv_next(&s->amp);
        x += 0.8f * transGen_next(&s->trans);   /* the transient skips filter and envelope */
        x *= s->velocity;
        if (s->distShape > 0.001f) x = waveshape(clampf(x, -1.0f, 1.0f), s->distShape);
        out[i] = clampf(x * s->volume, -1.0f, 1.0f);
    }
}

/* ------------------------------------------------------------------ */
/*  Plugin callbacks                                                   */
/* ------------------------------------------------------------------ */

static const rmp_host *g_host;

static int32_t c_init(const rmp_host *host) {
    g_host = host;
    init_tables();
    return 0;
}

static void c_voice_init(void *state) {
    FmDrumState *s = (FmDrumState *)state;
    memset(s, 0, sizeof(*s));
    memcpy(s->p, kDefaults, sizeof(s->p));
    osc_init(&s->mainOsc);
    osc_init(&s->modOsc);
    s->amp.stage = AMP_IDLE;
    s->trans.inc = (uint32_t)((44100.0f / FMDRUM_SR) * 1048576.0f);  /* the samples are 44.1 kHz */
    s->snapCoef = decay_coef(0.006f);
    s->velocity = 1.0f;
    s->volume = 0.9f;
    map_params(s);
}

static void c_note_on(void *state, float note, float velocity) {
    FmDrumState *s = (FmDrumState *)state;
    s->transpose = clampf(note, 0.0f, 127.0f) - 60.0f;
    s->velocity = clampf(velocity, 0.0f, 1.0f);
    osc_setPitch(&s->modOsc, s->modSemi + s->transpose);

    s->mainOsc.phase = 0;
    s->modOsc.phase = 0;
    s->mainOsc.noiseSeed = 12345;
    s->filter.ic1 = s->filter.ic2 = 0.0f;

    s->pitchEnv = 1.0f;
    s->snapEnv = (s->trans.waveform == 1) ? 1.0f : 0.0f;
    s->trans.phase = 0;
    s->trans.active = (s->trans.waveform >= 2);
    ampEnv_trigger(&s->amp);
}

static void c_note_off(void *state) {
    (void)state;   /* a drum: the decay ends the hit */
}

static void c_params(void *state, const float *p) {
    FmDrumState *s = (FmDrumState *)state;
    memcpy(s->p, p, 16 * sizeof(float));
    map_params(s);
}

/* API2 render (mono) */
static void c_render2(void *state, float *out, int32_t n) {
    FmDrumState *s = (FmDrumState *)state;
    if (n <= 0) return;
    uint64_t fpcr = ftz_on();
    render_block(s, out, n);
    ftz_off(fpcr);
}

/* API3 render (the same mono voice on both channels) */
static void c_render3(void *state, const rmp3_block *b) {
    FmDrumState *s = (FmDrumState *)state;
    int n = b->n;
    if (n <= 0) return;
    uint64_t fpcr = ftz_on();
    if (b->p && b->p_changed) {
        memcpy(s->p, b->p, 16 * sizeof(float));
        map_params(s);
    }
    render_block(s, b->out[0], n);
    if (b->out[1] && !(b->flags & RMP3_B_MONO_OUT))
        memcpy(b->out[1], b->out[0], (size_t)n * sizeof(float));
    ftz_off(fpcr);
}

static void c_attach(void *state, const rmp3_voice *voice) {
    (void)state; (void)voice;
}

/* ------------------------------------------------------------------ */
/*  CPU budget — estimate ~25% for this voice                          */
/* ------------------------------------------------------------------ */

#define FMDRUM_PERMILLE 250

/* ------------------------------------------------------------------ */
/*  Plugin tables                                                      */
/* ------------------------------------------------------------------ */

static const rmp3_plugin kPlugin3 = {
    RMP3_MAGIC,
    sizeof(rmp3_plugin),
    RMP3_MINOR,
    RMP3_KIND_MACHINE,
    0,                          /* caps: no arena needed */
    RMP3_CAP_GUARD | RMP3_CAP_STEREO | RMP3_CAP_BLOCK_P,
    0, 0,                       /* arena_min, arena_max */
    0, 0,                       /* arena_align, arena_guard */
    8, 0,                       /* max_latency, reserved */
    2,                          /* out_channels (stereo) */
    0, 0,                       /* in_channels, aux_channels */
    RMP3_AT_VOICE,
    0, 0,                       /* reserved */
    {0, 0, 0, 0, 0, 0},        /* reserved */
    NULL, NULL,                 /* init, reserved */
    NULL,                       /* voice_init */
    c_attach,
    c_render3,
    NULL,                       /* idle */
    {NULL, NULL, NULL, NULL},   /* reserved */
};

RMP_ENTRY const rmp_plugin rmp_entry = {
    RMP_PLUGIN_MAGIC,
    RMP_API3,
    sizeof(FmDrumState),
    FMDRUM_PERMILLE,
    c_init,
    c_voice_init,
    c_note_on,
    c_note_off,
    c_params,
    c_render2,
    {NULL, NULL, NULL, NULL, RMP_FN(&kPlugin3), NULL, NULL, NULL},
};
