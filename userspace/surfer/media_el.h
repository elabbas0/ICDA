#ifndef SURFER_MEDIA_EL_H
#define SURFER_MEDIA_EL_H

#include <stdint.h>
#include <stddef.h>
#include "paint.h"

/* <video> and <audio> playback for Surfer.  Media arrives either as one
 * progressive download (src = URL) or through Media Source Extensions
 * (src = a MediaSource: the page appends segments to source buffers, which
 * is how YouTube streams).  Either way it is demuxed and decoded with
 * Media's code (MP4 / WebM; H.264, VP8 / VP9, AAC, Opus, Vorbis).
 * Pictures come out as an ARGB image for paint; sound goes to the audio
 * sink the host installs (ICDA's mixer), and its position is the clock.
 * Without a sink (the host test harness) the wall clock is used. */

typedef struct media_el media_el_t;

typedef struct {
    long (*open)(uint32_t rate, uint32_t channels);
    long (*write)(long id, const int16_t *pcm, uint64_t bytes);
    long (*position)(long id);         /* frames played */
    long (*queued)(long id);           /* frames not yet played */
    void (*control)(long id, int paused, uint32_t volume);  /* volume 0..256 */
    void (*close)(long id);
} media_audio_t;

void media_set_audio(const media_audio_t *sink);

typedef struct {
    double time, duration;             /* seconds; duration 0 if unknown */
    double buffered_end;               /* seconds playable from the current time on */
    int    ready_state;                /* HTMLMediaElement readyState 0..4 */
    int    paused, ended, waiting, seeking;
    int    error;                      /* 0, or MediaError code (2 network, 3 decode, 4 not supported) */
    int    width, height;
    uint32_t frames_shown, frames_dropped;
} media_el_state_t;

/* progressive: plays url */
media_el_t *media_el_open(const char *url);
/* MSE: sources are added and fed by the page */
media_el_t *media_el_open_mse(void);
int         media_el_add_source(media_el_t *m);                       /* source id or -1 */
int         media_el_append(media_el_t *m, int src, const uint8_t *data, size_t len);
void        media_el_remove(media_el_t *m, int src, double start, double end);
/* buffered ranges of a source (or all sources together for src -1); returns the count */
int         media_el_buffered(media_el_t *m, int src, double *ranges, int max);
void        media_el_set_duration(media_el_t *m, double d);
void        media_el_end_of_stream(media_el_t *m);

void        media_el_close(media_el_t *m);
void        media_el_play(media_el_t *m);
void        media_el_pause(media_el_t *m);
void        media_el_seek(media_el_t *m, double t);
void        media_el_volume(media_el_t *m, double volume, int muted);
void        media_el_state(media_el_t *m, media_el_state_t *out);
const image_t *media_el_frame(media_el_t *m);   /* 0 until a picture is decoded */
int         media_el_size(media_el_t *m, int *w, int *h);

/* script handles (small integers) and the element each player draws into */
int         media_el_id(media_el_t *m);
media_el_t *media_el_get(int id);
void        media_el_bind(media_el_t *m, const void *node);
media_el_t *media_el_for_node(const void *node);

enum { MEDIA_NEW_FRAME = 1, MEDIA_SIZE_KNOWN = 2, MEDIA_STATE = 4 };
/* advances every open element; returns MEDIA_* flags */
int  media_el_tick_all(void);
/* the same from inside a long script (no flags; they wait for the next tick) */
void media_el_pump(void);
void media_el_close_all(void);
int  media_el_active(void);              /* any element open */

#endif
