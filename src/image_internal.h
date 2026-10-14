/*
 * The image decoders of the rasterizer. Each is a port of the decoder the
 * reference renderer uses (Go's image/png, image/jpeg and image/gif, and
 * golang.org/x/image's bmp and webp), so that an image part is the same
 * pixels in both and is refused by both or by neither.
 */
#ifndef BDF_IMAGE_INTERNAL_H
#define BDF_IMAGE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "bdf_internal.h"

/* The most pixels (width x height) an image is decoded with:
 * imgconv.MaxDecodePixels of the reference. A build may define it lower
 * (BDF_MAX_DECODE_PIXELS): the fuzzers do. */
#ifndef BDF_MAX_DECODE_PIXELS
#define BDF_MAX_DECODE_PIXELS (100 << 20)
#endif
#define BDFI_MAX_DECODE_PIXELS ((int64_t)BDF_MAX_DECODE_PIXELS)

/* imgconv.MaxJPEGScans: the scans of a JPEG image that are read. A decoder
 * goes through every block of an image once a scan, however little the
 * scan says, so a small file of nothing but scans takes as long as
 * thousands of images; encoders write about ten. (BDF_MAX_JPEG_SCANS lowers
 * it; what is drawn is then not what the reference draws.) */
#ifndef BDF_MAX_JPEG_SCANS
#define BDF_MAX_JPEG_SCANS 100
#endif
#define BDFI_MAX_JPEG_SCANS ((size_t)BDF_MAX_JPEG_SCANS)

/* A decoded image: premultiplied RGBA, 8 bits a channel, rows from the top,
 * 4*w bytes a row and nothing between the rows. */
typedef struct bdfi_bitmap {
  int32_t w, h;
  uint8_t *pix; /* from bdfi_malloc_array or bdfi_calloc_array: free() releases it */
} bdfi_bitmap;

/*
 * A decoder is two functions.
 *
 * bdfi_<fmt>_config reads the size of an image as DecodeConfig of the
 * reference's decoder does: 1 and the size where DecodeConfig returns no
 * error, 0 where it returns one (or panics). It does not allocate in
 * proportion to the image.
 *
 * bdfi_<fmt>_decode decodes as the reference renderer does: imgconv.Decode
 * (which is DecodeConfig for the size check and then Decode, with a panic
 * of either taken as an error) followed by the conversion of the image to
 * an *image.RGBA with draw.Draw(dst, dst.Bounds(), img, img.Bounds().Min,
 * draw.Src). It returns
 *
 *   BDF_OK             and the bitmap (the caller frees out->pix),
 *   BDF_ERR_TOO_LARGE  where DecodeConfig succeeds with a width or height
 *                      <= 0 or with more than BDFI_MAX_DECODE_PIXELS pixels
 *                      (and, as a guard of its own, before any allocation
 *                      for an image of more pixels than that),
 *   BDF_ERR_FORMAT     where Decode returns an error or panics, or the
 *                      image is empty,
 *   BDF_ERR_NOMEM      when memory runs out.
 *
 * On anything but BDF_OK *out is zeroed and nothing is left allocated.
 */
BDF_INTERNAL int bdfi_png_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h);
BDF_INTERNAL bdf_status bdfi_png_decode(const uint8_t *data, size_t len, bdfi_bitmap *out);
BDF_INTERNAL int bdfi_jpeg_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h);
BDF_INTERNAL bdf_status bdfi_jpeg_decode(const uint8_t *data, size_t len, bdfi_bitmap *out);
/* jpegScansEnd of imgconv: where the marker of the scan after the first max
 * scans of a JPEG image is, or SIZE_MAX for an image that has no such scan
 * (and for what is no JPEG image). bdfi_jpeg_decode reads an image up to
 * there, for BDFI_MAX_JPEG_SCANS. */
BDF_INTERNAL size_t bdfi_jpeg_scans_end(const uint8_t *data, size_t len, size_t max);
BDF_INTERNAL int bdfi_gif_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h);
BDF_INTERNAL bdf_status bdfi_gif_decode(const uint8_t *data, size_t len, bdfi_bitmap *out);
BDF_INTERNAL int bdfi_bmp_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h);
BDF_INTERNAL bdf_status bdfi_bmp_decode(const uint8_t *data, size_t len, bdfi_bitmap *out);
BDF_INTERNAL int bdfi_webp_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h);
BDF_INTERNAL bdf_status bdfi_webp_decode(const uint8_t *data, size_t len, bdfi_bitmap *out);

#endif
