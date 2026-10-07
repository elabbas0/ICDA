/* Video decoder dispatch: H.264 (OpenH264), VP8 / VP9 (libvpx). */
#include <stdlib.h>
#include <string.h>
#include "vdec.h"
#include "container.h"
#include "h264.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

struct vdec {
    int             codec;
    h264_t         *h264;
    vpx_codec_ctx_t vpx;
    int             vpx_open;
    int64_t         vpx_pts;
};

const char *vdec_codec_name(int codec) {
    switch (codec) {
    case CODEC_H264: return "H.264";
    case CODEC_HEVC: return "HEVC (H.265)";
    case CODEC_VP8: return "VP8";
    case CODEC_VP9: return "VP9";
    case CODEC_AV1: return "AV1";
    default: return "an unknown codec";
    }
}

static int vpx_start(vdec_t *d) {
    vpx_codec_dec_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.threads = 1;
    if (vpx_codec_dec_init(&d->vpx, d->codec == CODEC_VP8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK)
        return -1;
    d->vpx_open = 1;
    return 0;
}

vdec_t *vdec_open(int codec, const uint8_t *cfg, int cfg_len) {
    vdec_t *d = (vdec_t *)calloc(1, sizeof(vdec_t));
    int ok = 0;
    if (!d) return 0;
    d->codec = codec;
    if (codec == CODEC_H264) ok = (d->h264 = h264_open(cfg, cfg_len)) != 0;
    else if (codec == CODEC_VP8 || codec == CODEC_VP9) ok = vpx_start(d) == 0;
    if (!ok) {
        free(d);
        return 0;
    }
    return d;
}

static void from_h264(const h264_frame_t *f, vframe_t *out) {
    out->y = f->y;
    out->u = f->u;
    out->v = f->v;
    out->stride_y = f->stride_y;
    out->stride_uv = f->stride_uv;
    out->width = f->width;
    out->height = f->height;
    out->pts_us = f->pts;
}

/* VP8 / VP9 never reorder: a picture that comes out belongs to this sample */
static int vpx_take(vdec_t *d, vframe_t *out) {
    vpx_codec_iter_t it = 0;
    vpx_image_t *img = vpx_codec_get_frame(&d->vpx, &it);
    if (!img || img->fmt != VPX_IMG_FMT_I420) return 0;
    out->y = img->planes[0];
    out->u = img->planes[1];
    out->v = img->planes[2];
    out->stride_y = img->stride[0];
    out->stride_uv = img->stride[1];
    out->width = (int)img->d_w;
    out->height = (int)img->d_h;
    out->pts_us = d->vpx_pts;
    return 1;
}

int vdec_decode(vdec_t *d, const uint8_t *data, int len, int64_t pts_us, vframe_t *out) {
    if (d->h264) {
        h264_frame_t f;
        if (h264_decode(d->h264, data, len, pts_us, &f)) {
            from_h264(&f, out);
            return 1;
        }
        return 0;
    }
    if (d->vpx_open) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)len, 0, 0) != VPX_CODEC_OK) return 0;
        d->vpx_pts = pts_us;
        return vpx_take(d, out);
    }
    return 0;
}

int vdec_flush(vdec_t *d, vframe_t *out) {
    h264_frame_t f;
    if (d->h264 && h264_flush(d->h264, &f)) {
        from_h264(&f, out);
        return 1;
    }
    return 0;
}

void vdec_reset(vdec_t *d) {
    if (d->h264) h264_reset(d->h264);
    if (d->vpx_open) {               /* references from before the seek are useless */
        vpx_codec_destroy(&d->vpx);
        d->vpx_open = 0;
        vpx_start(d);
    }
}

void vdec_close(vdec_t *d) {
    if (!d) return;
    if (d->h264) h264_close(d->h264);
    if (d->vpx_open) vpx_codec_destroy(&d->vpx);
    free(d);
}
