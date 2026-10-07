#ifndef SURFER_IMAGE_H
#define SURFER_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include "paint.h"

/* Decodes PNG / JPEG / GIF / BMP (stb_image) or SVG (nanosvg) into ARGB;
 * very large images are downsampled so a page cannot exhaust memory.
 * Returns 0 on success. */
int  image_decode(const uint8_t *data, size_t len, image_t *out);
void image_release(image_t *img);

/* SVG markup rendered at its own size times scale; "currentColor" becomes
 * color (0xRRGGBB), as for an inline <svg> icon in coloured text. */
int  image_decode_svg(const char *text, size_t len, float scale, uint32_t color, image_t *out);
int  image_is_svg(const uint8_t *data, size_t len);

int  image_is_webp(const uint8_t *data, size_t len);
int  image_decode_webp(const uint8_t *data, size_t len, image_t *out);
/* every frame of an animated GIF or WebP and its delay in ms; caller frees */
int  image_decode_gif_frames(const uint8_t *data, size_t len, image_t **frames, int **delays_ms, int *count);

#endif
