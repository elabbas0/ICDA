#ifndef MEDIA_VDEC_H
#define MEDIA_VDEC_H

#include <stdint.h>

/* Video decoders behind one interface.  A decoded picture is YUV 4:2:0;
 * its planes stay valid until the next vdec call. */

typedef struct {
    const uint8_t *y, *u, *v;
    int            stride_y, stride_uv;
    int            width, height;
    int64_t        pts_us;
} vframe_t;

typedef struct vdec vdec_t;

/* codec: CODEC_* from container.h; cfg: avcC for H.264 */
vdec_t *vdec_open(int codec, const uint8_t *cfg, int cfg_len);
/* feeds one sample; 1 and *out filled when a picture is ready */
int     vdec_decode(vdec_t *d, const uint8_t *data, int len, int64_t pts_us, vframe_t *out);
/* after the last sample: pictures still held back for reordering */
int     vdec_flush(vdec_t *d, vframe_t *out);
void    vdec_reset(vdec_t *d);       /* before decoding from a new key frame */
void    vdec_close(vdec_t *d);
const char *vdec_codec_name(int codec);

#endif
