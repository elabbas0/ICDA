/* soundgen - renders every sound ICDA ships.
 *
 *   cc -O2 -o soundgen soundgen.c -lm
 *   ./soundgen ../../resources/audio ../../userspace
 *
 * The first directory receives the 18 interface sounds (48 kHz, 16-bit,
 * stereo; seeded at /usr/share/sounds and played through ic_sound).  The
 * second, optional, directory receives boot.wav, chime.wav and melody.wav
 * (22050 Hz, 16-bit, mono; the demo tracks in /usr/share/audio).
 * scripts/gen_sounds.py builds and runs this file with both directories.
 *
 * Design intent - "felt"
 * ----------------------
 * ICDA's interface is quiet chrome: neutral graphite surfaces, translucent
 * blurred materials, hairline separators, one accent colour, and motion that
 * is short (80-340 ms), decelerates when something arrives and accelerates
 * when it leaves.  The sounds are the audible form of that: matte, rounded
 * and small, never glassy, bright or percussive.
 *
 *   - One voice.  Every sound is built from the same "felt" tone: a sine
 *     fundamental with a faint octave and twelfth that die away faster than
 *     it does, as if a soft mallet touched a wooden bar.  No samples, no
 *     hard transients, no echo or reverb.
 *   - Soft edges.  Every note fades in over a raised cosine (12 ms on the
 *     shortest taps, up to 140 ms on the blooms) and dies away exponentially
 *     into a raised-cosine release, so files start and end on exact silence.
 *   - Dark spectrum.  Fundamentals stay between 208 and 831 Hz (one glint at
 *     1245 Hz), and the highest partial is below 4 kHz.  The only noise is a
 *     "veil" - a breath low-passed at 1.1 kHz - under the start menu and the
 *     assistant, like the blur behind a panel.
 *   - One key.  A-flat major pentatonic (Ab Bb C Eb F): any two sounds that
 *     overlap are consonant, and nothing can sound like an error.
 *   - Gesture mirrors motion.  Opening / on rises and ends on the upper note
 *     with a longer tail (decelerate); closing / off plays the same notes
 *     falling, shorter and darker (accelerate).  Pairs share notes and level.
 *   - Level follows frequency of use.  Flyouts and toggles peak at -16 dBFS
 *     and are over in about a third of a second; the start menu and
 *     notifications sit at -12; the rare ones (sound on/off, the assistant,
 *     the demo tracks) may bloom, at -10 to -8.
 *
 * Output is deterministic: the only random source is a fixed-seed generator
 * for the veil, so re-running on the same toolchain gives identical files.
 * No file may grow past the size of the one it replaces (they are embedded
 * in the kernel image); the limits are in the table at the bottom. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define TAU (2 * PI)

/* A-flat major pentatonic (Hz) */
#define AB3 207.65
#define EB4 311.13
#define AB4 415.30
#define BB4 466.16
#define C4  261.63
#define C5  523.25
#define EB5 622.25
#define F5  698.46
#define AB5 830.61
#define EB6 1244.51

typedef struct {
    double *l, *r;
    int n;        /* frames */
    int sr;       /* sample rate */
    int stereo;
} buf_t;

static buf_t buf_new(double seconds, int sr, int stereo) {
    buf_t b;
    b.n = (int)(seconds * sr);
    b.sr = sr;
    b.stereo = stereo;
    b.l = calloc((size_t)b.n, sizeof(double));
    b.r = calloc((size_t)b.n, sizeof(double));
    if (!b.l || !b.r) {
        fprintf(stderr, "soundgen: out of memory\n");
        exit(1);
    }
    return b;
}

/* ---- the voice -------------------------------------------------------------- */

typedef struct {
    double t0;      /* onset, seconds */
    double freq;    /* Hz */
    double amp;
    double attack;  /* raised-cosine fade-in, seconds */
    double tau;     /* decay time constant, seconds */
    double pan;     /* -1 left .. +1 right */
    double bright;  /* 0 = pure sine, 1 = full octave + twelfth */
    double bend;    /* cents the note starts away from its pitch (- = from below) */
    double bend_tau;/* how fast it settles, seconds */
} felt_t;

/* How many time constants a note is rendered for (-56 dB), and the share of
 * its length over which the release window takes it to exact silence. */
#define FELT_SPAN    6.5
#define FELT_RELEASE 0.40

static void felt_voice(buf_t *b, const felt_t *v) {
    double len = v->attack + FELT_SPAN * v->tau;
    double rel = FELT_RELEASE * len;
    int start = (int)(v->t0 * b->sr + 0.5);
    int count = (int)(len * b->sr);
    /* a hair of detune between the channels gives width without an effect */
    double det = b->stereo ? 0.0004 : 0.0;
    double gl = b->stereo ? cos((v->pan + 1.0) * PI / 4) : 1.0;
    double gr = b->stereo ? sin((v->pan + 1.0) * PI / 4) : 1.0;
    double phl = 0.0, phr = 0.0;
    for (int i = 0; i < count && start + i < b->n; i++) {
        double t = (double)i / b->sr;
        double env = exp(-t / v->tau);
        double f = v->freq;
        double h2, h3, sl, sr_;
        if (t < v->attack) env *= 0.5 - 0.5 * cos(PI * t / v->attack);
        if (t > len - rel) env *= 0.5 + 0.5 * cos(PI * (t - (len - rel)) / rel);
        if (v->bend != 0.0) f *= pow(2.0, v->bend / 1200.0 * exp(-t / v->bend_tau));
        h2 = v->bright * 0.18 * exp(-t / (0.50 * v->tau));
        h3 = v->bright * 0.05 * exp(-t / (0.30 * v->tau));
        sl = sin(phl) + h2 * sin(2 * phl) + h3 * sin(3 * phl);
        sr_ = sin(phr) + h2 * sin(2 * phr) + h3 * sin(3 * phr);
        b->l[start + i] += v->amp * env * gl * sl;
        b->r[start + i] += v->amp * env * gr * sr_;
        phl += TAU * f * (1.0 - det) / b->sr;
        phr += TAU * f * (1.0 + det) / b->sr;
    }
}

static void felt(buf_t *b, double t0, double freq, double amp, double attack, double tau, double pan) {
    felt_t v = { t0, freq, amp, attack, tau, pan, 1.0, 0.0, 0.0 };
    felt_voice(b, &v);
}

static void felt_ex(buf_t *b, double t0, double freq, double amp, double attack, double tau, double pan,
                    double bright, double bend, double bend_tau) {
    felt_t v = { t0, freq, amp, attack, tau, pan, bright, bend, bend_tau };
    felt_voice(b, &v);
}

/* ---- the veil --------------------------------------------------------------- */

static uint32_t rng_state;

static double rng_next(void) {      /* xorshift32, -1 .. 1 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (double)rng_state / 2147483648.0 - 1.0;
}

/* A breath of dark noise: four one-pole low-passes at 1.1 kHz, two high-passes
 * at 300 Hz, under a raised-cosine swell that peaks at `peak` (0..1) of its
 * length.  `amp` is its peak level next to a felt tone of amplitude 1. */
static void veil(buf_t *b, double t0, double dur, double peak, double amp, uint32_t seed) {
    int start = (int)(t0 * b->sr + 0.5);
    int count = (int)(dur * b->sr);
    double a_lp = 1.0 - exp(-TAU * 1100.0 / b->sr);
    double a_hp = exp(-TAU * 300.0 / b->sr);
    double *tmp = calloc((size_t)count, sizeof(double));
    if (!tmp) exit(1);
    for (int ch = 0; ch < (b->stereo ? 2 : 1); ch++) {
        double lp[4] = { 0, 0, 0, 0 }, hx[2] = { 0, 0 }, hy[2] = { 0, 0 };
        double top = 0.0;
        double *out = ch ? b->r : b->l;
        rng_state = seed + 0x9E3779B9u * (uint32_t)(ch + 1);
        for (int i = 0; i < count; i++) {
            double x = rng_next();
            double u = (double)i / count, env;
            for (int k = 0; k < 4; k++) {
                lp[k] += a_lp * (x - lp[k]);
                x = lp[k];
            }
            for (int k = 0; k < 2; k++) {
                double y = a_hp * (hy[k] + x - hx[k]);
                hx[k] = x;
                hy[k] = y;
                x = y;
            }
            env = u < peak ? 0.5 - 0.5 * cos(PI * u / peak) : 0.5 + 0.5 * cos(PI * (u - peak) / (1.0 - peak));
            tmp[i] = x * env;
            if (fabs(tmp[i]) > top) top = fabs(tmp[i]);
        }
        for (int i = 0; i < count && start + i < b->n; i++) out[start + i] += amp * tmp[i] / top;
        if (!b->stereo) break;
    }
    free(tmp);
}

/* ---- writing ---------------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* Scales the buffer to `peak_db` dBFS, drops the silent end, and writes a PCM
 * WAV.  Refuses to write a file larger than `max_bytes`. */
static void finish(buf_t *b, const char *dir, const char *name, double peak_db, long max_bytes) {
    char path[1024];
    int channels = b->stereo ? 2 : 1;
    int n = b->n, pad = b->sr / 200;
    double peak = 0.0, gain;
    uint32_t data_bytes;
    uint8_t h[44];
    FILE *f;
    for (int i = 0; i < b->n; i++) {
        if (fabs(b->l[i]) > peak) peak = fabs(b->l[i]);
        if (b->stereo && fabs(b->r[i]) > peak) peak = fabs(b->r[i]);
    }
    gain = pow(10.0, peak_db / 20.0) / peak;
    /* keep up to the last sample that is not zero once quantised, plus 5 ms */
    while (n > 1 && lrint(b->l[n - 1] * gain * 32767.0) == 0 &&
           (!b->stereo || lrint(b->r[n - 1] * gain * 32767.0) == 0)) n--;
    n = n + pad < b->n ? n + pad : b->n;
    data_bytes = (uint32_t)n * (uint32_t)channels * 2u;
    if ((long)data_bytes + 44 > max_bytes) {
        fprintf(stderr, "soundgen: %s.wav would be %u bytes, limit %ld\n", name, data_bytes + 44, max_bytes);
        exit(1);
    }
    if (lrint(b->l[0] * gain * 32767.0) != 0 || lrint(b->l[n - 1] * gain * 32767.0) != 0) {
        fprintf(stderr, "soundgen: %s.wav does not start and end on silence\n", name);
        exit(1);
    }
    snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                                   /* PCM */
    put16(h + 22, (uint32_t)channels);
    put32(h + 24, (uint32_t)b->sr);
    put32(h + 28, (uint32_t)(b->sr * channels * 2));
    put16(h + 32, (uint32_t)(channels * 2));
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < n; i++) {
        uint8_t s[4];
        put16(s, (uint32_t)(int32_t)lrint(b->l[i] * gain * 32767.0));
        if (b->stereo) put16(s + 2, (uint32_t)(int32_t)lrint(b->r[i] * gain * 32767.0));
        fwrite(s, 1, (size_t)channels * 2, f);
    }
    fclose(f);
    free(b->l);
    free(b->r);
    printf("%-16s %5d Hz %s %5.0f ms  %6u bytes  peak %5.1f dBFS\n", name, b->sr, b->stereo ? "stereo" : "mono  ",
           n * 1000.0 / b->sr, data_bytes + 44, peak_db);
}

/* ---- interface sounds (48 kHz stereo) ---------------------------------------- */

#define UI_SR 48000

/* Flyouts (volume, network, calendar panels): two neighbouring notes of the
 * scale, a quiet grace note leaning into the one that stays.  Opening rises,
 * closing plays the same pair falling, shorter and darker.  The four variants
 * are the same gesture on four pairs, so repeats never sound mechanical. */
static void flyouts(const char *dir, const long *open_max, const long *close_max) {
    static const double pair[4][2] = { { C5, EB5 }, { BB4, C5 }, { EB5, F5 }, { AB4, BB4 } };
    for (int v = 0; v < 4; v++) {
        char name[32];
        buf_t b = buf_new(0.8, UI_SR, 1);
        felt_ex(&b, 0.000, pair[v][0], 0.55, 0.012, 0.045, -0.15, 0.8, -40, 0.020);
        felt_ex(&b, 0.065, pair[v][1], 1.00, 0.014, 0.065, 0.15, 1.0, -25, 0.030);
        snprintf(name, sizeof name, "flyout_open_%d", v + 1);
        finish(&b, dir, name, -16, open_max[v]);

        b = buf_new(0.8, UI_SR, 1);
        felt_ex(&b, 0.000, pair[v][1], 0.70, 0.012, 0.040, 0.15, 0.6, 25, 0.020);
        felt_ex(&b, 0.055, pair[v][0], 1.00, 0.014, 0.055, -0.15, 0.5, 40, 0.030);
        snprintf(name, sizeof name, "flyout_close_%d", v + 1);
        finish(&b, dir, name, -16, close_max[v]);
    }
}

/* Switches: one soft tap.  On settles up into Eb5 with a faint glint an octave
 * above; off settles down into C5, a third lower, with a little weight below. */
static void toggles(const char *dir, long on_max, long off_max) {
    buf_t b = buf_new(0.7, UI_SR, 1);
    felt_ex(&b, 0.000, EB5, 1.00, 0.012, 0.055, 0.0, 1.0, -60, 0.018);
    felt_ex(&b, 0.030, EB6, 0.12, 0.020, 0.040, 0.2, 0.0, 0, 0);
    finish(&b, dir, "toggle_on", -16, on_max);

    b = buf_new(0.7, UI_SR, 1);
    felt_ex(&b, 0.000, C5, 1.00, 0.012, 0.050, 0.0, 0.5, 60, 0.018);
    felt_ex(&b, 0.030, C4, 0.20, 0.020, 0.050, -0.2, 0.0, 0, 0);
    finish(&b, dir, "toggle_off", -16, off_max);
}

/* Start menu: the panel fading in is a slow upward arpeggio over the tonic,
 * each step a little later than the last (decelerating), under a veil that
 * swells with it.  Closing is the same chord falling, quicker each step. */
static void start_menu(const char *dir, long open_max, long close_max) {
    buf_t b = buf_new(1.9, UI_SR, 1);
    veil(&b, 0.000, 0.45, 0.70, 0.08, 0x1CDA0001u);
    felt(&b, 0.000, AB3, 0.45, 0.030, 0.16, -0.40);
    felt(&b, 0.050, EB4, 0.55, 0.026, 0.16, 0.30);
    felt(&b, 0.115, AB4, 0.75, 0.022, 0.16, -0.20);
    felt(&b, 0.200, C5, 1.00, 0.020, 0.16, 0.20);
    felt(&b, 0.310, EB5, 0.60, 0.020, 0.16, 0.00);
    finish(&b, dir, "start_open", -12, open_max);

    b = buf_new(1.5, UI_SR, 1);
    veil(&b, 0.000, 0.30, 0.25, 0.06, 0x1CDA0002u);
    felt_ex(&b, 0.000, EB5, 0.70, 0.016, 0.11, 0.00, 0.7, 0, 0);
    felt_ex(&b, 0.080, C5, 1.00, 0.016, 0.11, 0.20, 0.7, 0, 0);
    felt_ex(&b, 0.140, AB4, 0.80, 0.018, 0.11, -0.20, 0.6, 0, 0);
    felt_ex(&b, 0.185, EB4, 0.60, 0.020, 0.11, 0.30, 0.5, 0, 0);
    finish(&b, dir, "start_close", -12, close_max);
}

/* Notification: two notes a fourth apart, the upper one resting on its own
 * lower octave.  Unhurried, and clear enough to notice from across a room. */
static void notification(const char *dir, long max) {
    buf_t b = buf_new(0.9, UI_SR, 1);
    felt(&b, 0.000, EB5, 0.80, 0.014, 0.090, -0.20);
    felt(&b, 0.110, AB5, 1.00, 0.014, 0.100, 0.20);
    felt_ex(&b, 0.110, AB4, 0.30, 0.020, 0.100, 0.00, 0.3, 0, 0);
    finish(&b, dir, "notify", -12, max);
}

/* Sound on (also the volume preview): a fifth, rising and left to ring.
 * Sound off: the same fifth falling, the lower note damped as if a hand had
 * been laid on it. */
static void sound_switch(const char *dir, long on_max, long mute_max) {
    buf_t b = buf_new(1.0, UI_SR, 1);
    felt(&b, 0.000, AB4, 0.75, 0.014, 0.090, -0.20);
    felt(&b, 0.085, EB5, 1.00, 0.014, 0.110, 0.20);
    finish(&b, dir, "sound_on", -10, on_max);

    b = buf_new(1.0, UI_SR, 1);
    felt(&b, 0.000, EB5, 1.00, 0.014, 0.090, 0.20);
    felt_ex(&b, 0.085, AB4, 0.80, 0.014, 0.045, -0.20, 0.3, 30, 0.030);
    finish(&b, dir, "sound_mute", -10, mute_max);
}

/* The assistant.  Listening: a warm chord opens slowly under two rising notes
 * that end on the sixth - an open question.  Thinking: a quiet pulse over a
 * held fifth, each beat softer than the one before, so it thins out rather
 * than nags.  Done: the question's top note steps back down to the tonic. */
static void assistant(const char *dir, long listen_max, long thinking_max, long end_max) {
    static const double pulse[6] = { C5, EB5, C5, F5, C5, EB5 };
    buf_t b = buf_new(2.9, UI_SR, 1);
    veil(&b, 0.000, 0.60, 0.60, 0.06, 0x1CDA0003u);
    felt_ex(&b, 0.000, AB3, 0.50, 0.120, 0.36, -0.35, 0.5, 0, 0);
    felt_ex(&b, 0.000, EB4, 0.55, 0.120, 0.36, 0.35, 0.5, 0, 0);
    felt_ex(&b, 0.000, BB4, 0.45, 0.140, 0.34, 0.00, 0.5, 0, 0);
    felt(&b, 0.160, C5, 0.80, 0.030, 0.28, -0.15);
    felt(&b, 0.340, F5, 0.90, 0.030, 0.30, 0.15);
    finish(&b, dir, "ai_listen", -9, listen_max);

    b = buf_new(3.1, UI_SR, 1);
    felt_ex(&b, 0.000, AB3, 0.40, 0.300, 0.40, -0.30, 0.3, 0, 0);
    felt_ex(&b, 0.000, EB4, 0.40, 0.300, 0.40, 0.30, 0.3, 0, 0);
    for (int i = 0; i < 6; i++)
        felt_ex(&b, 0.10 + 0.32 * i, pulse[i], pow(0.82, i), 0.045, 0.12, (i & 1) ? 0.30 : -0.30, 0.6, 0, 0);
    finish(&b, dir, "ai_thinking", -16, thinking_max);

    b = buf_new(2.9, UI_SR, 1);
    felt(&b, 0.000, F5, 0.70, 0.024, 0.20, 0.15);
    felt(&b, 0.130, EB5, 0.80, 0.024, 0.22, -0.15);
    felt(&b, 0.260, C5, 1.00, 0.026, 0.30, 0.00);
    felt_ex(&b, 0.260, AB3, 0.50, 0.100, 0.34, -0.35, 0.5, 0, 0);
    felt_ex(&b, 0.260, EB4, 0.45, 0.100, 0.34, 0.35, 0.5, 0, 0);
    felt_ex(&b, 0.260, AB4, 0.40, 0.100, 0.34, 0.00, 0.5, 0, 0);
    finish(&b, dir, "ai_end", -9, end_max);
}

/* ---- demo tracks (22050 Hz mono) --------------------------------------------- */

#define TRACK_SR 22050

/* boot.wav: the tonic chord unfolding upward, every note eased in. */
static void track_boot(const char *dir, long max) {
    buf_t b = buf_new(1.54, TRACK_SR, 0);
    felt(&b, 0.000, AB3, 0.50, 0.080, 0.20, 0);
    felt(&b, 0.060, EB4, 0.55, 0.070, 0.20, 0);
    felt(&b, 0.130, AB4, 0.70, 0.060, 0.20, 0);
    felt(&b, 0.210, C5, 0.90, 0.050, 0.19, 0);
    felt(&b, 0.300, EB5, 0.70, 0.050, 0.18, 0);
    finish(&b, dir, "boot", -8, max);
}

/* chime.wav: three soft steps up the scale, the last barely there. */
static void track_chime(const char *dir, long max) {
    buf_t b = buf_new(0.99, TRACK_SR, 0);
    felt(&b, 0.000, C5, 0.80, 0.015, 0.11, 0);
    felt(&b, 0.090, F5, 1.00, 0.015, 0.12, 0);
    felt_ex(&b, 0.180, AB5, 0.35, 0.020, 0.10, 0, 0.4, 0, 0);
    finish(&b, dir, "chime", -9, max);
}

/* melody.wav: an eight-note lullaby, an arch that climbs the scale and comes
 * home to the tonic (Ab4 C5 Eb5 F5 | Eb5 C5 Bb4 Ab4), over two bass notes. */
static void track_melody(const char *dir, long max) {
    static const double seq[8] = { AB4, C5, EB5, F5, EB5, C5, BB4, AB4 };
    buf_t b = buf_new(2.99, TRACK_SR, 0);
    for (int i = 0; i < 8; i++) felt(&b, 0.25 * i, seq[i], i == 7 ? 0.90 : 0.80, 0.020, 0.17, 0);
    felt_ex(&b, 0.000, AB3, 0.35, 0.040, 0.28, 0, 0.4, 0, 0);
    felt_ex(&b, 1.000, EB4, 0.30, 0.040, 0.28, 0, 0.4, 0, 0);
    finish(&b, dir, "melody", -9, max);
}

/* ---- main ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* The size of each file this generator replaced.  Nothing may be larger. */
    static const long open_max[4] = { 172984, 164360, 182012, 177068 };
    static const long close_max[4] = { 129728, 136364, 134288, 121164 };
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: soundgen <interface sound dir> [<demo track dir>]\n");
        return 1;
    }
    flyouts(argv[1], open_max, close_max);
    toggles(argv[1], 155564, 124552);
    start_menu(argv[1], 384044, 307244);
    notification(argv[1], 172844);
    sound_switch(argv[1], 614444, 384044);
    assistant(argv[1], 576044, 614444, 576044);
    if (argc == 3) {
        track_boot(argv[2], 68398);
        track_chime(argv[2], 44144);
        track_melody(argv[2], 132344);
    }
    return 0;
}
