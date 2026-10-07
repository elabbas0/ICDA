/* Video face of Media.
 *
 * Sources are byte buffers that may still be growing (a YouTube download):
 * a demuxer turns them into sample tables, the video decoder runs from the
 * tick a few pictures at a time, and the soundtrack goes to the kernel
 * mixer as a PCM stream whose position is the clock pictures follow.
 * Without sound the clock is the wall clock.  Late pictures are decoded
 * but not converted, so a slow machine drops frames instead of drifting. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libicda.h"
#include "gui.h"
#include "media.h"
#include "container.h"
#include "vdec.h"
#include "pdec.h"

#define AUDIO_AHEAD_MS  500
#define DECODE_BUDGET   4          /* pictures decoded per tick at most */
#define CONTROLS_HIDE_MS 2500
#define BAR_H           72

typedef struct {
    vsrc_t  *src;
    mfile_t  mf;
    int      ok;
} input_t;

static struct {
    int       open;
    char      title[160];
    char      error[200];
    input_t   in[2];
    int       nin;
    mtrack_t *vt, *at;
    input_t  *vin, *ain;
    vdec_t   *vd;
    pdec_t   *ad;
    int       vnext, anext;
    int       vflushed;
    /* clock */
    long      stream;
    int       arate, ach;
    int64_t   base_us;              /* presentation time at the stream's first frame */
    uint32_t  wall_ms;              /* wall clock reference (no sound) */
    int       paused, ended, buffering;
    int64_t   seek_target;          /* drop pictures before this after a seek */
    int64_t   audio_written_us;     /* end of what went to the mixer */
    /* pictures (ARGB, physical size) */
    uint32_t *cur, *next;
    int       cur_w, cur_h, next_w, next_h, cap_px;
    int64_t   cur_pts, next_pts;
    int       have_cur, have_next;
    int       out_w, out_h;         /* conversion size */
    int       full_range, bt709;
    /* controls */
    uint32_t  last_input_ms;
    int       hover, dragging;
    int       drag_x;
    int64_t   duration_us;
    int       frames_shown, frames_dropped;
} v;

/* ---- sources ---------------------------------------------------------------- */

static void free_src(vsrc_t *s) {
    if (!s) return;
    if (s->close) s->close(s);
    else {
        free(s->data);
        free(s);
    }
}

vsrc_t *vsrc_memory(uint8_t *data, size_t len) {
    vsrc_t *s = (vsrc_t *)calloc(1, sizeof(vsrc_t));
    if (!s) return 0;
    s->data = data;
    s->len = s->total = len;
    s->done = 1;
    return s;
}

static int input_parse(input_t *in) {
    vsrc_t *s = in->src;
    int r;
    if (!s->len) return 0;
    if (!in->mf.is_mkv && !in->mf.have_header && container_is_mkv(s->data, s->len)) in->mf.is_mkv = 1;
    r = in->mf.is_mkv ? mkv_parse(&in->mf, s->data, s->len) : mp4_parse(&in->mf, s->data, s->len);
    in->ok = r == 0 && in->mf.have_header;
    return r;
}

static int sample_ready(const input_t *in, const msample_t *s) {
    return s->off + s->size <= in->src->len;
}

/* ---- time ------------------------------------------------------------------------ */

static int64_t clock_us(void) {
    if (v.stream > 0 && v.arate) {
        long pos = icda_audio_stream_position(v.stream);
        if (pos < 0) pos = 0;
        return v.base_us + (int64_t)pos * 1000000 / v.arate;
    }
    if (v.paused || v.buffering || v.ended) return v.base_us;
    return v.base_us + (int64_t)(uint32_t)(ic_time_ms() - v.wall_ms) * 1000;
}

/* freezes the clock where it is (pause, buffering) */
static void clock_hold(void) {
    v.base_us = clock_us();
    v.wall_ms = ic_time_ms();
}

static void clock_run(void) {
    v.wall_ms = ic_time_ms();
}

/* ---- pictures ---------------------------------------------------------------------- */

static int ensure_buffers(int w, int h) {
    int need = w * h;
    if (need <= v.cap_px) return 0;
    {
        uint32_t *a = (uint32_t *)realloc(v.cur, (size_t)need * 4), *b;
        if (!a) return -1;
        v.cur = a;
        b = (uint32_t *)realloc(v.next, (size_t)need * 4);
        if (!b) return -1;
        v.next = b;
    }
    v.cap_px = need;
    return 0;
}

static inline uint8_t clamp8(int x) {
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* I420 -> ARGB at dw x dh, bilinear on luma, nearest on chroma */
static void convert(const vframe_t *f, uint32_t *dst, int dw, int dh) {
    int cr, cgu, cgv, cb;
    static int *xs, *xf, xcap;
    if (v.bt709) {
        cr = 459; cgu = 55; cgv = 136; cb = 541;           /* 1.793, 0.213, 0.533, 2.112 (x256) */
    } else {
        cr = 409; cgu = 100; cgv = 208; cb = 516;          /* 1.596, 0.392, 0.813, 2.017 */
    }
    if (xcap < dw) {
        free(xs);
        free(xf);
        xs = (int *)malloc(sizeof(int) * (size_t)dw);
        xf = (int *)malloc(sizeof(int) * (size_t)dw);
        xcap = xs && xf ? dw : 0;
        if (!xcap) return;
    }
    for (int x = 0; x < dw; x++) {
        int64_t sx = ((int64_t)x * 2 + 1) * f->width * 128 / dw - 128;     /* 8.8 fixed, pixel centres */
        if (sx < 0) sx = 0;
        if (sx > (int64_t)(f->width - 1) * 256) sx = (int64_t)(f->width - 1) * 256;
        xs[x] = (int)(sx >> 8);
        xf[x] = (int)(sx & 255);
    }
    for (int y = 0; y < dh; y++) {
        int64_t sy = ((int64_t)y * 2 + 1) * f->height * 128 / dh - 128;
        int y0, fy, y1, cy;
        const uint8_t *r0, *r1, *ur, *vr;
        uint32_t *o = dst + (size_t)y * (size_t)dw;
        if (sy < 0) sy = 0;
        if (sy > (int64_t)(f->height - 1) * 256) sy = (int64_t)(f->height - 1) * 256;
        y0 = (int)(sy >> 8);
        fy = (int)(sy & 255);
        y1 = y0 + 1 < f->height ? y0 + 1 : y0;
        cy = (y0 + (fy >= 128)) >> 1;
        if (cy >= (f->height + 1) / 2) cy = (f->height + 1) / 2 - 1;
        r0 = f->y + (size_t)y0 * (size_t)f->stride_y;
        r1 = f->y + (size_t)y1 * (size_t)f->stride_y;
        ur = f->u + (size_t)cy * (size_t)f->stride_uv;
        vr = f->v + (size_t)cy * (size_t)f->stride_uv;
        for (int x = 0; x < dw; x++) {
            int sx = xs[x], fx = xf[x], sx1 = sx + 1 < f->width ? sx + 1 : sx;
            int top = r0[sx] * (256 - fx) + r0[sx1] * fx, bot = r1[sx] * (256 - fx) + r1[sx1] * fx;
            int yy = (top * (256 - fy) + bot * fy) >> 16;
            int cx = (sx + (fx >= 128)) >> 1, uu = ur[cx] - 128, vv = vr[cx] - 128;
            int l = v.full_range ? yy * 256 : (yy - 16) * 298;
            o[x] = 0xFF000000u | ((uint32_t)clamp8((l + cr * vv + 128) >> 8) << 16) |
                   ((uint32_t)clamp8((l - cgu * uu - cgv * vv + 128) >> 8) << 8) | clamp8((l + cb * uu + 128) >> 8);
        }
    }
}

/* the picture area for a window of this size (logical), letterboxed */
static ic_rect_t picture_rect(ic_app_t *app) {
    int w = app->width, h = app->height, fw = v.vt ? v.vt->width : 16, fh = v.vt ? v.vt->height : 9;
    int dw = w, dh = fw ? (int)((int64_t)w * fh / fw) : h;
    if (dh > h) {
        dh = h;
        dw = fh ? (int)((int64_t)h * fw / fh) : w;
    }
    return ic_rect_make((w - dw) / 2, (h - dh) / 2, dw, dh);
}

/* decodes until a picture comes out; 1 if one is in next (or was dropped as late) */
static int decode_one(int64_t now) {
    vframe_t f;
    int got = 0;
    while (!got) {
        if (v.vnext < v.vt->n) {
            const msample_t *s = &v.vt->s[v.vnext];
            if (!sample_ready(v.vin, s)) {
                if (!v.vin->src->done) v.buffering = 1;
                return 0;
            }
            v.vnext++;
            got = vdec_decode(v.vd, v.vin->src->data + s->off, (int)s->size, s->pts_us, &f);
        } else if (!v.vflushed) {
            got = vdec_flush(v.vd, &f);
            if (!got) v.vflushed = 1;
        } else {
            return 0;
        }
    }
    if (f.pts_us < v.seek_target) return 1;                    /* before the seek point */
    if (f.pts_us < now - 60000 && (v.vnext < v.vt->n || !v.vflushed)) {
        v.frames_dropped++;
        return 1;                                             /* late: skip the conversion */
    }
    if (!v.out_w || !v.out_h || ensure_buffers(v.out_w, v.out_h) < 0) return 1;
    convert(&f, v.next, v.out_w, v.out_h);
    v.next_w = v.out_w;
    v.next_h = v.out_h;
    v.next_pts = f.pts_us;
    v.have_next = 1;
    return 1;
}

/* ---- sound --------------------------------------------------------------------------- */

static void open_stream(void) {
    if (v.stream > 0) icda_audio_stream_close(v.stream);
    v.stream = 0;
    if (!v.ad) return;
    v.stream = icda_audio_stream_open((uint32_t)v.arate, (uint32_t)v.ach);
    if (v.stream <= 0) v.stream = 0;
    else if (v.paused || v.buffering) icda_audio_stream_control(v.stream, 1, 256);
}

static void feed_audio(void) {
    static int16_t pcm[PDEC_MAX_FRAMES * 2];
    if (!v.ad || v.stream <= 0) return;
    for (int rounds = 0; rounds < 16; rounds++) {
        long queued = icda_audio_stream_queued(v.stream);
        const msample_t *s;
        int frames, skip = 0;
        if (queued < 0 || (int64_t)queued * 1000 / v.arate >= AUDIO_AHEAD_MS) break;
        if (v.anext >= v.at->n) break;
        s = &v.at->s[v.anext];
        if (!sample_ready(v.ain, s)) {
            if (!v.ain->src->done && queued * 1000 / v.arate < 100) v.buffering = 1;
            break;
        }
        v.anext++;
        frames = pdec_decode(v.ad, v.ain->src->data + s->off, (int)s->size, pcm, PDEC_MAX_FRAMES);
        if (frames <= 0) continue;
        /* trim what lies before the clock base (encoder priming, seek pre-roll) */
        if (s->pts_us < v.base_us) {
            int64_t cut = (v.base_us - s->pts_us) * v.arate / 1000000;
            if (cut >= frames) continue;
            skip = (int)cut;
        }
        {
            long sent = 0, want = frames - skip;
            const int16_t *p = pcm + skip * v.ach;
            while (sent < want) {
                long took = icda_audio_stream_write(v.stream, p + sent * v.ach, (uint64_t)(want - sent) * (uint64_t)v.ach * 2);
                if (took <= 0) break;
                sent += took / (v.ach * 2);
            }
        }
        v.audio_written_us = s->pts_us + (int64_t)frames * 1000000 / v.arate;
    }
}

/* ---- open / close --------------------------------------------------------------------- */

void video_close(void) {
    if (v.stream > 0) icda_audio_stream_close(v.stream);
    if (v.vd) vdec_close(v.vd);
    if (v.ad) pdec_close(v.ad);
    for (int i = 0; i < v.nin; i++) {
        container_free(&v.in[i].mf);
        free_src(v.in[i].src);
    }
    free(v.cur);
    free(v.next);
    memset(&v, 0, sizeof v);
}

/* picks tracks and opens decoders once the headers are in; 1 ready, 0 wait, -1 failed */
static int try_start(char *err, size_t errcap) {
    if (v.vd || v.ad) return 1;
    for (int i = 0; i < v.nin; i++) {
        if (!v.in[i].ok) {
            if (v.in[i].src->failed || (v.in[i].src->done && !v.in[i].mf.have_header)) {
                snprintf(err, errcap, "%s", v.in[i].src->failed ? v.in[i].src->error : "This file is not a video ICDA can read");
                return -1;
            }
            return 0;
        }
    }
    for (int i = 0; i < v.nin; i++) {
        mtrack_t *t;
        if (!v.vt && (t = container_track(&v.in[i].mf, TRACK_VIDEO))) {
            v.vt = t;
            v.vin = &v.in[i];
        }
        if (!v.at && (t = container_track(&v.in[i].mf, TRACK_AUDIO))) {
            v.at = t;
            v.ain = &v.in[i];
        }
        if (v.in[i].mf.duration_us > v.duration_us) v.duration_us = v.in[i].mf.duration_us;
    }
    if (!v.vt && !v.at) {
        int vc = 0;
        for (int i = 0; i < v.nin; i++)
            for (int k = 0; k < v.in[i].mf.ntracks; k++)
                if (v.in[i].mf.tracks[k].kind == TRACK_VIDEO) vc = v.in[i].mf.tracks[k].codec;
        snprintf(err, errcap, vc ? "This video uses %s, which ICDA cannot play yet" : "There is nothing ICDA can play in this file",
                 vdec_codec_name(vc));
        return -1;
    }
    if (v.vt) {
        v.vd = vdec_open(v.vt->codec, v.vt->cfg, v.vt->cfg_len);
        if (!v.vd) {
            snprintf(err, errcap, "This video uses %s, which ICDA cannot play yet", vdec_codec_name(v.vt->codec));
            return -1;
        }
        v.bt709 = v.vt->height >= 600;
    }
    if (v.at) {
        v.ad = pdec_open(v.at->codec, v.at->cfg, v.at->cfg_len, v.at->rate, v.at->channels);
        if (v.ad) {
            v.arate = pdec_rate(v.ad);
            v.ach = pdec_channels(v.ad);
        }
        if (!v.vt && !v.ad) {
            snprintf(err, errcap, "The sound uses %s, which ICDA cannot play yet", pdec_codec_name(v.at->codec));
            return -1;
        }
    }
    if (!v.duration_us) {
        const mtrack_t *t = v.vt ? v.vt : v.at;
        for (int i = 0; i < t->n; i++)
            if (t->s[i].pts_us > v.duration_us) v.duration_us = t->s[i].pts_us;
    }
    v.base_us = 0;
    open_stream();
    clock_run();
    return 1;
}

int video_open_sources(vsrc_t **srcs, int n, const char *title, char *err, size_t errcap) {
    video_close();
    for (int i = 0; i < n && i < 2; i++) {
        v.in[i].src = srcs[i];
        v.nin++;
    }
    snprintf(v.title, sizeof v.title, "%s", title ? title : "");
    v.open = 1;
    v.last_input_ms = ic_time_ms();
    v.hover = -1;
    for (int i = 0; i < v.nin; i++) input_parse(&v.in[i]);
    if (try_start(err, errcap) < 0) {
        video_close();
        return -1;
    }
    return 0;
}

int video_open(const char *path, char *err, size_t errcap) {
    size_t len = 0;
    uint8_t *data = media_read_file(path, &len);
    vsrc_t *s;
    const char *b = strrchr(path, '/');
    if (!data) {
        snprintf(err, errcap, "%s could not be read", b ? b + 1 : path);
        return -1;
    }
    s = vsrc_memory(data, len);
    if (!s) {
        free(data);
        return -1;
    }
    return video_open_sources(&s, 1, b ? b + 1 : path, err, errcap);
}

/* ---- seeking ---------------------------------------------------------------------------- */

static void seek_to(int64_t t) {
    if (!v.vd && !v.ad) return;
    if (t < 0) t = 0;
    if (v.duration_us && t > v.duration_us - 500000) t = v.duration_us - 500000;
    if (t < 0) t = 0;
    if (v.vt) {
        v.vnext = container_seek_index(v.vt, t);
        vdec_reset(v.vd);
        v.vflushed = 0;
        v.have_next = 0;
        t = v.vt->s[v.vnext].pts_us > t ? v.vt->s[v.vnext].pts_us : t;
    }
    if (v.at && v.ad) {
        int i = 0;
        while (i + 1 < v.at->n && v.at->s[i + 1].pts_us <= t) i++;
        v.anext = i > 0 ? i - 1 : 0;           /* one packet early: the decoder needs it to settle */
        pdec_reset(v.ad);
    }
    v.seek_target = t - 1000;
    v.base_us = t;
    v.ended = 0;
    open_stream();
    clock_run();
}

/* ---- tick -------------------------------------------------------------------------------- */

int video_tick(ic_app_t *app) {
    int redraw = 0;
    int was_buffering = v.buffering;
    if (!v.open) return 0;
    for (int i = 0; i < v.nin; i++) {
        vsrc_t *s = v.in[i].src;
        size_t before = s->len;
        if (s->pump) s->pump(s);
        if (s->len != before || !v.in[i].ok) input_parse(&v.in[i]);
    }
    if (!v.vd && !v.ad) {
        int r = try_start(v.error, sizeof v.error);
        if (r < 0) {
            v.open = 1;
            return 1;
        }
        if (r == 0) return (ic_time_ms() / 250) % 2 == 0;      /* spinner */
    }
    /* picture size follows the window */
    {
        ic_rect_t pr = picture_rect(app);
        int scale = gui_window_scale();
        v.out_w = pr.w * scale;
        v.out_h = pr.h * scale;
    }
    if (v.paused || v.ended) {
        if (!v.have_cur && v.vt) {               /* show something after a paused seek */
            if (!v.have_next) decode_one(v.seek_target);
            if (v.have_next) {
                uint32_t *t = v.cur;
                v.cur = v.next;
                v.next = t;
                v.cur_w = v.next_w;
                v.cur_h = v.next_h;
                v.cur_pts = v.next_pts;
                v.have_cur = 1;
                v.have_next = 0;
                redraw = 1;
            }
        }
        return redraw || (v.dragging);
    }
    v.buffering = 0;
    feed_audio();
    if (v.vt) {
        int64_t now = clock_us();
        for (int budget = DECODE_BUDGET; budget > 0; budget--) {
            if (!v.have_next && !decode_one(now)) break;
            if (v.have_next && v.next_pts <= now + 8000) {
                uint32_t *t = v.cur;
                v.cur = v.next;
                v.next = t;
                v.cur_w = v.next_w;
                v.cur_h = v.next_h;
                v.cur_pts = v.next_pts;
                v.have_cur = 1;
                v.have_next = 0;
                v.frames_shown++;
                redraw = 1;
                continue;
            }
            if (v.have_next) break;
        }
    }
    if (v.buffering != was_buffering) {
        if (v.buffering) {
            clock_hold();
            if (v.stream > 0) icda_audio_stream_control(v.stream, 1, 256);
        } else {
            clock_run();
            if (v.stream > 0) icda_audio_stream_control(v.stream, 0, 256);
        }
        redraw = 1;
    }
    /* the end: everything decoded and the sound has played out */
    {
        int video_done = !v.vt || (v.vnext >= v.vt->n && v.vflushed && !v.have_next);
        int audio_done = !v.ad || v.stream <= 0 ||
                         (v.anext >= v.at->n && (icda_audio_stream_queued(v.stream) <= 0 || clock_us() >= v.audio_written_us - 100000));
        int srcs_done = 1;
        for (int i = 0; i < v.nin; i++) srcs_done &= v.in[i].src->done;
        if (video_done && audio_done && srcs_done && !v.buffering) {
            v.ended = 1;
            clock_hold();
            if (v.duration_us && v.base_us > v.duration_us) v.base_us = v.duration_us;
            if (v.stream > 0) icda_audio_stream_close(v.stream);    /* its position would keep counting */
            v.stream = 0;
            redraw = 1;
        }
    }
    /* the time display moves every second; controls fade */
    {
        static int64_t last_sec = -1;
        int64_t sec = clock_us() / 1000000;
        if (sec != last_sec) {
            last_sec = sec;
            redraw = 1;
        }
        if (ic_time_ms() - v.last_input_ms < CONTROLS_HIDE_MS + 400) redraw |= !v.vt;
    }
    return redraw;
}

/* ---- drawing ------------------------------------------------------------------------------ */

static void put_picture(ic_canvas_t *c, ic_rect_t r) {
    int s = c->scale > 0 ? c->scale : 1;
    int x0 = r.x * s, y0 = r.y * s, w = r.w * s, h = r.h * s;
    if (!v.have_cur) return;
    if (v.cur_w != w || v.cur_h != h) {      /* window resized: stretch until the next picture */
        ic_canvas_t d = *c;
        d.scale = 1;
        ic_gfx_blit_scaled(&d, x0, y0, w, h, v.cur, v.cur_w, v.cur_h, v.cur_w, 0.0f, 255);
        return;
    }
    int bx0, by0, bx1, by1;
    if (!ic_canvas_bounds(c, &bx0, &by0, &bx1, &by1)) return;
    for (int y = 0; y < h; y++) {
        int py = y0 + y, xa = x0, xb = x0 + w;
        if (py < by0 || py >= by1) continue;
        if (xa < bx0) xa = bx0;
        if (xb > bx1) xb = bx1;
        if (xb <= xa) continue;
        memcpy(c->px + (size_t)py * (size_t)c->w + xa, v.cur + (size_t)y * (size_t)w + (xa - x0), (size_t)(xb - xa) * 4);
    }
}

static ic_rect_t bar_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - BAR_H, app->width, BAR_H);
}

static ic_rect_t ctl_rect(ic_app_t *app, int i) {
    /* 0 library, 1 play / pause, 2 back 10 s, 3 forward 10 s */
    ic_rect_t b = bar_rect(app);
    int y = b.y + 30;
    if (i == 0) return ic_rect_make(16, 16, 36, 36);
    if (i == 1) return ic_rect_make(20, y, 32, 32);
    if (i == 2) return ic_rect_make(60, y, 32, 32);
    return ic_rect_make(100, y, 32, 32);
}

static ic_rect_t seek_bar(ic_app_t *app) {
    ic_rect_t b = bar_rect(app);
    return ic_rect_make(20, b.y + 10, b.w - 40, 14);
}

static int controls_visible(void) {
    return v.paused || v.ended || v.dragging || !v.vt || ic_time_ms() - v.last_input_ms < CONTROLS_HIDE_MS;
}

static void fmt_t(int64_t us, char *o, size_t n) {
    int64_t s = us / 1000000;
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(o, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(o, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

void video_draw(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t pr = picture_rect(app);
    ic_gfx_fill(c, 0, 0, app->width, app->height, 0xFF000000u);
    if (v.error[0]) {
        ic_symbol_draw(c, IC_SYM_WARNING, (float)(app->width / 2), (float)(app->height / 2 - 30), 34.0f, 0xFFFFC14Du);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(24, app->height / 2, app->width - 48, 24), v.error,
                        0xFFFFFFFFu, IC_ALIGN_CENTER);
        ic_ui_icon_button(c, ctl_rect(app, 0), IC_SYM_CHEVRON_LEFT, v.hover == 0 ? IC_STATE_HOVER : IC_STATE_NORMAL);
        return;
    }
    if (v.vt) put_picture(c, pr);
    else {
        ic_symbol_draw(c, IC_SYM_MUSIC, (float)(app->width / 2), (float)(app->height / 2 - 20), 64.0f, 0x66FFFFFFu);
    }
    if ((!v.vd && !v.ad) || v.buffering) {
        /* a ring of dots turning */
        uint32_t t = ic_time_ms() / 100;
        for (int i = 0; i < 8; i++) {
            int on = (int)((t + (uint32_t)i) % 8);
            static const signed char ring[8][2] = { { 18, 0 }, { 13, 13 }, { 0, 18 }, { -13, 13 }, { -18, 0 }, { -13, -13 }, { 0, -18 }, { 13, -13 } };
            int cx = app->width / 2 + ring[i][0], cy = app->height / 2 + ring[i][1];
            ic_gfx_rrect(c, cx - 3, cy - 3, 6, 6, 3.0f, (uint32_t)(0x30 + on * 0x1C) << 24 | 0xFFFFFFu);
        }
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(0, app->height / 2 + 32, app->width, 18),
                        !v.vd && !v.ad ? "Opening" : "Buffering", 0xB3FFFFFFu, IC_ALIGN_CENTER);
    }
    if (!controls_visible()) return;
    {
        ic_rect_t b = bar_rect(app), s = seek_bar(app);
        int64_t pos = v.dragging ? (int64_t)((double)(v.drag_x - s.x) / s.w * (double)v.duration_us) : clock_us();
        float frac = v.duration_us ? (float)pos / (float)v.duration_us : 0;
        char t1[16], t2[16], tt[40];
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        /* shade top and bottom so white controls read on any picture */
        for (int i = 0; i < 6; i++) {
            ic_gfx_fill(c, 0, b.y + i * (BAR_H / 6), b.w, BAR_H / 6 + 1, (uint32_t)(0x18 + i * 0x14) << 24);
            ic_gfx_fill(c, 0, i * 11, app->width, 11, (uint32_t)(0x88 - i * 0x16) << 24);
        }
        ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(64, 16, app->width - 96, 36), v.title, 0xFFFFFFFFu,
                        IC_ALIGN_LEFT);
        ic_symbol_draw(c, IC_SYM_CHEVRON_LEFT, (float)(ctl_rect(app, 0).x + 18), (float)(ctl_rect(app, 0).y + 18), 20.0f,
                       v.hover == 0 ? 0xFFFFFFFFu : 0xCCFFFFFFu);
        /* seek bar: buffered part, played part, knob */
        ic_gfx_rrect(c, s.x, s.y + 5, s.w, 4, 2.0f, 0x40FFFFFFu);
        {
            const input_t *in = v.vin ? v.vin : v.ain;
            if (in && !in->src->done && in->src->total) {
                float bf = (float)in->src->len / (float)in->src->total;
                ic_gfx_rrect(c, s.x, s.y + 5, (int)(s.w * (bf > 1 ? 1 : bf)), 4, 2.0f, 0x55FFFFFFu);
            }
        }
        ic_gfx_rrect(c, s.x, s.y + 5, (int)(s.w * frac), 4, 2.0f, 0xFFFF3B5Cu);
        if (v.hover == 4 || v.dragging) ic_gfx_rrect(c, s.x + (int)(s.w * frac) - 7, s.y, 14, 14, 7.0f, 0xFFFF3B5Cu);
        for (int i = 1; i <= 3; i++) {
            ic_rect_t r = ctl_rect(app, i);
            ic_symbol_t sym = i == 1 ? (v.paused || v.ended ? IC_SYM_PLAY : IC_SYM_PAUSE) : i == 2 ? IC_SYM_CHEVRON_LEFT : IC_SYM_CHEVRON_RIGHT;
            ic_symbol_draw(c, sym, (float)(r.x + 16), (float)(r.y + 16), i == 1 ? 20.0f : 16.0f,
                           v.hover == i ? 0xFFFFFFFFu : 0xD9FFFFFFu);
        }
        fmt_t(pos, t1, sizeof t1);
        fmt_t(v.duration_us, t2, sizeof t2);
        snprintf(tt, sizeof tt, "%s / %s", t1, t2);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(144, b.y + 30, 200, 32), tt, 0xE6FFFFFFu, IC_ALIGN_LEFT);
        if (v.vt) {
            char info[96];
            snprintf(info, sizeof info, "%s  %dx%d", vdec_codec_name(v.vt->codec), v.vt->width, v.vt->height);
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(b.w - 420, b.y + 30, 400, 32), info, 0x99FFFFFFu,
                            IC_ALIGN_RIGHT);
        }
    }
    (void)pr;
}

/* ---- input ----------------------------------------------------------------------------------- */

static void toggle_pause(void) {
    if (v.ended) {
        v.paused = 0;
        seek_to(0);
        return;
    }
    if (v.paused) {
        v.paused = 0;
        clock_run();
        if (v.stream > 0) icda_audio_stream_control(v.stream, 0, 256);
    } else {
        clock_hold();
        v.paused = 1;
        if (v.stream > 0) icda_audio_stream_control(v.stream, 1, 256);
    }
}

int video_event(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t s = seek_bar(app);
    v.last_input_ms = ic_time_ms();
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        v.hover = -1;
        for (int i = 0; i < 4; i++)
            if (ic_ui_hit(ctl_rect(app, i), ev->x, ev->y)) v.hover = i;
        if (ic_ui_hit(ic_rect_make(s.x, s.y - 6, s.w, s.h + 12), ev->x, ev->y)) v.hover = 4;
        if (v.dragging) v.drag_x = ev->x < s.x ? s.x : ev->x > s.x + s.w ? s.x + s.w : ev->x;
        break;
    case IC_EV_MOUSE_DOWN:
        if (ic_ui_hit(ctl_rect(app, 0), ev->x, ev->y)) return VIDEO_EV_LIBRARY;
        if (v.error[0]) break;
        if (ic_ui_hit(ctl_rect(app, 1), ev->x, ev->y)) toggle_pause();
        else if (ic_ui_hit(ctl_rect(app, 2), ev->x, ev->y)) seek_to(clock_us() - 10000000);
        else if (ic_ui_hit(ctl_rect(app, 3), ev->x, ev->y)) seek_to(clock_us() + 10000000);
        else if (ic_ui_hit(ic_rect_make(s.x, s.y - 6, s.w, s.h + 12), ev->x, ev->y)) {
            v.dragging = 1;
            v.drag_x = ev->x;
        } else if (ev->y < app->height - BAR_H && ev->y > 60) toggle_pause();
        break;
    case IC_EV_MOUSE_UP:
        if (v.dragging) {
            v.dragging = 0;
            v.have_cur = v.paused ? 0 : v.have_cur;
            seek_to((int64_t)((double)(v.drag_x - s.x) / s.w * (double)v.duration_us));
        }
        break;
    case IC_EV_KEY:
        if (ev->key == IC_KEY_ESCAPE) return VIDEO_EV_LIBRARY;
        if (v.error[0]) break;
        if (ev->key == ' ' || ev->key == 'k') toggle_pause();
        else if (ev->key == IC_KEY_LEFT || ev->key == 'j') seek_to(clock_us() - (ev->key == 'j' ? 10000000 : 5000000));
        else if (ev->key == IC_KEY_RIGHT || ev->key == 'l') seek_to(clock_us() + (ev->key == 'l' ? 10000000 : 5000000));
        else if (ev->key == IC_KEY_HOME) seek_to(0);
        if (v.paused && ev->key != ' ') v.have_cur = 0;
        break;
    default:
        break;
    }
    return VIDEO_EV_NONE;
}

int video_is_open(void) {
    return v.open;
}

int video_open_url(const char *url, const char *title, char *err, size_t errcap) {
    (void)url;
    (void)title;
    snprintf(err, errcap, "Streaming is not available");
    return -1;
}
