#ifndef AUDIO_ASSETS_GEN_H
#define AUDIO_ASSETS_GEN_H

#include <stdint.h>

typedef struct {
    const char *path;
    const char *data;
    const char *data_end;
} generated_audio_asset_t;

extern const generated_audio_asset_t generated_audio_assets[];
extern const uint64_t generated_audio_asset_count;

#endif
