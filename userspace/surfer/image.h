#ifndef SURFER_IMAGE_H
#define SURFER_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include "paint.h"

/* Decodes PNG / JPEG / GIF / BMP into ARGB (stb_image); very large images
 * are downsampled so a page cannot exhaust memory.  Returns 0 on success. */
int  image_decode(const uint8_t *data, size_t len, image_t *out);
void image_release(image_t *img);

#endif
