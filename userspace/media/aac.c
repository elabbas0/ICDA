/* AAC-LC decoder (ISO/IEC 14496-3 general audio, low complexity profile).
 *
 * Handles what M4A files, MP4 video and YouTube send: single channel and
 * channel pair elements, long / start / short / stop windows with sine and
 * KBD shapes, M/S and intensity stereo, perceptual noise substitution,
 * pulses and TNS.  HE-AAC streams play their AAC-LC core (SBR extension
 * data is skipped), multichannel streams are folded down to stereo.
 *
 * Codebooks and band tables are VisualOn's (aac_tables.h, Apache-2.0); the
 * decoder itself is ICDA's own. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "aac.h"
#include "aac_tables.h"

#define MAX_CH      8
#define ZERO_HCB    0
#define ESC_HCB     11
#define NOISE_HCB   13
#define INTENSITY_HCB2 14
#define INTENSITY_HCB  15

enum { ONLY_LONG = 0, LONG_START, EIGHT_SHORT, LONG_STOP };
enum { ID_SCE = 0, ID_CPE, ID_CCE, ID_LFE, ID_DSE, ID_PCE, ID_FIL, ID_END };

static const int rate_tab[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                  16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
static const uint8_t tns_max_long[13]  = { 31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39 };
static const uint8_t tns_max_short[13] = { 9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };

/* ---- shared tables, built once ---------------------------------------------- */

typedef struct { int16_t child[2]; } hnode_t;    /* child < 0: leaf, value -child-1 */
typedef struct { hnode_t *n; int count; } htree_t;

static htree_t trees[12];   /* 1..11 spectral, 0 scale factors */
static float   pow43[8192];
static float   pow2sf[512];  /* 2^(0.25*(i-200)) */
static float   sine_long[1024], kbd_long[1024], sine_short[128], kbd_short[128];
/* DCT-IV twiddles: pre e^{-i pi (j+1/4)/M}, post e^{-i pi p/M} (scaled by 2/N) */
static float   pre_lc[512], pre_ls[512], post_lc[512], post_ls[512];
static float   pre_sc[64], pre_ss[64], post_sc[64], post_ss[64];
static float   fft_c512[256], fft_s512[256], fft_c64[32], fft_s64[32];
static int     tables_ready;

static int tree_add(htree_t *t, uint32_t code, int len, int value) {
    int node = 0;
    for (int b = len - 1; b >= 0; b--) {
        int bit = (int)((code >> b) & 1);
        if (b == 0) {
            t->n[node].child[bit] = (int16_t)(-value - 1);
            return 0;
        }
        if (t->n[node].child[bit] <= 0) {
            if (t->n[node].child[bit] < 0) return -1;        /* prefix clash */
            t->n[node].child[bit] = (int16_t)t->count;
            t->n[t->count].child[0] = t->n[t->count].child[1] = 0;
            t->count++;
        }
        node = t->n[node].child[bit];
    }
    return 0;
}

static void tree_build(htree_t *t, int entries, const uint16_t *lens, int shift, const void *codes, int wide) {
    t->n = (hnode_t *)calloc((size_t)entries * 2 + 2, sizeof(hnode_t));
    t->count = 1;
    for (int i = 0; i < entries; i++) {
        int len = shift < 0 ? lens[i] : (lens[i] >> shift) & 0xFF;
        uint32_t code = wide ? ((const uint32_t *)codes)[i] : ((const uint16_t *)codes)[i];
        tree_add(t, code, len, i);
    }
}

static double bessel_i0(double x2) {          /* I0(sqrt(x2)) */
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= x2 / 4.0 / ((double)k * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

static void kbd(float *w, int n, double alpha) {
    double local[1025], sum = 0, a2 = 4.0 * (alpha * M_PI / n) * (alpha * M_PI / n);
    for (int i = 0; i <= n; i++) {
        sum += bessel_i0((double)i * (n - i) * a2);
        local[i] = sum;
    }
    for (int i = 0; i < n; i++) w[i] = (float)sqrt(local[i] / sum);
}

static void build_tables(void) {
    if (tables_ready) return;
    tree_build(&trees[1], 81, &huff_ltab1_2[0][0][0][0], 8, &huff_ctab1[0][0][0][0], 0);
    tree_build(&trees[2], 81, &huff_ltab1_2[0][0][0][0], 0, &huff_ctab2[0][0][0][0], 0);
    tree_build(&trees[3], 81, &huff_ltab3_4[0][0][0][0], 8, &huff_ctab3[0][0][0][0], 0);
    tree_build(&trees[4], 81, &huff_ltab3_4[0][0][0][0], 0, &huff_ctab4[0][0][0][0], 0);
    tree_build(&trees[5], 81, &huff_ltab5_6[0][0], 8, &huff_ctab5[0][0], 0);
    tree_build(&trees[6], 81, &huff_ltab5_6[0][0], 0, &huff_ctab6[0][0], 0);
    tree_build(&trees[7], 64, &huff_ltab7_8[0][0], 8, &huff_ctab7[0][0], 0);
    tree_build(&trees[8], 64, &huff_ltab7_8[0][0], 0, &huff_ctab8[0][0], 0);
    tree_build(&trees[9], 169, &huff_ltab9_10[0][0], 8, &huff_ctab9[0][0], 0);
    tree_build(&trees[10], 169, &huff_ltab9_10[0][0], 0, &huff_ctab10[0][0], 0);
    tree_build(&trees[11], 289, &huff_ltab11[0][0], -1, &huff_ctab11[0][0], 0);
    tree_build(&trees[0], 121, huff_ltabscf, -1, huff_ctabscf, 1);
    for (int i = 0; i < 8192; i++) pow43[i] = (float)pow((double)i, 4.0 / 3.0);
    for (int i = 0; i < 512; i++) pow2sf[i] = (float)pow(2.0, 0.25 * (i - 200));
    for (int i = 0; i < 1024; i++) sine_long[i] = (float)sin(M_PI / 2048.0 * (i + 0.5));
    for (int i = 0; i < 128; i++) sine_short[i] = (float)sin(M_PI / 256.0 * (i + 0.5));
    kbd(kbd_long, 1024, 4.0);
    kbd(kbd_short, 128, 6.0);
    for (int j = 0; j < 512; j++) {
        double a = M_PI * (j + 0.25) / 1024.0, b = M_PI * j / 1024.0, s = 2.0 / 2048.0;
        pre_lc[j] = (float)cos(a);
        pre_ls[j] = (float)sin(a);
        post_lc[j] = (float)(cos(b) * s);
        post_ls[j] = (float)(sin(b) * s);
    }
    for (int j = 0; j < 64; j++) {
        double a = M_PI * (j + 0.25) / 128.0, b = M_PI * j / 128.0, s = 2.0 / 256.0;
        pre_sc[j] = (float)cos(a);
        pre_ss[j] = (float)sin(a);
        post_sc[j] = (float)(cos(b) * s);
        post_ss[j] = (float)(sin(b) * s);
    }
    for (int k = 0; k < 256; k++) {
        fft_c512[k] = (float)cos(2 * M_PI * k / 512.0);
        fft_s512[k] = (float)-sin(2 * M_PI * k / 512.0);
    }
    for (int k = 0; k < 32; k++) {
        fft_c64[k] = (float)cos(2 * M_PI * k / 64.0);
        fft_s64[k] = (float)-sin(2 * M_PI * k / 64.0);
    }
    tables_ready = 1;
}

/* ---- bits ------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    int            len;      /* bytes */
    int            pos;      /* bits */
    int            err;
} bits_t;

static unsigned getbits(bits_t *b, int n) {
    unsigned v = 0;
    while (n-- > 0) {
        int byte = b->pos >> 3;
        unsigned bit = 0;
        if (byte < b->len) bit = (b->p[byte] >> (7 - (b->pos & 7))) & 1;
        else b->err = 1;
        b->pos++;
        v = (v << 1) | bit;
    }
    return v;
}

static int huff(bits_t *b, const htree_t *t) {
    int node = 0;
    for (int depth = 0; depth < 20; depth++) {
        int c = t->n[node].child[getbits(b, 1)];
        if (c < 0) return -c - 1;
        if (c == 0) break;
        node = c;
    }
    b->err = 1;
    return 0;
}

/* ---- decoder state -------------------------------------------------------------- */

typedef struct {
    float overlap[1024];
    int   prev_shape;
} chstate_t;

typedef struct {
    int     window_sequence, window_shape, max_sfb;
    int     num_windows, num_groups, group_len[8];
    int     num_swb;
    const short *swb;        /* offsets, long: 0..1024, short: 0..128 */
    uint8_t cb[8][64];
    int     sf[8][64];
    int     ms_used[8][64];
    /* tns */
    int     tns_present;
    int     n_filt[8], tns_len[8][4], tns_order[8][4], tns_dir[8][4];
    float   tns_lpc[8][4][21];
    int     quant[1024];
    float   spec[1024];
} ics_t;

struct aac {
    int       rate_index, rate, channels;
    int       object_type;
    chstate_t ch[MAX_CH];
    ics_t     ics[2];
    uint32_t  noise_state;
    float     out[MAX_CH][1024];
    int       out_kind[MAX_CH];       /* 0 unused, 1 SCE / LFE, 2 CPE left, 3 CPE right */
    int       out_count;
    const uint8_t *frame;
};

/* ---- ics_info / sections / scale factors ---------------------------------------- */

static void swb_tables(const aac_t *a, ics_t *s) {
    int ri = a->rate_index > 11 ? 11 : a->rate_index;
    if (s->window_sequence == EIGHT_SHORT) {
        s->num_swb = sfBandTotalShort[ri];
        s->swb = &sfBandTabShort[sfBandTabShortOffset[ri]];
    } else {
        s->num_swb = sfBandTotalLong[ri];
        s->swb = &sfBandTabLong[sfBandTabLongOffset[ri]];
    }
}

static int ics_info(aac_t *a, bits_t *b, ics_t *s) {
    getbits(b, 1);                              /* reserved */
    s->window_sequence = (int)getbits(b, 2);
    s->window_shape = (int)getbits(b, 1);
    if (s->window_sequence == EIGHT_SHORT) {
        unsigned grouping;
        s->max_sfb = (int)getbits(b, 4);
        grouping = getbits(b, 7);
        s->num_windows = 8;
        s->num_groups = 1;
        s->group_len[0] = 1;
        for (int i = 6; i >= 0; i--) {
            if (grouping & (1u << i)) s->group_len[s->num_groups - 1]++;
            else s->group_len[s->num_groups++] = 1;
        }
    } else {
        s->max_sfb = (int)getbits(b, 6);
        s->num_windows = 1;
        s->num_groups = 1;
        s->group_len[0] = 1;
        if (getbits(b, 1)) return -1;           /* prediction: not in LC */
    }
    swb_tables(a, s);
    return s->max_sfb > s->num_swb ? -1 : 0;
}

static int section_data(bits_t *b, ics_t *s) {
    int bits = s->window_sequence == EIGHT_SHORT ? 3 : 5, esc = (1 << bits) - 1;
    for (int g = 0; g < s->num_groups; g++) {
        int k = 0;
        while (k < s->max_sfb) {
            int cb = (int)getbits(b, 4), len = 0, inc;
            if (cb == 12) return -1;
            do {
                inc = (int)getbits(b, bits);
                len += inc;
                if (b->err) return -1;
            } while (inc == esc);
            if (k + len > s->max_sfb) return -1;
            for (int i = 0; i < len; i++) s->cb[g][k++] = (uint8_t)cb;
        }
        for (; k < 64; k++) s->cb[g][k] = ZERO_HCB;
    }
    return 0;
}

static int scale_factors(bits_t *b, ics_t *s, int global_gain) {
    int sf = global_gain, is_pos = 0, noise = global_gain - 90, noise_first = 1;
    for (int g = 0; g < s->num_groups; g++) {
        for (int k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k];
            if (cb == ZERO_HCB) s->sf[g][k] = 0;
            else if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2) {
                is_pos += huff(b, &trees[0]) - 60;
                s->sf[g][k] = is_pos;
            } else if (cb == NOISE_HCB) {
                if (noise_first) {
                    noise += (int)getbits(b, 9) - 256;
                    noise_first = 0;
                } else noise += huff(b, &trees[0]) - 60;
                s->sf[g][k] = noise;
            } else {
                sf += huff(b, &trees[0]) - 60;
                if (sf < 0 || sf > 255) return -1;
                s->sf[g][k] = sf;
            }
        }
    }
    return b->err ? -1 : 0;
}

/* ---- TNS --------------------------------------------------------------------------- */

static int tns_data(bits_t *b, ics_t *s) {
    int is_short = s->window_sequence == EIGHT_SHORT;
    for (int w = 0; w < s->num_windows; w++) {
        int res = 0;
        s->n_filt[w] = (int)getbits(b, is_short ? 1 : 2);
        if (s->n_filt[w]) res = (int)getbits(b, 1);
        for (int f = 0; f < s->n_filt[w]; f++) {
            int order;
            s->tns_len[w][f] = (int)getbits(b, is_short ? 4 : 6);
            order = s->tns_order[w][f] = (int)getbits(b, is_short ? 3 : 5);
            if (order > 20) return -1;
            if (order) {
                int compress, nbits;
                float tmp[21], lpc[21], prev[21];
                double iqfac = ((1 << (res + 2)) - 0.5) / (M_PI / 2), iqfac_m = ((1 << (res + 2)) + 0.5) / (M_PI / 2);
                s->tns_dir[w][f] = (int)getbits(b, 1);
                compress = (int)getbits(b, 1);
                nbits = res + 3 - compress;
                for (int i = 0; i < order; i++) {
                    int v = (int)getbits(b, nbits);
                    if (v & (1 << (nbits - 1))) v -= 1 << nbits;       /* sign extend */
                    tmp[i] = (float)(v >= 0 ? sin(v / iqfac) : sin(v / iqfac_m));
                }
                /* reflection coefficients -> direct form */
                lpc[0] = 1;
                for (int m = 1; m <= order; m++) {
                    for (int i = 0; i < m; i++) prev[i] = lpc[i];
                    for (int i = 1; i < m; i++) lpc[i] = prev[i] + tmp[m - 1] * prev[m - i];
                    lpc[m] = tmp[m - 1];
                }
                memcpy(s->tns_lpc[w][f], lpc, sizeof(float) * (size_t)(order + 1));
            }
        }
    }
    return b->err ? -1 : 0;
}

static void tns_apply(const aac_t *a, ics_t *s) {
    int is_short = s->window_sequence == EIGHT_SHORT, ri = a->rate_index > 12 ? 12 : a->rate_index;
    int maxb = is_short ? tns_max_short[ri] : tns_max_long[ri];
    int max_order = is_short ? 7 : 12;
    if (maxb > s->max_sfb) maxb = s->max_sfb;
    for (int w = 0; w < s->num_windows; w++) {
        int top = s->num_swb;
        float *spec = s->spec + w * 128;
        for (int f = 0; f < s->n_filt[w]; f++) {
            int bottom = top - s->tns_len[w][f], order = s->tns_order[w][f];
            int start, end, size, inc, i0;
            if (bottom < 0) bottom = 0;
            if (order > max_order) order = max_order;
            start = s->swb[bottom < maxb ? bottom : maxb];
            end = s->swb[top < maxb ? top : maxb];
            top = bottom;
            size = end - start;
            if (!order || size <= 0) continue;
            {
                const float *lpc = s->tns_lpc[w][f];
                float state[24] = { 0 };
                if (s->tns_dir[w][f]) {
                    inc = -1;
                    i0 = end - 1;
                } else {
                    inc = 1;
                    i0 = start;
                }
                for (int n = 0, i = i0; n < size; n++, i += inc) {
                    float y = spec[i];
                    for (int j = 0; j < order; j++) y -= lpc[j + 1] * state[j];
                    for (int j = order - 1; j > 0; j--) state[j] = state[j - 1];
                    state[0] = y;
                    spec[i] = y;
                }
            }
        }
    }
}

/* ---- spectral data ------------------------------------------------------------------ */

static int escape(bits_t *b, int v) {
    int n = 0, sign = v < 0;
    if ((sign ? -v : v) != 16) return v;
    while (getbits(b, 1)) {
        if (++n > 8) {
            b->err = 1;
            return 0;
        }
    }
    v = (1 << (n + 4)) + (int)getbits(b, n + 4);
    return sign ? -v : v;
}

static int spectral_data(bits_t *b, ics_t *s) {
    int win0 = 0;
    memset(s->quant, 0, sizeof s->quant);
    for (int g = 0; g < s->num_groups; g++) {
        for (int k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k];
            int width = s->swb[k + 1] - s->swb[k];
            if (cb == ZERO_HCB || cb >= NOISE_HCB) continue;
            for (int w = 0; w < s->group_len[g]; w++) {
                int *q = s->quant + (win0 + w) * 128 + s->swb[k];
                const htree_t *t = &trees[cb];
                if (cb <= 4) {
                    for (int i = 0; i < width; i += 4) {
                        int idx = huff(b, t), v[4];
                        if (cb <= 2) {
                            v[0] = idx / 27 - 1;
                            v[1] = idx / 9 % 3 - 1;
                            v[2] = idx / 3 % 3 - 1;
                            v[3] = idx % 3 - 1;
                        } else {
                            v[0] = idx / 27;
                            v[1] = idx / 9 % 3;
                            v[2] = idx / 3 % 3;
                            v[3] = idx % 3;
                            for (int j = 0; j < 4; j++)
                                if (v[j] && getbits(b, 1)) v[j] = -v[j];
                        }
                        for (int j = 0; j < 4; j++) q[i + j] = v[j];
                    }
                } else {
                    for (int i = 0; i < width; i += 2) {
                        int idx = huff(b, t), y, z;
                        if (cb <= 6) {
                            y = idx / 9 - 4;
                            z = idx % 9 - 4;
                        } else {
                            int mod = cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
                            y = idx / mod;
                            z = idx % mod;
                            if (y && getbits(b, 1)) y = -y;
                            if (z && getbits(b, 1)) z = -z;
                            if (cb == ESC_HCB) {
                                y = escape(b, y);
                                z = escape(b, z);
                            }
                        }
                        q[i] = y;
                        q[i + 1] = z;
                    }
                }
                if (b->err) return -1;
            }
        }
        win0 += s->group_len[g];
    }
    return 0;
}

/* quantized values -> spectrum (intensity bands stay 0, noise bands get noise) */
static void dequant(aac_t *a, ics_t *s) {
    int win0 = 0;
    memset(s->spec, 0, sizeof s->spec);
    for (int g = 0; g < s->num_groups; g++) {
        for (int k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k], width = s->swb[k + 1] - s->swb[k];
            for (int w = 0; w < s->group_len[g]; w++) {
                int off = (win0 + w) * 128 + s->swb[k];
                float *x = s->spec + off;
                const int *q = s->quant + off;
                if (cb == ZERO_HCB || cb == INTENSITY_HCB || cb == INTENSITY_HCB2) continue;
                if (cb == NOISE_HCB) {
                    float energy = 0, scale;
                    for (int i = 0; i < width; i++) {
                        a->noise_state = a->noise_state * 1664525u + 1013904223u;
                        x[i] = (float)(int32_t)a->noise_state;
                        energy += x[i] * x[i];
                    }
                    {
                        int e = s->sf[g][k] + 200;
                        scale = (e >= 0 && e < 512 ? pow2sf[e] : 0) / sqrtf(energy > 0 ? energy : 1);
                    }
                    for (int i = 0; i < width; i++) x[i] *= scale;
                    continue;
                }
                {
                    float gain = pow2sf[s->sf[g][k] - 100 + 200];
                    for (int i = 0; i < width; i++) {
                        int v = q[i], m = v < 0 ? -v : v;
                        float f = m < 8192 ? pow43[m] : 0;
                        x[i] = (v < 0 ? -f : f) * gain;
                    }
                }
            }
        }
        win0 += s->group_len[g];
    }
}

/* ---- one ics -------------------------------------------------------------------------- */

static int ics_decode(aac_t *a, bits_t *b, ics_t *s, int common_window) {
    int global_gain = (int)getbits(b, 8);
    if (!common_window && ics_info(a, b, s) < 0) return -1;
    if (section_data(b, s) < 0) return -1;
    if (scale_factors(b, s, global_gain) < 0) return -1;
    /* pulses */
    {
        int pulse = (int)getbits(b, 1), np = 0, start = 0, off[4] = { 0 }, amp[4] = { 0 };
        if (pulse) {
            if (s->window_sequence == EIGHT_SHORT) return -1;
            np = (int)getbits(b, 2) + 1;
            start = (int)getbits(b, 6);
            for (int i = 0; i < np; i++) {
                off[i] = (int)getbits(b, 5);
                amp[i] = (int)getbits(b, 4);
            }
            if (start >= s->num_swb) return -1;
        }
        s->tns_present = (int)getbits(b, 1);
        if (s->tns_present && tns_data(b, s) < 0) return -1;
        if (getbits(b, 1)) return -1;           /* gain control: SSR only */
        if (spectral_data(b, s) < 0) return -1;
        if (pulse) {
            int k = s->swb[start];
            for (int i = 0; i < np; i++) {
                k += off[i];
                if (k >= 1024) break;
                if (s->quant[k] >= 0) s->quant[k] += amp[i];
                else s->quant[k] -= amp[i];
            }
        }
    }
    dequant(a, s);
    return 0;
}

/* ---- stereo tools --------------------------------------------------------------------- */

static void stereo(ics_t *l, ics_t *r, int ms_present) {
    int win0 = 0;
    for (int g = 0; g < r->num_groups; g++) {
        for (int k = 0; k < r->max_sfb; k++) {
            int cbr = r->cb[g][k], width = r->swb[k + 1] - r->swb[k];
            int ms = ms_present == 2 || (ms_present == 1 && l->ms_used[g][k]);
            for (int w = 0; w < r->group_len[g]; w++) {
                int off = (win0 + w) * 128 + r->swb[k];
                float *xl = l->spec + off, *xr = r->spec + off;
                if (cbr == INTENSITY_HCB || cbr == INTENSITY_HCB2) {
                    int e = 200 - r->sf[g][k];
                    float scale = e >= 0 && e < 512 ? pow2sf[e] : 0;
                    if (cbr == INTENSITY_HCB2) scale = -scale;
                    if (ms_present == 1 && l->ms_used[g][k]) scale = -scale;
                    for (int i = 0; i < width; i++) xr[i] = xl[i] * scale;
                } else if (ms && cbr != NOISE_HCB && l->cb[g][k] != NOISE_HCB) {
                    for (int i = 0; i < width; i++) {
                        float m = xl[i], sd = xr[i];
                        xl[i] = m + sd;
                        xr[i] = m - sd;
                    }
                }
            }
        }
        win0 += r->group_len[g];
    }
}

/* ---- filterbank ---------------------------------------------------------------------- */

typedef struct { float re, im; } cpx_t;

static void fft(cpx_t *z, int n, const float *wc, const float *ws) {
    /* in-place radix-2, natural order in and out, X[k] = sum x[j] e^{-2 pi i jk/n} */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            cpx_t t = z[i];
            z[i] = z[j];
            z[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float c = wc[k * step], s = ws[k * step];
                cpx_t *u = &z[i + k], *v = &z[i + k + half];
                float tr = v->re * c - v->im * s, ti = v->re * s + v->im * c;
                v->re = u->re - tr;
                v->im = u->im - ti;
                u->re += tr;
                u->im += ti;
            }
        }
    }
}

/* out[0..n-1] = IMDCT of in[0..n/2-1]: x[n] = 2/N sum X[k] cos(2pi/N (n+n0)(k+1/2)).
 * The spectrum goes through a DCT-IV (M = n/2 points, an M/2-point FFT with
 * pre and post twiddles); the IMDCT is that DCT-IV unfolded with its
 * symmetries. */
static void imdct(const float *in, float *out, int n) {
    int M = n / 2, q = M / 2, h = M / 2;
    const float *pc = n == 2048 ? pre_lc : pre_sc, *ps = n == 2048 ? pre_ls : pre_ss;
    const float *oc = n == 2048 ? post_lc : post_sc, *os = n == 2048 ? post_ls : post_ss;
    cpx_t z[512];
    float u[1024];
    for (int j = 0; j < q; j++) {
        float a = in[2 * j], b = in[M - 1 - 2 * j];
        z[j].re = a * pc[j] + b * ps[j];
        z[j].im = b * pc[j] - a * ps[j];
    }
    if (n == 2048) fft(z, 512, fft_c512, fft_s512);
    else fft(z, 64, fft_c64, fft_s64);
    for (int p = 0; p < q; p++) {
        float vr = z[p].re * oc[p] + z[p].im * os[p];
        float vi = z[p].im * oc[p] - z[p].re * os[p];
        u[2 * p] = vr;
        u[M - 1 - 2 * p] = -vi;
    }
    for (int i = 0; i < h; i++) out[i] = u[i + h];
    for (int i = h; i < 3 * h; i++) out[i] = -u[3 * h - 1 - i];
    for (int i = 3 * h; i < n; i++) out[i] = -u[i - 3 * h];
}

static void filterbank(chstate_t *c, const ics_t *s, float *pcm) {
    float buf[2048];
    const float *rise = c->prev_shape ? kbd_long : sine_long, *rise_s = c->prev_shape ? kbd_short : sine_short;
    const float *fall = s->window_shape ? kbd_long : sine_long, *fall_s = s->window_shape ? kbd_short : sine_short;
    if (s->window_sequence != EIGHT_SHORT) {
        imdct(s->spec, buf, 2048);
        if (s->window_sequence == LONG_STOP) {
            for (int i = 0; i < 448; i++) buf[i] = 0;
            for (int i = 0; i < 128; i++) buf[448 + i] *= rise_s[i];
        } else {
            for (int i = 0; i < 1024; i++) buf[i] *= rise[i];
        }
        if (s->window_sequence == LONG_START) {
            for (int i = 0; i < 128; i++) buf[1472 + i] *= fall_s[127 - i];
            for (int i = 1600; i < 2048; i++) buf[i] = 0;
        } else {
            for (int i = 0; i < 1024; i++) buf[1024 + i] *= fall[1023 - i];
        }
    } else {
        float sb[256];
        memset(buf, 0, sizeof buf);
        for (int w = 0; w < 8; w++) {
            const float *r = w == 0 ? rise_s : fall_s;
            float *o = buf + 448 + w * 128;
            imdct(s->spec + w * 128, sb, 256);
            for (int i = 0; i < 128; i++) {
                o[i] += sb[i] * r[i];
                o[128 + i] += sb[128 + i] * fall_s[127 - i];
            }
        }
    }
    for (int i = 0; i < 1024; i++) {
        pcm[i] = c->overlap[i] + buf[i];
        c->overlap[i] = buf[1024 + i];
    }
    c->prev_shape = s->window_shape;
}

static void finish_channel(aac_t *a, ics_t *s, int kind) {
    if (a->out_count >= MAX_CH) return;
    if (s->tns_present) tns_apply(a, s);
    filterbank(&a->ch[a->out_count], s, a->out[a->out_count]);
    a->out_kind[a->out_count++] = kind;
}

/* ---- public ---------------------------------------------------------------------------- */

aac_t *aac_open(const uint8_t *asc, int asc_len) {
    bits_t b = { asc, asc_len, 0, 0 };
    int aot, sri, chc;
    if (!asc || asc_len < 2) return 0;
    aot = (int)getbits(&b, 5);
    if (aot == 31) aot = 32 + (int)getbits(&b, 6);
    sri = (int)getbits(&b, 4);
    if (sri == 15) return 0;                    /* explicit rate: not used in practice */
    chc = (int)getbits(&b, 4);
    if (aot == 5 || aot == 29) {                /* explicit SBR / PS: decode the core */
        int ext = (int)getbits(&b, 4);
        if (ext == 15) getbits(&b, 24);
        aot = (int)getbits(&b, 5);
    }
    if (aot != 2 && aot != 1 && aot != 4) return 0;
    if (getbits(&b, 1)) return 0;               /* 960-sample frames */
    return aac_open_params(sri, chc);
}

aac_t *aac_open_params(int rate_index, int channel_config) {
    aac_t *a;
    if (rate_index < 0 || rate_index > 12) return 0;
    build_tables();
    a = (aac_t *)calloc(1, sizeof(aac_t));
    if (!a) return 0;
    a->rate_index = rate_index;
    a->rate = rate_tab[rate_index];
    a->channels = channel_config == 1 ? 1 : 2;
    a->noise_state = 0x1F2E3D4Cu;
    return a;
}

int aac_rate(const aac_t *a) {
    return a ? a->rate : 0;
}

int aac_channels(const aac_t *a) {
    return a ? a->channels : 0;
}

static inline int16_t clip16(float v) {
    int i = (int)(v + (v >= 0 ? 0.5f : -0.5f));
    return (int16_t)(i > 32767 ? 32767 : i < -32768 ? -32768 : i);
}

int aac_decode(aac_t *a, const uint8_t *au, int len, int16_t *out, int out_channels) {
    bits_t b = { au, len, 0, 0 };
    int elements = 0;
    a->out_count = 0;
    while (!b.err && b.pos + 3 <= len * 8) {
        int id = (int)getbits(&b, 3);
        if (id == ID_END) break;
        if (++elements > 16) return 0;
        if (id == ID_SCE || id == ID_LFE) {
            getbits(&b, 4);
            if (ics_decode(a, &b, &a->ics[0], 0) < 0) return 0;
            finish_channel(a, &a->ics[0], id == ID_LFE ? 4 : 1);
        } else if (id == ID_CPE) {
            int common, ms = 0;
            ics_t *l = &a->ics[0], *r = &a->ics[1];
            getbits(&b, 4);
            common = (int)getbits(&b, 1);
            if (common) {
                if (ics_info(a, &b, l) < 0) return 0;
                ms = (int)getbits(&b, 2);
                if (ms == 1)
                    for (int g = 0; g < l->num_groups; g++)
                        for (int k = 0; k < l->max_sfb; k++) l->ms_used[g][k] = (int)getbits(&b, 1);
                if (ms == 3) return 0;
                /* the right channel shares the window layout */
                r->window_sequence = l->window_sequence;
                r->window_shape = l->window_shape;
                r->max_sfb = l->max_sfb;
                r->num_windows = l->num_windows;
                r->num_groups = l->num_groups;
                memcpy(r->group_len, l->group_len, sizeof l->group_len);
                r->num_swb = l->num_swb;
                r->swb = l->swb;
            }
            if (ics_decode(a, &b, l, common) < 0) return 0;
            if (ics_decode(a, &b, r, common) < 0) return 0;
            if (common) stereo(l, r, ms);
            else stereo(l, r, 0);
            finish_channel(a, l, 2);
            finish_channel(a, r, 3);
        } else if (id == ID_FIL) {
            int cnt = (int)getbits(&b, 4);
            if (cnt == 15) cnt += (int)getbits(&b, 8) - 1;
            b.pos += cnt * 8;
        } else if (id == ID_DSE) {
            int align, cnt;
            getbits(&b, 4);
            align = (int)getbits(&b, 1);
            cnt = (int)getbits(&b, 8);
            if (cnt == 255) cnt += (int)getbits(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~7;
            b.pos += cnt * 8;
        } else {
            break;     /* PCE / CCE: keep what was decoded so far */
        }
    }
    if (!a->out_count) return 0;
    /* fold to the requested layout */
    {
        int li = -1, ri = -1, ci = -1;
        for (int i = 0; i < a->out_count; i++) {
            if (a->out_kind[i] == 2 && li < 0) li = i;
            else if (a->out_kind[i] == 3 && ri < 0) ri = i;
            else if (a->out_kind[i] == 1 && ci < 0) ci = i;
        }
        for (int n = 0; n < 1024; n++) {
            float l, r;
            if (li >= 0 && ri >= 0) {
                l = a->out[li][n];
                r = a->out[ri][n];
                if (ci >= 0) {
                    l += 0.7071f * a->out[ci][n];
                    r += 0.7071f * a->out[ci][n];
                }
            } else {
                l = r = a->out[ci >= 0 ? ci : 0][n];
            }
            if (out_channels == 1) out[n] = clip16((l + r) * 0.5f);
            else {
                out[2 * n] = clip16(l);
                out[2 * n + 1] = clip16(r);
            }
        }
    }
    return 1024;
}

void aac_reset(aac_t *a) {
    if (!a) return;
    for (int i = 0; i < MAX_CH; i++) {
        memset(a->ch[i].overlap, 0, sizeof a->ch[i].overlap);
        a->ch[i].prev_shape = 0;
    }
}

void aac_close(aac_t *a) {
    free(a);
}

/* ADTS: 7 or 9 byte header before each frame (.aac files) */
int aac_adts_parse(const uint8_t *p, int len, int *rate_index, int *channel_config, int *frame_len, int *header_len) {
    if (len < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return -1;
    *rate_index = (p[2] >> 2) & 15;
    *channel_config = ((p[2] & 1) << 2) | (p[3] >> 6);
    *frame_len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    *header_len = (p[1] & 1) ? 7 : 9;
    return *frame_len > *header_len ? 0 : -1;
}
