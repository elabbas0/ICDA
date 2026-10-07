#ifndef MEDIA_AAC_H
#define MEDIA_AAC_H

#include <stdint.h>

/* AAC-LC decoder (MPEG-4 audio, as in M4A files and MP4 video).
 * asc: the AudioSpecificConfig from the MP4 track (esds).
 * aac_decode() turns one raw access unit into interleaved s16 PCM with
 * out_channels channels (1 or 2) and returns the frames written (1024),
 * 0 if the unit could not be decoded. */

typedef struct aac aac_t;

aac_t *aac_open(const uint8_t *asc, int asc_len);
aac_t *aac_open_params(int rate_index, int channel_config);
int    aac_rate(const aac_t *a);
int    aac_channels(const aac_t *a);
int    aac_decode(aac_t *a, const uint8_t *au, int len, int16_t *out, int out_channels);
void   aac_reset(aac_t *a);
void   aac_close(aac_t *a);

/* ADTS frame header (.aac files); 0 if p starts one */
int    aac_adts_parse(const uint8_t *p, int len, int *rate_index, int *channel_config, int *frame_len,
                      int *header_len);

#endif
