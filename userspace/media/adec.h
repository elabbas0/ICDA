#ifndef MEDIA_ADEC_H
#define MEDIA_ADEC_H

#include <stdint.h>
#include <stddef.h>

/* Audio decoding behind one interface: WAV, MP3, FLAC, Ogg Vorbis and AAC
 * in MP4/M4A.  The whole file is held in memory (the VFS is RAM anyway);
 * output is interleaved signed 16-bit PCM at the file's own rate. */

typedef struct adec adec_t;

typedef struct {
    uint32_t rate;
    int      channels;          /* 1 or 2 */
    uint64_t frames;            /* total, 0 if unknown */
    char     format[16];        /* "MP3", "FLAC", ... */
    char     title[96];         /* from tags when present, else empty */
    char     artist[96];
} adec_info_t;

/* Takes ownership of data (malloc'd) on success. */
adec_t *adec_open_memory(uint8_t *data, size_t len, const char *name_hint);
const adec_info_t *adec_info(const adec_t *d);
/* Decodes up to frames frames; returns frames written, 0 at the end. */
long    adec_read(adec_t *d, int16_t *out, long frames);
int     adec_seek(adec_t *d, uint64_t frame);
void    adec_close(adec_t *d);

/* 1 if the name looks like an audio file adec can open */
int     adec_is_audio_name(const char *name);

#endif
