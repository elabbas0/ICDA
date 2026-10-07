#ifndef MEDIA_H264_H
#define MEDIA_H264_H

#include <stdint.h>

typedef struct h264 h264_t;

typedef struct {
    const uint8_t *y, *u, *v;
    int            stride_y, stride_uv;
    int            width, height;
    int64_t        pts;
} h264_frame_t;

h264_t *h264_open(const uint8_t *avcc, int len);      /* avcc NULL: Annex B input */
int     h264_decode(h264_t *h, const uint8_t *data, int len, int64_t pts, h264_frame_t *out);
int     h264_flush(h264_t *h, h264_frame_t *out);
void    h264_reset(h264_t *h);
void    h264_close(h264_t *h);

#endif
