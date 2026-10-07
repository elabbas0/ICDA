/* <video> / <audio> playback engine (see media_el.h).
 *
 * A source is a byte stream with its own demuxer state: the progressive
 * download, or one MSE source buffer (its init segment and media segments,
 * concatenated in the order they were appended).  Segments appended out of
 * order after a seek still parse - each fragment / cluster carries its own
 * times - and the sample tables are re-sorted into decode order.  The video
 * track comes from whichever source has one, the sound likewise. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "media_el.h"
#include "http.h"
#include "container.h"
#include "vdec.h"
#include "pdec.h"
#include "ic_time.h"

#define MAX_EL          16
#define MAX_SRC         4
#define AUDIO_AHEAD_MS  500
#define DECODE_MS       30      /* the most one tick spends decoding pictures */
#define PCM_FRAMES      5760

typedef struct {
    uint8_t *data;
    size_t   len, cap;
    mfile_t  mf;
    int      used;
} src_t;

struct media_el {
    int         mse;
    char        url[URL_CAP];
    http_req_t *req;
    size_t      req_copied;
    int         redirects;
    int         done, failed;   /* all data is here (download finished / endOfStream) */
    src_t       src[MAX_SRC];
    int         nsrc;
    double      duration_set;   /* MediaSource.duration */
    mtrack_t   *vt, *at;
    int         vsrc, asrc;
    vdec_t     *vd;
    pdec_t     *ad;
    int         vcodec, acodec;
    int         vnext, anext, vflushed;
    int64_t     vlast_dts, alast_dts;   /* last decoded sample, to find our place after a re-sort */
    /* clock */
    long        stream;
    int         arate, ach;
    double      base;
    double      wall_ref;
    int         paused, ended, waiting, playing_clock;
    double      seek_target;
    double      audio_end;
    double      volume;
    int         muted;
    /* pictures */
    image_t     frame, next;
    double      next_pts;
    int         have_next, have_frame;
    int         width, height, size_reported;
    uint32_t    shown, dropped;
    int         changed;
    const void *node;
};

static media_el_t *els[MAX_EL];
static const media_audio_t *sink;

void media_set_audio(const media_audio_t *s) { sink = s; }

static double now_ms(void) {
    return (double)ic_time_ms();
}

/* ---- sources ------------------------------------------------------------------ */

static int src_append(src_t *s, const uint8_t *d, size_t n) {
    if (s->len + n > s->cap) {
        size_t cap = s->cap ? s->cap : 1 << 20;
        uint8_t *nd;
        while (cap < s->len + n) cap *= 2;
        nd = (uint8_t *)realloc(s->data, cap);
        if (!nd) return -1;
        s->data = nd;
        s->cap = cap;
    }
    memcpy(s->data + s->len, d, n);
    s->len += n;
    if (!s->mf.have_header && !s->mf.is_mkv && container_is_mkv(s->data, s->len)) s->mf.is_mkv = 1;
    if (s->mf.is_mkv) (void)mkv_parse(&s->mf, s->data, s->len);
    else (void)mp4_parse(&s->mf, s->data, s->len);
    return 0;
}

static int sample_ready(const src_t *s, const msample_t *x) {
    return x->off + x->size <= s->len;
}

/* after segments arrived out of order: sort, then find our place again */
static void resort(media_el_t *m) {
    if (m->vt && container_sort(m->vt)) {
        int i = 0;
        while (i < m->vt->n && m->vt->s[i].dts_us <= m->vlast_dts) i++;
        if (m->vnext > 0) m->vnext = i;
    }
    if (m->at && container_sort(m->at)) {
        int i = 0;
        while (i < m->at->n && m->at->s[i].dts_us <= m->alast_dts) i++;
        if (m->anext > 0) m->anext = i;
    }
}

/* progressive download into source 0 */
static void pump(media_el_t *m) {
    http_req_t *r = m->req;
    if (!r) return;
    (void)http_poll(r, 0);
    if (r->body_len > m->req_copied && r->status >= 200 && r->status < 300) {
        (void)src_append(&m->src[0], r->body + m->req_copied, r->body_len - m->req_copied);
        m->req_copied = r->body_len;
    }
    if (r->state == HTTP_PENDING) return;
    if (r->state == HTTP_DONE && r->status >= 300 && r->status < 400 && r->location[0] && m->redirects < 5) {
        char next[URL_CAP];
        m->redirects++;
        if (url_resolve(m->url, r->location, next, sizeof next) == 0) {
            snprintf(m->url, sizeof m->url, "%s", next);
            http_free(r);
            m->req_copied = 0;
            m->req = http_open(m->url, "GET", 0);
            return;
        }
    }
    if (r->state == HTTP_DONE && r->status >= 200 && r->status < 300) m->done = 1;
    else m->failed = 1;
    http_free(r);
    m->req = 0;
}

/* ---- clock ---------------------------------------------------------------------- */

static double clock_now(media_el_t *m) {
    if (m->stream > 0 && m->arate && sink) {
        long pos = sink->position(m->stream);
        return m->base + (double)(pos > 0 ? pos : 0) / m->arate;
    }
    if (!m->playing_clock) return m->base;
    return m->base + (now_ms() - m->wall_ref) / 1000.0;
}

/* runs the wall clock only while actually playing */
static void clock_set_running(media_el_t *m, int run) {
    if (run == m->playing_clock) return;
    m->base = clock_now(m);
    m->wall_ref = now_ms();
    m->playing_clock = run;
}

static void stream_open(media_el_t *m) {
    if (m->stream > 0 && sink) sink->close(m->stream);
    m->stream = 0;
    if (!m->ad || !sink) return;
    m->stream = sink->open((uint32_t)m->arate, (uint32_t)m->ach);
    if (m->stream <= 0) {
        m->stream = 0;
        return;
    }
    sink->control(m->stream, m->paused || m->waiting, m->muted ? 0 : (uint32_t)(m->volume * 256));
}

static void sound_follow(media_el_t *m) {
    int run = !m->paused && !m->waiting && !m->ended && (m->vt || m->at);
    if (m->stream > 0 && sink) sink->control(m->stream, !run, m->muted ? 0 : (uint32_t)(m->volume * 256));
    if (m->stream > 0 && sink) {
        /* the sound position keeps the clock; base only moves on seeks */
    } else {
        clock_set_running(m, run);
    }
}

/* ---- pictures ------------------------------------------------------------------- */

static inline uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

static int convert(const vframe_t *f, image_t *img) {
    int bt709 = f->height >= 600;
    int cr = bt709 ? 459 : 409, cgu = bt709 ? 55 : 100, cgv = bt709 ? 136 : 208, cb = bt709 ? 541 : 516;
    if (img->w != f->width || img->h != f->height || !img->argb) {
        uint32_t *px = (uint32_t *)realloc(img->argb, (size_t)f->width * (size_t)f->height * 4);
        if (!px) return -1;
        img->argb = px;
        img->w = f->width;
        img->h = f->height;
    }
    for (int y = 0; y < f->height; y++) {
        const uint8_t *yr = f->y + (size_t)y * (size_t)f->stride_y;
        const uint8_t *ur = f->u + (size_t)(y >> 1) * (size_t)f->stride_uv;
        const uint8_t *vr = f->v + (size_t)(y >> 1) * (size_t)f->stride_uv;
        uint32_t *o = img->argb + (size_t)y * (size_t)f->width;
        for (int x = 0; x < f->width; x++) {
            int l = (yr[x] - 16) * 298, uu = ur[x >> 1] - 128, vv = vr[x >> 1] - 128;
            o[x] = 0xFF000000u | ((uint32_t)clamp8((l + cr * vv + 128) >> 8) << 16) |
                   ((uint32_t)clamp8((l - cgu * uu - cgv * vv + 128) >> 8) << 8) | clamp8((l + cb * uu + 128) >> 8);
        }
    }
    return 0;
}

/* decodes until a picture comes out; 1 if one did (shown or dropped as late) */
static int decode_one(media_el_t *m, double now) {
    vframe_t f;
    int got = 0;
    src_t *s = &m->src[m->vsrc];
    while (!got) {
        if (m->vnext < m->vt->n) {
            const msample_t *x = &m->vt->s[m->vnext];
            if (!sample_ready(s, x)) {
                if (!m->done) m->waiting = 1;
                return 0;
            }
            m->vnext++;
            m->vlast_dts = x->dts_us;
            got = vdec_decode(m->vd, s->data + x->off, (int)x->size, x->pts_us, &f);
        } else if (m->done && !m->vflushed) {
            got = vdec_flush(m->vd, &f);
            if (!got) m->vflushed = 1;
        } else {
            if (!m->done) m->waiting = 1;     /* MSE: the page has not appended more yet */
            return 0;
        }
    }
    if (f.pts_us / 1e6 < m->seek_target - 0.001) return 1;
    if (f.pts_us / 1e6 < now - 0.06 && m->have_frame && (m->vnext < m->vt->n || !m->vflushed)) {
        if (!(m->stream > 0) && !m->paused && f.pts_us / 1e6 < now - 0.25) {
            /* no sound to keep time and the pictures cannot keep up: slow
             * the clock down to them rather than dropping nearly all */
            m->base = f.pts_us / 1e6;
            m->wall_ref = now_ms();
        } else {
            m->dropped++;
            return 1;
        }
    }
    if (convert(&f, &m->next) != 0) return 1;
    m->next_pts = f.pts_us / 1e6;
    m->have_next = 1;
    if (m->width != f.width || m->height != f.height) {
        m->width = f.width;
        m->height = f.height;
        m->size_reported = 0;
    }
    return 1;
}

static void feed_audio(media_el_t *m) {
    static int16_t pcm[PCM_FRAMES * 2];
    src_t *s;
    if (!m->ad || m->stream <= 0 || !sink) return;
    s = &m->src[m->asrc];
    for (int rounds = 0; rounds < 16; rounds++) {
        long queued = sink->queued(m->stream);
        const msample_t *x;
        int frames, skip = 0;
        if (queued < 0 || (double)queued * 1000.0 / m->arate >= AUDIO_AHEAD_MS) break;
        if (m->anext >= m->at->n) {
            if (!m->done && (double)queued * 1000.0 / m->arate < 100) m->waiting = 1;
            break;
        }
        x = &m->at->s[m->anext];
        if (!sample_ready(s, x)) {
            if (!m->done && (double)queued * 1000.0 / m->arate < 100) m->waiting = 1;
            break;
        }
        m->anext++;
        m->alast_dts = x->dts_us;
        frames = pdec_decode(m->ad, s->data + x->off, (int)x->size, pcm, PCM_FRAMES);
        if (frames <= 0) continue;
        if (x->pts_us / 1e6 < m->base) {
            double cut = (m->base - x->pts_us / 1e6) * m->arate;
            if (cut >= frames) continue;
            skip = (int)cut;
        }
        {
            long sent = 0, want = frames - skip;
            const int16_t *p = pcm + skip * m->ach;
            while (sent < want) {
                long took = sink->write(m->stream, p + sent * m->ach, (uint64_t)(want - sent) * (uint64_t)m->ach * 2);
                if (took <= 0) break;
                sent += took / (m->ach * 2);
            }
        }
        m->audio_end = x->pts_us / 1e6 + (double)frames / m->arate;
    }
}

/* ---- tracks and decoders ------------------------------------------------------------ */

static void find_tracks(media_el_t *m) {
    for (int i = 0; i < m->nsrc; i++) {
        mtrack_t *t;
        if (!m->src[i].mf.have_header) continue;
        if (!m->vt && (t = container_track(&m->src[i].mf, TRACK_VIDEO))) {
            m->vd = vdec_open(t->codec, t->cfg, t->cfg_len);
            if (m->vd) {
                m->vt = t;
                m->vsrc = i;
                m->vcodec = t->codec;
                if (!m->width) {
                    m->width = t->width;
                    m->height = t->height;
                }
                m->changed = 1;
            }
        }
        if (!m->at && (t = container_track(&m->src[i].mf, TRACK_AUDIO))) {
            m->ad = pdec_open(t->codec, t->cfg, t->cfg_len, t->rate, t->channels);
            if (m->ad) {
                m->at = t;
                m->asrc = i;
                m->acodec = t->codec;
                m->arate = pdec_rate(m->ad);
                m->ach = pdec_channels(m->ad);
                stream_open(m);
                m->changed = 1;
            }
        }
    }
    if (m->mse) return;
    /* a progressive file: once its header is in, it is decided */
    if (m->src[0].mf.have_header && !m->vt && !m->at) m->failed = 1;
    if (m->done && !m->src[0].mf.have_header) m->failed = 1;
}

/* ---- open / close ------------------------------------------------------------------------ */

static media_el_t *new_el(void) {
    media_el_t *m;
    int slot = -1;
    for (int i = 0; i < MAX_EL; i++)
        if (!els[i]) {
            slot = i;
            break;
        }
    if (slot < 0) return 0;
    m = (media_el_t *)calloc(1, sizeof(media_el_t));
    if (!m) return 0;
    m->paused = 1;
    m->volume = 1.0;
    m->vlast_dts = m->alast_dts = INT64_MIN;
    els[slot] = m;
    return m;
}

media_el_t *media_el_open(const char *url) {
    media_el_t *m = new_el();
    if (!m) return 0;
    snprintf(m->url, sizeof m->url, "%s", url);
    m->nsrc = 1;
    m->src[0].used = 1;
    m->req = http_open(m->url, "GET", 0);
    if (!m->req) m->failed = 1;
    return m;
}

media_el_t *media_el_open_mse(void) {
    media_el_t *m = new_el();
    if (m) m->mse = 1;
    return m;
}

int media_el_add_source(media_el_t *m) {
    if (!m || m->nsrc >= MAX_SRC) return -1;
    m->src[m->nsrc].used = 1;
    return m->nsrc++;
}

int media_el_append(media_el_t *m, int id, const uint8_t *data, size_t len) {
    if (!m || id < 0 || id >= m->nsrc) return -1;
    if (src_append(&m->src[id], data, len) != 0) return -1;
    m->changed = 1;
    return 0;
}

void media_el_remove(media_el_t *m, int id, double start, double end) {
    /* the bytes stay; dropping samples would need the page to append them
     * again, which it does only for ranges it believes are gone */
    (void)m; (void)id; (void)start; (void)end;
}

void media_el_set_duration(media_el_t *m, double d) {
    if (m) {
        m->duration_set = d;
        m->changed = 1;
    }
}

void media_el_end_of_stream(media_el_t *m) {
    if (m) {
        m->done = 1;
        m->changed = 1;
    }
}

void media_el_close(media_el_t *m) {
    if (!m) return;
    for (int i = 0; i < MAX_EL; i++)
        if (els[i] == m) els[i] = 0;
    if (m->stream > 0 && sink) sink->close(m->stream);
    if (m->vd) vdec_close(m->vd);
    if (m->ad) pdec_close(m->ad);
    if (m->req) http_free(m->req);
    for (int i = 0; i < MAX_SRC; i++) {
        container_free(&m->src[i].mf);
        free(m->src[i].data);
    }
    free(m->frame.argb);
    free(m->next.argb);
    free(m);
}

void media_el_close_all(void) {
    for (int i = 0; i < MAX_EL; i++)
        if (els[i]) media_el_close(els[i]);
}

int media_el_active(void) {
    for (int i = 0; i < MAX_EL; i++)
        if (els[i]) return 1;
    return 0;
}

int media_el_id(media_el_t *m) {
    for (int i = 0; i < MAX_EL; i++)
        if (m && els[i] == m) return i;
    return -1;
}

media_el_t *media_el_get(int id) {
    return id >= 0 && id < MAX_EL ? els[id] : 0;
}

void media_el_bind(media_el_t *m, const void *node) {
    for (int i = 0; i < MAX_EL; i++)
        if (node && els[i] && els[i] != m && els[i]->node == node) els[i]->node = 0;
    if (m) m->node = node;
}

media_el_t *media_el_for_node(const void *node) {
    for (int i = 0; i < MAX_EL; i++)
        if (node && els[i] && els[i]->node == node) return els[i];
    return 0;
}

/* ---- control --------------------------------------------------------------------------------- */

void media_el_play(media_el_t *m) {
    if (!m) return;
    if (m->ended) media_el_seek(m, 0);
    if (!m->paused) return;
    m->paused = 0;
    sound_follow(m);
    m->changed = 1;
}

void media_el_pause(media_el_t *m) {
    if (!m || m->paused) return;
    m->paused = 1;
    sound_follow(m);
    if (m->stream > 0 && sink) m->base = clock_now(m);
    m->changed = 1;
}

void media_el_volume(media_el_t *m, double volume, int muted) {
    if (!m) return;
    m->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
    m->muted = muted;
    sound_follow(m);
}

void media_el_seek(media_el_t *m, double t) {
    if (!m) return;
    if (t < 0) t = 0;
    m->ended = 0;
    m->base = t;
    m->seek_target = t;
    m->wall_ref = now_ms();
    m->have_frame = 0;            /* the old picture stays up until the new one is decoded */
    if (m->vt) {
        m->vnext = container_seek_index(m->vt, (int64_t)(t * 1e6));
        m->vlast_dts = m->vnext > 0 ? m->vt->s[m->vnext - 1].dts_us : INT64_MIN;
        vdec_reset(m->vd);
        m->vflushed = 0;
        m->have_next = 0;
    }
    if (m->at && m->ad) {
        int i = 0;
        while (i + 1 < m->at->n && m->at->s[i + 1].pts_us / 1e6 <= t) i++;
        m->anext = i > 0 ? i - 1 : 0;
        m->alast_dts = m->anext > 0 ? m->at->s[m->anext - 1].dts_us : INT64_MIN;
        pdec_reset(m->ad);
    }
    stream_open(m);
    m->changed = 1;
}

/* ---- per tick ---------------------------------------------------------------------------------- */

/* a status line every few seconds while something is open */
#ifdef TLS_HOST
#define media_log(line) fprintf(stderr, "%s\n", line)
#else
#include "icda_sys.h"
static void media_log(const char *line) {
    char buf[300];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    icda_write_file("/dev/serial", buf, (uint64_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}
#endif

static void media_status(media_el_t *m) {
    static uint32_t next;
    char line[256];
    uint32_t now = ic_time_ms();
    if ((int32_t)(now - next) < 0) return;
    next = now + 3000;
    snprintf(line, sizeof line,
             "[media] %d t=%.2f p=%d w=%d e=%d done=%d fail=%d src=%d:%zu/%zu v=%d/%d a=%d/%d frames=%u/%u size=%dx%d stream=%ld",
             media_el_id(m), clock_now(m), m->paused, m->waiting, m->ended, m->done, m->failed, m->nsrc,
             m->src[0].len, m->nsrc > 1 ? m->src[1].len : 0, m->vnext, m->vt ? m->vt->n : -1, m->anext,
             m->at ? m->at->n : -1, m->shown, m->dropped, m->width, m->height, m->stream);
    media_log(line);
}

static int tick_one(media_el_t *m) {
    int flags = 0, was_waiting = m->waiting;
    if (m->mse) media_status(m);
    if (m->req) {
        size_t before = m->src[0].len;
        pump(m);
        if (m->src[0].len != before) flags |= MEDIA_STATE;
    }
    if (!m->vt || !m->at) find_tracks(m);
    resort(m);
    if (m->failed) return flags | (m->changed ? MEDIA_STATE : 0);
    if (m->width && !m->size_reported) {
        m->size_reported = 1;
        flags |= MEDIA_SIZE_KNOWN;
    }
    /* the first picture shows even while paused */
    if (m->vt && !m->have_frame) {
        if (!m->have_next) decode_one(m, m->seek_target);
        if (m->have_next) {
            image_t t = m->frame;
            m->frame = m->next;
            m->next = t;
            m->have_frame = 1;
            m->have_next = 0;
            flags |= MEDIA_NEW_FRAME;
        }
    }
    if (!m->paused && !m->ended && (m->vt || m->at)) {
        m->waiting = 0;
        feed_audio(m);
        if (m->vt) {
            double now = clock_now(m);
            uint32_t t0 = ic_time_ms();
            for (int n = 0; n < 64 && ic_time_ms() - t0 < DECODE_MS; n++) {
                if (!m->have_next && !decode_one(m, now)) break;
                if (m->have_next && m->next_pts <= now + 0.008) {
                    image_t t = m->frame;
                    m->frame = m->next;
                    m->next = t;
                    m->have_frame = 1;
                    m->have_next = 0;
                    m->shown++;
                    flags |= MEDIA_NEW_FRAME;
                    continue;
                }
                if (m->have_next) {
                    m->waiting = 0;        /* a picture is ready, only early */
                    break;
                }
            }
        }
        if (m->waiting != was_waiting) {
            sound_follow(m);
            flags |= MEDIA_STATE;
        } else if (!m->playing_clock && !(m->stream > 0)) {
            clock_set_running(m, 1);
        }
        /* MSE pages need not call endOfStream(): at the duration it is over */
        if (m->mse && !m->done && m->duration_set > 0) {
            const mtrack_t *t = m->vt ? m->vt : m->at;
            double last = t && t->n ? t->s[t->n - 1].pts_us / 1e6 : 0;
            if (clock_now(m) >= m->duration_set - 0.05 || (m->waiting && last >= m->duration_set - 0.25)) m->done = 1;
        }
        {
            int video_done = !m->vt || (m->vnext >= m->vt->n && m->vflushed && !m->have_next);
            int audio_done = !m->ad || m->stream <= 0 ||
                             (m->anext >= m->at->n && (sink->queued(m->stream) <= 0 || clock_now(m) >= m->audio_end - 0.1));
            if (video_done && audio_done && m->done) {
                m->base = clock_now(m);
                m->ended = 1;
                m->paused = 1;
                clock_set_running(m, 0);
                if (m->stream > 0 && sink) sink->close(m->stream);
                m->stream = 0;
                flags |= MEDIA_STATE;
            }
        }
    }
    if (m->changed) {
        m->changed = 0;
        flags |= MEDIA_STATE;
    }
    return flags;
}

static int pending_flags;

/* between script steps (from the script engine's interrupt hook): decoding
 * keeps up while a long script runs; what changed is reported by the next
 * media_el_tick_all() */
void media_el_pump(void) {
    static uint32_t last;
    uint32_t now = ic_time_ms();
    if (now - last < 15) return;
    last = now;
    for (int i = 0; i < MAX_EL; i++)
        if (els[i]) pending_flags |= tick_one(els[i]);
}

int media_el_tick_all(void) {
    int flags = pending_flags;
    pending_flags = 0;
    for (int i = 0; i < MAX_EL; i++)
        if (els[i]) flags |= tick_one(els[i]);
    return flags;
}

/* ---- state --------------------------------------------------------------------------------------- */

static double duration_of(media_el_t *m) {
    double d = m->duration_set > 0 ? m->duration_set : 0;
    for (int i = 0; i < m->nsrc && d <= 0; i++)
        if (m->src[i].mf.duration_us > 0) d = m->src[i].mf.duration_us / 1e6;
    if (d <= 0 && m->done) {
        const mtrack_t *t = m->vt ? m->vt : m->at;
        if (t)
            for (int i = 0; i < t->n; i++)
                if (t->s[i].pts_us / 1e6 > d) d = t->s[i].pts_us / 1e6;
    }
    return d;
}

/* ranges of a track that are actually here, as [start, end] pairs */
static int track_ranges(const mtrack_t *t, const src_t *s, double *r, int max) {
    int n = 0;
    double frame = 0.1;
    if (!t || !t->n) return 0;
    for (int i = 0; i < t->n; i++) {
        const msample_t *x = &t->s[i];
        double a = x->pts_us / 1e6, b;
        if (!sample_ready(s, x)) continue;
        b = a + (i + 1 < t->n && t->s[i + 1].pts_us > x->pts_us ? (t->s[i + 1].pts_us - x->pts_us) / 1e6 : frame);
        if (b - a > 0.5) b = a + frame;
        if (n && a <= r[2 * n - 1] + 0.25 && a >= r[2 * n - 2] - 0.25) {
            if (b > r[2 * n - 1]) r[2 * n - 1] = b;
            if (a < r[2 * n - 2]) r[2 * n - 2] = a;
        } else if (n < max) {
            r[2 * n] = a;
            r[2 * n + 1] = b;
            n++;
        }
    }
    return n;
}

int media_el_buffered(media_el_t *m, int id, double *ranges, int max) {
    if (!m) return 0;
    if (id >= 0) {
        mtrack_t *t;
        if (id >= m->nsrc || !m->src[id].mf.have_header) return 0;
        t = container_track(&m->src[id].mf, TRACK_VIDEO);
        if (!t) t = container_track(&m->src[id].mf, TRACK_AUDIO);
        return t ? track_ranges(t, &m->src[id], ranges, max) : 0;
    }
    if (m->vt) return track_ranges(m->vt, &m->src[m->vsrc], ranges, max);
    if (m->at) return track_ranges(m->at, &m->src[m->asrc], ranges, max);
    return 0;
}

void media_el_state(media_el_t *m, media_el_state_t *o) {
    double r[64];
    int n;
    memset(o, 0, sizeof *o);
    if (!m) return;
    o->time = clock_now(m);
    o->duration = duration_of(m);
    if (o->duration > 0 && o->time > o->duration) o->time = o->duration;
    n = media_el_buffered(m, -1, r, 32);
    for (int i = 0; i < n; i++)
        if (o->time >= r[2 * i] - 0.3 && o->time <= r[2 * i + 1]) o->buffered_end = r[2 * i + 1];
    o->paused = m->paused;
    o->ended = m->ended;
    o->waiting = m->waiting;
    o->error = m->failed ? (m->vt || m->at ? 3 : m->done ? 4 : 2) : 0;
    o->width = m->width;
    o->height = m->height;
    o->frames_shown = m->shown;
    o->frames_dropped = m->dropped;
    if (!m->vt && !m->at) o->ready_state = 0;
    else if (!m->have_frame && m->vt) o->ready_state = 1;
    else if (m->done || o->buffered_end > o->time + 2) o->ready_state = 4;
    else if (o->buffered_end > o->time + 0.2) o->ready_state = 3;
    else o->ready_state = 2;
}

const image_t *media_el_frame(media_el_t *m) {
    return m && m->frame.argb ? &m->frame : 0;
}

int media_el_size(media_el_t *m, int *w, int *h) {
    if (!m || !m->width) return 0;
    *w = m->width;
    *h = m->height;
    return 1;
}
