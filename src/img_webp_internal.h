/*
 * What the files of the WebP decoder share: img_webp.c reads the container
 * and converts the pixels, img_vp8.c decodes a lossy frame and img_vp8l.c a
 * lossless image (which is also how the alpha plane of a lossy one is
 * stored).
 */
#ifndef BDFI_WEBP_INTERNAL_H
#define BDFI_WEBP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "image_internal.h"

/*
 * A decoded lossy frame: the *image.YCbCr that Decoder.DecodeFrame of Go's
 * vp8 returns. The planes are whole macroblocks (y_stride and c_stride
 * bytes a row, 4:2:0), of which the image is the top left width x height.
 */
typedef struct bdfi_vp8_frame {
  int32_t width, height;
  uint8_t *buf; /* the three planes, from bdfi_calloc_array: free() releases it */
  const uint8_t *y, *cb, *cr;
  size_t y_stride, c_stride;
} bdfi_vp8_frame;

/*
 * A VP8 chunk is given as its bytes that are in the file (avail of them) and
 * the length the chunk states (n, which is avail or more): Go reads the
 * chunk through a reader limited to n and fails where the file ends before.
 *
 * bdfi_vp8_frame_header is Decoder.DecodeFrameHeader: 1 and the size where
 * it returns no error (0 x 0 for a frame that is not a key frame), else 0.
 */
BDF_INTERNAL int bdfi_vp8_frame_header(const uint8_t *data, size_t avail, int64_t n, int32_t *w,
                                       int32_t *h);

/* bdfi_vp8_decode_frame is DecodeFrameHeader and DecodeFrame. On anything
 * but BDF_OK nothing is left allocated. */
BDF_INTERNAL bdf_status bdfi_vp8_decode_frame(const uint8_t *data, size_t avail, int64_t n,
                                              bdfi_vp8_frame *out);

/* The header of a VP8L image is its first 5 bytes. bdfi_vp8l_header is
 * decodeHeader of Go's vp8l: 1 and the size, or 0 where it returns an error. */
#define BDFI_VP8L_HEADER_SIZE 5
BDF_INTERNAL int bdfi_vp8l_header(const uint8_t *data, size_t len, int32_t *w, int32_t *h);

/*
 * bdfi_vp8l_decode is what vp8l.Decode does after the header: data is what
 * follows the 5 bytes of the header, w and h the size the header states.
 * *pix is the Pix of the *image.NRGBA it returns: 4*w*h bytes, R, G, B, A,
 * not premultiplied, from bdfi_calloc_array. On anything but BDF_OK *pix is
 * NULL and nothing is left allocated.
 */
BDF_INTERNAL bdf_status bdfi_vp8l_decode(const uint8_t *data, size_t len, int32_t w, int32_t h,
                                         uint8_t **pix);

#endif
