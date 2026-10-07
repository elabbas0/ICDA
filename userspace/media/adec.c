/* Audio decoders: dr_wav, minimp3, dr_flac, stb_vorbis (all public domain
 * or MIT-0) and AAC in MP4 (aac.c + minimp4), behind adec.h. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "adec.h"
#include "aac.h"

#define DR_WAV_NO_STDIO
#include "third_party/dr_wav.h"
#define DR_FLAC_NO_STDIO
#include "third_party/dr_flac.h"
#define MINIMP3_NO_STDIO
#include "third_party/minimp3_ex.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"
#include "third_party/minimp4.h"

enum { K_WAV = 1, K_MP3, K_FLAC, K_OGG, K_M4A, K_ADTS };

struct adec {
    int          kind;
    uint8_t     *data;
    size_t       len;
    adec_info_t  info;
    drwav        wav;
    drflac      *flac;
    mp3dec_ex_t  mp3;
    stb_vorbis  *ogg;
    /* m4a */
    MP4D_demux_t mp4;
    int          track;
    unsigned     sample;           /* next AAC access unit */
    aac_t       *aac;
    int16_t      pcm[2048 * 2];    /* one decoded AAC frame */
    int          pcm_len, pcm_pos; /* frames */
    int          skip;             /* decoded frames still to drop (encoder priming) */
    /* adts: where each frame starts */
    uint32_t    *adts_off;
    unsigned     adts_count;
};

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != suf[i]) return 0;
    }
    return 1;
}

int adec_is_audio_name(const char *n) {
    return ends_with(n, ".wav") || ends_with(n, ".mp3") || ends_with(n, ".flac") || ends_with(n, ".ogg") ||
           ends_with(n, ".oga") || ends_with(n, ".m4a") || ends_with(n, ".aac");
}

static int kind_of(const uint8_t *d, size_t len, const char *hint) {
    if (len >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) return K_WAV;
    if (len >= 4 && !memcmp(d, "fLaC", 4)) return K_FLAC;
    if (len >= 4 && !memcmp(d, "OggS", 4)) return K_OGG;
    if (len >= 8 && !memcmp(d + 4, "ftyp", 4)) return K_M4A;
    if (len >= 7 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) return K_ADTS;
    if (len >= 3 && (!memcmp(d, "ID3", 3) || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))) return K_MP3;
    if (hint && ends_with(hint, ".mp3")) return K_MP3;
    return 0;
}

/* ---- MP4 memory reader for minimp4 ---------------------------------------- */

static int mp4_read(int64_t offset, void *buf, size_t size, void *token) {
    adec_t *d = (adec_t *)token;
    if (offset < 0 || (uint64_t)offset + size > d->len) return 1;
    memcpy(buf, d->data + offset, size);
    return 0;
}

static int open_m4a(adec_t *d) {
    if (!MP4D_open(&d->mp4, mp4_read, d, (int64_t)d->len)) return -1;
    d->track = -1;
    for (unsigned i = 0; i < d->mp4.track_count; i++) {
        MP4D_track_t *t = &d->mp4.track[i];
        if (t->handler_type == MP4D_HANDLER_TYPE_SOUN && t->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3) {
            d->track = (int)i;
            break;
        }
    }
    if (d->track < 0) return -1;
    {
        MP4D_track_t *t = &d->mp4.track[d->track];
        d->aac = aac_open(t->dsi, (int)t->dsi_bytes);
        if (!d->aac) return -1;
        d->info.rate = aac_rate(d->aac);
        d->info.channels = aac_channels(d->aac) > 1 ? 2 : 1;
        d->info.frames = t->sample_count ? (uint64_t)(t->sample_count - 1) * 1024 : 0;
    }
    strcpy(d->info.format, "AAC");
    return 0;
}

static int open_adts(adec_t *d) {
    size_t pos = 0;
    unsigned cap = 0;
    int ri = 0, cc = 0;
    while (pos + 7 <= d->len) {
        int r, c, flen, hlen;
        if (aac_adts_parse(d->data + pos, (int)(d->len - pos), &r, &c, &flen, &hlen) != 0) break;
        if (d->adts_count == cap) {
            uint32_t *n;
            cap = cap ? cap * 2 : 1024;
            n = (uint32_t *)realloc(d->adts_off, cap * sizeof(uint32_t));
            if (!n) return -1;
            d->adts_off = n;
        }
        if (!d->adts_count) {
            ri = r;
            cc = c;
        }
        d->adts_off[d->adts_count++] = (uint32_t)pos;
        pos += (size_t)flen;
    }
    if (d->adts_count < 2) return -1;
    d->aac = aac_open_params(ri, cc);
    if (!d->aac) return -1;
    d->info.rate = aac_rate(d->aac);
    d->info.channels = aac_channels(d->aac);
    d->info.frames = (uint64_t)(d->adts_count - 1) * 1024;
    strcpy(d->info.format, "AAC");
    return 0;
}

/* the next access unit: 0 at the end */
static int next_au(adec_t *d, const uint8_t **p, unsigned *bytes) {
    if (d->kind == K_ADTS) {
        int r, c, flen, hlen;
        uint32_t off;
        if (d->sample >= d->adts_count) return 0;
        off = d->adts_off[d->sample++];
        if (aac_adts_parse(d->data + off, (int)(d->len - off), &r, &c, &flen, &hlen) != 0 || off + (unsigned)flen > d->len)
            return 0;
        *p = d->data + off + hlen;
        *bytes = (unsigned)(flen - hlen);
        return 1;
    } else {
        MP4D_track_t *t = &d->mp4.track[d->track];
        while (d->sample < t->sample_count) {
            unsigned ts = 0, dur = 0;
            MP4D_file_offset_t off = MP4D_frame_offset(&d->mp4, (unsigned)d->track, d->sample++, bytes, &ts, &dur);
            if (!*bytes || off + *bytes > d->len) continue;
            *p = d->data + off;
            return 1;
        }
        return 0;
    }
}

/* decodes the next AAC frame into d->pcm; 0 at the end.  The first frame
 * after opening or seeking only primes the filterbank and is dropped. */
static int m4a_next(adec_t *d) {
    const uint8_t *au;
    unsigned bytes = 0;
    while (next_au(d, &au, &bytes)) {
        int n = aac_decode(d->aac, au, (int)bytes, d->pcm, d->info.channels);
        if (n > 0 && d->skip > 0) {
            d->skip--;
            continue;
        }
        if (n > 0) {
            d->pcm_len = n;
            d->pcm_pos = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- open / read / seek ----------------------------------------------------- */

adec_t *adec_open_memory(uint8_t *data, size_t len, const char *hint) {
    adec_t *d = (adec_t *)calloc(1, sizeof(adec_t));
    int ok = 0;
    if (!d) return 0;
    d->data = data;
    d->len = len;
    d->kind = kind_of(data, len, hint);
    switch (d->kind) {
    case K_WAV:
        if (drwav_init_memory(&d->wav, data, len, 0)) {
            d->info.rate = d->wav.sampleRate;
            d->info.channels = d->wav.channels >= 2 ? 2 : 1;
            d->info.frames = d->wav.totalPCMFrameCount;
            strcpy(d->info.format, "WAV");
            ok = 1;
        }
        break;
    case K_FLAC:
        d->flac = drflac_open_memory(data, len, 0);
        if (d->flac) {
            d->info.rate = d->flac->sampleRate;
            d->info.channels = d->flac->channels >= 2 ? 2 : 1;
            d->info.frames = d->flac->totalPCMFrameCount;
            strcpy(d->info.format, "FLAC");
            ok = 1;
        }
        break;
    case K_MP3:
        if (mp3dec_ex_open_buf(&d->mp3, data, len, MP3D_SEEK_TO_SAMPLE) == 0 && d->mp3.info.hz) {
            d->info.rate = (uint32_t)d->mp3.info.hz;
            d->info.channels = d->mp3.info.channels >= 2 ? 2 : 1;
            d->info.frames = d->mp3.samples / (uint64_t)(d->mp3.info.channels ? d->mp3.info.channels : 1);
            strcpy(d->info.format, "MP3");
            ok = 1;
        }
        break;
    case K_OGG: {
        int err = 0;
        d->ogg = stb_vorbis_open_memory(data, (int)len, &err, 0);
        if (d->ogg) {
            stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
            d->info.rate = vi.sample_rate;
            d->info.channels = vi.channels >= 2 ? 2 : 1;
            d->info.frames = stb_vorbis_stream_length_in_samples(d->ogg);
            strcpy(d->info.format, "Ogg Vorbis");
            ok = 1;
        }
        break;
    }
    case K_M4A:
        ok = open_m4a(d) == 0;
        d->skip = 1;
        break;
    case K_ADTS:
        ok = open_adts(d) == 0;
        d->skip = 1;
        break;
    default:
        break;
    }
    if (!ok || !d->info.rate) {
        adec_close(d);
        return 0;
    }
    return d;
}

const adec_info_t *adec_info(const adec_t *d) {
    return &d->info;
}

/* source channels -> 1 or 2 output channels */
static void downmix(const int16_t *in, int in_ch, int16_t *out, int out_ch, long frames) {
    for (long f = 0; f < frames; f++) {
        if (out_ch == 1) {
            out[f] = in[f * in_ch];
        } else if (in_ch == 1) {
            out[f * 2] = out[f * 2 + 1] = in[f];
        } else {
            out[f * 2] = in[f * in_ch];
            out[f * 2 + 1] = in[f * in_ch + 1];
        }
    }
}

long adec_read(adec_t *d, int16_t *out, long frames) {
    int ch = d->info.channels;
    switch (d->kind) {
    case K_WAV: {
        int src = d->wav.channels;
        if (src == ch) return (long)drwav_read_pcm_frames_s16(&d->wav, (drwav_uint64)frames, out);
        {
            int16_t tmp[1024 * 8];
            long done = 0;
            while (done < frames) {
                long want = frames - done > 1024 ? 1024 : frames - done;
                long got = (long)drwav_read_pcm_frames_s16(&d->wav, (drwav_uint64)want, tmp);
                if (got <= 0) break;
                downmix(tmp, src, out + done * ch, ch, got);
                done += got;
            }
            return done;
        }
    }
    case K_FLAC: {
        int src = d->flac->channels;
        if (src == ch) return (long)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)frames, out);
        {
            int16_t tmp[1024 * 8];
            long done = 0;
            while (done < frames) {
                long want = frames - done > 1024 ? 1024 : frames - done;
                long got = (long)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)want, tmp);
                if (got <= 0) break;
                downmix(tmp, src, out + done * ch, ch, got);
                done += got;
            }
            return done;
        }
    }
    case K_MP3: {
        int src = d->mp3.info.channels;
        if (src == ch) return (long)(mp3dec_ex_read(&d->mp3, out, (size_t)frames * (size_t)ch) / (size_t)ch);
        {
            int16_t tmp[1152 * 2];
            long done = 0;
            while (done < frames) {
                long want = frames - done > 1152 ? 1152 : frames - done;
                long got = (long)(mp3dec_ex_read(&d->mp3, tmp, (size_t)want * (size_t)src) / (size_t)src);
                if (got <= 0) break;
                downmix(tmp, src, out + done * ch, ch, got);
                done += got;
            }
            return done;
        }
    }
    case K_OGG:
        return stb_vorbis_get_samples_short_interleaved(d->ogg, ch, out, (int)(frames * ch));
    case K_M4A:
    case K_ADTS: {
        long done = 0;
        while (done < frames) {
            long take;
            if (d->pcm_pos >= d->pcm_len && !m4a_next(d)) break;
            take = d->pcm_len - d->pcm_pos;
            if (take > frames - done) take = frames - done;
            memcpy(out + done * ch, d->pcm + d->pcm_pos * ch, (size_t)take * (size_t)ch * sizeof(int16_t));
            d->pcm_pos += (int)take;
            done += take;
        }
        return done;
    }
    default:
        return 0;
    }
}

int adec_seek(adec_t *d, uint64_t frame) {
    switch (d->kind) {
    case K_WAV: return drwav_seek_to_pcm_frame(&d->wav, frame) ? 0 : -1;
    case K_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame) ? 0 : -1;
    case K_MP3: return mp3dec_ex_seek(&d->mp3, frame * (uint64_t)d->mp3.info.channels) == 0 ? 0 : -1;
    case K_OGG: return stb_vorbis_seek(d->ogg, (unsigned)frame) ? 0 : -1;
    case K_M4A:
    case K_ADTS:
        d->sample = (unsigned)(frame / 1024);
        d->pcm_len = d->pcm_pos = 0;
        d->skip = 1;
        aac_reset(d->aac);
        return 0;
    default:
        return -1;
    }
}

void adec_close(adec_t *d) {
    if (!d) return;
    switch (d->kind) {
    case K_WAV: drwav_uninit(&d->wav); break;
    case K_FLAC: if (d->flac) drflac_close(d->flac); break;
    case K_MP3: mp3dec_ex_close(&d->mp3); break;
    case K_OGG: if (d->ogg) stb_vorbis_close(d->ogg); break;
    case K_M4A:
        if (d->aac) aac_close(d->aac);
        MP4D_close(&d->mp4);
        break;
    case K_ADTS:
        if (d->aac) aac_close(d->aac);
        free(d->adts_off);
        break;
    default: break;
    }
    free(d->data);
    free(d);
}
