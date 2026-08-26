/*
 * audiod - the ICDA audio daemon (user space).
 *
 * Owns everything about audio EXCEPT the DMA device: WAV parsing,
 * rate conversion, track naming and playback policy.  The kernel
 * exposes only a PCM device (claim / read_chunk / finish) and this
 * daemon is its sole producer.
 *
 * Streams straight off the VFS with positioned reads - no giant
 * buffers, a song never touches memory as a whole.
 *
 * Interface (well-known queue "/audiod", 64-byte messages):
 *   'P' + path  play that file (aborts the current one)
 *   'S'         stop playback
 */
#include "icda_sys.h"
#include "gui_proto.h"

#define HDR_CAP        65536U
#define WIN_FRAMES     8192U
#define OUT_CHUNK      16384U
#define OUT_RATE       48000U

static uint8_t  hdr_buf[HDR_CAP];
static uint8_t  win_buf[WIN_FRAMES * 4U];
static uint8_t  out_chunk[OUT_CHUNK];

static uint32_t src_rate;
static uint32_t src_channels;
static uint32_t src_bits;
static uint64_t data_off;
static uint32_t data_len;
static char     cur_path[128];
static char     pending_name[64];

static uint64_t out_total;
static uint64_t out_pos;
static uint32_t win_start;      /* source frame of window start */
static uint32_t win_frames;     /* frames actually read into window */
static uint64_t token;
static int      streaming;
static int      claimed;

static uint64_t s_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void s_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void dbg(const char *stage, int64_t v) {
    (void)stage;
    (void)v;
    /* no file logging in production - keeps the root directory clean
     * and avoids a VFS write on every state transition. */
}

static const char *basename_of(const char *path) {
    const char *base = path;
    if (path) {
        for (uint64_t i = 0; path[i]; i++) {
            if (path[i] == '/') base = &path[i + 1];
        }
    }
    return base;
}

/* Minimal RIFF/WAVE reader over the header buffer. */
static int wav_parse(void) {
    uint8_t *data = hdr_buf;
    uint64_t pos = 12;
    uint32_t found_rate = 0, found_channels = 0, found_bits = 0;

    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return -1;
    if (data[8] != 'W' || data[9] != 'A' || data[10] != 'V' || data[11] != 'E') return -1;

    while (pos + 8 <= HDR_CAP) {
        const uint8_t *id = data + pos;
        uint32_t sz = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) |
                      ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
        pos += 8;
        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ' && pos + 16 <= HDR_CAP) {
            uint16_t fmt = (uint16_t)(data[pos] | (data[pos + 1] << 8));
            found_channels = (uint32_t)(data[pos + 2] | (data[pos + 3] << 8));
            found_rate = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) |
                         ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
            found_bits = (uint32_t)(data[pos + 14] | (data[pos + 15] << 8));
            if (fmt != 1) return -1;
            if (found_channels < 1 || found_channels > 2) return -1;
            if (found_bits != 8 && found_bits != 16) return -1;
            if (found_rate == 0) return -1;
        } else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            data_off = pos;
            data_len = sz;
            break;
        }
        pos += sz + (sz & 1U);
    }

    if (!data_len || !found_rate || !found_channels || !found_bits) return -1;
    src_rate = found_rate;
    src_channels = found_channels;
    src_bits = found_bits;
    return 0;
}

static uint32_t bytes_per_frame(void) {
    return (src_bits / 8U) * src_channels;
}

static int16_t win_sample(uint32_t frame) {
    uint32_t idx = frame - win_start;
    uint32_t bytes_per = (src_bits / 8U) * src_channels;
    const uint8_t *p = win_buf + (uint64_t)idx * bytes_per;
    int32_t acc = 0;
    for (uint32_t ch = 0; ch < src_channels; ch++) {
        const uint8_t *q = p + ch * (src_bits / 8U);
        if (src_bits == 16) {
            acc += (int16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8));
        } else {
            acc += (int16_t)(((int32_t)q[0] - 128) << 8);
        }
    }
    return (int16_t)(acc / (int32_t)src_channels);
}

/* Make sure source frames [frame, frame + needed) are in the window. */
static int window_fill(uint32_t frame, uint32_t needed) {
    if (frame >= win_start && frame + needed <= win_start + win_frames) return 0;
    win_start = frame;
    {
        uint64_t bytes = (uint64_t)WIN_FRAMES * ((src_bits / 8U) * src_channels);
        uint64_t off = data_off + (uint64_t)frame * bytes_per_frame();
        int64_t n = (int64_t)icda_read_file_at(cur_path, off,
                                               (char *)win_buf, (uint32_t)bytes);
        if (n < 0) return -1;
        win_frames = (uint32_t)((uint64_t)n / bytes_per_frame());
    }
    if (win_frames == 0) return -1;
    return 0;
}

static uint32_t generate_chunk(void) {
    uint64_t frames_total = out_total / 4U;
    uint64_t done = out_pos / 4U;
    uint64_t left = frames_total - done;
    uint32_t frames = OUT_CHUNK / 4U;
    int16_t *out = (int16_t *)out_chunk;
    uint32_t src_needed;

    if (left < frames) frames = (uint32_t)left;
    if (frames == 0) return 0;

    src_needed = (uint32_t)(((done + frames) * (uint64_t)src_rate) / OUT_RATE) -
                 (uint32_t)((done * (uint64_t)src_rate) / OUT_RATE) + 1U;
    if (window_fill((uint32_t)((done * (uint64_t)src_rate) / OUT_RATE), src_needed) != 0) {
        return 0;
    }

    for (uint32_t i = 0; i < frames; i++) {
        uint64_t of = done + i;
        uint32_t sf = (uint32_t)((of * (uint64_t)src_rate) / OUT_RATE);
        int16_t s = win_sample(sf);
        out[i * 2U] = s;
        out[i * 2U + 1U] = s;
    }
    return frames * 4U;
}

static void start_play(const char *path) {
    int64_t n;

    s_copy(cur_path, path, sizeof(cur_path));
    n = (int64_t)icda_read_file(cur_path, (char *)hdr_buf, HDR_CAP);
    dbg("hdr", n);
    if (n <= 44) return;
    if (wav_parse() != 0) {
        dbg("parse-fail", 0);
        return;
    }
    {
        uint64_t frames_total = (uint64_t)data_len / bytes_per_frame();
        out_total = (((uint64_t)frames_total * OUT_RATE + src_rate - 1U) / src_rate) * 4U;
    }
    if (out_total == 0) return;

    icda_stop_audio();
    s_copy(pending_name, basename_of(path), sizeof(pending_name));

    claimed = 0;
    streaming = 1;
    out_pos = 0;
    win_frames = 0;
    win_start = 0;
    dbg("stream-start", (int64_t)out_total);
}

int main(void) {
    uint64_t q = 0;

    dbg("spawned", 0);
    for (int i = 0; i < 100 && !q; i++) {
        q = icda_msg_open("/audiod");
        if (!q) {
            dbg("open-retry", (int64_t)i);
            icda_sleep(10);
        }
    }
    if (!q) return -1;
    dbg("up", (int64_t)q);

    for (;;) {
        gui_msg_t m;

        while (icda_msg_poll(q) > 0) {
            if (icda_msg_recv(q, &m, 0) != 0) break;
            {
                uint8_t cmd = ((uint8_t *)&m)[0];
                if (cmd == 'P') {
                    char path[128];
                    s_copy(path, (const char *)(((uint8_t *)&m) + 4), sizeof(path));
                    if (path[0]) start_play(path);
                } else if (cmd == 'S') {
                    streaming = 0;
                    claimed = 0;
                    icda_stop_audio();
                }
            }
        }

        if (streaming) {
            if (!claimed) {
                uint64_t rate = 0;
                if (icda_audio_claim(&token, &rate) != 0) {
                    dbg("claim-fail", 0);
                    streaming = 0;
                } else {
                    icda_audio_tag(pending_name);
                    claimed = 1;
                }
            }
            if (streaming && claimed) {
                if (out_pos >= out_total) {
                    icda_audio_finish(token);
                    streaming = 0;
                    claimed = 0;
                } else {
                    uint32_t gen = generate_chunk();
                    if (gen == 0) {
                        icda_audio_finish(token);
                        streaming = 0;
                        claimed = 0;
                    } else {
                        uint32_t acc = icda_audio_read_chunk(token, out_chunk, gen);
                        if (acc == (uint32_t)-1) {
                            streaming = 0;
                            claimed = 0;
                        } else if (acc == 0) {
                            icda_sleep(1);
                        } else {
                            out_pos += acc;
                        }
                    }
                }
            }
        }

        icda_sleep(1);
    }
}
