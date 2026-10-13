/*
 * WebP images: a port of Go's golang.org/x/image/webp and of the part of
 * golang.org/x/image/riff it uses (BSD license, see
 * third_party/go-image/LICENSE), and of the conversions of Go's image/draw
 * that bring what the decoder returns to premultiplied RGBA. This file reads
 * the container and the alpha plane and converts; img_vp8.c decodes a lossy
 * frame and img_vp8l.c a lossless image.
 *
 * As in Go, the image is the first VP8 or VP8L chunk of the file: an
 * animation (whose frames are inside ANMF chunks) has a size, from its VP8X
 * chunk, and no image. Chunks that are not known are skipped, and a chunk
 * that is skipped has to be in the file whole.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img_webp_internal.h"

/* ---- The container (riff/riff.go) ----------------------------------------- */

/* u32: the first four bytes of b as a little-endian integer. */
static uint32_t bdfi_webp_u32(const uint8_t *b) {
  return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

#define BDFI_WEBP_CHUNK_HEADER_SIZE 8

/*
 * riff.Reader, over a file that is in memory. Go reads a chunk's data
 * through a reader that gives at most what the chunk states and ends where
 * the file ends; here a chunk is a pointer and the number of its bytes that
 * the file has, and what a decoder has read of it does not need to be kept.
 */
typedef struct bdfi_webp_riff {
  const uint8_t *data;
  size_t len;
  size_t pos;         /* where the data of the chunk begins that Next returned last */
  uint32_t total_len; /* what is left of the RIFF chunk, by the length it states */
  uint32_t chunk_len; /* the length the chunk states */
  int padded;         /* the chunk's length is odd: a byte follows it */
} bdfi_webp_riff;

/* riff.NewReader and NewListReader: 0 where they return an error or the
 * form is not WEBP. */
static int bdfi_webp_riff_init(bdfi_webp_riff *z, const uint8_t *data, size_t len) {
  memset(z, 0, sizeof *z);
  if (len < BDFI_WEBP_CHUNK_HEADER_SIZE) {
    return 0;
  }
  if (memcmp(data, "RIFF", 4) != 0) {
    return 0;
  }
  uint32_t chunk_len = bdfi_webp_u32(data + 4);
  if (chunk_len < 4 || len < BDFI_WEBP_CHUNK_HEADER_SIZE + 4) {
    return 0;
  }
  if (memcmp(data + 8, "WEBP", 4) != 0) {
    return 0;
  }
  z->data = data;
  z->len = len;
  z->pos = BDFI_WEBP_CHUNK_HEADER_SIZE + 4;
  z->total_len = chunk_len - 4;
  return 1;
}

/* Reader.Next: the next chunk, its four characters in *id, its data in
 * *chunk and how much of it the file has in *avail. 0 where Go returns an
 * error, which the end of the chunks is too. */
static int bdfi_webp_riff_next(bdfi_webp_riff *z, const uint8_t **id, const uint8_t **chunk,
                               size_t *avail) {
  /* Drain the rest of the previous chunk: Go reads it to its end and fails
   * if the file has less of it than the chunk states, whether a decoder
   * has read some of it or not. */
  if (z->chunk_len != 0) {
    if ((uint64_t)z->chunk_len > (uint64_t)(z->len - z->pos)) {
      return 0;
    }
    z->pos += (size_t)z->chunk_len;
    z->total_len -= z->chunk_len;
    z->chunk_len = 0;
  }
  if (z->padded) {
    if (z->total_len == 0) {
      return 0;
    }
    z->total_len--;
    if (z->pos >= z->len) {
      return 0;
    }
    z->pos++;
  }

  /* We are done if we have no more data. */
  if (z->total_len == 0) {
    return 0;
  }

  /* Read the next chunk header. */
  if (z->total_len < BDFI_WEBP_CHUNK_HEADER_SIZE) {
    return 0;
  }
  z->total_len -= BDFI_WEBP_CHUNK_HEADER_SIZE;
  if (z->len - z->pos < BDFI_WEBP_CHUNK_HEADER_SIZE) {
    return 0;
  }
  *id = z->data + z->pos;
  z->chunk_len = bdfi_webp_u32(z->data + z->pos + 4);
  z->pos += BDFI_WEBP_CHUNK_HEADER_SIZE;
  if (z->chunk_len > z->total_len) {
    return 0;
  }
  z->padded = (z->chunk_len & 1) == 1;
  *chunk = z->data + z->pos;
  *avail = z->len - z->pos;
  if ((uint64_t)*avail > (uint64_t)z->chunk_len) {
    *avail = (size_t)z->chunk_len;
  }
  return 1;
}

/* ---- The alpha plane (webp/decode.go) ------------------------------------- */

/* readAlpha: the alpha values of an image of w x h pixels from what follows
 * the first byte of an ALPH chunk, as they are stored: filtered. */
static bdf_status bdfi_webp_read_alpha(const uint8_t *data, size_t avail, uint32_t width_minus_one,
                                       uint32_t height_minus_one, uint32_t compression,
                                       uint8_t **alpha) {
  /* the VP8X chunk was refused if this is more than 1<<31-1 */
  uint64_t n = ((uint64_t)width_minus_one + 1) * ((uint64_t)height_minus_one + 1);
  *alpha = NULL;
  switch (compression) {
  case 0: {
    /* Go makes the plane and then finds that the chunk is too short. */
    if ((uint64_t)avail < n) {
      return BDF_ERR_FORMAT;
    }
    if (n > (uint64_t)BDFI_MAX_DECODE_PIXELS) {
      return BDF_ERR_TOO_LARGE;
    }
    uint8_t *a = bdfi_malloc_array((size_t)n, 1);
    if (!a) {
      return BDF_ERR_NOMEM;
    }
    memcpy(a, data, (size_t)n);
    *alpha = a;
    return BDF_OK;
  }
  case 1: {
    /* The values are the green of a VP8L image without its header (which
     * Go makes up: 14 bits each for the width and the height). */
    if (width_minus_one > 0x3fff || height_minus_one > 0x3fff) {
      return BDF_ERR_FORMAT;
    }
    uint8_t *pix = NULL;
    bdf_status st = bdfi_vp8l_decode(data, avail, (int32_t)width_minus_one + 1,
                                     (int32_t)height_minus_one + 1, &pix);
    if (st != BDF_OK) {
      return st;
    }
    uint8_t *a = bdfi_malloc_array((size_t)n, 1);
    if (!a) {
      free(pix);
      return BDF_ERR_NOMEM;
    }
    for (size_t i = 0; i < (size_t)n; i++) {
      a[i] = pix[4 * i + 1];
    }
    free(pix);
    *alpha = a;
    return BDF_OK;
  }
  default:
    return BDF_ERR_FORMAT;
  }
}

/* unfilterAlpha: undoes the prediction of the alpha values (len of them,
 * stride a row), in place. The sums are uint8 and wrap. */
static void bdfi_webp_unfilter_alpha(uint8_t *alpha, size_t len, size_t stride, uint32_t filter) {
  if (len == 0 || stride == 0) {
    return;
  }
  switch (filter) {
  case 1: /* Horizontal filter. */
    for (size_t i = 1; i < stride; i++) {
      alpha[i] = (uint8_t)(alpha[i] + alpha[i - 1]);
    }
    for (size_t i = stride; i < len; i += stride) {
      /* The first column is equivalent to the vertical filter. */
      alpha[i] = (uint8_t)(alpha[i] + alpha[i - stride]);

      for (size_t j = 1; j < stride; j++) {
        alpha[i + j] = (uint8_t)(alpha[i + j] + alpha[i + j - 1]);
      }
    }
    break;

  case 2: /* Vertical filter. */
    /* The first row is equivalent to the horizontal filter. */
    for (size_t i = 1; i < stride; i++) {
      alpha[i] = (uint8_t)(alpha[i] + alpha[i - 1]);
    }

    for (size_t i = stride; i < len; i++) {
      alpha[i] = (uint8_t)(alpha[i] + alpha[i - stride]);
    }
    break;

  case 3: /* Gradient filter. */
    /* The first row is equivalent to the horizontal filter. */
    for (size_t i = 1; i < stride; i++) {
      alpha[i] = (uint8_t)(alpha[i] + alpha[i - 1]);
    }

    for (size_t i = stride; i < len; i += stride) {
      /* The first column is equivalent to the vertical filter. */
      alpha[i] = (uint8_t)(alpha[i] + alpha[i - stride]);

      /* The interior is predicted on the three top/left pixels. */
      for (size_t j = 1; j < stride; j++) {
        int32_t c = alpha[i + j - stride - 1];
        int32_t b = alpha[i + j - stride];
        int32_t a = alpha[i + j - 1];
        int32_t x = a + b - c;
        if (x < 0) {
          x = 0;
        } else if (x > 255) {
          x = 255;
        }
        alpha[i + j] = (uint8_t)(alpha[i + j] + x);
      }
    }
    break;

  default: /* 0: not filtered */
    break;
  }
}

/* ---- To premultiplied RGBA (image/draw, image/color) ---------------------- */

/*
 * What color.YCbCrToRGB and color.YCbCr.RGBA compute before they shift:
 * a channel times 65536, or a value outside of 24 bits where the channel is
 * below 0 or above 255.
 */
typedef struct bdfi_webp_rgb {
  int32_t r, g, b;
} bdfi_webp_rgb;

static bdfi_webp_rgb bdfi_webp_ycbcr(uint8_t y, uint8_t cb, uint8_t cr) {
  int32_t yy1 = (int32_t)y * 0x10101;
  int32_t cb1 = (int32_t)cb - 128;
  int32_t cr1 = (int32_t)cr - 128;
  bdfi_webp_rgb c;
  c.r = yy1 + 91881 * cr1;
  c.g = yy1 - 22554 * cb1 - 46802 * cr1;
  c.b = yy1 + 116130 * cb1;
  return c;
}

/* The 8 bits of a channel: v >> 16, or 0 or 255 where it left the range (Go
 * writes this as ^(v >> 31), cut to a uint8). */
static uint8_t bdfi_webp_channel8(int32_t v) {
  if (((uint32_t)v & 0xff000000u) == 0) {
    return (uint8_t)(v >> 16);
  }
  return v < 0 ? 0 : 255;
}

/* The 16 bits of a channel: v >> 8, or 0 or 0xffff where it left the range. */
static uint32_t bdfi_webp_channel16(int32_t v) {
  if (((uint32_t)v & 0xff000000u) == 0) {
    return (uint32_t)v >> 8;
  }
  return v < 0 ? 0 : 0xffff;
}

/* imageutil.DrawYCbCr for 4:2:0, which draw.Draw uses for an *image.YCbCr:
 * the image without alpha, to dst (4 * width * height bytes). */
static void bdfi_webp_draw_ycbcr(const bdfi_vp8_frame *f, uint8_t *dst) {
  size_t w = (size_t)f->width, h = (size_t)f->height;
  for (size_t y = 0; y < h; y++) {
    const uint8_t *yrow = f->y + y * f->y_stride;
    const uint8_t *cbrow = f->cb + (y / 2) * f->c_stride;
    const uint8_t *crrow = f->cr + (y / 2) * f->c_stride;
    for (size_t x = 0; x < w; x++) {
      bdfi_webp_rgb c = bdfi_webp_ycbcr(yrow[x], cbrow[x / 2], crrow[x / 2]);
      dst[0] = bdfi_webp_channel8(c.r);
      dst[1] = bdfi_webp_channel8(c.g);
      dst[2] = bdfi_webp_channel8(c.b);
      dst[3] = 255;
      dst += 4;
    }
  }
}

/* An *image.NYCbCrA has no fast path in draw.Draw: drawRGBA reads each
 * pixel with RGBA64At, which is color.NYCbCrA.RGBA (16 bits a channel,
 * multiplied by the alpha), and keeps the high 8 bits. alpha is width *
 * height values. */
static void bdfi_webp_draw_nycbcra(const bdfi_vp8_frame *f, const uint8_t *alpha, uint8_t *dst) {
  size_t w = (size_t)f->width, h = (size_t)f->height;
  for (size_t y = 0; y < h; y++) {
    const uint8_t *yrow = f->y + y * f->y_stride;
    const uint8_t *cbrow = f->cb + (y / 2) * f->c_stride;
    const uint8_t *crrow = f->cr + (y / 2) * f->c_stride;
    const uint8_t *arow = alpha + y * w;
    for (size_t x = 0; x < w; x++) {
      bdfi_webp_rgb c = bdfi_webp_ycbcr(yrow[x], cbrow[x / 2], crrow[x / 2]);
      uint32_t a = (uint32_t)arow[x] * 0x101;
      dst[0] = (uint8_t)((bdfi_webp_channel16(c.r) * a / 0xffff) >> 8);
      dst[1] = (uint8_t)((bdfi_webp_channel16(c.g) * a / 0xffff) >> 8);
      dst[2] = (uint8_t)((bdfi_webp_channel16(c.b) * a / 0xffff) >> 8);
      dst[3] = (uint8_t)(a >> 8);
      dst += 4;
    }
  }
}

/* drawNRGBASrc, which draw.Draw uses for an *image.NRGBA, done where the
 * pixels are: n pixels, R, G, B, A, to premultiplied. */
static void bdfi_webp_premultiply(uint8_t *pix, size_t n) {
  for (size_t i = 0; i < n; i++, pix += 4) {
    /* an opaque pixel comes out as it is: c * 0xffff / 0xff >> 8 is c */
    if (pix[3] == 0xff) {
      continue;
    }
    uint32_t sa = (uint32_t)pix[3] * 0x101;
    pix[0] = (uint8_t)(((uint32_t)pix[0] * sa / 0xff) >> 8);
    pix[1] = (uint8_t)(((uint32_t)pix[1] * sa / 0xff) >> 8);
    pix[2] = (uint8_t)(((uint32_t)pix[2] * sa / 0xff) >> 8);
    pix[3] = (uint8_t)(sa >> 8);
  }
}

/* ---- The decoder (webp/decode.go) ----------------------------------------- */

/* The bits of the first byte of a VP8X chunk. */
#define BDFI_WEBP_ALPHA_BIT (1 << 4)

/* What decode keeps from chunk to chunk. */
typedef struct bdfi_webp_state {
  uint8_t *alpha; /* the alpha plane, once an ALPH chunk was read */
  int want_alpha; /* the VP8X chunk says there is alpha, and no ALPH chunk came yet */
  int seen_vp8x;
  uint32_t width_minus_one, height_minus_one; /* of the VP8X chunk */
} bdfi_webp_state;

/* The size of the VP8X chunk is the one of the image: Go compares as int. */
static int bdfi_webp_same_size(const bdfi_webp_state *s, int32_t w, int32_t h) {
  return (int64_t)w == (int64_t)s->width_minus_one + 1 &&
         (int64_t)h == (int64_t)s->height_minus_one + 1;
}

/* The ALPH case of decode. */
static bdf_status bdfi_webp_alph(bdfi_webp_state *s, const uint8_t *chunk, size_t avail) {
  if (!s->want_alpha) {
    return BDF_ERR_FORMAT;
  }
  s->want_alpha = 0;
  /* Read the Pre-processing | Filter | Compression byte. */
  if (avail < 1) {
    return BDF_ERR_FORMAT;
  }
  bdf_status st = bdfi_webp_read_alpha(chunk + 1, avail - 1, s->width_minus_one,
                                       s->height_minus_one, chunk[0] & 0x03u, &s->alpha);
  if (st != BDF_OK) {
    return st;
  }
  size_t stride = (size_t)s->width_minus_one + 1;
  bdfi_webp_unfilter_alpha(s->alpha, stride * ((size_t)s->height_minus_one + 1), stride,
                           (uint32_t)(chunk[0] >> 2) & 0x03u);
  return BDF_OK;
}

/* The VP8 case of decode, after the frame header: the frame, and its
 * conversion (with the alpha plane if there is one). */
static bdf_status bdfi_webp_vp8(const bdfi_webp_state *s, const uint8_t *chunk, size_t avail,
                                uint32_t chunk_len, bdfi_bitmap *out) {
  bdfi_vp8_frame frame;
  bdf_status st = bdfi_vp8_decode_frame(chunk, avail, (int64_t)chunk_len, &frame);
  if (st != BDF_OK) {
    return st;
  }
  uint8_t *pix = bdfi_malloc_array((size_t)frame.width * (size_t)frame.height, 4);
  if (!pix) {
    free(frame.buf);
    return BDF_ERR_NOMEM;
  }
  if (s->alpha) {
    bdfi_webp_draw_nycbcra(&frame, s->alpha, pix);
  } else {
    bdfi_webp_draw_ycbcr(&frame, pix);
  }
  free(frame.buf);
  out->w = frame.width;
  out->h = frame.height;
  out->pix = pix;
  return BDF_OK;
}

/*
 * decode: goes through the chunks up to the first that holds an image. With
 * config_only it is DecodeConfig and gives the size in *w and *h (which a
 * VP8X chunk states before any image); else it is Decode, and gives the
 * image in *out. On anything but BDF_OK *out is as it was and nothing is
 * left allocated but what s holds.
 */
static bdf_status bdfi_webp_chunks(bdfi_webp_state *s, const uint8_t *data, size_t len,
                                   int config_only, int32_t *w, int32_t *h, bdfi_bitmap *out) {
  bdfi_webp_riff z;
  if (!bdfi_webp_riff_init(&z, data, len)) {
    return BDF_ERR_FORMAT;
  }
  for (;;) {
    const uint8_t *id = NULL, *chunk = NULL;
    size_t avail = 0;
    if (!bdfi_webp_riff_next(&z, &id, &chunk, &avail)) {
      return BDF_ERR_FORMAT;
    }

    if (memcmp(id, "ALPH", 4) == 0) {
      bdf_status st = bdfi_webp_alph(s, chunk, avail);
      if (st != BDF_OK) {
        return st;
      }

    } else if (memcmp(id, "VP8 ", 4) == 0) {
      if (s->want_alpha || z.chunk_len > 0x7fffffffu) {
        return BDF_ERR_FORMAT;
      }
      int32_t fw = 0, fh = 0;
      if (!bdfi_vp8_frame_header(chunk, avail, (int64_t)z.chunk_len, &fw, &fh)) {
        return BDF_ERR_FORMAT;
      }
      if (s->seen_vp8x && !bdfi_webp_same_size(s, fw, fh)) {
        return BDF_ERR_FORMAT;
      }
      if (config_only) {
        *w = fw;
        *h = fh;
        return BDF_OK;
      }
      return bdfi_webp_vp8(s, chunk, avail, z.chunk_len, out);

    } else if (memcmp(id, "VP8L", 4) == 0) {
      if (s->alpha) {
        return BDF_ERR_FORMAT;
      }
      /* With a VP8X chunk Go reads the header twice, to compare the sizes
       * before it decodes; without one the decoder reads it. It fails
       * alike. */
      int32_t lw = 0, lh = 0;
      if (!bdfi_vp8l_header(chunk, avail, &lw, &lh)) {
        return BDF_ERR_FORMAT;
      }
      if (config_only) {
        *w = lw;
        *h = lh;
        return BDF_OK;
      }
      if (s->seen_vp8x && !bdfi_webp_same_size(s, lw, lh)) {
        return BDF_ERR_FORMAT;
      }
      uint8_t *pix = NULL;
      bdf_status st = bdfi_vp8l_decode(chunk + BDFI_VP8L_HEADER_SIZE, avail - BDFI_VP8L_HEADER_SIZE,
                                       lw, lh, &pix);
      if (st != BDF_OK) {
        return st;
      }
      bdfi_webp_premultiply(pix, (size_t)lw * (size_t)lh);
      out->w = lw;
      out->h = lh;
      out->pix = pix;
      return BDF_OK;

    } else if (memcmp(id, "VP8X", 4) == 0) {
      if (s->seen_vp8x) {
        return BDF_ERR_FORMAT;
      }
      s->seen_vp8x = 1;
      if (z.chunk_len != 10 || avail < 10) {
        return BDF_ERR_FORMAT;
      }
      s->want_alpha = (chunk[0] & BDFI_WEBP_ALPHA_BIT) != 0;
      s->width_minus_one = (uint32_t)chunk[4] | (uint32_t)chunk[5] << 8 | (uint32_t)chunk[6] << 16;
      s->height_minus_one = (uint32_t)chunk[7] | (uint32_t)chunk[8] << 8 | (uint32_t)chunk[9] << 16;
      uint64_t cw = (uint64_t)s->width_minus_one + 1;
      uint64_t ch = (uint64_t)s->height_minus_one + 1;
      if (cw * ch > 0x7fffffffu) {
        /* Go: the product has to fit in an int32. */
        return BDF_ERR_FORMAT;
      }
      if (config_only) {
        *w = (int32_t)cw;
        *h = (int32_t)ch;
        return BDF_OK;
      }
    }
  }
}

static bdf_status bdfi_webp_run(const uint8_t *data, size_t len, int config_only, int32_t *w,
                                int32_t *h, bdfi_bitmap *out) {
  bdfi_webp_state s;
  memset(&s, 0, sizeof s);
  bdf_status st = bdfi_webp_chunks(&s, data, len, config_only, w, h, out);
  free(s.alpha);
  return st;
}

BDF_INTERNAL int bdfi_webp_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h) {
  bdfi_bitmap none;
  memset(&none, 0, sizeof none);
  *w = 0;
  *h = 0;
  if (bdfi_webp_run(data, len, 1, w, h, &none) != BDF_OK) {
    *w = 0;
    *h = 0;
    return 0;
  }
  return 1;
}

BDF_INTERNAL bdf_status bdfi_webp_decode(const uint8_t *data, size_t len, bdfi_bitmap *out) {
  memset(out, 0, sizeof *out);
  /* What the reference does before it decodes: the size DecodeConfig reads
   * has to be one an image can have. (Where DecodeConfig fails, Decode
   * fails too.) */
  int32_t w = 0, h = 0;
  if (bdfi_webp_run(data, len, 1, &w, &h, out) == BDF_OK) {
    if (w <= 0 || h <= 0 || (int64_t)w > BDFI_MAX_DECODE_PIXELS / (int64_t)h) {
      return BDF_ERR_TOO_LARGE;
    }
  }
  bdf_status st = bdfi_webp_run(data, len, 0, &w, &h, out);
  if (st != BDF_OK) {
    memset(out, 0, sizeof *out);
  }
  return st;
}
