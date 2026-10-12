/*
 * BMP images: a port of golang.org/x/image/bmp (BSD license, see
 * third_party/go-image/LICENSE), reader.go, so that an image decodes to the
 * bytes the reference renderer draws and the files that package refuses are
 * refused. It reads what that package reads and nothing else: a
 * BITMAPINFOHEADER, BITMAPV4HEADER or BITMAPV5HEADER, one plane, no
 * compression (or BI_BITFIELDS with the masks of BGRA), 1, 2, 4 or 8 bits a
 * pixel with a colour table and 24 or 32 bits without one, with the pixels
 * directly after the header and the table.
 *
 * The package returns an *image.Paletted, an *image.RGBA or an *image.NRGBA;
 * the pixels are converted here as the reference converts each of them to
 * premultiplied RGBA, without the image in between.
 */
#include <stdlib.h>
#include <string.h>

#include "image_internal.h"

/* The lengths decodeConfig names. */
#define BDFI_BMP_FILE_HEADER_LEN    14
#define BDFI_BMP_INFO_HEADER_LEN    40
#define BDFI_BMP_V4_INFO_HEADER_LEN 108
#define BDFI_BMP_V5_INFO_HEADER_LEN 124

/* What decodeConfig returns, and where the file goes on. */
typedef struct bdfi_bmp_header {
  int64_t width, height; /* Go's int: a height of -2^31 in the file is 2^31 here */
  uint32_t bpp;          /* 1, 2, 4, 8, 24 or 32 */
  int top_down;
  int allow_alpha;
  uint32_t colors;        /* the entries of the colour table: 0 for 24 and 32 bits */
  const uint8_t *palette; /* the table: blue, green, red and a byte of padding an entry */
  size_t pixels;          /* the offset of the first row */
} bdfi_bmp_header;

/* readUint16 */
static uint32_t bdfi_bmp_u16(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8);
}

/* readUint32 */
static uint32_t bdfi_bmp_u32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* int(int32(readUint32(b))) */
static int64_t bdfi_bmp_i32(const uint8_t *b) {
  int64_t v = (int64_t)bdfi_bmp_u32(b);
  return v > INT32_MAX ? v - ((int64_t)1 << 32) : v;
}

/*
 * decodeConfig: 1 and the header, 0 where it returns an error. Go reads the
 * headers with io.ReadFull, so a file that ends inside one is an error.
 */
static int bdfi_bmp_read_header(const uint8_t *data, size_t len, bdfi_bmp_header *h) {
  memset(h, 0, sizeof *h);
  if (len < BDFI_BMP_FILE_HEADER_LEN + 4) {
    return 0;
  }
  if (data[0] != 'B' || data[1] != 'M') {
    return 0;
  }
  uint32_t offset = bdfi_bmp_u32(data + 10);
  uint32_t info_len = bdfi_bmp_u32(data + 14);
  if (info_len != BDFI_BMP_INFO_HEADER_LEN && info_len != BDFI_BMP_V4_INFO_HEADER_LEN &&
      info_len != BDFI_BMP_V5_INFO_HEADER_LEN) {
    return 0;
  }
  size_t header_len = BDFI_BMP_FILE_HEADER_LEN + (size_t)info_len;
  if (len < header_len) {
    return 0;
  }
  int64_t width = bdfi_bmp_i32(data + 18);
  int64_t height = bdfi_bmp_i32(data + 22);
  int top_down = 0;
  if (height < 0) {
    height = -height;
    top_down = 1;
  }
  if (width < 0 || height < 0) {
    return 0;
  }
  if ((width == 0) != (height == 0)) {
    /* 0x0 is taken, Nx0 and 0xN are not */
    return 0;
  }
  /* safemath.Mul3(width, height, 4): the product has to fit Go's int. Neither factor is more
   * than 2^31, so their product does not overflow here. */
  if ((uint64_t)width * (uint64_t)height > (uint64_t)INT64_MAX / 4) {
    return 0;
  }
  uint32_t planes = bdfi_bmp_u16(data + 26);
  uint32_t bpp = bdfi_bmp_u16(data + 28);
  uint32_t compression = bdfi_bmp_u32(data + 30);
  /* BI_BITFIELDS with the masks no compression implies is no compression */
  if (compression == 3 && info_len > BDFI_BMP_INFO_HEADER_LEN &&
      bdfi_bmp_u32(data + 54) == 0xff0000 && bdfi_bmp_u32(data + 58) == 0xff00 &&
      bdfi_bmp_u32(data + 62) == 0xff && bdfi_bmp_u32(data + 66) == 0xff000000) {
    compression = 0;
  }
  if (planes != 1 || compression != 0) {
    return 0;
  }
  h->width = width;
  h->height = height;
  h->bpp = bpp;
  h->top_down = top_down;
  h->pixels = header_len;
  switch (bpp) {
  case 1:
  case 2:
  case 4:
  case 8: {
    uint32_t color_used = bdfi_bmp_u32(data + 46);
    if (color_used == 0) {
      color_used = (uint32_t)1 << bpp;
    } else if (color_used > ((uint32_t)1 << bpp)) {
      return 0;
    }
    if (offset != BDFI_BMP_FILE_HEADER_LEN + info_len + color_used * 4) {
      return 0;
    }
    /* io.ReadFull of the colour table */
    if (len - header_len < (size_t)color_used * 4) {
      return 0;
    }
    h->colors = color_used;
    h->palette = data + header_len;
    h->pixels = header_len + (size_t)color_used * 4;
    return 1;
  }
  case 24:
    if (offset != BDFI_BMP_FILE_HEADER_LEN + info_len) {
      return 0;
    }
    return 1;
  case 32:
    if (offset != BDFI_BMP_FILE_HEADER_LEN + info_len) {
      return 0;
    }
    /* the alpha of a BITMAPINFOHEADER is padding, that of the later headers is alpha */
    h->allow_alpha = info_len > BDFI_BMP_INFO_HEADER_LEN;
    return 1;
  default:
    return 0;
  }
}

/*
 * decodePaletted and the conversion of an *image.Paletted: draw.Draw has no
 * case for it and goes through drawRGBA, which reads Paletted.RGBA64At, the
 * palette's color.RGBA{r, g, b, 0xff} as 16 bits a channel, and keeps the
 * high byte of each: r, g, b, 0xff. Returns 0 for an index outside the
 * colour table, which is errInvalidPaletteIndex.
 */
static int bdfi_bmp_paletted_row(const bdfi_bmp_header *h, const uint8_t *b, uint8_t *p) {
  uint32_t bpp = h->bpp;
  uint32_t mask = ((uint32_t)1 << bpp) - 1;
  size_t width = (size_t)h->width;
  size_t byte_index = 0;
  uint32_t bit_index = 8;
  for (size_t x = 0; x < width; x++) {
    bit_index -= bpp;
    uint32_t palette_index = ((uint32_t)b[byte_index] >> bit_index) & mask;
    if (palette_index >= h->colors) {
      return 0;
    }
    const uint8_t *c = h->palette + 4 * (size_t)palette_index;
    p[0] = c[2];
    p[1] = c[1];
    p[2] = c[0];
    p[3] = 0xff;
    p += 4;
    if (bit_index == 0) {
      byte_index++;
      bit_index = 8;
    }
  }
  return 1;
}

/* decodeRGB: an *image.RGBA, which the reference takes as it is. */
static void bdfi_bmp_rgb_row(size_t width, const uint8_t *b, uint8_t *p) {
  for (size_t x = 0; x < width; x++) {
    /* BGR in the file */
    p[0] = b[2];
    p[1] = b[1];
    p[2] = b[0];
    p[3] = 0xff;
    p += 4;
    b += 3;
  }
}

/*
 * decodeNRGBA and the conversion of an *image.NRGBA, drawNRGBASrc of
 * image/draw: each colour times the alpha, in 16 bits, and the high byte of
 * the result. Without alpha the fourth byte of the file is ignored and the
 * pixel is opaque, which the same arithmetic leaves as it is.
 */
static void bdfi_bmp_nrgba_row(size_t width, int allow_alpha, const uint8_t *b, uint8_t *p) {
  for (size_t x = 0; x < width; x++) {
    /* BGRA in the file */
    uint32_t sa = (allow_alpha ? (uint32_t)b[3] : 0xff) * 0x101;
    uint32_t sr = (uint32_t)b[2] * sa / 0xff;
    uint32_t sg = (uint32_t)b[1] * sa / 0xff;
    uint32_t sb = (uint32_t)b[0] * sa / 0xff;
    p[0] = (uint8_t)(sr >> 8);
    p[1] = (uint8_t)(sg >> 8);
    p[2] = (uint8_t)(sb >> 8);
    p[3] = (uint8_t)(sa >> 8);
    p += 4;
    b += 4;
  }
}

/*
 * bmp.DecodeConfig. Go's int holds the height 2^31 that a file stating
 * -2^31 has and an int32_t does not: it is given as INT32_MAX, which every
 * caller refuses as too large just as it would refuse 2^31.
 */
BDF_INTERNAL int bdfi_bmp_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h) {
  bdfi_bmp_header hd;
  *w = 0;
  *h = 0;
  if (!bdfi_bmp_read_header(data, len, &hd)) {
    return 0;
  }
  *w = (int32_t)hd.width;
  *h = hd.height > INT32_MAX ? INT32_MAX : (int32_t)hd.height;
  return 1;
}

/* bmp.Decode, after the size check of imgconv.Decode, and the conversion to premultiplied RGBA. */
BDF_INTERNAL bdf_status bdfi_bmp_decode(const uint8_t *data, size_t len, bdfi_bitmap *out) {
  memset(out, 0, sizeof *out);
  bdfi_bmp_header h;
  if (!bdfi_bmp_read_header(data, len, &h)) {
    return BDF_ERR_FORMAT;
  }
  /* imgconv.Decode refuses these sizes on what DecodeConfig says, before it decodes: Go would
   * allocate the whole image at once (and panic or run out of memory for the largest) */
  if (h.width <= 0 || h.height <= 0 || h.width > BDFI_MAX_DECODE_PIXELS / h.height) {
    return BDF_ERR_TOO_LARGE;
  }
  size_t width = (size_t)h.width, height = (size_t)h.height;
  /* the bytes of a row in the file, padded to four */
  size_t row_bytes;
  if (h.bpp == 32) {
    row_bytes = 4 * width;
  } else if (h.bpp == 24) {
    row_bytes = (3 * width + 3) & ~(size_t)3;
  } else {
    size_t pixels_per_byte = 8 / (size_t)h.bpp;
    row_bytes = ((width + pixels_per_byte - 1) / pixels_per_byte + 3) & ~(size_t)3;
  }
  /* Every row is read with io.ReadFull and a short one is an error, so a file without all the
   * rows is refused, here before anything is allocated for it. What follows the rows is not
   * read. */
  if ((len - h.pixels) / row_bytes < height) {
    return BDF_ERR_FORMAT;
  }
  uint8_t *pix = bdfi_malloc_array(width * height, 4);
  if (!pix) {
    return BDF_ERR_NOMEM;
  }
  const uint8_t *b = data + h.pixels;
  for (size_t i = 0; i < height; i++) {
    /* the rows of the file go from the bottom up unless the height was negative */
    size_t y = h.top_down ? i : height - 1 - i;
    uint8_t *p = pix + y * width * 4;
    if (h.bpp == 32) {
      bdfi_bmp_nrgba_row(width, h.allow_alpha, b, p);
    } else if (h.bpp == 24) {
      bdfi_bmp_rgb_row(width, b, p);
    } else if (!bdfi_bmp_paletted_row(&h, b, p)) {
      free(pix);
      return BDF_ERR_FORMAT;
    }
    b += row_bytes;
  }
  out->w = (int32_t)h.width;
  out->h = (int32_t)h.height;
  out->pix = pix;
  return BDF_OK;
}
