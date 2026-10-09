/* Audio mixer: every sound in ICDA plays through here, together.
 *
 * One 48 kHz stereo 16-bit HDA stream runs while anything is audible.  A
 * kernel thread keeps the ring filled about 60 ms ahead of the controller's
 * position (LPIB) and mixes, per output frame, every active voice:
 *
 *   music    one track started by the music player (SYS_AUDIO_PLAY_FILE);
 *            starting another replaces only this voice
 *   effects  short UI sounds (SYS_AUDIO_MIX / MIX_EFFECT); any number play
 *            at once and never stop anything else
 *   streams  PCM pushed by apps such as the media player (MIX_STREAM_*)
 *
 * Voices read their WAV data in place (the VFS keeps it) with linear
 * resampling, so starting a sound costs nothing.  Mixing is integer math,
 * one multiply-add per voice per sample, followed by a soft limiter so many
 * sounds together do not clip harshly.  With nothing to play for a moment
 * the stream stops and the thread only checks for work. */
#include "playback.h"

#include "hda.h"
#include "../console/console.h"
#include "../serial/serial.h"
#include "../../proc/sched.h"
#include "../../memory/heap.h"
#include "../../cpu/tsc.h"

#define MIX_RATE        48000U
#define FRAME_BYTES     4U                      /* stereo s16 */
#define RING_BYTES      65536U                  /* 341 ms; matches the HDA buffer descriptor list */
#define LEAD_BYTES      11520U                  /* 60 ms written ahead */
#define CHUNK_FRAMES    512U
#define MAX_VOICES      32
#define MAX_STREAMS     4
#define STREAM_SECONDS  2U
#define IDLE_STOP_TICKS 150U                    /* stop the stream after 1.5 s of silence */

#define AUDIO_MIN_RATE_HZ   8000U
#define AUDIO_MAX_RATE_HZ   96000U
#define AUDIO_MAX_WAV_BYTES (64U * 1024U * 1024U)

enum { V_FREE = 0, V_MUSIC, V_EFFECT };

typedef struct {
    int            kind;
    const uint8_t *pcm;
    uint32_t       frames;
    uint16_t       channels, bits;
    uint32_t       rate;
    uint64_t       pos;         /* source frame, 16.16 fixed point */
    uint32_t       step;        /* source frames per output frame, 16.16 */
    int32_t        vol;         /* 0..256 */
} voice_t;

typedef struct {
    int       used;
    int16_t  *ring;             /* interleaved frames at the stream's rate */
    uint32_t  cap;              /* frames */
    uint64_t  rd, wr;           /* frames consumed / written (monotonic) */
    uint64_t  frac;             /* 16.16 position past rd */
    uint32_t  step;
    uint16_t  channels;
    uint32_t  rate;
    int       paused;
    int32_t   vol;
} stream_t;

static voice_t  voices[MAX_VOICES];
static stream_t streams[MAX_STREAMS];
static int32_t  mix_acc[CHUNK_FRAMES * 2];
static int16_t  mix_out[CHUNK_FRAMES * 2];
static uint8_t  silence[4096];

static int      out_running;
static uint64_t write_abs, play_abs;            /* bytes written / played since start */
static uint64_t silence_abs;                    /* ring cleared up to here */
static uint32_t last_lpib;
/* playback position: the controller's LPIB when it advances (real HDA),
 * otherwise elapsed TSC time since the stream started (QEMU reads LPIB 0) */
static int      pos_from_lpib, lpib_changes, lpib_bad;
static uint64_t lpib_change_us;
static uint64_t run_us;
static uint64_t idle_since;

static char     music_name[64];
static uint64_t music_frames_total;
static process_t *audio_worker_proc = 0;

static void audio_update_hud(int force_clear);

/* ---- small helpers ------------------------------------------------------- */

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void append_text(char *dst, const char *src, uint64_t cap) {
    uint64_t out = str_len(dst);
    uint64_t i = 0;
    while (src && src[i] && out + 1 < cap) dst[out++] = src[i++];
    dst[out] = 0;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_wav(const uint8_t *buf, uint64_t size, uint16_t *channels_out, uint32_t *rate_out,
                     uint16_t *bits_out, const uint8_t **data_out, uint32_t *data_size_out) {
    uint64_t off = 12;
    uint16_t fmt_tag = 0, channels = 0, bits = 0, block_align = 0;
    uint32_t rate = 0, data_size = 0, frame_bytes;
    const uint8_t *data = 0;
    int have_fmt = 0;

    if (!buf || size < 44) return -1;
    if (!(buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F')) return -1;
    if (!(buf[8] == 'W' && buf[9] == 'A' && buf[10] == 'V' && buf[11] == 'E')) return -1;
    while (off + 8 <= size) {
        const uint8_t *chunk = &buf[off];
        uint32_t chunk_size = read_le32(chunk + 4);
        uint64_t next = off + 8ULL + chunk_size + (chunk_size & 1U);
        if (chunk[0] == 'f' && chunk[1] == 'm' && chunk[2] == 't' && chunk[3] == ' ') {
            if (chunk_size < 16 || off + 24 > size) return -1;
            fmt_tag = read_le16(chunk + 8);
            channels = read_le16(chunk + 10);
            rate = read_le32(chunk + 12);
            block_align = read_le16(chunk + 20);
            bits = read_le16(chunk + 22);
            have_fmt = 1;
        } else if (chunk[0] == 'd' && chunk[1] == 'a' && chunk[2] == 't' && chunk[3] == 'a') {
            data = chunk + 8;
            data_size = chunk_size;
            if ((uint64_t)(data - buf) + data_size > size) data_size = (uint32_t)(size - (uint64_t)(data - buf));
            break;
        }
        if (next > size) break;
        off = next;
    }
    if (!have_fmt || !data || (fmt_tag != 1 && fmt_tag != 0xFFFE)) return -1;
    if (channels < 1 || channels > 2 || !(bits == 8 || bits == 16)) return -1;
    if (rate < AUDIO_MIN_RATE_HZ || rate > AUDIO_MAX_RATE_HZ) return -1;
    frame_bytes = (uint32_t)channels * (bits / 8U);
    if (block_align != frame_bytes) return -1;
    if (data_size == 0 || data_size > AUDIO_MAX_WAV_BYTES) return -1;
    data_size -= data_size % frame_bytes;
    if (!data_size) return -1;
    *channels_out = channels;
    *rate_out = rate;
    *bits_out = bits;
    *data_out = data;
    *data_size_out = data_size;
    return 0;
}

/* ---- voices ---------------------------------------------------------------- */

static int32_t voice_sample(const voice_t *v, uint32_t frame, int ch) {
    uint32_t c = v->channels == 2 ? (uint32_t)ch : 0;
    if (v->bits == 16) return (int16_t)read_le16(v->pcm + ((size_t)frame * v->channels + c) * 2U);
    return ((int32_t)v->pcm[(size_t)frame * v->channels + c] - 128) << 8;
}

static int voice_load(voice_t *v, const char *path, int kind, int32_t vol) {
    uint64_t size = 0;
    const uint8_t *file = (const uint8_t *)vfs_read(vfs_root(), path, &size);
    uint16_t ch = 0, bits = 0;
    uint32_t rate = 0, bytes = 0;
    const uint8_t *pcm = 0;
    if (!file || !size || parse_wav(file, size, &ch, &rate, &bits, &pcm, &bytes) != 0) return -1;
    v->pcm = pcm;
    v->channels = ch;
    v->bits = bits;
    v->rate = rate;
    v->frames = bytes / ((uint32_t)ch * (bits / 8U));
    v->pos = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / MIX_RATE);
    v->vol = vol;
    v->kind = kind;
    return 0;
}

/* a free voice, or the effect closest to its end */
static voice_t *voice_slot(void) {
    voice_t *best = 0;
    uint64_t best_left = ~0ULL;
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].kind == V_FREE) return &voices[i];
    for (int i = 0; i < MAX_VOICES; i++) {
        uint64_t left;
        if (voices[i].kind != V_EFFECT) continue;
        left = ((uint64_t)voices[i].frames << 16) - voices[i].pos;
        if (left < best_left) {
            best_left = left;
            best = &voices[i];
        }
    }
    return best;
}

/* ---- mixing ------------------------------------------------------------------ */

static int anything_audible(void) {
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].kind != V_FREE) return 1;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i].used && !streams[i].paused && streams[i].wr > streams[i].rd) return 1;
    return 0;
}

static void mix_voices(uint32_t frames) {
    for (int i = 0; i < MAX_VOICES; i++) {
        voice_t *v = &voices[i];
        uint64_t end;
        if (v->kind == V_FREE) continue;
        end = (uint64_t)v->frames << 16;
        for (uint32_t f = 0; f < frames; f++) {
            uint32_t idx, nxt;
            int32_t frac, l0, r0, l1, r1;
            if (v->pos >= end) {
                v->kind = V_FREE;
                break;
            }
            idx = (uint32_t)(v->pos >> 16);
            nxt = idx + 1 < v->frames ? idx + 1 : idx;
            frac = (int32_t)(v->pos & 0xFFFF) >> 1;      /* 15 bits keeps the products in range */
            l0 = voice_sample(v, idx, 0);
            r0 = voice_sample(v, idx, 1);
            l1 = voice_sample(v, nxt, 0);
            r1 = voice_sample(v, nxt, 1);
            mix_acc[f * 2] += ((l0 + (((l1 - l0) * frac) >> 15)) * v->vol) >> 8;
            mix_acc[f * 2 + 1] += ((r0 + (((r1 - r0) * frac) >> 15)) * v->vol) >> 8;
            v->pos += v->step;
        }
    }
}

static void mix_streams(uint32_t frames) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &streams[i];
        if (!s->used || s->paused) continue;
        for (uint32_t f = 0; f < frames; f++) {
            uint32_t a, b;
            int32_t frac, l0, r0, l1, r1;
            if (s->rd + 1 >= s->wr) break;              /* need two frames to interpolate */
            a = (uint32_t)(s->rd % s->cap);
            b = (uint32_t)((s->rd + 1) % s->cap);
            frac = (int32_t)(s->frac & 0xFFFF) >> 1;
            l0 = s->ring[a * s->channels];
            r0 = s->ring[a * s->channels + (s->channels - 1)];
            l1 = s->ring[b * s->channels];
            r1 = s->ring[b * s->channels + (s->channels - 1)];
            mix_acc[f * 2] += ((l0 + (((l1 - l0) * frac) >> 15)) * s->vol) >> 8;
            mix_acc[f * 2 + 1] += ((r0 + (((r1 - r0) * frac) >> 15)) * s->vol) >> 8;
            s->frac += s->step;
            s->rd += s->frac >> 16;
            s->frac &= 0xFFFF;
        }
    }
}

/* soft knee above -6 dBFS instead of hard clipping */
static int16_t limit(int32_t x) {
    const int32_t knee = 16384;
    int32_t a = x < 0 ? -x : x;
    if (a > knee) {
        a = knee + (a - knee) / 3;
        if (a > 32767) a = 32767;
    }
    return (int16_t)(x < 0 ? -a : a);
}

/* Master volume (0..256) and mute: the taskbar's volume control. */
static int32_t master_volume = 256;
static int master_muted;

void audio_master_set(uint32_t volume, int muted) {
    master_volume = (int32_t)(volume > 256 ? 256 : volume);
    master_muted = muted ? 1 : 0;
}

uint32_t audio_master_get(int *muted) {
    if (muted) *muted = master_muted;
    return (uint32_t)master_volume;
}

static void mix_into_ring(uint64_t at, uint32_t bytes) {
    uint32_t frames = bytes / FRAME_BYTES;
    for (uint32_t i = 0; i < frames * 2; i++) mix_acc[i] = 0;
    mix_voices(frames);
    mix_streams(frames);
    {
        /* perceived loudness follows the square of the slider */
        int32_t g = master_muted ? 0 : master_volume * master_volume / 256;
        for (uint32_t i = 0; i < frames * 2; i++) mix_out[i] = limit((int32_t)(((int64_t)mix_acc[i] * g) >> 8));
    }
    (void)hda_stream_write((uint32_t)(at % RING_BYTES), (const uint8_t *)mix_out, bytes);
}

static int output_start(void) {
    if (out_running) return 0;
    if (!hda_available()) return -1;
    if (hda_stream_start_s16_stereo((uint16_t)MIX_RATE, RING_BYTES) != 0) return -1;
    write_abs = play_abs = 0;
    silence_abs = 0;
    last_lpib = 0;
    pos_from_lpib = 0;
    lpib_changes = 0;
    out_running = 1;
    idle_since = sched_ticks();
    return 0;
}

/* Keeps the ring LEAD_BYTES ahead of the controller and silence beyond, so
 * a late wake-up plays a gap rather than stale audio. */
static void output_service(void) {
    uint32_t lpib, delta;
    uint64_t target;
    int was_empty;
    if (!out_running) return;
    lpib = hda_diag_lpi_b() % RING_BYTES;
    if (lpib != last_lpib && write_abs) {
        lpib_changes++;
        lpib_change_us = tsc_us();
    }
    if (!pos_from_lpib && !lpib_bad && lpib_changes >= 5 && tsc_us() - run_us < 300000) pos_from_lpib = 1;
    if (pos_from_lpib && tsc_us() - lpib_change_us > 50000) {
        /* LPIB stopped moving while audio plays: continue on the TSC from here */
        pos_from_lpib = 0;
        lpib_bad = 1;
        run_us = tsc_us() - play_abs / FRAME_BYTES * 1000000ULL / MIX_RATE;
    }
    if (pos_from_lpib) {
        delta = (lpib + RING_BYTES - last_lpib) % RING_BYTES;
        play_abs += delta;
    } else if (write_abs) {
        uint64_t frames = (tsc_us() - run_us) * MIX_RATE / 1000000ULL;
        play_abs = frames * FRAME_BYTES;
    }
    last_lpib = lpib;
    if (write_abs < play_abs) write_abs = play_abs;  /* underrun: skip ahead */
    target = play_abs + LEAD_BYTES;
    was_empty = write_abs == 0;
    while (write_abs < target) {
        uint32_t off = (uint32_t)(write_abs % RING_BYTES);
        uint32_t span = RING_BYTES - off;
        uint32_t chunk = (uint32_t)(target - write_abs);
        if (chunk > span) chunk = span;
        if (chunk > CHUNK_FRAMES * FRAME_BYTES) chunk = CHUNK_FRAMES * FRAME_BYTES;
        mix_into_ring(write_abs, chunk);
        write_abs += chunk;
    }
    {
        /* silence over the part of the ring the controller reaches next;
         * only the newly exposed part each time, so this costs one write
         * per byte of audio */
        uint64_t at = silence_abs > write_abs ? silence_abs : write_abs;
        uint64_t end = play_abs + RING_BYTES - 4096;
        while (at < end) {
            uint32_t off = (uint32_t)(at % RING_BYTES);
            uint32_t n = RING_BYTES - off;
            if (n > sizeof(silence)) n = sizeof(silence);
            if (at + n > end) n = (uint32_t)(end - at);
            (void)hda_stream_write(off, silence, n);
            at += n;
        }
        silence_abs = at;
    }
    if (was_empty) {
        (void)hda_stream_run();
        run_us = tsc_us();
    }
    if (anything_audible()) {
        idle_since = sched_ticks();
    } else if (sched_ticks() - idle_since > IDLE_STOP_TICKS) {
        hda_stop_playback();
        out_running = 0;
    }
}

static void audio_playback_worker(void) {
    for (;;) {
        if (out_running) {
            output_service();
            audio_update_hud(0);
            sched_sleep(1);
        } else {
            if (anything_audible() && output_start() == 0) {
                output_service();
                continue;
            }
            sched_sleep(1);             /* a new sound starts within a tick */
        }
    }
}

/* Called right after a sound is queued.  Starting the controller (codec
 * commands, stream reset) is left to the worker: done here it would run in
 * the caller's system call, and the window manager would stall on it. */
static void kick(void) {
}

/* ---- music (one track) ------------------------------------------------------ */

static voice_t *music_voice(void) {
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].kind == V_MUSIC) return &voices[i];
    return 0;
}

static void audio_update_hud(int force_clear) {
    static uint64_t last = ~0ULL;
    voice_t *m = music_voice();
    char text[96];
    uint64_t left;
    if (force_clear || !m) {
        if (last != ~0ULL) console_clear_overlay_top_right();
        last = ~0ULL;
        return;
    }
    left = (m->frames - (m->pos >> 16)) / (m->rate ? m->rate : 1);
    if (left == last) return;
    last = left;
    text[0] = 0;
    append_text(text, "playing ", sizeof(text));
    append_text(text, music_name, sizeof(text));
    append_text(text, " ", sizeof(text));
    {
        char num[24];
        uint64_t pos = sizeof(num) - 1;
        num[pos] = 0;
        do { num[--pos] = (char)('0' + left % 10U); left /= 10U; } while (left && pos > 0);
        append_text(text, &num[pos], sizeof(text));
    }
    append_text(text, "s left", sizeof(text));
    console_set_overlay_top_right(text, CONSOLE_STYLE_ACCENT);
}

int audio_playback_init(void) {
    for (int i = 0; i < MAX_VOICES; i++) voices[i].kind = V_FREE;
    if (!audio_worker_proc) {
        audio_worker_proc = proc_create_kernel(audio_playback_worker);
        if (!audio_worker_proc) return -1;
    }
    return 0;
}

int audio_playback_play_wav(vfs_node_t *cwd, const char *path) {
    voice_t *v;
    const char *base = path;
    (void)cwd;
    if (!path || !*path || !hda_available()) return -1;
    audio_playback_stop();
    v = voice_slot();
    if (!v || voice_load(v, path, V_MUSIC, 256) != 0) {
        serial_write("audio: wav unreadable or unsupported, refusing\n");
        if (v) v->kind = V_FREE;
        return -1;
    }
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    copy_text(music_name, base, sizeof(music_name));
    music_frames_total = v->frames;
    kick();
    return 0;
}

void audio_playback_stop(void) {
    voice_t *m = music_voice();
    if (m) m->kind = V_FREE;
    audio_update_hud(1);
}

int audio_playback_claim(uint64_t pid, uint64_t *token_out, uint32_t *sample_rate_out) {
    (void)pid;
    (void)token_out;
    (void)sample_rate_out;
    return -1;
}

uint32_t audio_playback_read_chunk(uint64_t token, uint8_t *dst, uint32_t cap) {
    (void)token;
    (void)dst;
    (void)cap;
    return 0;
}

void audio_playback_finish(uint64_t token) {
    (void)token;
}

void audio_playback_tick(void) {
}

int audio_playback_status(audio_playback_status_t *out) {
    voice_t *m = music_voice();
    if (!out) return -1;
    out->active = m ? 1 : 0;
    out->total_seconds = m && m->rate ? (music_frames_total + m->rate - 1) / m->rate : 0;
    out->seconds_left = m && m->rate ? (m->frames - (m->pos >> 16) + m->rate - 1) / m->rate : 0;
    copy_text(out->name, m ? music_name : "", sizeof(out->name));
    return 0;
}

/* ---- effects ------------------------------------------------------------------ */

int audio_effect_play(const char *path, uint32_t volume) {
    voice_t *v;
    if (!path || !*path || !hda_available()) return -1;
    v = voice_slot();
    if (!v) return -1;
    if (voice_load(v, path, V_EFFECT, (int32_t)(volume > 256 ? 256 : volume)) != 0) {
        v->kind = V_FREE;
        return -1;
    }
    kick();
    return 0;
}

/* ---- streams ------------------------------------------------------------------ */

int audio_stream_open(uint32_t rate, uint32_t channels) {
    if (rate < AUDIO_MIN_RATE_HZ || rate > AUDIO_MAX_RATE_HZ || channels < 1 || channels > 2) return -1;
    if (!hda_available()) return -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &streams[i];
        if (s->used) continue;
        s->cap = rate * STREAM_SECONDS;
        s->ring = (int16_t *)kmalloc((size_t)s->cap * channels * sizeof(int16_t));
        if (!s->ring) return -1;
        s->used = 1;
        s->rd = s->wr = s->frac = 0;
        s->rate = rate;
        s->channels = (uint16_t)channels;
        s->step = (uint32_t)(((uint64_t)rate << 16) / MIX_RATE);
        s->paused = 0;
        s->vol = 256;
        return i + 1;
    }
    return -1;
}

static stream_t *stream_get(int id) {
    if (id < 1 || id > MAX_STREAMS || !streams[id - 1].used) return 0;
    return &streams[id - 1];
}

/* copies whole frames of interleaved s16; returns bytes taken */
int64_t audio_stream_write(int id, const int16_t *pcm, uint64_t bytes) {
    stream_t *s = stream_get(id);
    uint64_t frames, space, n;
    if (!s || !pcm) return -1;
    frames = bytes / (s->channels * sizeof(int16_t));
    space = s->cap - (s->wr - s->rd) - 1;
    n = frames < space ? frames : space;
    for (uint64_t f = 0; f < n; f++) {
        uint32_t at = (uint32_t)((s->wr + f) % s->cap);
        for (uint16_t c = 0; c < s->channels; c++) s->ring[at * s->channels + c] = pcm[f * s->channels + c];
    }
    s->wr += n;
    if (n) kick();
    return (int64_t)(n * s->channels * sizeof(int16_t));
}

/* frames played so far (for A/V sync) */
int64_t audio_stream_position(int id) {
    stream_t *s = stream_get(id);
    return s ? (int64_t)s->rd : -1;
}

int64_t audio_stream_queued(int id) {
    stream_t *s = stream_get(id);
    return s ? (int64_t)(s->wr - s->rd) : -1;
}

int audio_stream_control(int id, int paused, uint32_t volume) {
    stream_t *s = stream_get(id);
    if (!s) return -1;
    s->paused = paused;
    s->vol = (int32_t)(volume > 256 ? 256 : volume);
    if (!paused) kick();
    return 0;
}

void audio_stream_close(int id) {
    stream_t *s = stream_get(id);
    if (!s) return;
    kfree(s->ring);
    s->ring = 0;
    s->used = 0;
}
