#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include <stdint.h>
#include "../../fs/vfs.h"

typedef struct {
    uint64_t active;
    uint64_t seconds_left;
    uint64_t total_seconds;
    char name[64];
} audio_playback_status_t;

int audio_playback_init(void);
int audio_playback_play_wav(vfs_node_t *cwd, const char *path);
int audio_playback_claim(uint64_t pid, uint64_t *token_out, uint32_t *sample_rate_out);
uint32_t audio_playback_read_chunk(uint64_t token, uint8_t *dst, uint32_t cap);
void audio_playback_finish(uint64_t token);
void audio_playback_stop(void);
void audio_playback_tick(void);
int audio_playback_status(audio_playback_status_t *out);

/* Effects: short sounds mixed over everything else (volume 0..256). */
int     audio_effect_play(const char *path, uint32_t volume);

/* Streams: apps push interleaved s16 PCM at their own rate; ids from 1. */
int     audio_stream_open(uint32_t rate, uint32_t channels);
int64_t audio_stream_write(int id, const int16_t *pcm, uint64_t bytes);
int64_t audio_stream_position(int id);
int64_t audio_stream_queued(int id);
int     audio_stream_control(int id, int paused, uint32_t volume);
void    audio_stream_close(int id);

#endif
