#include "icda_sys.h"

#include <stdint.h>

#define AUDIOPLAY_REQUEST_PATH "/home/.audio.request"
#define AUDIOPLAY_PATH_CAP 128

static char audioplay_path[AUDIOPLAY_PATH_CAP];

uint64_t audioplay_main(void) {
    uint64_t path_len;

    path_len = icda_read_file(AUDIOPLAY_REQUEST_PATH, audioplay_path, sizeof(audioplay_path));
    if ((long)path_len <= 0) {
        icda_exit(1);
    }

    if (path_len >= sizeof(audioplay_path)) {
        path_len = sizeof(audioplay_path) - 1U;
    }
    audioplay_path[path_len] = 0;

    if ((long)icda_play_audio_file(audioplay_path) < 0) {
        icda_exit(1);
    }

    icda_exit(0);
    return 0;
}
