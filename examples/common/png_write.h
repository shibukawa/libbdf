/*
 * A small PNG writer for the examples: an RGBA image, uncompressed (stored
 * deflate blocks), with nothing beyond libc. A real application would hand
 * the pixels to libpng, stb_image_write, a platform's image codecs or its
 * toolkit; this keeps the examples free of a dependency.
 */
#ifndef BDF_EXAMPLES_PNG_WRITE_H
#define BDF_EXAMPLES_PNG_WRITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * png_write_rgba writes width × height pixels of 4 bytes (red, green, blue,
 * alpha) to path. premultiplied says the colours are multiplied by the
 * alpha, as libbdf draws them; they are divided out, as PNG wants them.
 * It returns 0, or -1 when the file cannot be written.
 */
int png_write_rgba(const char *path, const uint8_t *pixels, int width, int height,
                   int premultiplied);

#ifdef __cplusplus
}
#endif

#endif
