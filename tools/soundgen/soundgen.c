/* soundgen - renders ICDA's interface sounds into resources/audio/.
 *
 *   cc -O2 -o soundgen soundgen.c -lm && ./soundgen ../../resources/audio
 *
 * Every sound is synthesized here, nothing is sampled.  The palette is meant
 * to feel like material rather than like a synthesizer: rounded glass tones
 * with quiet inharmonic overtones, woody taps from tuned resonators, breaths
 * of filtered air and a slowly shimmering pad ("aurora"), all placed in the
 * same space - pre-delay, early reflections, a soft stereo tail and, for the
 * rarer sounds, a dark ping-pong echo.  Harmony stays in D (sus2 / add9 /
 * major 7th colours) and in the middle register; nothing is shrill.
 *
 * Frequent sounds (flyouts, toggles) are short, soft and low in level; rare
 * ones (sound on, the assistant) are allowed to bloom.  Tails are rendered
 * in full, trimmed where they fall below -66 dB and faded out smoothly.
 *
 * Output: 48 kHz, 16-bit, stereo WAV. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define TAU 6.28318530717958647692
#define PI (TAU / 2)

/* pitches (Hz) */
#define D3 146.83
#define A3 220.00
#define D4 293.66
#define E4 329.63
#define FS4 369.99
#define A4 440.00
#define B4 493.88
#define CS5 554.37
#define D5 587.33
#define E5 659.26
#define FS5 739.99
#define A5 880.00
#define B5 987.77
#define CS6 1108.73
#define D6 1174.66
#define E6 1318.51

typedef struct {
    float *l, *r;
    int n;
} buf_t;

static buf_t buf_new(double seconds) {
    buf_t b;
    b.n = (int)(seconds * SR);
    b.l = calloc((size_t)b.n, sizeof(float));
    b.r = calloc((size_t)b.n, sizeof(float));
    return b;
}

static uint32_t rng = 0x1CDA2026u;
static double noise(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (double)(rng & 0xFFFFFF) / 8388608.0 - 1.0;
}

/* equal-power pan: -1 left .. +1 right */
static void put(buf_t *b, int i, double v, double pan) {
    double a = (pan + 1) * PI / 4;
    if (i < 0 || i >= b->n) return;
    b->l[i] += (float)(v * cos(a));
    b->r[i] += (float)(v * sin(a));
}

/* raised-cosine attack, exponential decay */
static double env(double t, double attack, double decay) {
    double a = t < attack ? 0.5 - 0.5 * cos(t / attack * PI) : 1.0;
    return a * exp(-(t > attack ? t - attack : 0) / decay);
}

/* ---- voices ------------------------------------------------------------------ */

/* Glass: a round sine with a soft octave, and the inharmonic partials of a
 * struck glass that die away first.  A whisper of FM in the first
 * milliseconds gives the strike its edge without brightness. */
static void glass(buf_t *b, double t0, double f, double amp, double pan, double decay) {
    int s = (int)(t0 * SR), len = (int)((decay * 7 + 0.05) * SR);
    double p1 = 0, p2 = 0, p3 = 0, p4 = 0, pm = 0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double fm = 0.35 * exp(-t / 0.012) * sin(pm);
        double v = sin(p1 + fm) * env(t, 0.004, decay);
        v += 0.22 * sin(p2) * env(t, 0.003, decay * 0.45);
        v += 0.10 * sin(p3) * env(t, 0.002, decay * 0.22);
        v += 0.04 * sin(p4) * env(t, 0.002, decay * 0.10);
        p1 += TAU * f / SR;
        p2 += TAU * f * 2.0 / SR;
        p3 += TAU * f * 2.76 / SR;
        p4 += TAU * f * 5.40 / SR;
        pm += TAU * f * 1.5 / SR;
        put(b, s + i, v * amp, pan);
    }
}

/* Two-pole resonator, for taps */
typedef struct { double a1, a2, g, y1, y2; } reso_t;
static reso_t reso(double f, double q) {
    reso_t r;
    double w = TAU * f / SR, rad = exp(-w / (2 * q));
    r.a1 = 2 * rad * cos(w);
    r.a2 = -rad * rad;
    r.g = (1 - rad * rad) * 0.5;
    r.y1 = r.y2 = 0;
    return r;
}
static double reso_run(reso_t *r, double x) {
    double y = r->g * x + r->a1 * r->y1 + r->a2 * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* Tap: a brief excitation ringing two tuned modes, like a fingertip on a
 * small piece of wood or a phone's glass. */
static void tap(buf_t *b, double t0, double f, double amp, double pan, double q) {
    int s = (int)(t0 * SR), len = (int)(0.18 * SR);
    reso_t m1 = reso(f, q), m2 = reso(f * 2.31, q * 0.8), m3 = reso(f * 4.1, q * 0.5);
    double lp = 0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double x = t < 0.004 ? noise() * (0.5 - 0.5 * cos(t / 0.004 * TAU)) : 0;
        double v;
        lp += 0.35 * (x - lp);
        v = reso_run(&m1, lp) * 1.0 + reso_run(&m2, lp) * 0.45 + reso_run(&m3, lp) * 0.15;
        put(b, s + i, v * amp * 6.0, pan);
    }
}

/* Air: noise through a resonant band-pass sweeping f0 -> f1, swelling in
 * and out like a breath. */
static void air(buf_t *b, double t0, double dur, double f0, double f1, double amp, double pan, double drift) {
    int s = (int)(t0 * SR), len = (int)(dur * SR);
    double low = 0, band = 0, q = 1.3;
    for (int i = 0; i < len; i++) {
        double x = (double)i / len;
        double f = f0 * pow(f1 / f0, x);
        double c = 2 * sin(PI * f / SR);
        double high = noise() - low - q * band;
        double e = sin(x * PI);
        band += c * high;
        low += c * band;
        put(b, s + i, band * e * e * e * amp, pan + drift * (x - 0.5));
    }
}

/* Aurora: each note a cluster of slightly detuned sines drifting against
 * each other, with a slow swell and release - a pad that shimmers. */
static void aurora(buf_t *b, double t0, double dur, double f, double amp, double attack, double release, double spread) {
    static const double det[5] = { -0.0062, -0.0021, 0.0, 0.0024, 0.0058 };
    static const double pans[5] = { -0.8, -0.35, 0.0, 0.4, 0.85 };
    int s = (int)(t0 * SR), len = (int)((dur + release * 4) * SR);
    double ph[5] = { 0.1, 0.7, 0.3, 0.9, 0.5 };
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double a = t < attack ? 0.5 - 0.5 * cos(t / attack * PI) : 1.0;
        double e = a * (t > dur ? exp(-(t - dur) / release) : 1.0);
        for (int k = 0; k < 5; k++) {
            double vib = 1 + 0.0015 * sin(TAU * (0.21 + 0.07 * k) * t + k);
            ph[k] += TAU * f * (1 + det[k]) * vib / SR;
            put(b, s + i, (sin(ph[k]) + 0.08 * sin(2 * ph[k])) * e * amp * 0.28, pans[k] * spread);
        }
    }
}

/* ---- space -------------------------------------------------------------------- */

typedef struct { float *d; int n, i; float fb, lp, damp; } comb_t;
typedef struct { float *d; int n, i; } allpass_t;

static float comb_run(comb_t *c, float x) {
    float y = c->d[c->i];
    c->lp = y * (1 - c->damp) + c->lp * c->damp;
    c->d[c->i] = x + c->lp * c->fb;
    if (++c->i >= c->n) c->i = 0;
    return y;
}

static float allpass_run(allpass_t *a, float x) {
    float y = a->d[a->i];
    a->d[a->i] = x + y * 0.5f;
    if (++a->i >= a->n) a->i = 0;
    return y - x;
}

/* A room: pre-delay, a handful of stereo early reflections, then a
 * Freeverb-style tail.  size scales the delays, decay is the comb feedback
 * (0.80 small .. 0.90 large), damp darkens the tail, wet its level. */
static void space(buf_t *b, double size, double decay, double damp, double wet, double predelay_ms) {
    static const int cd[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static const int ad[4] = { 556, 441, 341, 225 };
    static const double er_ms[6] = { 7.1, 11.3, 16.9, 23.4, 29.8, 37.2 };
    static const double er_g[6] = { 0.42, 0.36, 0.30, 0.24, 0.19, 0.15 };
    int pre = (int)(predelay_ms * SR / 1000);
    float *wl = calloc((size_t)b->n, 4), *wr = calloc((size_t)b->n, 4);
    comb_t cl[8], cr[8];
    allpass_t al[4], ar[4];
    for (int k = 0; k < 8; k++) {
        int n = (int)(cd[k] * 48.0 / 44.1 * size);
        cl[k] = (comb_t){ calloc((size_t)n, 4), n, 0, (float)decay, 0, (float)damp };
        cr[k] = (comb_t){ calloc((size_t)n + 23, 4), n + 23, 0, (float)decay, 0, (float)damp };
    }
    for (int k = 0; k < 4; k++) {
        int n = (int)(ad[k] * 48.0 / 44.1);
        al[k] = (allpass_t){ calloc((size_t)n, 4), n, 0 };
        ar[k] = (allpass_t){ calloc((size_t)n + 23, 4), n + 23, 0 };
    }
    /* early reflections, alternating sides */
    for (int k = 0; k < 6; k++) {
        int d = pre + (int)(er_ms[k] * size * SR / 1000);
        for (int i = 0; i + d < b->n; i++) {
            float x = (k & 1 ? b->r[i] : b->l[i]) * (float)er_g[k];
            if (k & 1) wl[i + d] += x;
            else wr[i + d] += x;
        }
    }
    /* tail */
    for (int i = 0; i < b->n; i++) {
        int j = i - pre;
        float in = j >= 0 ? (b->l[j] + b->r[j]) * 0.022f : 0, ol = 0, orr = 0;
        for (int k = 0; k < 8; k++) {
            ol += comb_run(&cl[k], in);
            orr += comb_run(&cr[k], in);
        }
        for (int k = 0; k < 4; k++) {
            ol = allpass_run(&al[k], ol);
            orr = allpass_run(&ar[k], orr);
        }
        wl[i] += ol;
        wr[i] += orr;
    }
    for (int i = 0; i < b->n; i++) {
        b->l[i] = (float)(b->l[i] + wl[i] * wet);
        b->r[i] = (float)(b->r[i] + wr[i] * wet);
    }
    for (int k = 0; k < 8; k++) { free(cl[k].d); free(cr[k].d); }
    for (int k = 0; k < 4; k++) { free(al[k].d); free(ar[k].d); }
    free(wl);
    free(wr);
}

/* Ping-pong echo, each repeat darker than the last. */
static void echo(buf_t *b, double delay_ms, double feedback, double wet, double cutoff) {
    int d = (int)(delay_ms * SR / 1000);
    float *el = calloc((size_t)b->n, 4), *er = calloc((size_t)b->n, 4);
    double c = 1 - exp(-TAU * cutoff / SR), lpl = 0, lpr = 0;
    for (int i = d; i < b->n; i++) {
        /* left echo feeds right and back */
        double inl = (b->l[i - d] + b->r[i - d]) * 0.5 + er[i - d] * feedback;
        double inr = el[i - d] * feedback;
        lpl += c * (inl - lpl);
        lpr += c * (inr - lpr);
        el[i] = (float)lpl;
        er[i] = (float)lpr;
    }
    for (int i = 0; i < b->n; i++) {
        b->l[i] += (float)(el[i] * wet);
        b->r[i] += (float)(er[i] * wet);
    }
    free(el);
    free(er);
}

/* one-pole low-pass sweeping from f0 to f1 over the sound (closing) */
static void darken(buf_t *b, double f0, double f1, double over) {
    double l = 0, r = 0;
    int n = (int)(over * SR);
    for (int i = 0; i < b->n; i++) {
        double x = i < n ? (double)i / n : 1.0;
        double f = f0 * pow(f1 / f0, x);
        double c = 1 - exp(-TAU * f / SR);
        l += c * (b->l[i] - l);
        r += c * (b->r[i] - r);
        b->l[i] = (float)l;
        b->r[i] = (float)r;
    }
}

/* ---- output ---------------------------------------------------------------- */

static int no_trim;   /* loops keep their exact length */

static void finish_and_write(buf_t *b, const char *dir, const char *name, double peak_db) {
    char path[512];
    FILE *f;
    double peak = 1e-9, gain, dcl = 0, dcr = 0, pl = 0, pr = 0;
    int n = b->n, fade;
    uint32_t data_bytes;
    /* DC blocker */
    for (int i = 0; i < b->n; i++) {
        double xl = b->l[i], xr = b->r[i];
        dcl = xl - pl + 0.9995 * dcl;
        dcr = xr - pr + 0.9995 * dcr;
        pl = xl;
        pr = xr;
        b->l[i] = (float)dcl;
        b->r[i] = (float)dcr;
    }
    for (int i = 0; i < b->n; i++) {
        if (fabs(b->l[i]) > peak) peak = fabs(b->l[i]);
        if (fabs(b->r[i]) > peak) peak = fabs(b->r[i]);
    }
    /* the tail ends where it falls below -66 dB of the peak */
    if (!no_trim) {
        double floor_ = peak * pow(10, -66.0 / 20);
        while (n > SR / 20 && fabs(b->l[n - 1]) < floor_ && fabs(b->r[n - 1]) < floor_) n--;
        n += SR / 50;
        if (n > b->n) n = b->n;
    }
    fade = no_trim ? 0 : (n / 6 < SR / 8 ? n / 6 : SR / 8);
    gain = pow(10, peak_db / 20) / peak;
    data_bytes = (uint32_t)n * 4;
    snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    {
        uint8_t h[44];
        uint32_t v;
        memcpy(h, "RIFF", 4);
        v = 36 + data_bytes; memcpy(h + 4, &v, 4);
        memcpy(h + 8, "WAVEfmt ", 8);
        v = 16; memcpy(h + 16, &v, 4);
        h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
        v = SR; memcpy(h + 24, &v, 4);
        v = SR * 4; memcpy(h + 28, &v, 4);
        h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &data_bytes, 4);
        fwrite(h, 1, 44, f);
    }
    for (int i = 0; i < n; i++) {
        double g = gain;
        if (!no_trim && i < 96) g *= 0.5 - 0.5 * cos((double)i / 96 * PI);
        if (fade && i > n - fade) g *= 0.5 - 0.5 * cos((double)(n - i) / fade * PI);
        int16_t s[2];
        double l = b->l[i] * g, r = b->r[i] * g;
        /* gentle saturation instead of a hard clip */
        l = tanh(l * 1.05) / tanh(1.05);
        r = tanh(r * 1.05) / tanh(1.05);
        s[0] = (int16_t)(l * 32000);
        s[1] = (int16_t)(r * 32000);
        fwrite(s, 2, 2, f);
    }
    fclose(f);
    free(b->l);
    free(b->r);
    printf("%-22s %5.0f ms  peak %.1f dBFS\n", name, n * 1000.0 / SR, peak_db);
}

/* ---- the sounds ------------------------------------------------------------- */

/* Start menu opening: a breath rising into a soft sus2 glass chord that
 * blooms into the room, one dark echo behind it. */
static void start_open(const char *dir) {
    buf_t b = buf_new(2.0);
    air(&b, 0.0, 0.26, 600, 3200, 0.05, 0.0, 0.6);
    tap(&b, 0.055, 1250, 0.10, -0.1, 18);
    glass(&b, 0.060, A4, 0.20, -0.35, 0.30);
    glass(&b, 0.078, E5, 0.17, 0.30, 0.30);
    glass(&b, 0.096, B5, 0.10, 0.05, 0.26);
    aurora(&b, 0.05, 0.10, A4 * 2, 0.025, 0.06, 0.25, 0.9);
    echo(&b, 135, 0.28, 0.16, 2400);
    space(&b, 1.0, 0.86, 0.35, 0.55, 14);
    finish_and_write(&b, dir, "start_open", -11);
}

/* Closing: the same colour folding down and away, darker and shorter. */
static void start_close(const char *dir) {
    buf_t b = buf_new(1.6);
    air(&b, 0.0, 0.22, 2600, 500, 0.045, 0.0, -0.6);
    glass(&b, 0.020, E5, 0.15, 0.30, 0.20);
    glass(&b, 0.050, A4, 0.17, -0.30, 0.24);
    tap(&b, 0.050, 900, 0.08, -0.1, 14);
    darken(&b, 7000, 1800, 0.5);
    space(&b, 0.9, 0.84, 0.45, 0.45, 12);
    finish_and_write(&b, dir, "start_close", -13);
}

/* Taskbar flyouts: a soft tap with a small glass answer and a lift of air;
 * four variants (picked at random) differ in pitch and grain. */
static void flyouts(const char *dir) {
    static const double ping[4] = { B5, A5, CS6, FS5 };
    static const double wood[4] = { 1450, 1300, 1550, 1380 };
    static const double pans[4] = { 0.15, -0.12, 0.22, -0.2 };
    char name[32];
    for (int v = 0; v < 4; v++) {
        buf_t b = buf_new(1.0);
        tap(&b, 0.0, wood[v], 0.16, pans[v], 20);
        glass(&b, 0.012, ping[v], 0.07, -pans[v], 0.12);
        air(&b, 0.0, 0.12, 1500, 4200, 0.02, pans[v], 0.4);
        space(&b, 0.6, 0.80, 0.4, 0.38, 8);
        snprintf(name, sizeof name, "flyout_open_%d", v + 1);
        finish_and_write(&b, dir, name, -15);
    }
    for (int v = 0; v < 4; v++) {
        buf_t b = buf_new(0.9);
        tap(&b, 0.0, wood[v] * 0.72, 0.15, -pans[v], 16);
        glass(&b, 0.010, ping[v] / 2, 0.05, pans[v], 0.09);
        air(&b, 0.0, 0.10, 3200, 900, 0.018, -pans[v], -0.4);
        darken(&b, 6000, 2200, 0.2);
        space(&b, 0.55, 0.78, 0.5, 0.32, 8);
        snprintf(name, sizeof name, "flyout_close_%d", v + 1);
        finish_and_write(&b, dir, name, -17);
    }
}

/* Switches: a two-part detent - tap, then a short glass note a fifth up
 * (on) or down (off). */
static void toggles(const char *dir) {
    buf_t on = buf_new(0.9), off = buf_new(0.8);
    tap(&on, 0.0, 1700, 0.14, -0.1, 22);
    glass(&on, 0.006, E5, 0.06, -0.1, 0.07);
    tap(&on, 0.038, 2100, 0.10, 0.12, 22);
    glass(&on, 0.040, B5, 0.07, 0.12, 0.10);
    space(&on, 0.55, 0.78, 0.4, 0.30, 6);
    finish_and_write(&on, dir, "toggle_on", -16);

    tap(&off, 0.0, 1900, 0.12, 0.1, 20);
    glass(&off, 0.006, B4 * 2, 0.05, 0.1, 0.06);
    tap(&off, 0.036, 1350, 0.11, -0.12, 18);
    glass(&off, 0.038, E5, 0.06, -0.12, 0.08);
    darken(&off, 5000, 2600, 0.15);
    space(&off, 0.5, 0.76, 0.5, 0.26, 6);
    finish_and_write(&off, dir, "toggle_off", -18);
}

/* Sound on: an aurora blooms open - D add9 spread wide, glass sparkle over
 * it, echoes trailing into a long tail.  Mute: the colour closes and sinks. */
static void sound_switches(const char *dir) {
    buf_t on = buf_new(3.2), off = buf_new(2.0);
    static const double chord[4] = { D4, A4, E5, FS5 };
    for (int k = 0; k < 4; k++) aurora(&on, 0.0, 0.55, chord[k], 0.10 - 0.012 * k, 0.12 + 0.04 * k, 0.45, 1.0);
    aurora(&on, 0.0, 0.5, D3, 0.06, 0.15, 0.5, 0.3);
    air(&on, 0.0, 0.45, 500, 5000, 0.04, 0.0, 0.8);
    glass(&on, 0.09, A5, 0.08, -0.4, 0.40);
    glass(&on, 0.15, CS6, 0.06, 0.4, 0.40);
    glass(&on, 0.21, E6, 0.05, 0.0, 0.45);
    echo(&on, 180, 0.35, 0.18, 2000);
    space(&on, 1.25, 0.89, 0.3, 0.7, 20);
    finish_and_write(&on, dir, "sound_on", -10);

    aurora(&off, 0.0, 0.18, A4, 0.08, 0.02, 0.18, 0.8);
    aurora(&off, 0.0, 0.18, E5, 0.05, 0.02, 0.16, 0.8);
    glass(&off, 0.02, A4, 0.12, 0.2, 0.22);
    glass(&off, 0.10, D4, 0.15, -0.2, 0.30);
    darken(&off, 6000, 500, 0.6);
    space(&off, 1.0, 0.85, 0.5, 0.5, 14);
    finish_and_write(&off, dir, "sound_mute", -13);
}

static void assistant(const char *dir) {
    /* listen: a rising breath opening onto a bright, airy aurora - "I'm here" */
    {
        buf_t b = buf_new(3.0);
        static const double up[3] = { D5, A5, E6 };
        air(&b, 0.0, 0.40, 400, 6500, 0.06, 0.0, 0.9);
        aurora(&b, 0.12, 0.35, A4, 0.06, 0.15, 0.45, 1.0);
        aurora(&b, 0.12, 0.35, E5, 0.05, 0.18, 0.45, 1.0);
        for (int k = 0; k < 3; k++) glass(&b, 0.16 + 0.055 * k, up[k], 0.09 - 0.015 * k, -0.5 + 0.5 * k, 0.32);
        echo(&b, 160, 0.32, 0.18, 2600);
        space(&b, 1.25, 0.89, 0.3, 0.7, 20);
        finish_and_write(&b, dir, "ai_listen", -11);
    }
    /* thinking: a slow breathing shimmer.  Rendered over four periods with
     * the room; the middle two loop without a seam. */
    {
        const double period = 1.6;
        buf_t b = buf_new(period * 4);
        buf_t loop = buf_new(period * 2);
        for (int i = 0; i < b.n; i++) {
            double t = (double)i / SR;
            double breath = 0.55 + 0.45 * sin(TAU * t / period - PI / 2);
            double v1 = sin(TAU * A4 * t + 0.4 * sin(TAU * 0.25 * t));
            double v2 = sin(TAU * E5 * 1.0015 * t);
            double v3 = sin(TAU * D4 * 0.999 * t);
            put(&b, i, (0.5 * v1 + 0.35 * v2) * breath * 0.12, -0.35);
            put(&b, i, (0.5 * v2 + 0.40 * v3) * breath * 0.12, 0.35);
        }
        for (int rep = 0; rep < 4; rep++) glass(&b, rep * period + 0.2, FS5, 0.025, rep & 1 ? 0.5 : -0.5, 0.35);
        space(&b, 1.1, 0.87, 0.45, 0.6, 16);
        for (int i = 0; i < loop.n; i++) {
            loop.l[i] = b.l[i + (int)(period * SR)];
            loop.r[i] = b.r[i + (int)(period * SR)];
        }
        free(b.l);
        free(b.r);
        no_trim = 1;
        finish_and_write(&loop, dir, "ai_thinking", -20);
        no_trim = 0;
    }
    /* end: settles down onto D, the aurora closing gently */
    {
        buf_t b = buf_new(3.0);
        static const double down[4] = { E6, B5, FS5, D5 };
        for (int k = 0; k < 4; k++) glass(&b, 0.07 * k, down[k], 0.08 - 0.008 * k, 0.45 - 0.3 * k, 0.30 + 0.05 * k);
        aurora(&b, 0.10, 0.40, D4, 0.07, 0.10, 0.55, 0.9);
        aurora(&b, 0.10, 0.40, A4, 0.05, 0.12, 0.50, 0.9);
        darken(&b, 9000, 2200, 1.2);
        echo(&b, 170, 0.30, 0.15, 2000);
        space(&b, 1.2, 0.88, 0.35, 0.65, 18);
        finish_and_write(&b, dir, "ai_end", -12);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    start_open(dir);
    start_close(dir);
    flyouts(dir);
    toggles(dir);
    sound_switches(dir);
    assistant(dir);
    return 0;
}
