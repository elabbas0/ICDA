#ifndef MEDIA_PDEC_H
#define MEDIA_PDEC_H

#include <stdint.h>

/* Audio decoders for packets out of a container (video soundtracks and
 * YouTube): AAC, Opus, Vorbis.  Output is interleaved s16, 1 or 2 channels. */

typedef struct pdec pdec_t;

pdec_t *pdec_open(int codec, const uint8_t *cfg, int cfg_len, int rate, int channels);
int     pdec_rate(const pdec_t *d);
int     pdec_channels(const pdec_t *d);
/* returns frames written (at most max_frames), 0 if nothing came out */
int     pdec_decode(pdec_t *d, const uint8_t *pkt, int len, int16_t *out, int max_frames);
void    pdec_reset(pdec_t *d);
void    pdec_close(pdec_t *d);
const char *pdec_codec_name(int codec);

#define PDEC_MAX_FRAMES 5760          /* the most one packet can produce (Opus 120 ms) */

#endif
