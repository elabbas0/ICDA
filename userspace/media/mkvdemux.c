/* Matroska / WebM demuxer.
 *
 * Parsed as a flat stream of elements: the Segment and Cluster headers are
 * entered rather than skipped, so clusters of unknown size (live and
 * streamed files) and data that is still arriving both work; anything
 * incomplete waits for the next call. */
#include <stdlib.h>
#include <string.h>
#include "container.h"

#define ID_EBML         0x1A45DFA3u
#define ID_SEGMENT      0x18538067u
#define ID_INFO         0x1549A966u
#define ID_TIMESCALE    0x2AD7B1u
#define ID_DURATION     0x4489u
#define ID_TRACKS       0x1654AE6Bu
#define ID_TRACKENTRY   0xAEu
#define ID_TRACKNUMBER  0xD7u
#define ID_TRACKTYPE    0x83u
#define ID_CODECID      0x86u
#define ID_CODECPRIVATE 0x63A2u
#define ID_CODECDELAY   0x56AAu
#define ID_SEEKPREROLL  0x56BBu
#define ID_VIDEO        0xE0u
#define ID_PIXELWIDTH   0xB0u
#define ID_PIXELHEIGHT  0xBAu
#define ID_AUDIO        0xE1u
#define ID_SAMPLINGFREQ 0xB5u
#define ID_CHANNELS     0x9Fu
#define ID_CLUSTER      0x1F43B675u
#define ID_TIMECODE     0xE7u
#define ID_SIMPLEBLOCK  0xA3u
#define ID_BLOCKGROUP   0xA0u
#define ID_BLOCK        0xA1u
#define ID_REFBLOCK     0xFBu

#define SIZE_UNKNOWN    (~0ull)

int container_is_mkv(const uint8_t *d, size_t len) {
    return len >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3;
}

/* element ID (marker kept); bytes used, 0 incomplete, -1 broken */
static int read_id(const uint8_t *p, size_t avail, uint32_t *id) {
    int n;
    if (!avail) return 0;
    if (p[0] & 0x80) n = 1;
    else if (p[0] & 0x40) n = 2;
    else if (p[0] & 0x20) n = 3;
    else if (p[0] & 0x10) n = 4;
    else return -1;
    if ((size_t)n > avail) return 0;
    *id = 0;
    for (int i = 0; i < n; i++) *id = (*id << 8) | p[i];
    return n;
}

/* variable size integer (marker removed); all ones = unknown */
static int read_vint(const uint8_t *p, size_t avail, uint64_t *v) {
    int n = 1;
    uint64_t x;
    if (!avail) return 0;
    while (n <= 8 && !(p[0] & (0x80 >> (n - 1)))) n++;
    if (n > 8) return -1;
    if ((size_t)n > avail) return 0;
    x = p[0] & (0xFF >> n);
    {
        int all_ones = x == (uint64_t)(0xFF >> n);
        for (int i = 1; i < n; i++) {
            x = (x << 8) | p[i];
            if (p[i] != 0xFF) all_ones = 0;
        }
        *v = all_ones ? SIZE_UNKNOWN : x;
    }
    return n;
}

static uint64_t read_uint(const uint8_t *p, uint64_t len) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < len && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static double read_float(const uint8_t *p, uint64_t len) {
    if (len == 4) {
        union { uint32_t u; float f; } c;
        c.u = (uint32_t)read_uint(p, 4);
        return c.f;
    }
    if (len == 8) {
        union { uint64_t u; double d; } c;
        c.u = read_uint(p, 8);
        return c.d;
    }
    return 0;
}

static mtrack_t *track_by_number(mfile_t *f, int num) {
    for (int i = 0; i < f->ntracks; i++)
        if (f->tracks[i].id == num) return &f->tracks[i];
    return 0;
}

static int codec_from_id(const char *id, size_t n, int *kind) {
    static const struct { const char *name; int codec, kind; } map[] = {
        { "V_VP9", CODEC_VP9, TRACK_VIDEO }, { "V_VP8", CODEC_VP8, TRACK_VIDEO },
        { "V_MPEG4/ISO/AVC", CODEC_H264, TRACK_VIDEO }, { "V_MPEGH/ISO/HEVC", CODEC_HEVC, TRACK_VIDEO },
        { "V_AV1", CODEC_AV1, TRACK_VIDEO }, { "A_OPUS", CODEC_OPUS, TRACK_AUDIO },
        { "A_VORBIS", CODEC_VORBIS, TRACK_AUDIO }, { "A_AAC", CODEC_AAC, TRACK_AUDIO },
        { "A_MPEG/L3", CODEC_MP3, TRACK_AUDIO },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t l = strlen(map[i].name);
        if (n >= l && !memcmp(id, map[i].name, l) && (n == l || id[l] == 0 || id[l] == '/')) {
            *kind = map[i].kind;
            return map[i].codec;
        }
    }
    *kind = 0;
    return CODEC_UNKNOWN;
}

/* walks the children of a master element held in memory */
typedef void (*child_fn)(mfile_t *f, mtrack_t *t, uint32_t id, const uint8_t *p, uint64_t len);

static void each_child(mfile_t *f, mtrack_t *t, const uint8_t *p, uint64_t len, child_fn fn) {
    uint64_t pos = 0;
    while (pos < len) {
        uint32_t id;
        uint64_t size;
        int a = read_id(p + pos, (size_t)(len - pos), &id), b;
        if (a <= 0) return;
        b = read_vint(p + pos + a, (size_t)(len - pos - a), &size);
        if (b <= 0 || size == SIZE_UNKNOWN || pos + a + b + size > len) return;
        fn(f, t, id, p + pos + a + b, size);
        pos += a + b + size;
    }
}

static void video_child(mfile_t *f, mtrack_t *t, uint32_t id, const uint8_t *p, uint64_t len) {
    (void)f;
    if (id == ID_PIXELWIDTH) t->width = (int)read_uint(p, len);
    else if (id == ID_PIXELHEIGHT) t->height = (int)read_uint(p, len);
}

static void audio_child(mfile_t *f, mtrack_t *t, uint32_t id, const uint8_t *p, uint64_t len) {
    (void)f;
    if (id == ID_SAMPLINGFREQ) t->rate = (int)read_float(p, len);
    else if (id == ID_CHANNELS) t->channels = (int)read_uint(p, len);
}

static void track_child(mfile_t *f, mtrack_t *t, uint32_t id, const uint8_t *p, uint64_t len) {
    if (id == ID_TRACKNUMBER) t->id = (int)read_uint(p, len);
    else if (id == ID_TRACKTYPE) {
        uint64_t ty = read_uint(p, len);
        if (!t->kind) t->kind = ty == 1 ? TRACK_VIDEO : ty == 2 ? TRACK_AUDIO : 0;
    } else if (id == ID_CODECID) {
        int kind;
        t->codec = codec_from_id((const char *)p, (size_t)len, &kind);
        if (kind) t->kind = kind;
    } else if (id == ID_CODECPRIVATE) {
        free(t->cfg);
        t->cfg = (uint8_t *)malloc(len ? (size_t)len : 1);
        if (t->cfg) {
            memcpy(t->cfg, p, (size_t)len);
            t->cfg_len = (int)len;
        }
    } else if (id == ID_CODECDELAY) t->codec_delay_us = (int64_t)(read_uint(p, len) / 1000);
    else if (id == ID_SEEKPREROLL) t->seek_preroll_us = (int64_t)(read_uint(p, len) / 1000);
    else if (id == ID_VIDEO) each_child(f, t, p, len, video_child);
    else if (id == ID_AUDIO) each_child(f, t, p, len, audio_child);
}

static void tracks_child(mfile_t *f, mtrack_t *unused, uint32_t id, const uint8_t *p, uint64_t len) {
    mtrack_t *t;
    (void)unused;
    if (id != ID_TRACKENTRY || f->ntracks >= MAX_TRACKS) return;
    t = &f->tracks[f->ntracks];
    memset(t, 0, sizeof *t);
    each_child(f, t, p, len, track_child);
    if (t->codec == CODEC_UNKNOWN || !t->kind) {
        free(t->cfg);
        memset(t, 0, sizeof *t);
        return;
    }
    t->timescale = 1000000;
    f->ntracks++;
}

static void info_child(mfile_t *f, mtrack_t *t, uint32_t id, const uint8_t *p, uint64_t len) {
    (void)t;
    if (id == ID_TIMESCALE) f->mkv_timescale_ns = read_uint(p, len);
    else if (id == ID_DURATION) {
        double d = read_float(p, len);
        f->duration_us = (int64_t)(d * (double)(f->mkv_timescale_ns ? f->mkv_timescale_ns : 1000000) / 1000.0);
    }
}

/* a Block / SimpleBlock: track, relative time, flags, then frames */
static void add_block(mfile_t *f, const uint8_t *p, uint64_t len, uint64_t abs_off, int simple, int has_ref) {
    uint64_t track;
    int n = read_vint(p, (size_t)len, &track), lacing, frames = 1;
    mtrack_t *t;
    int16_t rel;
    uint8_t flags;
    int64_t pts_us;
    int key;
    uint64_t pos, sizes[64];
    if (n <= 0 || (uint64_t)n + 3 > len) return;
    t = track_by_number(f, (int)track);
    if (!t) return;
    rel = (int16_t)((p[n] << 8) | p[n + 1]);
    flags = p[n + 2];
    pos = (uint64_t)n + 3;
    {
        uint64_t ts = f->mkv_timescale_ns ? f->mkv_timescale_ns : 1000000;
        pts_us = (int64_t)((f->mkv_cluster_time + rel) * (int64_t)ts / 1000);
    }
    if (t->kind == TRACK_AUDIO) pts_us -= t->codec_delay_us;
    key = simple ? (flags & 0x80) != 0 : !has_ref;
    if (t->kind == TRACK_AUDIO) key = 1;
    lacing = (flags >> 1) & 3;
    if (lacing) {
        uint64_t total = 0;
        if (pos >= len) return;
        frames = p[pos++] + 1;
        if (frames > 64) return;
        if (lacing == 1) {                               /* Xiph */
            for (int i = 0; i < frames - 1; i++) {
                uint64_t s = 0;
                while (pos < len && p[pos] == 255) { s += 255; pos++; }
                if (pos >= len) return;
                s += p[pos++];
                sizes[i] = s;
                total += s;
            }
        } else if (lacing == 3) {                        /* EBML */
            uint64_t s;
            int m = read_vint(p + pos, (size_t)(len - pos), &s);
            if (m <= 0) return;
            pos += (uint64_t)m;
            sizes[0] = s;
            total = s;
            for (int i = 1; i < frames - 1; i++) {
                uint64_t raw;
                int64_t delta;
                m = read_vint(p + pos, (size_t)(len - pos), &raw);
                if (m <= 0) return;
                pos += (uint64_t)m;
                delta = (int64_t)raw - (((int64_t)1 << (7 * m - 1)) - 1);
                sizes[i] = (uint64_t)((int64_t)sizes[i - 1] + delta);
                total += sizes[i];
            }
        } else {                                         /* fixed */
            uint64_t each = (len - pos) / (uint64_t)frames;
            for (int i = 0; i < frames - 1; i++) sizes[i] = each;
            total = each * (uint64_t)(frames - 1);
        }
        if (pos + total > len) return;
        sizes[frames - 1] = len - pos - total;
    } else {
        sizes[0] = len - pos;
    }
    for (int i = 0; i < frames; i++) {
        container_add_sample(t, abs_off + pos, (uint32_t)sizes[i], key, pts_us);
        pos += sizes[i];
    }
}

static void group_child_find(const uint8_t *p, uint64_t len, const uint8_t **block, uint64_t *blen, int *has_ref) {
    uint64_t pos = 0;
    *block = 0;
    *has_ref = 0;
    while (pos < len) {
        uint32_t id;
        uint64_t size;
        int a = read_id(p + pos, (size_t)(len - pos), &id), b;
        if (a <= 0) return;
        b = read_vint(p + pos + a, (size_t)(len - pos - a), &size);
        if (b <= 0 || size == SIZE_UNKNOWN || pos + a + b + size > len) return;
        if (id == ID_BLOCK) {
            *block = p + pos + a + b;
            *blen = size;
        } else if (id == ID_REFBLOCK) *has_ref = 1;
        pos += a + b + size;
    }
}

int mkv_parse(mfile_t *f, const uint8_t *data, size_t len) {
    f->is_mkv = 1;
    while (f->parsed < len) {
        uint32_t id;
        uint64_t size;
        size_t pos = f->parsed;
        int a = read_id(data + pos, len - pos, &id), b;
        if (a < 0) return f->have_header ? 0 : -1;
        if (a == 0) break;
        b = read_vint(data + pos + a, len - pos - a, &size);
        if (b < 0) return f->have_header ? 0 : -1;
        if (b == 0) break;
        /* containers whose children are read as they come */
        if (id == ID_SEGMENT || id == ID_CLUSTER) {
            f->parsed = pos + (size_t)a + (size_t)b;
            continue;
        }
        if (size == SIZE_UNKNOWN) return f->have_header ? 0 : -1;
        if (pos + (size_t)a + (size_t)b + size > len) break;           /* not all here yet */
        {
            const uint8_t *body = data + pos + a + b;
            uint64_t body_off = pos + (uint64_t)a + (uint64_t)b;
            switch (id) {
            case ID_INFO: each_child(f, 0, body, size, info_child); break;
            case ID_TRACKS:
                if (!f->have_header) {
                    each_child(f, 0, body, size, tracks_child);
                    f->have_header = 1;
                }
                break;
            case ID_TIMECODE: f->mkv_cluster_time = (int64_t)read_uint(body, size); break;
            case ID_SIMPLEBLOCK:
                if (f->have_header) add_block(f, body, size, body_off, 1, 0);
                break;
            case ID_BLOCKGROUP:
                if (f->have_header) {
                    const uint8_t *blk;
                    uint64_t blen = 0;
                    int ref;
                    group_child_find(body, size, &blk, &blen, &ref);
                    if (blk) add_block(f, blk, blen, (uint64_t)(blk - data), 0, ref);
                }
                break;
            default: break;          /* EBML header, SeekHead, Cues, Tags ... */
            }
        }
        f->parsed = pos + (size_t)a + (size_t)b + (size_t)size;
    }
    return f->have_header || f->parsed < len ? 0 : -1;
}
