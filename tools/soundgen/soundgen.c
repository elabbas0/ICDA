/* soundgen - renders ICDA's interface sounds into resources/audio/.
 *
 *   cc -O2 -o soundgen soundgen.c -lm && ./soundgen ../../resources/audio
 *
 * Every sound is synthesized here, nothing is sampled.  They share one
 * palette so they read as a family: D major pentatonic (D E F# A B), glassy
 * FM bells over a soft marimba body, a breath of filtered air, and the same
 * small stereo room.  Opening sounds rise, closing ones fall and are a
 * little darker and quieter; frequent sounds are short and soft, rare ones
 * (sound on, the assistant) are allowed to bloom.
 *
 * Output: 48 kHz, 16-bit, stereo WAV, peak-normalized per sound. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define TAU 6.28318530717958647692

/* pitches (Hz) */
#define D5 587.33
#define E5 659.26
#define FS5 739.99
#define A5 880.00
#define B5 987.77
#define D6 1174.66
#define E6 1318.51
#define FS6 1479.98
#define A6 1760.00
#define B6 1975.53
#define D4 293.66
#define A4 440.00
#define FS4 369.99

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
    double a = (pan + 1) * TAU / 8;
    if (i < 0 || i >= b->n) return;
    b->l[i] += (float)(v * cos(a));
    b->r[i] += (float)(v * sin(a));
}

/* smooth attack, exponential decay (tau seconds) */
static double env(double t, double attack, double tau) {
    double a = t < attack ? 0.5 - 0.5 * cos(t / attack * TAU / 2) : 1.0;
    return a * exp(-(t > attack ? t - attack : 0) / tau);
}

/* Glass bell: an FM pair whose brightness fades faster than its body, plus
 * an inharmonic shimmer partial. */
static void bell(buf_t *b, double t0, double f, double amp, double pan, double tau, double bright) {
    int s = (int)(t0 * SR), len = (int)((tau * 7 + 0.05) * SR);
    double ph = 0, mph = 0, sph = 0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double idx = bright * exp(-t / (tau * 0.35));
        double m = sin(mph) * idx;
        double v = sin(ph + m) * env(t, 0.003, tau);
        v += 0.18 * sin(sph) * env(t, 0.002, tau * 0.25);
        ph += TAU * f / SR;
        mph += TAU * f * 3.5 / SR;
        sph += TAU * f * 2.756 / SR;
        put(b, s + i, v * amp, pan);
    }
}

/* Marimba-like body: fundamental plus a quickly fading 4th harmonic and a
 * soft mallet transient. */
static void mallet(buf_t *b, double t0, double f, double amp, double pan, double tau) {
    int s = (int)(t0 * SR), len = (int)((tau * 6 + 0.03) * SR);
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double v = sin(TAU * f * t) * env(t, 0.002, tau);
        v += 0.35 * sin(TAU * f * 3.98 * t) * env(t, 0.001, tau * 0.18);
        v += 0.12 * noise() * exp(-t / 0.0025);
        put(b, s + i, v * amp, pan);
    }
}

/* Pitch-gliding sine "bubble" (f0 -> f1 over glide seconds). */
static void bubble(buf_t *b, double t0, double f0, double f1, double glide, double amp, double pan, double tau) {
    int s = (int)(t0 * SR), len = (int)((tau * 6 + glide) * SR);
    double ph = 0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double k = t < glide ? t / glide : 1.0;
        double f = f0 + (f1 - f0) * (1 - (1 - k) * (1 - k));
        ph += TAU * f / SR;
        put(b, s + i, (sin(ph) + 0.12 * sin(2 * ph)) * env(t, 0.002, tau) * amp, pan);
    }
}

/* Soft click: band-limited noise burst. */
static void tick(buf_t *b, double t0, double amp, double pan, double tone) {
    int s = (int)(t0 * SR), len = (int)(0.012 * SR);
    double lp = 0, prev = 0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR;
        double x = noise();
        lp += tone * (x - lp);
        double hp = lp - prev;
        prev = lp;
        put(b, s + i, hp * exp(-t / 0.0018) * amp, pan);
    }
}

/* Air: noise through a resonant band-pass sweeping f0 -> f1. */
static void air(buf_t *b, double t0, double dur, double f0, double f1, double amp, double pan) {
    int s = (int)(t0 * SR), len = (int)(dur * SR);
    double low = 0, band = 0, q = 0.9;
    for (int i = 0; i < len; i++) {
        double t = (double)i / dur / SR;
        double f = f0 * pow(f1 / f0, t);
        double c = 2 * sin(TAU / 2 * f / SR);
        double high = noise() - low - q * band;
        band += c * high;
        low += c * band;
        double e = sin(t * TAU / 2);
        put(b, s + i, band * e * e * amp, pan + 0.4 * sin(t * 5));
    }
}

/* Pad: detuned soft saws through a gentle low-pass, slow swell. */
static void pad(buf_t *b, double t0, double dur, double f, double amp, double cutoff) {
    int s = (int)(t0 * SR), len = (int)(dur * SR);
    double ph[3] = { 0, 0.33, 0.71 }, det[3] = { 0.996, 1.0, 1.0045 }, lpl = 0, lpr = 0;
    double c = 1 - exp(-TAU * cutoff / SR);
    for (int i = 0; i < len; i++) {
        double t = (double)i / SR, x = (double)i / len;
        double v[3];
        for (int k = 0; k < 3; k++) {
            ph[k] += f * det[k] / SR;
            ph[k] -= floor(ph[k]);
            v[k] = 2 * ph[k] - 1;
        }
        lpl += c * ((v[0] + v[1] * 0.7) - lpl);
        lpr += c * ((v[2] + v[1] * 0.7) - lpr);
        double e = sin(x * TAU / 2);
        (void)t;
        if (s + i < b->n) {
            b->l[s + i] += (float)(lpl * e * amp);
            b->r[s + i] += (float)(lpr * e * amp);
        }
    }
}

/* ---- room ------------------------------------------------------------------ */

typedef struct {
    float *d;
    int n, i;
    float fb, lp, damp;
} comb_t;

typedef struct {
    float *d;
    int n, i;
} allpass_t;

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

/* A small, bright room (Freeverb topology, short decay). */
static void room(buf_t *b, double size, double mix, double damp) {
    static const int cd[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static const int ad[4] = { 556, 441, 341, 225 };
    comb_t cl[8], cr[8];
    allpass_t al[4], ar[4];
    for (int k = 0; k < 8; k++) {
        int n = (int)(cd[k] * 48.0 / 44.1 * size);
        cl[k] = (comb_t){ calloc((size_t)n, 4), n, 0, 0.78f, 0, (float)damp };
        cr[k] = (comb_t){ calloc((size_t)n + 25, 4), n + 25, 0, 0.78f, 0, (float)damp };
    }
    for (int k = 0; k < 4; k++) {
        int n = (int)(ad[k] * 48.0 / 44.1);
        al[k] = (allpass_t){ calloc((size_t)n, 4), n, 0 };
        ar[k] = (allpass_t){ calloc((size_t)n + 25, 4), n + 25, 0 };
    }
    for (int i = 0; i < b->n; i++) {
        float in = (b->l[i] + b->r[i]) * 0.015f, ol = 0, orr = 0;
        for (int k = 0; k < 8; k++) {
            ol += comb_run(&cl[k], in);
            orr += comb_run(&cr[k], in);
        }
        for (int k = 0; k < 4; k++) {
            ol = allpass_run(&al[k], ol);
            orr = allpass_run(&ar[k], orr);
        }
        b->l[i] = (float)(b->l[i] * (1 - mix * 0.5) + ol * mix);
        b->r[i] = (float)(b->r[i] * (1 - mix * 0.5) + orr * mix);
    }
    for (int k = 0; k < 8; k++) {
        free(cl[k].d);
        free(cr[k].d);
    }
    for (int k = 0; k < 4; k++) {
        free(al[k].d);
        free(ar[k].d);
    }
}

/* one-pole low-pass sweeping from f0 to f1 over the sound (darkening) */
static void tone(buf_t *b, double f0, double f1) {
    double l = 0, r = 0;
    for (int i = 0; i < b->n; i++) {
        double f = f0 * pow(f1 / f0, (double)i / b->n);
        double c = 1 - exp(-TAU * f / SR);
        l += c * (b->l[i] - l);
        r += c * (b->r[i] - r);
        b->l[i] = (float)l;
        b->r[i] = (float)r;
    }
}

/* ---- output ---------------------------------------------------------------- */

static int no_fades;   /* loops must not dip at the seam */

static void finish_and_write(buf_t *b, const char *dir, const char *name, double peak_db) {
    char path[512];
    FILE *f;
    double peak = 1e-9, gain, dcl = 0, dcr = 0, pl = 0, pr = 0;
    int fade = SR / 100;
    uint32_t data_bytes = (uint32_t)b->n * 4;
    /* DC blocker */
    for (int i = 0; i < b->n; i++) {
        double xl = b->l[i], xr = b->r[i];
        dcl = xl - pl + 0.995 * dcl;
        dcr = xr - pr + 0.995 * dcr;
        pl = xl;
        pr = xr;
        b->l[i] = (float)dcl;
        b->r[i] = (float)dcr;
    }
    for (int i = 0; i < b->n; i++) {
        if (fabs(b->l[i]) > peak) peak = fabs(b->l[i]);
        if (fabs(b->r[i]) > peak) peak = fabs(b->r[i]);
    }
    gain = pow(10, peak_db / 20) / peak;
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
    for (int i = 0; i < b->n; i++) {
        double g = gain;
        if (!no_fades && i < 64) g *= (double)i / 64;
        if (!no_fades && i > b->n - fade) g *= (double)(b->n - i) / fade;
        int16_t s[2];
        double l = b->l[i] * g, r = b->r[i] * g;
        s[0] = (int16_t)(l > 1 ? 32767 : l < -1 ? -32767 : l * 32767);
        s[1] = (int16_t)(r > 1 ? 32767 : r < -1 ? -32767 : r * 32767);
        fwrite(s, 2, 2, f);
    }
    fclose(f);
    free(b->l);
    free(b->r);
    printf("%-22s %5.0f ms  peak %.1f dBFS\n", name, b->n * 1000.0 / SR, peak_db);
}

/* ---- the sounds ------------------------------------------------------------- */

static void start_open(const char *dir) {
    buf_t b = buf_new(0.62);
    air(&b, 0.0, 0.22, 900, 3800, 0.10, -0.2);
    mallet(&b, 0.010, A5, 0.22, -0.25, 0.07);
    bell(&b, 0.010, A5, 0.30, -0.25, 0.10, 1.4);
    bell(&b, 0.065, D6, 0.34, 0.25, 0.14, 1.6);
    bell(&b, 0.065, D6 * 2.0, 0.05, 0.3, 0.06, 0.6);
    room(&b, 0.7, 0.28, 0.35);
    finish_and_write(&b, dir, "start_open", -5);
}

static void start_close(const char *dir) {
    buf_t b = buf_new(0.5);
    air(&b, 0.0, 0.18, 3200, 900, 0.08, 0.2);
    bell(&b, 0.005, D6, 0.26, 0.25, 0.08, 1.2);
    mallet(&b, 0.055, A5, 0.24, -0.25, 0.08);
    bell(&b, 0.055, A5, 0.20, -0.25, 0.09, 0.8);
    tone(&b, 9000, 3500);
    room(&b, 0.6, 0.24, 0.45);
    finish_and_write(&b, dir, "start_close", -7);
}

/* taskbar flyouts: four bubbly variants each way, picked at random */
static void flyouts(const char *dir) {
    static const double up[4][2] = { { E6, B6 }, { FS6, A6 }, { D6, A6 }, { B5, FS6 } };
    static const double pans[4] = { 0.15, -0.1, 0.25, -0.2 };
    char name[32];
    for (int v = 0; v < 4; v++) {
        buf_t b = buf_new(0.34);
        bubble(&b, 0.0, up[v][0] * 0.82, up[v][0], 0.028, 0.32, pans[v], 0.05);
        bell(&b, 0.032, up[v][1], 0.13, -pans[v], 0.06, 0.9);
        tick(&b, 0.0, 0.10, pans[v], 0.5);
        room(&b, 0.5, 0.20, 0.4);
        snprintf(name, sizeof name, "flyout_open_%d", v + 1);
        finish_and_write(&b, dir, name, -8);
    }
    for (int v = 0; v < 4; v++) {
        buf_t b = buf_new(0.3);
        bubble(&b, 0.0, up[v][1] * 0.72, up[v][1] * 0.52, 0.045, 0.30, -pans[v], 0.045);
        tick(&b, 0.0, 0.07, -pans[v], 0.35);
        tone(&b, 7000, 2600);
        room(&b, 0.45, 0.16, 0.5);
        snprintf(name, sizeof name, "flyout_close_%d", v + 1);
        finish_and_write(&b, dir, name, -10);
    }
}

static void toggles(const char *dir) {
    buf_t on = buf_new(0.32), off = buf_new(0.28);
    tick(&on, 0.0, 0.25, 0.0, 0.6);
    mallet(&on, 0.008, B5, 0.30, -0.15, 0.045);
    mallet(&on, 0.050, E6, 0.32, 0.15, 0.06);
    bell(&on, 0.050, E6, 0.10, 0.15, 0.06, 0.7);
    room(&on, 0.45, 0.16, 0.4);
    finish_and_write(&on, dir, "toggle_on", -8);

    tick(&off, 0.0, 0.20, 0.0, 0.35);
    mallet(&off, 0.008, E6 * 0.75, 0.26, 0.15, 0.04);
    mallet(&off, 0.048, A5, 0.26, -0.15, 0.05);
    tone(&off, 5500, 2200);
    room(&off, 0.4, 0.12, 0.5);
    finish_and_write(&off, dir, "toggle_off", -10);
}

static void sound_switches(const char *dir) {
    buf_t on = buf_new(1.15), off = buf_new(0.7);
    /* sound on: the full pentatonic chord blooms open */
    static const double chord[5] = { D5, FS5, A5, D6, E6 };
    pad(&on, 0.0, 1.0, D4, 0.10, 1400);
    pad(&on, 0.0, 1.0, A4, 0.06, 1600);
    for (int k = 0; k < 5; k++) {
        bell(&on, 0.03 * k, chord[k], 0.20, -0.5 + 0.25 * k, 0.28, 1.3);
        mallet(&on, 0.03 * k, chord[k], 0.08, -0.5 + 0.25 * k, 0.1);
    }
    air(&on, 0.0, 0.35, 1200, 5200, 0.05, 0.0);
    room(&on, 0.9, 0.32, 0.3);
    finish_and_write(&on, dir, "sound_on", -4);

    /* mute: the same voice folds down and closes */
    bell(&off, 0.0, A5, 0.24, 0.2, 0.12, 0.9);
    bell(&off, 0.07, D5, 0.26, -0.2, 0.16, 0.6);
    mallet(&off, 0.07, D5, 0.16, -0.2, 0.09);
    tone(&off, 6000, 650);
    room(&off, 0.6, 0.2, 0.55);
    finish_and_write(&off, dir, "sound_mute", -8);
}

static void assistant(const char *dir) {
    /* listen: a quick upward shimmer with a breath, "I'm here" */
    {
        buf_t b = buf_new(1.1);
        static const double up[5] = { D5, A5, E6, FS6, A6 };
        pad(&b, 0.0, 0.9, A4, 0.07, 1800);
        air(&b, 0.0, 0.45, 700, 6000, 0.09, 0.0);
        for (int k = 0; k < 5; k++) bell(&b, 0.035 * k, up[k], 0.17 + 0.02 * k, -0.6 + 0.3 * k, 0.16, 1.5);
        room(&b, 0.9, 0.3, 0.3);
        finish_and_write(&b, dir, "ai_listen", -5);
    }
    /* thinking: a soft two-tone pulse, rendered three times over so the
     * middle period loops without a seam */
    {
        const double period = 0.8;
        buf_t b = buf_new(period * 4);
        buf_t loop = buf_new(period * 2);
        for (int rep = 0; rep < 4; rep++) {
            double t = rep * period;
            bell(&b, t, A5, 0.12, -0.3, 0.18, 0.5);
            bell(&b, t + period / 2, E6, 0.09, 0.3, 0.16, 0.5);
            pad(&b, t, period, D4, 0.035, 900);
            pad(&b, t, period, A4, 0.02, 1100);
        }
        room(&b, 0.8, 0.3, 0.45);
        for (int i = 0; i < loop.n; i++) {
            loop.l[i] = b.l[i + (int)(period * SR)];
            loop.r[i] = b.r[i + (int)(period * SR)];
        }
        free(b.l);
        free(b.r);
        no_fades = 1;
        finish_and_write(&loop, dir, "ai_thinking", -12);
        no_fades = 0;
    }
    /* end: settles back down onto the tonic */
    {
        buf_t b = buf_new(1.3);
        static const double down[4] = { FS6, D6, A5, D5 };
        for (int k = 0; k < 4; k++) bell(&b, 0.06 * k, down[k], 0.20, 0.45 - 0.3 * k, 0.2 + 0.06 * k, 1.1);
        pad(&b, 0.15, 1.1, D4, 0.08, 1200);
        pad(&b, 0.15, 1.1, FS4, 0.05, 1200);
        mallet(&b, 0.18, D5, 0.12, 0.0, 0.12);
        tone(&b, 9000, 2400);
        room(&b, 1.0, 0.32, 0.35);
        finish_and_write(&b, dir, "ai_end", -6);
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
