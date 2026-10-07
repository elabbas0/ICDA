/* MP4 / MOV / M4A demuxer: classic sample tables (stbl) and fragmented
 * files (moof/traf/trun, as YouTube's DASH streams are). */
#include <stdlib.h>
#include <string.h>
#include "container.h"

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static uint32_t rd16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

/* ---- shared container helpers ------------------------------------------------ */

mtrack_t *container_track(mfile_t *f, int kind) {
    for (int i = 0; i < f->ntracks; i++)
        if (f->tracks[i].kind == kind && f->tracks[i].codec != CODEC_UNKNOWN) return &f->tracks[i];
    return 0;
}

int container_add_sample(mtrack_t *t, uint64_t off, uint32_t size, int key, int64_t pts_us) {
    return container_add_sample_dts(t, off, size, key, pts_us, pts_us);
}

int container_add_sample_dts(mtrack_t *t, uint64_t off, uint32_t size, int key, int64_t pts_us, int64_t dts_us) {
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 1024;
        msample_t *s = (msample_t *)realloc(t->s, (size_t)cap * sizeof(msample_t));
        if (!s) return -1;
        t->s = s;
        t->cap = cap;
    }
    t->s[t->n].off = off;
    t->s[t->n].size = size;
    t->s[t->n].key = (uint8_t)key;
    t->s[t->n].pts_us = pts_us;
    t->s[t->n].dts_us = dts_us;
    if (t->n > 0 && dts_us < t->s[t->n - 1].dts_us) t->unsorted = 1;
    t->n++;
    return 0;
}

int container_seek_index(const mtrack_t *t, int64_t t_us) {
    int best = 0;
    for (int i = 0; i < t->n; i++) {
        if (!t->s[i].key) continue;
        if (t->s[i].pts_us <= t_us) best = i;
        else if (i > best) break;
    }
    return best;
}

void container_free(mfile_t *f) {
    for (int i = 0; i < f->ntracks; i++) {
        free(f->tracks[i].s);
        free(f->tracks[i].cfg);
    }
    memset(f, 0, sizeof *f);
}

int container_is_mp4(const uint8_t *d, size_t len) {
    if (len < 12) return 0;
    {
        uint32_t t = rd32(d + 4);
        return t == FOURCC('f', 't', 'y', 'p') || t == FOURCC('m', 'o', 'o', 'v') || t == FOURCC('m', 'd', 'a', 't') ||
               t == FOURCC('w', 'i', 'd', 'e') || t == FOURCC('f', 'r', 'e', 'e') || t == FOURCC('s', 't', 'y', 'p');
    }
}

static int64_t to_us(int64_t v, uint32_t timescale) {
    if (!timescale) return 0;
    return (v / (int64_t)timescale) * 1000000 + (v % (int64_t)timescale) * 1000000 / (int64_t)timescale;
}

/* ---- boxes ------------------------------------------------------------------------- */

typedef struct {
    uint32_t type;
    const uint8_t *body;     /* after the header */
    size_t   len;            /* body length */
    size_t   total;          /* header + body */
} box_t;

/* 1 if a whole box starts at p (within avail), -1 broken, 0 incomplete */
static int next_box(const uint8_t *p, size_t avail, box_t *b) {
    uint64_t size;
    size_t hdr = 8;
    if (avail < 8) return 0;
    size = rd32(p);
    b->type = rd32(p + 4);
    if (size == 1) {
        if (avail < 16) return 0;
        size = rd64(p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = avail;
    }
    if (size < hdr) return -1;
    if (size > avail) return 0;
    b->body = p + hdr;
    b->len = (size_t)size - hdr;
    b->total = (size_t)size;
    return 1;
}

/* the first child of type t inside a box body (skipping `skip` bytes first) */
static int find_child(const uint8_t *body, size_t len, size_t skip, uint32_t t, box_t *out) {
    size_t pos = skip;
    while (pos < len) {
        box_t b;
        if (next_box(body + pos, len - pos, &b) != 1) return 0;
        if (b.type == t) {
            *out = b;
            return 1;
        }
        pos += b.total;
    }
    return 0;
}

static void set_cfg(mtrack_t *t, const uint8_t *p, size_t n) {
    free(t->cfg);
    t->cfg = (uint8_t *)malloc(n ? n : 1);
    if (!t->cfg) {
        t->cfg_len = 0;
        return;
    }
    memcpy(t->cfg, p, n);
    t->cfg_len = (int)n;
}

/* ES descriptor length: up to four 7-bit groups */
static size_t desc_len(const uint8_t **p, const uint8_t *end) {
    size_t n = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        uint8_t c = *(*p)++;
        n = (n << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return n;
}

static void parse_esds(mtrack_t *t, const uint8_t *p, size_t len) {
    const uint8_t *end = p + len;
    p += 4;                                     /* version, flags */
    if (p >= end || *p++ != 0x03) return;
    desc_len(&p, end);
    if (p + 3 > end) return;
    {
        uint8_t flags = p[2];
        p += 3;
        if (flags & 0x80) p += 2;
        if (flags & 0x40) p += 1 + (p < end ? *p : 0);
        if (flags & 0x20) p += 2;
    }
    if (p >= end || *p++ != 0x04) return;
    desc_len(&p, end);
    if (p + 13 > end) return;
    {
        uint8_t oti = p[0];
        if (oti == 0x40 || oti == 0x66 || oti == 0x67 || oti == 0x68) t->codec = CODEC_AAC;
        else if (oti == 0x69 || oti == 0x6B) t->codec = CODEC_MP3;
        p += 13;
    }
    if (p < end && *p == 0x05) {
        size_t n;
        p++;
        n = desc_len(&p, end);
        if (p + n <= end) set_cfg(t, p, n);
    }
}

static void parse_stsd(mtrack_t *t, const uint8_t *p, size_t len) {
    box_t e;
    if (len < 8 || next_box(p + 8, len - 8, &e) != 1) return;
    if (t->kind == TRACK_VIDEO) {
        box_t c;
        if (e.len < 78) return;
        t->width = (int)rd16(e.body + 24);
        t->height = (int)rd16(e.body + 26);
        if (e.type == FOURCC('a', 'v', 'c', '1') || e.type == FOURCC('a', 'v', 'c', '3')) {
            if (find_child(e.body, e.len, 78, FOURCC('a', 'v', 'c', 'C'), &c)) {
                t->codec = CODEC_H264;
                set_cfg(t, c.body, c.len);
            }
        } else if (e.type == FOURCC('h', 'v', 'c', '1') || e.type == FOURCC('h', 'e', 'v', '1')) {
            t->codec = CODEC_HEVC;
        } else if (e.type == FOURCC('v', 'p', '0', '9')) {
            t->codec = CODEC_VP9;
        } else if (e.type == FOURCC('a', 'v', '0', '1')) {
            t->codec = CODEC_AV1;
        }
    } else {
        size_t skip = 28;
        box_t c;
        uint32_t version;
        if (e.len < 28) return;
        version = rd16(e.body + 8);
        t->channels = (int)rd16(e.body + 16);
        t->rate = (int)(rd32(e.body + 24) >> 16);
        if (version == 1) skip += 16;
        else if (version == 2) skip += 36;
        if (e.type == FOURCC('m', 'p', '4', 'a')) {
            if (find_child(e.body, e.len, skip, FOURCC('e', 's', 'd', 's'), &c)) parse_esds(t, c.body, c.len);
            else if (find_child(e.body, e.len, skip, FOURCC('w', 'a', 'v', 'e'), &c)) {
                box_t d;
                if (find_child(c.body, c.len, 0, FOURCC('e', 's', 'd', 's'), &d)) parse_esds(t, d.body, d.len);
            }
        } else if (e.type == FOURCC('O', 'p', 'u', 's')) {
            if (find_child(e.body, e.len, skip, FOURCC('d', 'O', 'p', 's'), &c)) set_cfg(t, c.body, c.len);
            t->codec = CODEC_OPUS;
        } else if (e.type == FOURCC('.', 'm', 'p', '3')) {
            t->codec = CODEC_MP3;
        }
    }
}

/* builds the sample list from a trak's stbl */
static void build_samples(mtrack_t *t, const box_t *stbl) {
    box_t stts, ctts = { 0 }, stss = { 0 }, stsc, stsz, stco;
    int has_ctts, has_stss, co64 = 0;
    uint32_t n, chunks, stsc_n, stts_n, ctts_n = 0, stss_n = 0;
    uint32_t fixed_size;
    if (!find_child(stbl->body, stbl->len, 0, FOURCC('s', 't', 't', 's'), &stts) ||
        !find_child(stbl->body, stbl->len, 0, FOURCC('s', 't', 's', 'c'), &stsc) ||
        !find_child(stbl->body, stbl->len, 0, FOURCC('s', 't', 's', 'z'), &stsz))
        return;
    if (!find_child(stbl->body, stbl->len, 0, FOURCC('s', 't', 'c', 'o'), &stco)) {
        if (!find_child(stbl->body, stbl->len, 0, FOURCC('c', 'o', '6', '4'), &stco)) return;
        co64 = 1;
    }
    has_ctts = find_child(stbl->body, stbl->len, 0, FOURCC('c', 't', 't', 's'), &ctts);
    has_stss = find_child(stbl->body, stbl->len, 0, FOURCC('s', 't', 's', 's'), &stss);
    if (stsz.len < 12 || stts.len < 8 || stsc.len < 8 || stco.len < 8) return;
    fixed_size = rd32(stsz.body + 4);
    n = rd32(stsz.body + 8);
    if (!fixed_size && stsz.len < 12 + (size_t)n * 4) return;
    chunks = rd32(stco.body + 4);
    if (stco.len < 8 + (size_t)chunks * (co64 ? 8 : 4)) return;
    stsc_n = rd32(stsc.body + 4);
    if (stsc.len < 8 + (size_t)stsc_n * 12) return;
    stts_n = rd32(stts.body + 4);
    if (stts.len < 8 + (size_t)stts_n * 8) return;
    if (has_ctts) {
        ctts_n = rd32(ctts.body + 4);
        if (ctts.len < 8 + (size_t)ctts_n * 8) has_ctts = 0;
    }
    if (has_stss) {
        stss_n = rd32(stss.body + 4);
        if (stss.len < 8 + (size_t)stss_n * 4) has_stss = 0;
    }
    {
        uint32_t si = 0, stsc_i = 0, stts_i = 0, stts_left = stts_n ? rd32(stts.body + 8) : 0;
        uint32_t ctts_i = 0, ctts_left = has_ctts && ctts_n ? rd32(ctts.body + 8) : 0, stss_i = 0;
        int64_t dts = 0;
        for (uint32_t c = 0; c < chunks && si < n; c++) {
            uint64_t off = co64 ? rd64(stco.body + 8 + (size_t)c * 8) : rd32(stco.body + 8 + (size_t)c * 4);
            uint32_t per;
            while (stsc_i + 1 < stsc_n && rd32(stsc.body + 8 + (size_t)(stsc_i + 1) * 12) <= c + 1) stsc_i++;
            per = stsc_n ? rd32(stsc.body + 8 + (size_t)stsc_i * 12 + 4) : 0;
            for (uint32_t k = 0; k < per && si < n; k++, si++) {
                uint32_t size = fixed_size ? fixed_size : rd32(stsz.body + 12 + (size_t)si * 4);
                int64_t cts = 0;
                int key = 1;
                if (has_ctts && ctts_i < ctts_n) {
                    cts = (int32_t)rd32(ctts.body + 8 + (size_t)ctts_i * 8 + 4);
                    if (--ctts_left == 0 && ++ctts_i < ctts_n) ctts_left = rd32(ctts.body + 8 + (size_t)ctts_i * 8);
                }
                if (has_stss) {
                    while (stss_i < stss_n && rd32(stss.body + 8 + (size_t)stss_i * 4) < si + 1) stss_i++;
                    key = stss_i < stss_n && rd32(stss.body + 8 + (size_t)stss_i * 4) == si + 1;
                }
                container_add_sample_dts(t, off, size, key, to_us(dts + cts - t->edit_offset, t->timescale),
                                         to_us(dts - t->edit_offset, t->timescale));
                off += size;
                if (stts_i < stts_n) {
                    dts += rd32(stts.body + 8 + (size_t)stts_i * 8 + 4);
                    if (--stts_left == 0 && ++stts_i < stts_n) stts_left = rd32(stts.body + 8 + (size_t)stts_i * 8);
                }
            }
        }
        t->next_dts = dts;
    }
}

static void parse_trak(mfile_t *f, const box_t *trak) {
    box_t tkhd, mdia, mdhd, hdlr, minf, stbl, stsd, edts;
    mtrack_t *t;
    uint32_t handler;
    if (f->ntracks >= MAX_TRACKS) return;
    if (!find_child(trak->body, trak->len, 0, FOURCC('m', 'd', 'i', 'a'), &mdia)) return;
    if (!find_child(mdia.body, mdia.len, 0, FOURCC('h', 'd', 'l', 'r'), &hdlr) || hdlr.len < 12) return;
    handler = rd32(hdlr.body + 8);
    if (handler != FOURCC('v', 'i', 'd', 'e') && handler != FOURCC('s', 'o', 'u', 'n')) return;
    t = &f->tracks[f->ntracks];
    memset(t, 0, sizeof *t);
    t->kind = handler == FOURCC('v', 'i', 'd', 'e') ? TRACK_VIDEO : TRACK_AUDIO;
    if (find_child(trak->body, trak->len, 0, FOURCC('t', 'k', 'h', 'd'), &tkhd) && tkhd.len >= 24)
        t->id = (int)rd32(tkhd.body + (tkhd.body[0] == 1 ? 20 : 12));
    if (find_child(mdia.body, mdia.len, 0, FOURCC('m', 'd', 'h', 'd'), &mdhd) && mdhd.len >= 24)
        t->timescale = rd32(mdhd.body + (mdhd.body[0] == 1 ? 20 : 12));
    if (!t->timescale) t->timescale = 1000;
    /* edit list: where presentation starts in media time */
    if (find_child(trak->body, trak->len, 0, FOURCC('e', 'd', 't', 's'), &edts)) {
        box_t elst;
        if (find_child(edts.body, edts.len, 0, FOURCC('e', 'l', 's', 't'), &elst) && elst.len >= 8) {
            int v1 = elst.body[0] == 1;
            uint32_t cnt = rd32(elst.body + 4);
            size_t esz = v1 ? 20 : 12;
            for (uint32_t i = 0; i < cnt && 8 + (i + 1) * esz <= elst.len; i++) {
                const uint8_t *e = elst.body + 8 + i * esz;
                int64_t mt = v1 ? (int64_t)rd64(e + 8) : (int64_t)(int32_t)rd32(e + 4);
                if (mt >= 0) {
                    t->edit_offset = mt;
                    break;
                }
            }
        }
    }
    if (!find_child(mdia.body, mdia.len, 0, FOURCC('m', 'i', 'n', 'f'), &minf)) return;
    if (!find_child(minf.body, minf.len, 0, FOURCC('s', 't', 'b', 'l'), &stbl)) return;
    if (find_child(stbl.body, stbl.len, 0, FOURCC('s', 't', 's', 'd'), &stsd)) parse_stsd(t, stsd.body, stsd.len);
    if (t->codec == CODEC_UNKNOWN) {
        free(t->cfg);
        return;
    }
    if (t->codec == CODEC_AAC && !t->rate) t->rate = 44100;
    f->ntracks++;
    build_samples(t, &stbl);
}

static void parse_moov(mfile_t *f, const box_t *moov) {
    size_t pos = 0;
    box_t mvhd, mvex;
    if (find_child(moov->body, moov->len, 0, FOURCC('m', 'v', 'h', 'd'), &mvhd) && mvhd.len >= 32) {
        int v1 = mvhd.body[0] == 1;
        uint32_t ts = rd32(mvhd.body + (v1 ? 20 : 12));
        uint64_t dur = v1 ? rd64(mvhd.body + 24) : rd32(mvhd.body + 16);
        if (ts && dur != 0xFFFFFFFFu && dur != ~0ull) f->duration_us = to_us((int64_t)dur, ts);
    }
    while (pos < moov->len) {
        box_t b;
        if (next_box(moov->body + pos, moov->len - pos, &b) != 1) break;
        if (b.type == FOURCC('t', 'r', 'a', 'k')) parse_trak(f, &b);
        pos += b.total;
    }
    if (find_child(moov->body, moov->len, 0, FOURCC('m', 'v', 'e', 'x'), &mvex)) {
        size_t p2 = 0;
        f->fragmented = 1;
        while (p2 < mvex.len) {
            box_t b;
            if (next_box(mvex.body + p2, mvex.len - p2, &b) != 1) break;
            if (b.type == FOURCC('t', 'r', 'e', 'x') && b.len >= 24) {
                int id = (int)rd32(b.body + 4);
                for (int i = 0; i < f->ntracks; i++)
                    if (f->tracks[i].id == id) {
                        f->tracks[i].def_duration = rd32(b.body + 12);
                        f->tracks[i].def_size = rd32(b.body + 16);
                        f->tracks[i].def_flags = rd32(b.body + 20);
                    }
            }
            p2 += b.total;
        }
    }
    f->have_header = 1;
}

/* ---- fragments ------------------------------------------------------------------------ */

static void parse_traf(mfile_t *f, const box_t *traf, uint64_t moof_off) {
    box_t tfhd, b;
    mtrack_t *t = 0;
    uint32_t flags;
    uint64_t base = moof_off;
    uint32_t dur, size, sflags;
    size_t pos = 0;
    if (!find_child(traf->body, traf->len, 0, FOURCC('t', 'f', 'h', 'd'), &tfhd) || tfhd.len < 8) return;
    flags = rd32(tfhd.body) & 0xFFFFFF;
    {
        int id = (int)rd32(tfhd.body + 4);
        for (int i = 0; i < f->ntracks; i++)
            if (f->tracks[i].id == id) t = &f->tracks[i];
    }
    if (!t) return;
    dur = t->def_duration;
    size = t->def_size;
    sflags = t->def_flags;
    {
        const uint8_t *p = tfhd.body + 8, *end = tfhd.body + tfhd.len;
        if ((flags & 0x01) && p + 8 <= end) {
            base = rd64(p);
            p += 8;
        }
        if ((flags & 0x02) && p + 4 <= end) p += 4;
        if ((flags & 0x08) && p + 4 <= end) {
            dur = rd32(p);
            p += 4;
        }
        if ((flags & 0x10) && p + 4 <= end) {
            size = rd32(p);
            p += 4;
        }
        if ((flags & 0x20) && p + 4 <= end) sflags = rd32(p);
    }
    if (find_child(traf->body, traf->len, 0, FOURCC('t', 'f', 'd', 't'), &b) && b.len >= 8)
        t->next_dts = b.body[0] == 1 && b.len >= 12 ? (int64_t)rd64(b.body + 4) : (int64_t)rd32(b.body + 4);
    while (pos < traf->len) {
        if (next_box(traf->body + pos, traf->len - pos, &b) != 1) break;
        pos += b.total;
        if (b.type != FOURCC('t', 'r', 'u', 'n') || b.len < 8) continue;
        {
            uint32_t tf = rd32(b.body) & 0xFFFFFF, cnt = rd32(b.body + 4), first_flags = sflags;
            int v1 = b.body[0] == 1, have_first = 0;
            const uint8_t *p = b.body + 8, *end = b.body + b.len;
            uint64_t off = base;
            if (tf & 0x01) {
                off = base + (uint64_t)(int64_t)(int32_t)rd32(p);
                p += 4;
            }
            if (tf & 0x04) {
                first_flags = rd32(p);
                have_first = 1;
                p += 4;
            }
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t sd = dur, ss = size, sf = i == 0 && have_first ? first_flags : sflags;
                int64_t cts = 0;
                if (tf & 0x100) { if (p + 4 > end) break; sd = rd32(p); p += 4; }
                if (tf & 0x200) { if (p + 4 > end) break; ss = rd32(p); p += 4; }
                if (tf & 0x400) { if (p + 4 > end) break; sf = rd32(p); p += 4; }
                if (tf & 0x800) {
                    if (p + 4 > end) break;
                    cts = v1 ? (int64_t)(int32_t)rd32(p) : (int64_t)rd32(p);
                    p += 4;
                }
                if (i == 0 && have_first) sf = first_flags;
                container_add_sample_dts(t, off, ss, !(sf & 0x10000), to_us(t->next_dts + cts - t->edit_offset, t->timescale),
                                         to_us(t->next_dts - t->edit_offset, t->timescale));
                off += ss;
                t->next_dts += sd;
            }
        }
    }
}

int mp4_parse(mfile_t *f, const uint8_t *data, size_t len) {
    while (f->parsed < len) {
        box_t b;
        int r = next_box(data + f->parsed, len - f->parsed, &b);
        if (r < 0) return f->have_header ? 0 : -1;
        if (r == 0) {
            /* a large mdat that is still arriving does not block what follows */
            if (len - f->parsed >= 16) {
                uint32_t type = rd32(data + f->parsed + 4);
                if (type == FOURCC('m', 'd', 'a', 't')) {
                    uint64_t sz = rd32(data + f->parsed);
                    if (sz == 1) sz = rd64(data + f->parsed + 8);
                    if (!f->have_header || f->fragmented) break;   /* moov after mdat, or fragments: wait */
                    f->parsed += (size_t)sz;                      /* samples are indexed already */
                    continue;
                }
            }
            break;
        }
        if (b.type == FOURCC('m', 'o', 'o', 'v') && !f->have_header) parse_moov(f, &b);
        else if (b.type == FOURCC('m', 'o', 'o', 'f') && f->have_header) {
            size_t pos = 0;
            f->fragmented = 1;
            while (pos < b.len) {
                box_t c;
                if (next_box(b.body + pos, b.len - pos, &c) != 1) break;
                if (c.type == FOURCC('t', 'r', 'a', 'f')) parse_traf(f, &c, f->parsed);
                pos += c.total;
            }
        }
        f->parsed += b.total;
    }
    return f->have_header || f->parsed < len ? 0 : -1;
}

static int cmp_dts(const void *a, const void *b) {
    const msample_t *x = (const msample_t *)a, *y = (const msample_t *)b;
    if (x->dts_us != y->dts_us) return x->dts_us < y->dts_us ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off ? 1 : 0;   /* stable: file order */
}

int container_sort(mtrack_t *t) {
    if (!t->unsorted) return 0;
    qsort(t->s, (size_t)t->n, sizeof(msample_t), cmp_dts);
    t->unsorted = 0;
    return 1;
}
