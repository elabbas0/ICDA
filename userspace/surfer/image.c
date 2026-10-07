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
    if (image_is_svg(data, len)) return image_decode_svg((const char *)data, len, 2.0f, 0, out);
    if (image_is_webp(data, len)) return image_decode_webp(data, len, out);
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

/* ---- SVG ------------------------------------------------------------------ */

/* nanosvg only uses sscanf for "#rrggbb" and "#rgb" colours */
static int nsvg_scan_hex(const char *s, const char *fmt, unsigned int *r, unsigned int *g, unsigned int *b) {
    int digits = fmt[2] == '2' ? 2 : 1;
    unsigned int v[3];
    if (*s++ != '#') return 0;
    for (int k = 0; k < 3; k++) {
        v[k] = 0;
        for (int d = 0; d < digits; d++) {
            char c = *s++;
            int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (x < 0) return k;
            v[k] = v[k] * 16 + (unsigned int)x;
        }
    }
    *r = v[0];
    *g = v[1];
    *b = v[2];
    return 3;
}
#define sscanf nsvg_scan_hex
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg.h"
#include "third_party/nanosvgrast.h"
#undef sscanf

#define SVG_MAX_SIDE 1024

int image_is_svg(const uint8_t *data, size_t len) {
    size_t i = 0, lim = len < 1024 ? len : 1024;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i = 3;
    while (i < lim && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
    if (i >= lim || data[i] != '<') return 0;
    for (; i + 4 <= lim; i++)
        if (data[i] == '<' && data[i + 1] == 's' && data[i + 2] == 'v' && data[i + 3] == 'g') return 1;
    return 0;
}

int image_decode_svg(const char *text, size_t len, float scale, uint32_t color, image_t *out) {
    char *buf, hex[8];
    size_t o = 0;
    NSVGimage *img;
    NSVGrasterizer *rast;
    unsigned char *rgba;
    float w, h;
    int iw, ih;

    memset(out, 0, sizeof(*out));
    if (!text || !len || len > 8u * 1024 * 1024) return -1;
    buf = (char *)malloc(len + 1);
    if (!buf) return -1;
    hex[0] = '#';
    for (int k = 0; k < 6; k++) hex[1 + k] = "0123456789abcdef"[(color >> (20 - 4 * k)) & 15];
    hex[7] = 0;
    for (size_t i = 0; i < len; i++) {
        /* currentColor -> the text colour (same or shorter, fits in place) */
        if ((text[i] == 'c' || text[i] == 'C') && i + 12 <= len && strncmp(text + i + 1, "urrent", 6) == 0 &&
            (text[i + 7] == 'C' || text[i + 7] == 'c') && strncmp(text + i + 8, "olor", 4) == 0) {
            memcpy(buf + o, hex, 7);
            o += 7;
            i += 11;
            continue;
        }
        buf[o++] = text[i];
    }
    buf[o] = 0;
    img = nsvgParse(buf, "px", 96.0f);
    free(buf);
    if (!img) return -1;
    w = img->width > 0 ? img->width : 24;
    h = img->height > 0 ? img->height : 24;
    if (scale <= 0) scale = 1;
    while ((w * scale > SVG_MAX_SIDE || h * scale > SVG_MAX_SIDE) && scale > 0.05f) scale *= 0.5f;
    iw = (int)(w * scale + 0.5f);
    ih = (int)(h * scale + 0.5f);
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    rast = nsvgCreateRasterizer();
    rgba = (unsigned char *)calloc((size_t)iw * (size_t)ih, 4);
    if (!rast || !rgba) {
        if (rast) nsvgDeleteRasterizer(rast);
        free(rgba);
        nsvgDelete(img);
        return -1;
    }
    nsvgRasterize(rast, img, 0, 0, scale, rgba, iw, ih, iw * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    out->argb = (uint32_t *)malloc((size_t)iw * (size_t)ih * 4);
    if (!out->argb) {
        free(rgba);
        return -1;
    }
    out->w = iw;
    out->h = ih;
    for (size_t i = 0; i < (size_t)iw * (size_t)ih; i++) {
        const unsigned char *p = rgba + i * 4;
        out->argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    free(rgba);
    return 0;
}

/* ---- WebP (libwebp, BSD) and animated GIF / WebP ------------------------------- */

#include "src/webp/decode.h"
#include "src/webp/demux.h"

int image_is_webp(const uint8_t *data, size_t len) {
    return len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4);
}

/* lossy and lossless; BGRA bytes are ARGB words on x86 */
int image_decode_webp(const uint8_t *data, size_t len, image_t *out) {
    WebPDecoderConfig cfg;
    int f = 1;
    memset(out, 0, sizeof(*out));
    if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(data, len, &cfg.input) != VP8_STATUS_OK) return -1;
    while (cfg.input.width / f > IMAGE_MAX_SIDE || cfg.input.height / f > IMAGE_MAX_SIDE) f++;
    out->w = cfg.input.width / f > 0 ? cfg.input.width / f : 1;
    out->h = cfg.input.height / f > 0 ? cfg.input.height / f : 1;
    if (f > 1) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = out->w;
        cfg.options.scaled_height = out->h;
    }
    out->argb = malloc((size_t)out->w * (size_t)out->h * 4);
    if (!out->argb) return -1;
    cfg.output.colorspace = MODE_BGRA;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = (uint8_t *)out->argb;
    cfg.output.u.RGBA.stride = out->w * 4;
    cfg.output.u.RGBA.size = (size_t)out->w * (size_t)out->h * 4;
    if (WebPDecode(data, len, &cfg) != VP8_STATUS_OK) {
        free(out->argb);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

/* animated WebP: every frame composited on the canvas */
static int webp_frames(const uint8_t *data, size_t len, image_t **frames, int **delays_ms, int *count) {
    WebPData wd = { data, len };
    WebPAnimDecoderOptions opt;
    WebPAnimDecoder *dec;
    WebPAnimInfo info;
    int prev_ts = 0, cap = 0;
    if (!WebPAnimDecoderOptionsInit(&opt)) return -1;
    opt.color_mode = MODE_BGRA;
    dec = WebPAnimDecoderNew(&wd, &opt);
    if (!dec) return -1;
    if (!WebPAnimDecoderGetInfo(dec, &info) || info.frame_count < 2 ||
        (int64_t)info.canvas_width * info.canvas_height * info.frame_count > 200LL * 1000 * 1000) {
        WebPAnimDecoderDelete(dec);
        return -1;
    }
    cap = (int)info.frame_count;
    *frames = (image_t *)calloc((size_t)cap, sizeof(image_t));
    *delays_ms = (int *)calloc((size_t)cap, sizeof(int));
    while (*frames && *delays_ms && *count < cap && WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t *buf;
        int ts;
        image_t *im = &(*frames)[*count];
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
        im->w = (int)info.canvas_width;
        im->h = (int)info.canvas_height;
        im->argb = malloc((size_t)im->w * (size_t)im->h * 4);
        if (!im->argb) break;
        memcpy(im->argb, buf, (size_t)im->w * (size_t)im->h * 4);
        (*delays_ms)[*count] = ts - prev_ts > 10 ? ts - prev_ts : 100;
        prev_ts = ts;
        (*count)++;
    }
    WebPAnimDecoderDelete(dec);
    if (*count > 1) return 0;
    for (int i = 0; i < *count; i++) image_release(&(*frames)[i]);
    free(*frames);
    free(*delays_ms);
    *frames = 0;
    *delays_ms = 0;
    *count = 0;
    return -1;
}

int image_decode_gif_frames(const uint8_t *data, size_t len, image_t **frames, int **delays_ms, int *count) {
    int w, h, z, comp, *delays = 0;
    unsigned char *all;
    *frames = 0;
    *delays_ms = 0;
    *count = 0;
    if (image_is_webp(data, len)) return webp_frames(data, len, frames, delays_ms, count);
    if (len < 6 || memcmp(data, "GIF", 3) != 0) return -1;
    all = stbi_load_gif_from_memory(data, (int)len, &delays, &w, &h, &z, &comp, 4);
    if (!all || z <= 0) return -1;
    if ((int64_t)w * h * z > 200LL * 1000 * 1000) {
        stbi_image_free(all);
        free(delays);
        return -1;
    }
    *frames = (image_t *)calloc((size_t)z, sizeof(image_t));
    *delays_ms = (int *)calloc((size_t)z, sizeof(int));
    if (!*frames || !*delays_ms) {
        free(*frames);
        free(*delays_ms);
        stbi_image_free(all);
        free(delays);
        return -1;
    }
    for (int k = 0; k < z; k++) {
        const unsigned char *src = all + (size_t)k * (size_t)w * (size_t)h * 4;
        image_t *im = &(*frames)[k];
        im->w = w;
        im->h = h;
        im->argb = malloc((size_t)w * (size_t)h * 4);
        if (!im->argb) break;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            im->argb[i] = ((uint32_t)src[i * 4 + 3] << 24) | ((uint32_t)src[i * 4] << 16) |
                          ((uint32_t)src[i * 4 + 1] << 8) | src[i * 4 + 2];
        (*delays_ms)[k] = delays && delays[k] > 10 ? delays[k] : 100;
        *count = k + 1;
    }
    stbi_image_free(all);
    free(delays);
    return *count > 0 ? 0 : -1;
}
