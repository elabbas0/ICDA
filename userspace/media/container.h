#ifndef MEDIA_CONTAINER_H
#define MEDIA_CONTAINER_H

#include <stdint.h>
#include <stddef.h>

/* What the demuxers (mp4demux.c, mkvdemux.c) turn a file into: per track a
 * table of samples (where each one is in the file and when it plays).
 * Files are parsed from memory; a growing buffer (a download in progress)
 * can be parsed again with more data and only new boxes are read. */

enum { TRACK_VIDEO = 1, TRACK_AUDIO };
enum {
    CODEC_UNKNOWN = 0,
    CODEC_H264, CODEC_HEVC, CODEC_VP8, CODEC_VP9, CODEC_AV1,
    CODEC_AAC, CODEC_MP3, CODEC_OPUS, CODEC_VORBIS
};

typedef struct {
    uint64_t off;
    uint32_t size;
    uint8_t  key;
    int64_t  pts_us;          /* presentation time, edit list applied */
    int64_t  dts_us;          /* decode time: samples decode in this order */
} msample_t;

typedef struct {
    int        id;
    int        kind;          /* TRACK_* */
    int        codec;         /* CODEC_* */
    uint8_t   *cfg;           /* avcC, AudioSpecificConfig, CodecPrivate ... */
    int        cfg_len;
    int        width, height;
    int        rate, channels;
    uint32_t   timescale;
    int64_t    edit_offset;   /* media time where presentation starts (timescale units) */
    int64_t    next_dts;      /* fragments: decode time after the last sample */
    uint32_t   def_duration, def_size, def_flags;     /* trex */
    int64_t    codec_delay_us, seek_preroll_us;       /* webm opus */
    msample_t *s;
    int        n, cap;
    int        unsorted;      /* a sample arrived with an earlier decode time */
} mtrack_t;

#define MAX_TRACKS 8

typedef struct {
    mtrack_t tracks[MAX_TRACKS];
    int      ntracks;
    int64_t  duration_us;
    size_t   parsed;          /* bytes of top-level boxes / clusters fully read */
    int      have_header;     /* moov / Tracks seen */
    int      fragmented;
    int      is_mkv;
    /* mkv cluster state */
    uint64_t mkv_segment_data, mkv_timescale_ns;
    int64_t  mkv_cluster_time;
} mfile_t;

/* 0 ok (possibly more to come), -1 not this container / broken */
int  mp4_parse(mfile_t *f, const uint8_t *data, size_t len);
int  mkv_parse(mfile_t *f, const uint8_t *data, size_t len);
int  container_is_mp4(const uint8_t *data, size_t len);
int  container_is_mkv(const uint8_t *data, size_t len);
void container_free(mfile_t *f);

mtrack_t *container_track(mfile_t *f, int kind);
int       container_add_sample(mtrack_t *t, uint64_t off, uint32_t size, int key, int64_t pts_us);
int       container_add_sample_dts(mtrack_t *t, uint64_t off, uint32_t size, int key, int64_t pts_us, int64_t dts_us);
/* sorts samples into decode order after segments arrived out of order (MSE
 * appends after a seek); returns 1 if anything moved */
int       container_sort(mtrack_t *t);
/* the last key sample at or before t_us (index), 0 if none */
int       container_seek_index(const mtrack_t *t, int64_t t_us);

#endif
