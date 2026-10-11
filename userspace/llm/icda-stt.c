/* icda-stt - the assistant's ears (a Linux program, run by ICDA's Linux
 * personality; built against whisper.cpp by userspace/llm/build-stt.sh).
 *
 *   icda-stt <model.bin> <recording.wav> <text out>
 *
 * Transcribes a 16 kHz mono 16-bit WAV with a Whisper model (ggml-*.bin) and
 * writes the words to <text out> - written under another name and renamed
 * into place, so the file appears only when whole. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "whisper.h"

int main(int argc, char **argv) {
    FILE *f;
    long size;
    unsigned char *wav;
    const unsigned char *pcm = 0;
    unsigned data = 0, off = 12;
    float *samples;
    int n;
    struct whisper_context *ctx;
    struct whisper_full_params p;
    char part[512];
    if (argc < 4) {
        fprintf(stderr, "usage: icda-stt <model.bin> <recording.wav> <text out>\n");
        return 1;
    }
    f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    wav = malloc((size_t)size);
    if (!wav || fread(wav, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);
    while (off + 8 <= (unsigned)size) {               /* the "data" chunk */
        unsigned len = wav[off + 4] | wav[off + 5] << 8 | wav[off + 6] << 16 | (unsigned)wav[off + 7] << 24;
        if (!memcmp(wav + off, "data", 4)) {
            pcm = wav + off + 8;
            data = len <= (unsigned)size - off - 8 ? len : (unsigned)size - off - 8;
            break;
        }
        off += 8 + len + (len & 1);
    }
    if (!pcm) return 1;
    n = (int)(data / 2);
    samples = malloc(sizeof(float) * (size_t)n);
    if (!samples) return 1;
    for (int i = 0; i < n; i++) samples[i] = (float)(short)(pcm[2 * i] | pcm[2 * i + 1] << 8) / 32768.0f;

    ctx = whisper_init_from_file_with_params(argv[1], whisper_context_default_params());
    if (!ctx) return 1;
    p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = 4;
    p.language = "en";
    p.print_progress = 0;
    p.print_realtime = 0;
    p.print_timestamps = 0;
    p.print_special = 0;
    p.no_timestamps = 1;
    p.single_segment = 1;
    if (whisper_full(ctx, p, samples, n) != 0) return 1;
    snprintf(part, sizeof(part), "%s.part", argv[3]);
    f = fopen(part, "wb");
    if (!f) return 1;
    for (int i = 0; i < whisper_full_n_segments(ctx); i++) fputs(whisper_full_get_segment_text(ctx, i), f);
    fclose(f);
    whisper_free(ctx);
    return rename(part, argv[3]) == 0 ? 0 : 1;
}
