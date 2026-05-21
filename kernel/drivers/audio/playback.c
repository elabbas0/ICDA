#include "playback.h"

#include "hda.h"
#include "../console/console.h"
#include "../../proc/sched.h"

#define AUDIO_OUTPUT_CHANNELS 2U
#define AUDIO_OUTPUT_BITS     16U
#define AUDIO_OUTPUT_BYTES    4U
#define AUDIO_OUTPUT_RATE     48000U

typedef struct {
    const uint8_t *source_pcm;
    uint32_t source_frames;
    uint16_t source_channels;
    uint16_t source_bits;
    uint32_t source_rate;
    uint32_t pcm_len;
    uint32_t played_len;
    uint32_t sample_rate;
    uint32_t total_seconds;
    uint32_t active;
    uint32_t hud_seconds_last;
    uint64_t start_tick;
    uint64_t filled_len;
    uint32_t dma_buffer_len;
    uint32_t request_pending;
    char name[64];
    char pending_path[128];
} audio_playback_state_t;

static audio_playback_state_t audio_state;
static uint8_t audio_fill_buf[4096];
static process_t *audio_worker_proc = 0;

static void audio_update_hud(int force_clear);
static int audio_start_pending_request(void);

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
    while (src && src[i] && out + 1 < cap) {
        dst[out++] = src[i++];
    }
    dst[out] = 0;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int parse_wav(const uint8_t *buf, uint64_t size,
                     uint16_t *channels_out, uint32_t *rate_out,
                     uint16_t *bits_out, const uint8_t **data_out,
                     uint32_t *data_size_out) {
    uint64_t off = 12;
    uint16_t fmt_tag = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    const uint8_t *data = 0;
    uint32_t data_size = 0;
    int have_fmt = 0;

    if (!buf || size < 44) return -1;
    if (!(buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F')) return -1;
    if (!(buf[8] == 'W' && buf[9] == 'A' && buf[10] == 'V' && buf[11] == 'E')) return -1;

    while (off + 8 <= size) {
        const uint8_t *chunk = &buf[off];
        uint32_t chunk_size = read_le32(chunk + 4);
        uint64_t next = off + 8ULL + chunk_size + (chunk_size & 1U);
        if (next > size) break;

        if (chunk[0] == 'f' && chunk[1] == 'm' && chunk[2] == 't' && chunk[3] == ' ') {
            if (chunk_size < 16) return -1;
            fmt_tag = read_le16(chunk + 8);
            channels = read_le16(chunk + 10);
            rate = read_le32(chunk + 12);
            bits = read_le16(chunk + 22);
            have_fmt = 1;
        } else if (chunk[0] == 'd' && chunk[1] == 'a' && chunk[2] == 't' && chunk[3] == 'a') {
            data = chunk + 8;
            data_size = chunk_size;
        }
        off = next;
    }

    if (!have_fmt || !data || fmt_tag != 1 || channels == 0 || rate == 0) return -1;
    if (!(bits == 8 || bits == 16)) return -1;

    *channels_out = channels;
    *rate_out = rate;
    *bits_out = bits;
    *data_out = data;
    *data_size_out = data_size;
    return 0;
}

static int16_t sample_at(const uint8_t *data, uint32_t frame_index, uint16_t channels, uint16_t bits) {
    uint32_t sample_index = frame_index * channels;

    if (bits == 8) {
        int32_t sum = 0;
        for (uint16_t ch = 0; ch < channels; ch++) {
            sum += ((int32_t)data[sample_index + ch] - 128) << 8;
        }
        return (int16_t)(sum / (int32_t)channels);
    }

    {
        int32_t sum = 0;
        const uint8_t *p = data + (sample_index * 2U);
        for (uint16_t ch = 0; ch < channels; ch++) {
            int16_t s = (int16_t)read_le16(p + (ch * 2U));
            sum += s;
        }
        return (int16_t)(sum / (int32_t)channels);
    }
}

static void basename_from_path(const char *path, char *out, uint64_t cap) {
    const char *base = path;
    uint64_t i;

    if (!path || !out || cap == 0) return;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/') base = &path[i + 1];
    }
    copy_text(out, base, cap);
}

static void audio_clear_state(void) {
    audio_state.source_pcm = 0;
    audio_state.source_frames = 0;
    audio_state.source_channels = 0;
    audio_state.source_bits = 0;
    audio_state.source_rate = 0;
    audio_state.pcm_len = 0;
    audio_state.played_len = 0;
    audio_state.sample_rate = 0;
    audio_state.total_seconds = 0;
    audio_state.active = 0;
    audio_state.start_tick = 0;
    audio_state.filled_len = 0;
    audio_state.dma_buffer_len = 0;
    audio_state.name[0] = 0;
}

static void audio_generate_range(uint64_t start_byte, uint8_t *dst, uint32_t length) {
    uint32_t frames = length / AUDIO_OUTPUT_BYTES;
    int16_t *samples = (int16_t *)dst;

    for (uint32_t i = 0; i < frames; i++) {
        uint64_t out_frame = (start_byte / AUDIO_OUTPUT_BYTES) + i;
        uint32_t src_frame = (uint32_t)((out_frame * (uint64_t)audio_state.source_rate) / (uint64_t)audio_state.sample_rate);
        int16_t sample;

        if (src_frame >= audio_state.source_frames) {
            src_frame = audio_state.source_frames - 1U;
        }
        sample = sample_at(audio_state.source_pcm, src_frame, audio_state.source_channels, audio_state.source_bits);
        samples[i * 2U] = sample;
        samples[i * 2U + 1U] = sample;
    }
}

static int audio_fill_available(void) {
    uint64_t elapsed_ticks;
    uint64_t consumed_target;
    uint64_t fill_target;
    uint64_t bytes_per_second;

    if (!audio_state.active || !audio_state.source_pcm || !audio_state.sample_rate || !audio_state.dma_buffer_len) {
        return -1;
    }

    bytes_per_second = (uint64_t)audio_state.sample_rate * AUDIO_OUTPUT_BYTES;
    elapsed_ticks = sched_ticks() - audio_state.start_tick;
    consumed_target = (elapsed_ticks * bytes_per_second) / 100ULL;
    if (consumed_target > audio_state.pcm_len) {
        consumed_target = audio_state.pcm_len;
    }
    audio_state.played_len = (uint32_t)consumed_target;

    fill_target = consumed_target + audio_state.dma_buffer_len;
    if (fill_target > (uint64_t)audio_state.pcm_len + audio_state.dma_buffer_len) {
        fill_target = (uint64_t)audio_state.pcm_len + audio_state.dma_buffer_len;
    }

    while (audio_state.filled_len < fill_target) {
        uint32_t offset = (uint32_t)(audio_state.filled_len % audio_state.dma_buffer_len);
        uint32_t span = audio_state.dma_buffer_len - offset;
        uint64_t remaining_fill = fill_target - audio_state.filled_len;
        uint32_t chunk = remaining_fill < span ? (uint32_t)remaining_fill : span;
        uint64_t audio_remaining = (audio_state.filled_len < audio_state.pcm_len)
            ? ((uint64_t)audio_state.pcm_len - audio_state.filled_len)
            : 0;

        chunk &= ~(AUDIO_OUTPUT_BYTES - 1U);
        if (chunk == 0) {
            break;
        }
        if (chunk > (uint32_t)sizeof(audio_fill_buf)) {
            chunk = (uint32_t)sizeof(audio_fill_buf);
        }
        chunk &= ~(AUDIO_OUTPUT_BYTES - 1U);
        if (chunk == 0) {
            return -1;
        }

        if (audio_remaining > 0) {
            uint32_t audio_chunk = audio_remaining < chunk ? (uint32_t)audio_remaining : chunk;
            audio_chunk &= ~(AUDIO_OUTPUT_BYTES - 1U);
            if (audio_chunk > 0) {
                audio_generate_range(audio_state.filled_len, audio_fill_buf, audio_chunk);
            }
            for (uint32_t i = audio_chunk; i + 1U < chunk; i += 2U) {
                audio_fill_buf[i] = 0;
                audio_fill_buf[i + 1U] = 0;
            }
        } else {
            for (uint32_t i = 0; i + 1U < chunk; i += 2U) {
                audio_fill_buf[i] = 0;
                audio_fill_buf[i + 1U] = 0;
            }
        }

        if (hda_stream_write(offset, audio_fill_buf, chunk) != 0) {
            return -1;
        }
        audio_state.filled_len += chunk;
    }

    return 0;
}

static void audio_playback_worker(void) {
    for (;;) {
        if (audio_state.request_pending) {
            if (audio_start_pending_request() != 0) {
                audio_playback_stop();
            }
            sched_sleep(1);
            continue;
        }

        if (audio_state.active) {
            if (audio_fill_available() != 0 || audio_state.played_len >= audio_state.pcm_len) {
                audio_playback_stop();
            } else {
                audio_update_hud(0);
            }
            sched_sleep(4);
            continue;
        }

        sched_sleep(10);
    }
}

static void audio_update_hud(int force_clear) {
    char text[96];
    uint64_t seconds_left;

    if (force_clear || !audio_state.active || !audio_state.sample_rate || !audio_state.pcm_len) {
        console_clear_overlay_top_right();
        audio_state.hud_seconds_last = 0xFFFFFFFFU;
        return;
    }

    seconds_left = (audio_state.pcm_len > audio_state.played_len)
        ? (uint64_t)(audio_state.pcm_len - audio_state.played_len)
        : 0;
    seconds_left = (seconds_left + ((uint64_t)audio_state.sample_rate * AUDIO_OUTPUT_BYTES) - 1U) /
                   ((uint64_t)audio_state.sample_rate * AUDIO_OUTPUT_BYTES);
    if (seconds_left == audio_state.hud_seconds_last) {
        return;
    }
    audio_state.hud_seconds_last = (uint32_t)seconds_left;

    text[0] = 0;
    append_text(text, "playing ", sizeof(text));
    append_text(text, audio_state.name, sizeof(text));
    append_text(text, " ", sizeof(text));
    {
        char num[24];
        uint64_t value = seconds_left;
        uint64_t pos = sizeof(num) - 1;
        uint64_t digits;
        num[pos] = 0;
        if (value == 0) {
            num[--pos] = '0';
        } else {
            while (value && pos > 0) {
                num[--pos] = (char)('0' + (value % 10U));
                value /= 10U;
            }
        }
        digits = str_len(&num[pos]);
        while (digits < 3) {
            append_text(text, " ", sizeof(text));
            digits++;
        }
        append_text(text, &num[pos], sizeof(text));
    }
    append_text(text, "s left", sizeof(text));
    console_set_overlay_top_right(text, CONSOLE_STYLE_ACCENT);
}

int audio_playback_init(void) {
    audio_clear_state();
    audio_state.hud_seconds_last = 0xFFFFFFFFU;
    if (!audio_worker_proc) {
        audio_worker_proc = proc_create_kernel(audio_playback_worker);
        if (!audio_worker_proc) {
            return -1;
        }
    }
    return 0;
}

void audio_playback_stop(void) {
    hda_stop_playback();
    audio_state.request_pending = 0;
    audio_state.pending_path[0] = 0;
    audio_clear_state();
    audio_update_hud(1);
}

static int audio_start_pending_request(void) {
    uint64_t size = 0;
    const uint8_t *file_data;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t sample_rate = 0;
    const uint8_t *pcm = 0;
    uint32_t pcm_size = 0;
    uint32_t total_frames;
    uint32_t out_frames;
    const char *path = audio_state.pending_path;

    if (!path || !*path || !hda_available()) {
        return -1;
    }

    file_data = (const uint8_t *)vfs_read(vfs_root(), path, &size);
    if (!file_data || size == 0) {
        return -1;
    }
    if (parse_wav(file_data, size, &channels, &sample_rate, &bits, &pcm, &pcm_size) != 0) {
        return -1;
    }

    total_frames = pcm_size / (channels * (bits / 8U));
    out_frames = (uint32_t)(((uint64_t)total_frames * AUDIO_OUTPUT_RATE + sample_rate - 1U) / sample_rate);
    if (out_frames == 0) {
        return -1;
    }

    audio_state.source_pcm = pcm;
    audio_state.source_frames = total_frames;
    audio_state.source_channels = channels;
    audio_state.source_bits = bits;
    audio_state.source_rate = sample_rate;
    audio_state.pcm_len = out_frames * AUDIO_OUTPUT_BYTES;
    audio_state.played_len = 0;
    audio_state.sample_rate = AUDIO_OUTPUT_RATE;
    audio_state.total_seconds = (audio_state.pcm_len + ((AUDIO_OUTPUT_RATE * AUDIO_OUTPUT_BYTES) - 1U)) /
                                (AUDIO_OUTPUT_RATE * AUDIO_OUTPUT_BYTES);
    audio_state.active = 1;
    audio_state.request_pending = 0;
    audio_state.filled_len = 0;
    audio_state.dma_buffer_len = 65536U;
    if (audio_state.dma_buffer_len > audio_state.pcm_len && audio_state.pcm_len != 0) {
        audio_state.dma_buffer_len = audio_state.pcm_len;
    }
    if (audio_state.dma_buffer_len < 4096U) {
        audio_state.dma_buffer_len = 4096U;
    }
    audio_state.dma_buffer_len &= ~(AUDIO_OUTPUT_BYTES - 1U);
    basename_from_path(path, audio_state.name, sizeof(audio_state.name));
    audio_state.hud_seconds_last = 0xFFFFFFFFU;

    if (hda_stream_start_s16_stereo((uint16_t)audio_state.sample_rate, audio_state.dma_buffer_len) != 0) {
        audio_playback_stop();
        return -1;
    }
    audio_state.start_tick = sched_ticks();
    if (audio_fill_available() != 0) {
        audio_playback_stop();
        return -1;
    }
    audio_update_hud(0);
    return 0;
}

int audio_playback_play_wav(vfs_node_t *cwd, const char *path) {
    uint64_t i = 0;

    (void)cwd;

    if (!path || !*path || !hda_available()) {
        return -1;
    }

    audio_playback_stop();

    while (path[i] && i + 1 < sizeof(audio_state.pending_path)) {
        audio_state.pending_path[i] = path[i];
        i++;
    }
    if (i == 0 || path[i] != 0) {
        audio_state.pending_path[0] = 0;
        return -1;
    }
    audio_state.pending_path[i] = 0;
    audio_state.request_pending = 0;
    return audio_start_pending_request() == 0 ? 0 : -1;
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
    if (audio_state.active) {
        audio_update_hud(0);
    }
}

int audio_playback_status(audio_playback_status_t *out) {
    uint64_t remaining = 0;

    if (!out) return -1;
    out->active = audio_state.active ? 1 : 0;
    out->total_seconds = audio_state.total_seconds;
    if (audio_state.active && audio_state.sample_rate && audio_state.pcm_len > audio_state.played_len) {
        remaining = (uint64_t)(audio_state.pcm_len - audio_state.played_len);
        out->seconds_left = (remaining + ((uint64_t)audio_state.sample_rate * AUDIO_OUTPUT_BYTES) - 1U) /
                            ((uint64_t)audio_state.sample_rate * AUDIO_OUTPUT_BYTES);
    } else {
        out->seconds_left = 0;
    }
    copy_text(out->name, audio_state.name, sizeof(out->name));
    return 0;
}
