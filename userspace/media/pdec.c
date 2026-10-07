/* Packet audio decoders: AAC (aac.c), Opus (libopus, BSD) and Vorbis
 * (stb_vorbis; its push API reads Ogg pages, so each packet from WebM is
 * wrapped in a small page of its own). */
#include <stdlib.h>
#include <string.h>
#include "pdec.h"
#include "container.h"
#include "aac.h"
#include "opus_multistream.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

struct pdec {
    int    codec;
    int    rate, channels;            /* output */
    aac_t *aac;
    /* opus */
    OpusMSDecoder *opus;
    int    opus_ch;
    uint8_t opus_head[64];
    int    opus_head_len;
    /* vorbis */
    stb_vorbis *vorbis;
    uint8_t *vhdr;                    /* the header pages, for reset */
    int     vhdr_len;
    uint32_t vseq;
    uint8_t *page;
    int     page_cap;
};

const char *pdec_codec_name(int codec) {
    switch (codec) {
    case CODEC_AAC: return "AAC";
    case CODEC_MP3: return "MP3";
    case CODEC_OPUS: return "Opus";
    case CODEC_VORBIS: return "Vorbis";
    default: return "an unknown codec";
    }
}

static inline int16_t clip16(int v) {
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* ---- Opus ------------------------------------------------------------------------ */

static int opus_start(pdec_t *d) {
    const uint8_t *h = d->opus_head;
    int ch = 2, family = 0, streams = 1, coupled = 1, err = 0;
    unsigned char map[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    if (d->opus_head_len >= 19 && !memcmp(h, "OpusHead", 8)) {
        ch = h[9];
        family = h[18];
        if (family && d->opus_head_len >= 21 + ch && ch <= 8) {
            streams = h[19];
            coupled = h[20];
            memcpy(map, h + 21, (size_t)ch);
        }
    }
    if (ch < 1 || ch > 8) return -1;
    if (!family) {
        streams = 1;
        coupled = ch == 2;
    }
    d->opus = opus_multistream_decoder_create(48000, ch, streams, coupled, map, &err);
    if (!d->opus || err != OPUS_OK) return -1;
    d->opus_ch = ch;
    d->rate = 48000;
    d->channels = ch == 1 ? 1 : 2;
    return 0;
}

/* MP4's dOps is OpusHead without the magic, version 0 and big endian */
static void opus_head_from(pdec_t *d, const uint8_t *cfg, int len) {
    if (len >= 8 && !memcmp(cfg, "OpusHead", 8)) {
        d->opus_head_len = len < (int)sizeof d->opus_head ? len : (int)sizeof d->opus_head;
        memcpy(d->opus_head, cfg, (size_t)d->opus_head_len);
    } else if (len >= 11) {
        uint8_t *h = d->opus_head;
        memcpy(h, "OpusHead", 8);
        h[8] = 1;
        h[9] = cfg[1];
        h[10] = cfg[3];
        h[11] = cfg[2];
        h[12] = cfg[7];
        h[13] = cfg[6];
        h[14] = cfg[5];
        h[15] = cfg[4];
        h[16] = cfg[9];
        h[17] = cfg[8];
        h[18] = cfg[10];
        d->opus_head_len = 19;
        if (cfg[10] && len >= 13 + cfg[1] && cfg[1] <= 8) {
            memcpy(h + 19, cfg + 11, (size_t)(2 + cfg[1]));
            d->opus_head_len = 21 + cfg[1];
        }
    } else {
        d->opus_head_len = 0;
    }
}

static int opus_frames(pdec_t *d, const uint8_t *pkt, int len, int16_t *out, int max_frames) {
    static int16_t tmp[PDEC_MAX_FRAMES * 8];
    int n = opus_multistream_decode(d->opus, pkt, len, tmp, max_frames < PDEC_MAX_FRAMES ? max_frames : PDEC_MAX_FRAMES, 0);
    if (n <= 0) return 0;
    if (d->opus_ch <= 2) {
        memcpy(out, tmp, (size_t)n * (size_t)d->opus_ch * 2);
        return n;
    }
    /* Vorbis channel order: FL FC FR RL RR LFE ... folded to stereo */
    for (int i = 0; i < n; i++) {
        const int16_t *s = tmp + i * d->opus_ch;
        int l, r;
        if (d->opus_ch >= 5) {
            l = s[0] + (s[1] * 181 >> 8) + (s[3] * 128 >> 8);
            r = s[2] + (s[1] * 181 >> 8) + (s[4] * 128 >> 8);
        } else if (d->opus_ch == 3) {
            l = s[0] + (s[1] * 181 >> 8);
            r = s[2] + (s[1] * 181 >> 8);
        } else {
            l = s[0] + (s[2] * 181 >> 8);
            r = s[1] + (s[3] * 181 >> 8);
        }
        out[2 * i] = clip16(l * 3 / 4);
        out[2 * i + 1] = clip16(r * 3 / 4);
    }
    return n;
}

/* ---- Vorbis in Ogg pages -------------------------------------------------------------- */

static uint32_t ogg_crc(const uint8_t *p, int n) {
    static uint32_t table[256];
    uint32_t crc = 0;
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = r & 0x80000000u ? (r << 1) ^ 0x04C11DB7u : r << 1;
            table[i] = r;
        }
    for (int i = 0; i < n; i++) crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

/* one page holding the given packets; returns its length */
static int ogg_page(uint8_t *dst, int bos, uint32_t seq, const uint8_t *const *pk, const int *len, int count) {
    int segs = 0, pos;
    uint8_t *lace = dst + 27;
    for (int i = 0; i < count; i++) {
        int n = len[i];
        while (n >= 255) {
            lace[segs++] = 255;
            n -= 255;
        }
        lace[segs++] = (uint8_t)n;
    }
    memcpy(dst, "OggS", 4);
    dst[4] = 0;
    dst[5] = bos ? 2 : 0;
    memset(dst + 6, 0, 8);                  /* granule position */
    dst[14] = 0x49; dst[15] = 0x43; dst[16] = 0x44; dst[17] = 0x41;   /* serial */
    dst[18] = (uint8_t)seq; dst[19] = (uint8_t)(seq >> 8); dst[20] = (uint8_t)(seq >> 16); dst[21] = (uint8_t)(seq >> 24);
    memset(dst + 22, 0, 4);
    dst[26] = (uint8_t)segs;
    pos = 27 + segs;
    for (int i = 0; i < count; i++) {
        memcpy(dst + pos, pk[i], (size_t)len[i]);
        pos += len[i];
    }
    {
        uint32_t c = ogg_crc(dst, pos);
        dst[22] = (uint8_t)c; dst[23] = (uint8_t)(c >> 8); dst[24] = (uint8_t)(c >> 16); dst[25] = (uint8_t)(c >> 24);
    }
    return pos;
}

static int vorbis_open_headers(pdec_t *d) {
    int used = 0, err = 0;
    if (d->vorbis) stb_vorbis_close(d->vorbis);
    d->vorbis = stb_vorbis_open_pushdata(d->vhdr, d->vhdr_len, &used, &err, 0);
    if (!d->vorbis) return -1;
    d->vseq = 2;
    return 0;
}

/* CodecPrivate: 2, Xiph-laced sizes of the first two headers, then the three headers */
static int vorbis_start(pdec_t *d, const uint8_t *cfg, int len) {
    int pos = 1, sz[3], sum = 0;
    const uint8_t *pk[3];
    stb_vorbis_info info;
    if (len < 3 || cfg[0] != 2) return -1;
    for (int i = 0; i < 2; i++) {
        sz[i] = 0;
        while (pos < len && cfg[pos] == 255) {
            sz[i] += 255;
            pos++;
        }
        if (pos >= len) return -1;
        sz[i] += cfg[pos++];
        sum += sz[i];
    }
    sz[2] = len - pos - sum;
    if (sz[2] <= 0) return -1;
    pk[0] = cfg + pos;
    pk[1] = pk[0] + sz[0];
    pk[2] = pk[1] + sz[1];
    d->vhdr = (uint8_t *)malloc((size_t)len + 600);
    if (!d->vhdr) return -1;
    d->vhdr_len = ogg_page(d->vhdr, 1, 0, pk, sz, 1);
    d->vhdr_len += ogg_page(d->vhdr + d->vhdr_len, 0, 1, pk + 1, sz + 1, 2);
    if (vorbis_open_headers(d) < 0) return -1;
    info = stb_vorbis_get_info(d->vorbis);
    d->rate = (int)info.sample_rate;
    d->channels = info.channels >= 2 ? 2 : 1;
    return 0;
}

static int vorbis_frames(pdec_t *d, const uint8_t *pkt, int len, int16_t *out, int max_frames) {
    int plen, pos = 0, total = 0;
    if (d->page_cap < len + 400) {
        uint8_t *n = (uint8_t *)realloc(d->page, (size_t)len + 400);
        if (!n) return 0;
        d->page = n;
        d->page_cap = len + 400;
    }
    plen = ogg_page(d->page, 0, d->vseq++, &pkt, &len, 1);
    while (pos < plen) {
        int ch = 0, samples = 0;
        float **o = 0;
        int used = stb_vorbis_decode_frame_pushdata(d->vorbis, d->page + pos, plen - pos, &ch, &o, &samples);
        if (used <= 0) break;
        pos += used;
        for (int i = 0; i < samples && total < max_frames; i++, total++) {
            float l = o[0][i], r = ch > 1 ? o[1][i] : l;
            if (d->channels == 1) out[total] = clip16((int)(l * 32767.0f));
            else {
                out[2 * total] = clip16((int)(l * 32767.0f));
                out[2 * total + 1] = clip16((int)(r * 32767.0f));
            }
        }
    }
    return total;
}

/* ---- interface ------------------------------------------------------------------------- */

pdec_t *pdec_open(int codec, const uint8_t *cfg, int cfg_len, int rate, int channels) {
    pdec_t *d = (pdec_t *)calloc(1, sizeof(pdec_t));
    if (!d) return 0;
    d->codec = codec;
    (void)rate;
    (void)channels;
    if (codec == CODEC_AAC) {
        d->aac = aac_open(cfg, cfg_len);
        if (d->aac) {
            d->rate = aac_rate(d->aac);
            d->channels = aac_channels(d->aac);
        }
    } else if (codec == CODEC_OPUS) {
        opus_head_from(d, cfg, cfg_len);
        if (opus_start(d) < 0) d->rate = 0;
    } else if (codec == CODEC_VORBIS) {
        if (vorbis_start(d, cfg, cfg_len) < 0) d->rate = 0;
    }
    if (!d->rate) {
        pdec_close(d);
        return 0;
    }
    return d;
}

int pdec_rate(const pdec_t *d) { return d->rate; }
int pdec_channels(const pdec_t *d) { return d->channels; }

int pdec_decode(pdec_t *d, const uint8_t *pkt, int len, int16_t *out, int max_frames) {
    if (d->aac) return max_frames >= 1024 ? aac_decode(d->aac, pkt, len, out, d->channels) : 0;
    if (d->opus) return opus_frames(d, pkt, len, out, max_frames);
    if (d->vorbis) return vorbis_frames(d, pkt, len, out, max_frames);
    return 0;
}

void pdec_reset(pdec_t *d) {
    if (d->aac) aac_reset(d->aac);
    if (d->opus) opus_multistream_decoder_ctl(d->opus, OPUS_RESET_STATE);
    if (d->vorbis) vorbis_open_headers(d);
}

void pdec_close(pdec_t *d) {
    if (!d) return;
    if (d->aac) aac_close(d->aac);
    if (d->opus) opus_multistream_decoder_destroy(d->opus);
    if (d->vorbis) stb_vorbis_close(d->vorbis);
    free(d->vhdr);
    free(d->page);
    free(d);
}
