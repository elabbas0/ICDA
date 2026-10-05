#include "image.h"
#include <stdlib.h>
#include <string.h>

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_SIMD
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define IMAGE_MAX_SIDE 2048

int image_decode(const uint8_t *data, size_t len, image_t *out) {
    int w, h, n, f = 1;
    unsigned char *rgba;
    memset(out, 0, sizeof(*out));
    if (!data || len == 0 || len > 0x7FFFFFFF) return -1;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &n) || w <= 0 || h <= 0) return -1;
    if ((int64_t)w * h > 40LL * 1000 * 1000) return -1;
    rgba = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    if (!rgba) return -1;
    while (w / f > IMAGE_MAX_SIDE || h / f > IMAGE_MAX_SIDE) f++;
    out->w = w / f > 0 ? w / f : 1;
    out->h = h / f > 0 ? h / f : 1;
    out->argb = malloc((size_t)out->w * (size_t)out->h * 4);
    if (!out->argb) { stbi_image_free(rgba); return -1; }
    for (int y = 0; y < out->h; y++) {
        const unsigned char *src = rgba + (size_t)(y * f) * (size_t)w * 4;
        uint32_t *dst = out->argb + (size_t)y * (size_t)out->w;
        for (int x = 0; x < out->w; x++) {
            const unsigned char *p = src + (size_t)(x * f) * 4;
            dst[x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    stbi_image_free(rgba);
    return 0;
}

void image_release(image_t *img) {
    if (!img) return;
    free(img->argb);
    img->argb = 0;
    img->w = img->h = 0;
}
