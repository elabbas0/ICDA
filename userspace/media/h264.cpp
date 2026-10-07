/* H.264 through OpenH264 (third_party/openh264, BSD-2-Clause): MP4 samples
 * (length-prefixed NAL units) become Annex B for the decoder, which
 * returns pictures in display order. */
#include <stdlib.h>
#include <string.h>
#include "codec_api.h"
#include "codec_app_def.h"

extern "C" {
#include "h264.h"
}

struct h264 {
    ISVCDecoder *dec;
    int          nal_len;          /* bytes in each NAL length prefix */
    uint8_t     *buf;
    int          cap;
    uint8_t     *params;           /* SPS and PPS with start codes */
    int          params_len;
    int          need_params;
};

static int grow(h264_t *h, int need) {
    if (need <= h->cap) return 0;
    int cap = need + need / 2 + 4096;
    uint8_t *n = (uint8_t *)realloc(h->buf, (size_t)cap);
    if (!n) return -1;
    h->buf = n;
    h->cap = cap;
    return 0;
}

static int put_nal(uint8_t *dst, const uint8_t *nal, int len) {
    dst[0] = dst[1] = dst[2] = 0;
    dst[3] = 1;
    memcpy(dst + 4, nal, (size_t)len);
    return len + 4;
}

extern "C" h264_t *h264_open(const uint8_t *avcc, int len) {
    h264_t *h = (h264_t *)calloc(1, sizeof(h264_t));
    SDecodingParam p;
    if (!h) return 0;
    h->nal_len = 4;
    /* avcC: version, profile, compat, level, 0xFC|lengthSizeMinusOne, 0xE0|numSPS, ... */
    if (avcc && len >= 7) {
        int pos = 6, nsps = avcc[5] & 31, npps;
        h->nal_len = (avcc[4] & 3) + 1;
        h->params = (uint8_t *)malloc((size_t)len * 2 + 64);
        if (!h->params) {
            free(h);
            return 0;
        }
        for (int i = 0; i < nsps && pos + 2 <= len; i++) {
            int n = (avcc[pos] << 8) | avcc[pos + 1];
            pos += 2;
            if (pos + n > len) break;
            h->params_len += put_nal(h->params + h->params_len, avcc + pos, n);
            pos += n;
        }
        if (pos < len) {
            npps = avcc[pos++];
            for (int i = 0; i < npps && pos + 2 <= len; i++) {
                int n = (avcc[pos] << 8) | avcc[pos + 1];
                pos += 2;
                if (pos + n > len) break;
                h->params_len += put_nal(h->params + h->params_len, avcc + pos, n);
                pos += n;
            }
        }
        h->need_params = h->params_len > 0;
    } else {
        h->nal_len = 0;                 /* Annex B already */
    }
    if (WelsCreateDecoder(&h->dec) != 0 || !h->dec) {
        free(h->params);
        free(h);
        return 0;
    }
    memset(&p, 0, sizeof p);
    p.sVideoProperty.size = sizeof p.sVideoProperty;
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    p.eEcActiveIdc = ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE;
    p.uiTargetDqLayer = (uint8_t)-1;
    if (h->dec->Initialize(&p) != 0) {
        WelsDestroyDecoder(h->dec);
        free(h->params);
        free(h);
        return 0;
    }
    {
        int32_t level = WELS_LOG_QUIET;
        h->dec->SetOption(DECODER_OPTION_TRACE_LEVEL, &level);
    }
    return h;
}

static int take(SBufferInfo *info, uint8_t **planes, h264_frame_t *out) {
    if (info->iBufferStatus != 1 || !planes[0]) return 0;
    out->y = planes[0];
    out->u = planes[1];
    out->v = planes[2];
    out->stride_y = info->UsrData.sSystemBuffer.iStride[0];
    out->stride_uv = info->UsrData.sSystemBuffer.iStride[1];
    out->width = info->UsrData.sSystemBuffer.iWidth;
    out->height = info->UsrData.sSystemBuffer.iHeight;
    out->pts = (int64_t)info->uiOutYuvTimeStamp;
    return 1;
}

extern "C" int h264_decode(h264_t *h, const uint8_t *data, int len, int64_t pts, h264_frame_t *out) {
    int n = 0;
    uint8_t *planes[3] = { 0, 0, 0 };
    SBufferInfo info;
    if (grow(h, len * 2 + h->params_len + 64) < 0) return 0;
    if (h->need_params) {
        memcpy(h->buf, h->params, (size_t)h->params_len);
        n = h->params_len;
        h->need_params = 0;
    }
    if (h->nal_len) {
        int pos = 0;
        while (pos + h->nal_len <= len) {
            int sz = 0;
            for (int i = 0; i < h->nal_len; i++) sz = (sz << 8) | data[pos + i];
            pos += h->nal_len;
            if (sz <= 0 || pos + sz > len) break;
            n += put_nal(h->buf + n, data + pos, sz);
            pos += sz;
        }
    } else {
        memcpy(h->buf + n, data, (size_t)len);
        n += len;
    }
    memset(&info, 0, sizeof info);
    info.uiInBsTimeStamp = (unsigned long long)pts;
    h->dec->DecodeFrameNoDelay(h->buf, n, planes, &info);
    return take(&info, planes, out);
}

extern "C" int h264_flush(h264_t *h, h264_frame_t *out) {
    int32_t left = 0;
    uint8_t *planes[3] = { 0, 0, 0 };
    SBufferInfo info;
    h->dec->GetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &left);
    if (left <= 0) return 0;
    memset(&info, 0, sizeof info);
    h->dec->FlushFrame(planes, &info);
    return take(&info, planes, out);
}

extern "C" void h264_reset(h264_t *h) {
    /* drop pictures held for reordering; the parameter sets go in again */
    h264_frame_t f;
    while (h264_flush(h, &f)) {}
    h->need_params = h->params_len > 0;
}

extern "C" void h264_close(h264_t *h) {
    if (!h) return;
    if (h->dec) {
        h->dec->Uninitialize();
        WelsDestroyDecoder(h->dec);
    }
    free(h->buf);
    free(h->params);
    free(h);
}
