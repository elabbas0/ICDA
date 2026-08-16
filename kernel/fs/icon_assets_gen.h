#ifndef ICON_ASSETS_GEN_H
#define ICON_ASSETS_GEN_H

#include <stdint.h>

typedef struct {
    const char *path;
    const char *data;
    const char *data_end;
} generated_icon_asset_t;

extern const generated_icon_asset_t generated_icon_assets[];
extern const uint64_t generated_icon_asset_count;

#endif
