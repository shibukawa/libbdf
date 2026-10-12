/*
 * JPEG images: a port of Go's image/jpeg (BSD license, see
 * third_party/go-image/LICENSE), the decoder of Go 1.27 in reader.go,
 * scan.go, huffman.go and the inverse transform of dct.go, so that an image
 * part decodes to the bytes the reference renderer draws and is refused
 * where the reference refuses it. The image the Go decoder returns is
 * converted as draw.Draw converts it into an *image.RGBA (drawGray,
 * imageutil.DrawYCbCr, drawCMYK, or the *image.RGBA of an RGB JPEG as it
 * is), pixel by pixel, without building the Go image.
 *
 * The decoder is sequential (baseline and extended) and progressive
 * Huffman-coded DCT at 8 bits a sample, with one, three or four components.
 * Everything else is an error, and every error is BDF_ERR_FORMAT: the
 * reference does not tell Go's FormatError from its UnsupportedError.
 *
 * Go reads through a buffer of 4096 bytes that it can step back in by two.
 * Here the file is in memory and the position in it is the buffer's index:
 * where the Go code asks whether two more bytes are buffered (the fast path
 * of readByteStuffedByte), the answer is whether two more bytes are left,
 * which comes to the same, because the slow path differs from the fast one
 * only in what it leaves behind on an error, and those errors either end the
 * decoding or are followed by unreadByteStuffedByte, which takes back what
 * either path consumed.
 */
#include <stdlib.h>
#include <string.h>

#include "image_internal.h"

#define BDFI_JPEG_BLOCK_SIZE      64
#define BDFI_JPEG_DC_TABLE        0
#define BDFI_JPEG_AC_TABLE        1
#define BDFI_JPEG_MAX_TC          1
#define BDFI_JPEG_MAX_TH          3
#define BDFI_JPEG_MAX_TQ          3
#define BDFI_JPEG_MAX_COMPONENTS  4
#define BDFI_JPEG_MAX_CODE_LENGTH 16
#define BDFI_JPEG_MAX_N_CODES     256
#define BDFI_JPEG_LUT_SIZE        8

/* The markers reader.go knows. */
#define BDFI_JPEG_SOF0  0xc0
#define BDFI_JPEG_SOF1  0xc1
#define BDFI_JPEG_SOF2  0xc2
#define BDFI_JPEG_DHT   0xc4
#define BDFI_JPEG_RST0  0xd0
#define BDFI_JPEG_RST7  0xd7
#define BDFI_JPEG_SOI   0xd8
#define BDFI_JPEG_EOI   0xd9
#define BDFI_JPEG_SOS   0xda
#define BDFI_JPEG_DQT   0xdb
#define BDFI_JPEG_DRI   0xdd
#define BDFI_JPEG_COM   0xfe
#define BDFI_JPEG_APP0  0xe0
#define BDFI_JPEG_APP14 0xee
#define BDFI_JPEG_APP15 0xef

/* image.YCbCrSubsampleRatio: how the chroma planes of d.img3 are laid out. */
#define BDFI_JPEG_RATIO_444 0
#define BDFI_JPEG_RATIO_422 1
#define BDFI_JPEG_RATIO_420 2
#define BDFI_JPEG_RATIO_440 3
#define BDFI_JPEG_RATIO_411 4
#define BDFI_JPEG_RATIO_410 5

/* The planes of the image: Y (or the gray of a one-component image), Cb, Cr
 * and black. */
#define BDFI_JPEG_PLANE_Y  0
#define BDFI_JPEG_PLANE_CB 1
#define BDFI_JPEG_PLANE_CR 2
#define BDFI_JPEG_PLANE_K  3

/* unzig in reader.go: from the zig-zag order to the natural one. */
static const uint8_t bdfi_jpeg_unzig[BDFI_JPEG_BLOCK_SIZE] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* component in reader.go. */
typedef struct bdfi_jpeg_component {
  int32_t h, v;               /* sampling factors */
  uint8_t c;                  /* identifier */
  uint8_t tq;                 /* quantization table */
  int32_t expand_h, expand_v; /* what a sample covers in flex mode */
} bdfi_jpeg_component;

/* huffman in huffman.go. */
typedef struct bdfi_jpeg_huffman {
  int32_t n_codes;
  /* For the next 8 bits: the value in the high byte and 1 plus the length of
   * its code in the low one, or 0 when the code is longer than 8 bits. */
  uint16_t lut[1 << BDFI_JPEG_LUT_SIZE];
  uint8_t vals[BDFI_JPEG_MAX_N_CODES];
  int32_t min_codes[BDFI_JPEG_MAX_CODE_LENGTH];
  int32_t max_codes[BDFI_JPEG_MAX_CODE_LENGTH];
  int32_t vals_indices[BDFI_JPEG_MAX_CODE_LENGTH];
} bdfi_jpeg_huffman;

/* The tables of the decoder that reading the size of an image does without:
 * they are on the heap, the rest of the decoder is small. */
typedef struct bdfi_jpeg_tables {
  bdfi_jpeg_huffman huff[BDFI_JPEG_MAX_TC + 1][BDFI_JPEG_MAX_TH + 1];
  int32_t quant[BDFI_JPEG_MAX_TQ + 1][BDFI_JPEG_BLOCK_SIZE]; /* in zig-zag order */
} bdfi_jpeg_tables;

/* A component of a scan, in processSOS. */
typedef struct bdfi_jpeg_scan_component {
  uint8_t comp_index;
  uint8_t td; /* DC table */
  uint8_t ta; /* AC table */
} bdfi_jpeg_scan_component;

/* decoder in reader.go. */
typedef struct bdfi_jpeg_decoder {
  /* d.bytes: data[pos:len] is what has not been read */
  const uint8_t *data;
  size_t len, pos;
  int32_t n_unreadable; /* bytes the last readByteStuffedByte took: 0, 1 or 2 */
  /* d.bits: the low bits_n bits of bits_a have not been read; bits_m is the
   * mask of the next one, or 0 when there is none */
  uint32_t bits_a, bits_m;
  int32_t bits_n;

  int32_t width, height;

  /* d.img1, or d.img3 and d.blackPix: zeroed planes of whole MCUs */
  int has_img;
  uint8_t *plane[BDFI_JPEG_MAX_COMPONENTS];
  size_t stride[BDFI_JPEG_MAX_COMPONENTS];
  int ratio; /* of d.img3 */

  int flex; /* a subsampling image.YCbCr has no ratio for: samples are spread as they are stored */
  int32_t max_h, max_v;

  int32_t ri; /* restart interval */
  int32_t n_comp;

  int baseline, progressive;
  int jfif;
  int adobe_transform_valid;
  uint8_t adobe_transform;
  uint16_t eob_run;

  bdfi_jpeg_component comp[BDFI_JPEG_MAX_COMPONENTS];
  int32_t *prog_coeffs[BDFI_JPEG_MAX_COMPONENTS]; /* blocks kept between progressive scans */
  bdfi_jpeg_tables *t;                            /* NULL while only the size is read */
  uint8_t tmp[2 * BDFI_JPEG_BLOCK_SIZE];
} bdfi_jpeg_decoder;

/* ---- Go's int32 ---------------------------------------------------------- */

/*
 * Go's int32 arithmetic wraps, and coefficients of a damaged file do
 * overflow. C leaves the overflow of a signed integer undefined, so the sums
 * and products are made on uint32_t and brought back with bdfi_jpeg_i32.
 */
static int32_t bdfi_jpeg_i32(uint32_t v) {
  return v > 0x7fffffffu ? -(int32_t)(0xffffffffu - v) - 1 : (int32_t)v;
}

static int32_t bdfi_jpeg_add(int32_t a, int32_t b) {
  return bdfi_jpeg_i32((uint32_t)a + (uint32_t)b);
}

static int32_t bdfi_jpeg_sub(int32_t a, int32_t b) {
  return bdfi_jpeg_i32((uint32_t)a - (uint32_t)b);
}

static int32_t bdfi_jpeg_mul(int32_t a, int32_t b) {
  return bdfi_jpeg_i32((uint32_t)a * (uint32_t)b);
}

/* a << n of Go, for n < 32. */
static int32_t bdfi_jpeg_shl(int32_t a, uint32_t n) {
  return bdfi_jpeg_i32((uint32_t)a << n);
}

/* x >> n of Go on the int32 with the bits of x, for 0 < n < 32: the sign
 * fills in, which C does not promise for a negative value. */
static uint32_t bdfi_jpeg_sar(uint32_t x, uint32_t n) {
  int32_t v = bdfi_jpeg_i32(x);
  return (uint32_t)(v < 0 ? ~(~v >> n) : v >> n);
}

/* ---- The inverse transform (dct.go) -------------------------------------- */

/* c(x, bits) of dct.go for the constants the inverse transform uses. */
#define BDFI_JPEG_SQRT2INV_COS6_18 70936u  /* c(sqrt2inv_cos6, 18) */
#define BDFI_JPEG_SQRT2INV_SIN6_18 171254u /* c(sqrt2inv_sin6, 18) */
#define BDFI_JPEG_SQRT2INV_8       181u    /* c(sqrt2inv, 8) */
#define BDFI_JPEG_COS3_12          3406u   /* c(cos3, 12) */
#define BDFI_JPEG_SIN3_12          2276u   /* c(sin3, 12) */
#define BDFI_JPEG_COS1_12          4017u   /* c(cos1, 12) */
#define BDFI_JPEG_SIN1_12          799u    /* c(sin1, 12) */
#define BDFI_JPEG_SQRT2INV_COS6_12 1108u   /* c(sqrt2inv_cos6, 12) */
#define BDFI_JPEG_SQRT2INV_SIN6_12 2676u   /* c(sqrt2inv_sin6, 12) */
#define BDFI_JPEG_SQRT2INV_14      11585u  /* c(sqrt2inv, 14) */

/* dctBox with a negated sine, as the inverse transform calls it: the values
 * are int32 of Go held in uint32_t, where they wrap as Go's do. */
static void bdfi_jpeg_dct_box(uint32_t x0, uint32_t x1, uint32_t kcos, uint32_t sine, uint32_t *y0,
                              uint32_t *y1) {
  uint32_t ksin = 0u - sine;
  uint32_t ksum = kcos * (x0 + x1);
  *y0 = ksum + (ksin - kcos) * x1;
  *y1 = ksum - (kcos + ksin) * x0;
}

/* a, b = a+b, a-b */
static void bdfi_jpeg_butterfly(uint32_t *a, uint32_t *b) {
  uint32_t sum = *a + *b, diff = *a - *b;
  *a = sum;
  *b = diff;
}

/* idctRows in dct.go. */
static void bdfi_jpeg_idct_rows(int32_t *b) {
  for (int i = 0; i < 8; i++) {
    int32_t *x = b + 8 * i;
    uint32_t x0 = (uint32_t)x[0], x7 = (uint32_t)x[1], x2 = (uint32_t)x[2], x5 = (uint32_t)x[3];
    uint32_t x1 = (uint32_t)x[4], x6 = (uint32_t)x[5], x3 = (uint32_t)x[6], x4 = (uint32_t)x[7];

    x0 <<= 17;
    x1 <<= 17;
    bdfi_jpeg_butterfly(&x0, &x1);
    bdfi_jpeg_dct_box(x2, x3, BDFI_JPEG_SQRT2INV_COS6_18, BDFI_JPEG_SQRT2INV_SIN6_18, &x2, &x3);
    bdfi_jpeg_butterfly(&x1, &x2);
    bdfi_jpeg_butterfly(&x0, &x3);

    x4 <<= 7;
    x7 <<= 7;
    bdfi_jpeg_butterfly(&x7, &x4);
    x6 *= BDFI_JPEG_SQRT2INV_8;
    x5 *= BDFI_JPEG_SQRT2INV_8;
    bdfi_jpeg_butterfly(&x7, &x5);
    bdfi_jpeg_butterfly(&x4, &x6);
    bdfi_jpeg_dct_box(bdfi_jpeg_sar(x4, 2), bdfi_jpeg_sar(x7, 2), BDFI_JPEG_COS3_12,
                      BDFI_JPEG_SIN3_12, &x4, &x7);
    bdfi_jpeg_dct_box(bdfi_jpeg_sar(x5, 2), bdfi_jpeg_sar(x6, 2), BDFI_JPEG_COS1_12,
                      BDFI_JPEG_SIN1_12, &x5, &x6);

    bdfi_jpeg_butterfly(&x0, &x7);
    bdfi_jpeg_butterfly(&x1, &x6);
    bdfi_jpeg_butterfly(&x2, &x5);
    bdfi_jpeg_butterfly(&x3, &x4);

    x[0] = bdfi_jpeg_i32(x0);
    x[1] = bdfi_jpeg_i32(x1);
    x[2] = bdfi_jpeg_i32(x2);
    x[3] = bdfi_jpeg_i32(x3);
    x[4] = bdfi_jpeg_i32(x4);
    x[5] = bdfi_jpeg_i32(x5);
    x[6] = bdfi_jpeg_i32(x6);
    x[7] = bdfi_jpeg_i32(x7);
  }
}

/* idctCols in dct.go. */
static void bdfi_jpeg_idct_cols(int32_t *b) {
  for (int i = 0; i < 8; i++) {
    uint32_t x0 = (uint32_t)b[0 * 8 + i], x7 = (uint32_t)b[1 * 8 + i];
    uint32_t x2 = (uint32_t)b[2 * 8 + i], x5 = (uint32_t)b[3 * 8 + i];
    uint32_t x1 = (uint32_t)b[4 * 8 + i], x6 = (uint32_t)b[5 * 8 + i];
    uint32_t x3 = (uint32_t)b[6 * 8 + i], x4 = (uint32_t)b[7 * 8 + i];

    x0 += (uint32_t)1 << 19;

    /* x0, x1 = (x0+x1)>>2, (x0-x1)>>2 */
    bdfi_jpeg_butterfly(&x0, &x1);
    x0 = bdfi_jpeg_sar(x0, 2);
    x1 = bdfi_jpeg_sar(x1, 2);
    bdfi_jpeg_dct_box(bdfi_jpeg_sar(x2, 13), bdfi_jpeg_sar(x3, 13), BDFI_JPEG_SQRT2INV_COS6_12,
                      BDFI_JPEG_SQRT2INV_SIN6_12, &x2, &x3);
    bdfi_jpeg_butterfly(&x1, &x2);
    bdfi_jpeg_butterfly(&x0, &x3);

    bdfi_jpeg_butterfly(&x7, &x4);
    x5 = bdfi_jpeg_sar(x5, 13) * BDFI_JPEG_SQRT2INV_14;
    x6 = bdfi_jpeg_sar(x6, 13) * BDFI_JPEG_SQRT2INV_14;
    bdfi_jpeg_butterfly(&x7, &x5);
    bdfi_jpeg_butterfly(&x4, &x6);
    bdfi_jpeg_dct_box(bdfi_jpeg_sar(x4, 14), bdfi_jpeg_sar(x7, 14), BDFI_JPEG_COS3_12,
                      BDFI_JPEG_SIN3_12, &x4, &x7);
    bdfi_jpeg_dct_box(bdfi_jpeg_sar(x5, 14), bdfi_jpeg_sar(x6, 14), BDFI_JPEG_COS1_12,
                      BDFI_JPEG_SIN1_12, &x5, &x6);

    bdfi_jpeg_butterfly(&x0, &x7);
    bdfi_jpeg_butterfly(&x1, &x6);
    bdfi_jpeg_butterfly(&x2, &x5);
    bdfi_jpeg_butterfly(&x3, &x4);

    b[0 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x0, 18));
    b[1 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x1, 18));
    b[2 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x2, 18));
    b[3 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x3, 18));
    b[4 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x4, 18));
    b[5 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x5, 18));
    b[6 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x6, 18));
    b[7 * 8 + i] = bdfi_jpeg_i32(bdfi_jpeg_sar(x7, 18));
  }
}

/* idct in dct.go: rows, then columns. */
static void bdfi_jpeg_idct(int32_t *b) {
  bdfi_jpeg_idct_rows(b);
  bdfi_jpeg_idct_cols(b);
}

/* ---- Bytes (reader.go) --------------------------------------------------- */

/* decoder.unreadByteStuffedByte: gives the bytes of the last
 * readByteStuffedByte back, and with them the 8 bits if they are all still
 * there. */
static void bdfi_jpeg_unread_byte_stuffed_byte(bdfi_jpeg_decoder *d) {
  d->pos -= (size_t)d->n_unreadable;
  d->n_unreadable = 0;
  if (d->bits_n >= 8) {
    d->bits_a >>= 8;
    d->bits_n -= 8;
    d->bits_m >>= 8;
  }
}

/* decoder.readByte: the next byte, whatever it is. At the end of the data Go's
 * fill returns io.ErrUnexpectedEOF. */
static bdf_status bdfi_jpeg_read_byte(bdfi_jpeg_decoder *d, uint8_t *x) {
  if (d->pos == d->len) {
    return BDF_ERR_FORMAT;
  }
  *x = d->data[d->pos++];
  d->n_unreadable = 0;
  return BDF_OK;
}

/* decoder.readByteStuffedByte: the next byte of entropy-coded data, where
 * 0xff is followed by a 0x00 that is not data. Anything else after 0xff is
 * errMissingFF00: a marker, the end of the data of a scan. */
static bdf_status bdfi_jpeg_read_byte_stuffed_byte(bdfi_jpeg_decoder *d, uint8_t *x) {
  if (d->len - d->pos >= 2) {
    uint8_t c = d->data[d->pos++];
    d->n_unreadable = 1;
    if (c != 0xff) {
      *x = c;
      return BDF_OK;
    }
    if (d->data[d->pos] != 0x00) {
      return BDF_ERR_FORMAT;
    }
    d->pos++;
    d->n_unreadable = 2;
    *x = 0xff;
    return BDF_OK;
  }

  /* The slow path, for the last byte of the data and after it. */
  d->n_unreadable = 0;
  if (d->pos == d->len) {
    return BDF_ERR_FORMAT;
  }
  uint8_t c = d->data[d->pos++];
  d->n_unreadable = 1;
  if (c != 0xff) {
    *x = c;
    return BDF_OK;
  }
  /* the byte after it is not there to read */
  return BDF_ERR_FORMAT;
}

/* What readFull and ignore start with: the bytes read ahead for the bits are
 * given back, if the last one has not been touched. */
static void bdfi_jpeg_unread_overshot(bdfi_jpeg_decoder *d) {
  if (d->n_unreadable != 0) {
    if (d->bits_n >= 8) {
      bdfi_jpeg_unread_byte_stuffed_byte(d);
    }
    d->n_unreadable = 0;
  }
}

/* decoder.readFull: exactly n bytes into p (n > 0). */
static bdf_status bdfi_jpeg_read_full(bdfi_jpeg_decoder *d, uint8_t *p, size_t n) {
  bdfi_jpeg_unread_overshot(d);
  if (d->len - d->pos < n) {
    d->pos = d->len;
    return BDF_ERR_FORMAT;
  }
  memcpy(p, d->data + d->pos, n);
  d->pos += n;
  return BDF_OK;
}

/* decoder.ignore: skips n bytes. */
static bdf_status bdfi_jpeg_ignore(bdfi_jpeg_decoder *d, int32_t n) {
  bdfi_jpeg_unread_overshot(d);
  if (d->len - d->pos < (size_t)n) {
    d->pos = d->len;
    return BDF_ERR_FORMAT;
  }
  d->pos += (size_t)n;
  return BDF_OK;
}

/* ---- Bits and Huffman codes (huffman.go) --------------------------------- */

/* decoder.ensureNBits: reads bytes until there are n bits. Its callers have
 * seen that there are fewer, and ask for 16 at most, so bits_n stays below
 * 24. */
static bdf_status bdfi_jpeg_ensure_n_bits(bdfi_jpeg_decoder *d, int32_t n) {
  for (;;) {
    uint8_t c = 0;
    if (bdfi_jpeg_read_byte_stuffed_byte(d, &c) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    d->bits_a = d->bits_a << 8 | c;
    d->bits_n += 8;
    if (d->bits_m == 0) {
      d->bits_m = 1 << 7;
    } else {
      d->bits_m <<= 8;
    }
    if (d->bits_n >= n) {
      break;
    }
  }
  return BDF_OK;
}

/* decoder.receiveExtend: the signed integer in the next t bits, t <= 16. */
static bdf_status bdfi_jpeg_receive_extend(bdfi_jpeg_decoder *d, uint8_t t, int32_t *out) {
  if (d->bits_n < (int32_t)t) {
    if (bdfi_jpeg_ensure_n_bits(d, (int32_t)t) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
  }
  d->bits_n -= (int32_t)t;
  d->bits_m >>= t;
  int32_t s = (int32_t)1 << t;
  int32_t x = (int32_t)((d->bits_a >> (uint32_t)d->bits_n) & (uint32_t)(s - 1));
  /* Go's branchless form of this is 0 for t == 0, where x is 0 */
  if (t != 0 && x < (s >> 1)) {
    x += 1 - s;
  }
  *out = x;
  return BDF_OK;
}

/* decoder.processDHT: Define Huffman Table. */
static bdf_status bdfi_jpeg_process_dht(bdfi_jpeg_decoder *d, int32_t n) {
  while (n > 0) {
    if (n < 17) {
      return BDF_ERR_FORMAT;
    }
    if (bdfi_jpeg_read_full(d, d->tmp, 17) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    uint8_t tc = (uint8_t)(d->tmp[0] >> 4);
    if (tc > BDFI_JPEG_MAX_TC) {
      return BDF_ERR_FORMAT;
    }
    uint8_t th = (uint8_t)(d->tmp[0] & 0x0f);
    /* a baseline image has two tables of each kind */
    if (th > BDFI_JPEG_MAX_TH || (d->baseline && th > 1)) {
      return BDF_ERR_FORMAT;
    }
    bdfi_jpeg_huffman *h = &d->t->huff[tc][th];

    /* n_codes[i] is the number of codes of i+1 bits */
    h->n_codes = 0;
    int32_t n_codes[BDFI_JPEG_MAX_CODE_LENGTH];
    for (int i = 0; i < BDFI_JPEG_MAX_CODE_LENGTH; i++) {
      n_codes[i] = (int32_t)d->tmp[i + 1];
      h->n_codes += n_codes[i];
    }
    if (h->n_codes == 0 || h->n_codes > BDFI_JPEG_MAX_N_CODES) {
      return BDF_ERR_FORMAT;
    }
    n -= h->n_codes + 17;
    if (n < 0) {
      return BDF_ERR_FORMAT;
    }
    if (bdfi_jpeg_read_full(d, h->vals, (size_t)h->n_codes) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }

    /* The look-up table. The lengths may claim more codes than there are of
     * a length: Go then writes over entries, the code truncated to 8 bits. */
    memset(h->lut, 0, sizeof h->lut);
    uint32_t x = 0, code = 0;
    for (uint32_t i = 0; i < BDFI_JPEG_LUT_SIZE; i++) {
      code <<= 1;
      for (int32_t j = 0; j < n_codes[i]; j++) {
        uint8_t base = (uint8_t)(code << (7 - i));
        uint16_t lut_value = (uint16_t)(((uint32_t)h->vals[x] << 8) | (2 + i));
        for (uint32_t k = 0; k < ((uint32_t)1 << (7 - i)); k++) {
          h->lut[base | k] = lut_value;
        }
        code++;
        x++;
      }
    }

    int32_t c = 0, index = 0;
    for (int i = 0; i < BDFI_JPEG_MAX_CODE_LENGTH; i++) {
      int32_t count = n_codes[i];
      if (count == 0) {
        h->min_codes[i] = -1;
        h->max_codes[i] = -1;
        h->vals_indices[i] = -1;
      } else {
        h->min_codes[i] = c;
        h->max_codes[i] = c + count - 1;
        h->vals_indices[i] = index;
        c += count;
        index += count;
      }
      c <<= 1;
    }
  }
  return BDF_OK;
}

/* decoder.decodeHuffman: the next value coded with h. */
static bdf_status bdfi_jpeg_decode_huffman(bdfi_jpeg_decoder *d, const bdfi_jpeg_huffman *h,
                                           uint8_t *value) {
  if (h->n_codes == 0) {
    return BDF_ERR_FORMAT; /* the table was never defined */
  }

  int look_up = 1;
  if (d->bits_n < 8) {
    if (bdfi_jpeg_ensure_n_bits(d, 8) != BDF_OK) {
      /* The data of the scan has ended (a marker, or the end of the file),
       * but the next code may be in the bits read before: give back what the
       * attempt read and take the bits one by one. */
      if (d->n_unreadable != 0) {
        bdfi_jpeg_unread_byte_stuffed_byte(d);
      }
      look_up = 0;
    }
  }
  if (look_up) {
    uint16_t v = h->lut[(d->bits_a >> (uint32_t)(d->bits_n - BDFI_JPEG_LUT_SIZE)) & 0xff];
    if (v != 0) {
      uint32_t n = (uint32_t)(v & 0xff) - 1;
      d->bits_n -= (int32_t)n;
      d->bits_m >>= n;
      *value = (uint8_t)(v >> 8);
      return BDF_OK;
    }
  }

  int32_t code = 0;
  for (int i = 0; i < BDFI_JPEG_MAX_CODE_LENGTH; i++) {
    if (d->bits_n == 0) {
      if (bdfi_jpeg_ensure_n_bits(d, 1) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
    }
    if ((d->bits_a & d->bits_m) != 0) {
      code |= 1;
    }
    d->bits_n--;
    d->bits_m >>= 1;
    if (code <= h->max_codes[i]) {
      /* The index is in the table: a code that is beyond the codes of every
       * shorter length is at least the first code of this one. */
      *value = h->vals[h->vals_indices[i] + code - h->min_codes[i]];
      return BDF_OK;
    }
    code <<= 1;
  }
  return BDF_ERR_FORMAT; /* no code of 16 bits or fewer */
}

/* decoder.decodeBit. */
static bdf_status bdfi_jpeg_decode_bit(bdfi_jpeg_decoder *d, int *bit) {
  if (d->bits_n == 0) {
    if (bdfi_jpeg_ensure_n_bits(d, 1) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
  }
  *bit = (d->bits_a & d->bits_m) != 0;
  d->bits_n--;
  d->bits_m >>= 1;
  return BDF_OK;
}

/* decoder.decodeBits: the next n bits, 0 < n < 16. */
static bdf_status bdfi_jpeg_decode_bits(bdfi_jpeg_decoder *d, int32_t n, uint32_t *bits) {
  if (d->bits_n < n) {
    if (bdfi_jpeg_ensure_n_bits(d, n) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
  }
  uint32_t ret = d->bits_a >> (uint32_t)(d->bits_n - n);
  ret &= ((uint32_t)1 << (uint32_t)n) - 1;
  d->bits_n -= n;
  d->bits_m >>= (uint32_t)n;
  *bits = ret;
  return BDF_OK;
}

/* ---- Segments (reader.go) ------------------------------------------------ */

/* decoder.processSOF: the frame header. */
static bdf_status bdfi_jpeg_process_sof(bdfi_jpeg_decoder *d, int32_t n) {
  if (d->n_comp != 0) {
    return BDF_ERR_FORMAT; /* a second frame */
  }
  switch (n) {
  case 6 + 3 * 1: /* gray */
    d->n_comp = 1;
    break;
  case 6 + 3 * 3: /* YCbCr or RGB */
    d->n_comp = 3;
    break;
  case 6 + 3 * 4: /* YCbCrK or CMYK */
    d->n_comp = 4;
    break;
  default:
    return BDF_ERR_FORMAT;
  }
  if (bdfi_jpeg_read_full(d, d->tmp, (size_t)n) != BDF_OK) {
    return BDF_ERR_FORMAT;
  }
  /* 8 bits a sample only */
  if (d->tmp[0] != 8) {
    return BDF_ERR_FORMAT;
  }
  d->height = ((int32_t)d->tmp[1] << 8) + (int32_t)d->tmp[2];
  d->width = ((int32_t)d->tmp[3] << 8) + (int32_t)d->tmp[4];
  if ((int32_t)d->tmp[5] != d->n_comp) {
    return BDF_ERR_FORMAT;
  }

  for (int i = 0; i < d->n_comp; i++) {
    d->comp[i].c = d->tmp[6 + 3 * i];
    for (int j = 0; j < i; j++) {
      if (d->comp[i].c == d->comp[j].c) {
        return BDF_ERR_FORMAT; /* an identifier twice */
      }
    }

    d->comp[i].tq = d->tmp[8 + 3 * i];
    if (d->comp[i].tq > BDFI_JPEG_MAX_TQ) {
      return BDF_ERR_FORMAT;
    }

    uint8_t hv = d->tmp[7 + 3 * i];
    int32_t h = (int32_t)(hv >> 4), v = (int32_t)(hv & 0x0f);
    if (h < 1 || 4 < h || v < 1 || 4 < v) {
      return BDF_ERR_FORMAT;
    }
    if (h == 3 || v == 3) {
      return BDF_ERR_FORMAT;
    }
    switch (d->n_comp) {
    case 1:
      /* one component is not interleaved: its sampling factors do not
       * matter, an MCU is one block */
      h = 1;
      v = 1;
      break;
    case 3:
      /* any factors that divide the largest ones, checked below */
      break;
    case 4:
      /* only [0x11 0x11 0x11 0x11] and [0x22 0x11 0x11 0x22] */
      switch (i) {
      case 0:
        if (hv != 0x11 && hv != 0x22) {
          return BDF_ERR_FORMAT;
        }
        break;
      case 1:
      case 2:
        if (hv != 0x11) {
          return BDF_ERR_FORMAT;
        }
        break;
      default:
        if (d->comp[0].h != h || d->comp[0].v != v) {
          return BDF_ERR_FORMAT;
        }
        break;
      }
      break;
    default:
      break;
    }

    d->max_h = d->max_h > h ? d->max_h : h;
    d->max_v = d->max_v > v ? d->max_v : v;
    d->comp[i].h = h;
    d->comp[i].v = v;
  }

  if (d->n_comp == 3) {
    for (int i = 0; i < 3; i++) {
      if (d->max_h % d->comp[i].h != 0 || d->max_v % d->comp[i].v != 0) {
        return BDF_ERR_FORMAT;
      }
    }
  }

  for (int i = 0; i < d->n_comp; i++) {
    d->comp[i].expand_h = d->max_h / d->comp[i].h;
    d->comp[i].expand_v = d->max_v / d->comp[i].v;
  }
  return BDF_OK;
}

/* decoder.processDQT: Define Quantization Table. */
static bdf_status bdfi_jpeg_process_dqt(bdfi_jpeg_decoder *d, int32_t n) {
  while (n > 0) {
    n--;
    uint8_t x = 0;
    if (bdfi_jpeg_read_byte(d, &x) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    uint8_t tq = (uint8_t)(x & 0x0f);
    if (tq > BDFI_JPEG_MAX_TQ) {
      return BDF_ERR_FORMAT;
    }
    int32_t *q = d->t->quant[tq];
    uint8_t pq = (uint8_t)(x >> 4);
    if (pq == 0) {
      if (n < BDFI_JPEG_BLOCK_SIZE) {
        break;
      }
      n -= BDFI_JPEG_BLOCK_SIZE;
      if (bdfi_jpeg_read_full(d, d->tmp, BDFI_JPEG_BLOCK_SIZE) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
      for (int i = 0; i < BDFI_JPEG_BLOCK_SIZE; i++) {
        q[i] = (int32_t)d->tmp[i];
      }
    } else if (pq == 1) {
      if (n < 2 * BDFI_JPEG_BLOCK_SIZE) {
        break;
      }
      n -= 2 * BDFI_JPEG_BLOCK_SIZE;
      if (bdfi_jpeg_read_full(d, d->tmp, 2 * BDFI_JPEG_BLOCK_SIZE) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
      for (int i = 0; i < BDFI_JPEG_BLOCK_SIZE; i++) {
        q[i] = ((int32_t)d->tmp[2 * i] << 8) | (int32_t)d->tmp[2 * i + 1];
      }
    } else {
      return BDF_ERR_FORMAT;
    }
  }
  if (n != 0) {
    return BDF_ERR_FORMAT;
  }
  return BDF_OK;
}

/* decoder.processDRI: Define Restart Interval. */
static bdf_status bdfi_jpeg_process_dri(bdfi_jpeg_decoder *d, int32_t n) {
  if (n != 2) {
    return BDF_ERR_FORMAT;
  }
  if (bdfi_jpeg_read_full(d, d->tmp, 2) != BDF_OK) {
    return BDF_ERR_FORMAT;
  }
  d->ri = ((int32_t)d->tmp[0] << 8) + (int32_t)d->tmp[1];
  return BDF_OK;
}

/* decoder.processApp0Marker: every APP0 segment of 5 bytes or more says
 * again whether the image is JFIF. */
static bdf_status bdfi_jpeg_process_app0(bdfi_jpeg_decoder *d, int32_t n) {
  if (n < 5) {
    return bdfi_jpeg_ignore(d, n);
  }
  if (bdfi_jpeg_read_full(d, d->tmp, 5) != BDF_OK) {
    return BDF_ERR_FORMAT;
  }
  n -= 5;
  d->jfif = d->tmp[0] == 'J' && d->tmp[1] == 'F' && d->tmp[2] == 'I' && d->tmp[3] == 'F' &&
            d->tmp[4] == 0;
  if (n > 0) {
    return bdfi_jpeg_ignore(d, n);
  }
  return BDF_OK;
}

/* decoder.processApp14Marker: the colour transform Adobe's segment states. */
static bdf_status bdfi_jpeg_process_app14(bdfi_jpeg_decoder *d, int32_t n) {
  if (n < 12) {
    return bdfi_jpeg_ignore(d, n);
  }
  if (bdfi_jpeg_read_full(d, d->tmp, 12) != BDF_OK) {
    return BDF_ERR_FORMAT;
  }
  n -= 12;
  if (d->tmp[0] == 'A' && d->tmp[1] == 'd' && d->tmp[2] == 'o' && d->tmp[3] == 'b' &&
      d->tmp[4] == 'e') {
    d->adobe_transform_valid = 1;
    d->adobe_transform = d->tmp[11];
  }
  if (n > 0) {
    return bdfi_jpeg_ignore(d, n);
  }
  return BDF_OK;
}

/* ---- Scans (scan.go) ----------------------------------------------------- */

/* decoder.makeImg: the planes of the image, mxx by myy MCUs, zeroed as Go's
 * are: what no scan writes stays 0. */
static bdf_status bdfi_jpeg_make_img(bdfi_jpeg_decoder *d, int64_t mxx, int64_t myy) {
  /* The size was checked on what bdfi_jpeg_config read before the decoding
   * began; this is the guard at the allocation itself. */
  if (d->width <= 0 || d->height <= 0 ||
      (int64_t)d->width > BDFI_MAX_DECODE_PIXELS / (int64_t)d->height) {
    return BDF_ERR_TOO_LARGE;
  }
  if (d->n_comp == 1) {
    d->plane[BDFI_JPEG_PLANE_Y] = bdfi_calloc_array((size_t)(8 * mxx), (size_t)(8 * myy));
    if (!d->plane[BDFI_JPEG_PLANE_Y]) {
      return BDF_ERR_NOMEM;
    }
    d->stride[BDFI_JPEG_PLANE_Y] = (size_t)(8 * mxx);
    d->has_img = 1;
    return BDF_OK;
  }

  /* Flex mode is for what image.YCbCr has no ratio for: Cb and Cr sampled
   * differently, Y not sampled the most, or another ratio than the six. */
  int32_t h_ratio = 1, v_ratio = 1;
  d->ratio = BDFI_JPEG_RATIO_444;
  if (d->comp[1].h != d->comp[2].h || d->comp[1].v != d->comp[2].v || d->max_h != d->comp[0].h ||
      d->max_v != d->comp[0].v) {
    d->flex = 1;
  } else {
    h_ratio = d->max_h / d->comp[1].h;
    v_ratio = d->max_v / d->comp[1].v;
    switch (h_ratio << 4 | v_ratio) {
    case 0x11:
      d->ratio = BDFI_JPEG_RATIO_444;
      break;
    case 0x12:
      d->ratio = BDFI_JPEG_RATIO_440;
      break;
    case 0x21:
      d->ratio = BDFI_JPEG_RATIO_422;
      break;
    case 0x22:
      d->ratio = BDFI_JPEG_RATIO_420;
      break;
    case 0x41:
      d->ratio = BDFI_JPEG_RATIO_411;
      break;
    case 0x42:
      d->ratio = BDFI_JPEG_RATIO_410;
      break;
    default:
      d->flex = 1;
      break;
    }
  }
  if (d->flex) {
    /* a 4:4:4 image that the samples of every component are spread over */
    h_ratio = 1;
    v_ratio = 1;
  }

  /* image.NewYCbCr: the width and the height are multiples of 8 times the
   * ratio, so the chroma planes are that much smaller exactly. */
  size_t w = (size_t)(8 * d->max_h * mxx), h = (size_t)(8 * d->max_v * myy);
  size_t cw = w / (size_t)h_ratio, ch = h / (size_t)v_ratio;
  d->plane[BDFI_JPEG_PLANE_Y] = bdfi_calloc_array(w, h);
  d->plane[BDFI_JPEG_PLANE_CB] = bdfi_calloc_array(cw, ch);
  d->plane[BDFI_JPEG_PLANE_CR] = bdfi_calloc_array(cw, ch);
  if (!d->plane[BDFI_JPEG_PLANE_Y] || !d->plane[BDFI_JPEG_PLANE_CB] ||
      !d->plane[BDFI_JPEG_PLANE_CR]) {
    return BDF_ERR_NOMEM;
  }
  d->stride[BDFI_JPEG_PLANE_Y] = w;
  d->stride[BDFI_JPEG_PLANE_CB] = cw;
  d->stride[BDFI_JPEG_PLANE_CR] = cw;

  if (d->n_comp == 4) {
    size_t kw = (size_t)(8 * d->comp[3].h * mxx), kh = (size_t)(8 * d->comp[3].v * myy);
    d->plane[BDFI_JPEG_PLANE_K] = bdfi_calloc_array(kw, kh);
    if (!d->plane[BDFI_JPEG_PLANE_K]) {
      return BDF_ERR_NOMEM;
    }
    d->stride[BDFI_JPEG_PLANE_K] = kw;
  }
  d->has_img = 1;
  return BDF_OK;
}

/*
 * decoder.reconstructBlock: dequantizes the block (b is changed), transforms
 * it and stores it in the plane of its component. bx and by count blocks of
 * the component, which has comp.h * mxx of them across and comp.v * myy
 * down, as its plane has (in flex mode every sample is spread over expand_h
 * by expand_v pixels of a plane of the full size): the block is inside the
 * plane wherever the scans and reconstructProgressiveImage take bx and by
 * from, so nothing is checked here, as nothing panics in Go.
 */
static void bdfi_jpeg_reconstruct_block(bdfi_jpeg_decoder *d, int32_t *b, int64_t bx, int64_t by,
                                        int comp_index) {
  const int32_t *qt = d->t->quant[d->comp[comp_index].tq];
  for (int zig = 0; zig < BDFI_JPEG_BLOCK_SIZE; zig++) {
    b[bdfi_jpeg_unzig[zig]] = bdfi_jpeg_mul(b[bdfi_jpeg_unzig[zig]], qt[zig]);
  }
  bdfi_jpeg_idct(b);

  size_t h = 0, v = 0;
  if (d->flex) {
    h = (size_t)d->comp[comp_index].expand_h;
    v = (size_t)d->comp[comp_index].expand_v;
    bx *= (int64_t)h;
    by *= (int64_t)v;
  }

  /* a one-component image has its gray where the others have Y */
  size_t stride = d->stride[comp_index];
  uint8_t *dst = d->plane[comp_index] + 8 * ((size_t)by * stride + (size_t)bx);

  if (d->flex) {
    for (size_t y = 0; y < 8; y++) {
      for (size_t x = 0; x < 8; x++) {
        int32_t c = b[8 * y + x] + 128;
        uint8_t val = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
        for (size_t yy = 0; yy < v; yy++) {
          for (size_t xx = 0; xx < h; xx++) {
            dst[(y * v + yy) * stride + x * h + xx] = val;
          }
        }
      }
    }
    return;
  }

  /* level shift, clip, store */
  for (size_t y = 0; y < 8; y++) {
    uint8_t *row = dst + y * stride;
    for (size_t x = 0; x < 8; x++) {
      int32_t c = b[8 * y + x] + 128;
      row[x] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
    }
  }
}

/* decoder.refineNonZeroes: refines the coefficients of b that are not zero,
 * in zig-zag order, up to the nz-th zero one if nz >= 0. It returns where it
 * stopped. */
static bdf_status bdfi_jpeg_refine_non_zeroes(bdfi_jpeg_decoder *d, int32_t *b, int32_t zig,
                                              int32_t zig_end, int32_t nz, int32_t delta,
                                              int32_t *stopped) {
  for (; zig <= zig_end; zig++) {
    int u = bdfi_jpeg_unzig[zig];
    if (b[u] == 0) {
      if (nz == 0) {
        break;
      }
      nz--;
      continue;
    }
    int bit = 0;
    if (bdfi_jpeg_decode_bit(d, &bit) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    if (!bit) {
      continue;
    }
    if (b[u] >= 0) {
      b[u] = bdfi_jpeg_add(b[u], delta);
    } else {
      b[u] = bdfi_jpeg_sub(b[u], delta);
    }
  }
  *stopped = zig;
  return BDF_OK;
}

/* decoder.refine: a block of a successive approximation refinement scan. */
static bdf_status bdfi_jpeg_refine(bdfi_jpeg_decoder *d, int32_t *b, const bdfi_jpeg_huffman *h,
                                   int32_t zig_start, int32_t zig_end, int32_t delta) {
  /* A DC coefficient gets one bit (zig_end is 0 too: processSOS refused the
   * other scans that start at 0). */
  if (zig_start == 0) {
    int bit = 0;
    if (bdfi_jpeg_decode_bit(d, &bit) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    if (bit) {
      b[0] = bdfi_jpeg_i32((uint32_t)b[0] | (uint32_t)delta);
    }
    return BDF_OK;
  }

  int32_t zig = zig_start;
  if (d->eob_run == 0) {
    for (; zig <= zig_end; zig++) {
      int32_t z = 0;
      uint8_t value = 0;
      if (bdfi_jpeg_decode_huffman(d, h, &value) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
      uint8_t val0 = (uint8_t)(value >> 4);
      uint8_t val1 = (uint8_t)(value & 0x0f);

      if (val1 == 0) {
        if (val0 != 0x0f) {
          d->eob_run = (uint16_t)(1u << val0);
          if (val0 != 0) {
            uint32_t bits = 0;
            if (bdfi_jpeg_decode_bits(d, (int32_t)val0, &bits) != BDF_OK) {
              return BDF_ERR_FORMAT;
            }
            d->eob_run = (uint16_t)(d->eob_run | bits);
          }
          break;
        }
      } else if (val1 == 1) {
        int bit = 0;
        if (bdfi_jpeg_decode_bit(d, &bit) != BDF_OK) {
          return BDF_ERR_FORMAT;
        }
        z = bit ? delta : -delta;
      } else {
        return BDF_ERR_FORMAT; /* no such code in a refinement */
      }

      if (bdfi_jpeg_refine_non_zeroes(d, b, zig, zig_end, (int32_t)val0, delta, &zig) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
      if (zig > zig_end) {
        return BDF_ERR_FORMAT; /* more coefficients than the band has */
      }
      if (z != 0) {
        b[bdfi_jpeg_unzig[zig]] = z;
      }
    }
  }
  if (d->eob_run > 0) {
    d->eob_run--;
    if (bdfi_jpeg_refine_non_zeroes(d, b, zig, zig_end, -1, delta, &zig) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
  }
  return BDF_OK;
}

/*
 * decoder.findRST: skips to the restart marker that is expected. d->tmp has
 * the next two bytes of the data. Fill bytes and stuffed 0xff are passed
 * over; any other marker is an error.
 */
static bdf_status bdfi_jpeg_find_rst(bdfi_jpeg_decoder *d, uint8_t expected_rst) {
  for (;;) {
    /* at the bottom 2-i bytes are read into tmp[i:2] */
    size_t i = 0;
    if (d->tmp[0] == 0xff) {
      if (d->tmp[1] == expected_rst) {
        return BDF_OK;
      } else if (d->tmp[1] == 0xff) {
        i = 1;
      } else if (d->tmp[1] != 0x00) {
        return BDF_ERR_FORMAT;
      }
    } else if (d->tmp[1] == 0xff) {
      d->tmp[0] = 0xff;
      i = 1;
    }
    if (bdfi_jpeg_read_full(d, d->tmp + i, 2 - i) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
  }
}

/* decoder.processSOS: a scan header and the data of the scan. */
static bdf_status bdfi_jpeg_process_sos(bdfi_jpeg_decoder *d, int32_t n) {
  if (d->n_comp == 0) {
    return BDF_ERR_FORMAT; /* no frame yet */
  }
  if (n < 6 || 4 + 2 * d->n_comp < n || n % 2 != 0) {
    return BDF_ERR_FORMAT;
  }
  if (bdfi_jpeg_read_full(d, d->tmp, (size_t)n) != BDF_OK) {
    return BDF_ERR_FORMAT;
  }
  int32_t n_comp = (int32_t)d->tmp[0];
  if (n != 4 + 2 * n_comp) {
    return BDF_ERR_FORMAT;
  }
  bdfi_jpeg_scan_component scan[BDFI_JPEG_MAX_COMPONENTS];
  memset(scan, 0, sizeof scan);
  int32_t total_hv = 0;
  for (int i = 0; i < n_comp; i++) {
    uint8_t cs = d->tmp[1 + 2 * i];
    int comp_index = -1;
    for (int j = 0; j < d->n_comp; j++) {
      if (cs == d->comp[j].c) {
        comp_index = j;
      }
    }
    if (comp_index < 0) {
      return BDF_ERR_FORMAT;
    }
    scan[i].comp_index = (uint8_t)comp_index;
    for (int j = 0; j < i; j++) {
      if (scan[i].comp_index == scan[j].comp_index) {
        return BDF_ERR_FORMAT; /* a component twice */
      }
    }
    total_hv += d->comp[comp_index].h * d->comp[comp_index].v;

    /* a baseline image has two tables of each kind */
    scan[i].td = (uint8_t)(d->tmp[2 + 2 * i] >> 4);
    if (scan[i].td > BDFI_JPEG_MAX_TH || (d->baseline && scan[i].td > 1)) {
      return BDF_ERR_FORMAT;
    }
    scan[i].ta = (uint8_t)(d->tmp[2 + 2 * i] & 0x0f);
    if (scan[i].ta > BDFI_JPEG_MAX_TH || (d->baseline && scan[i].ta > 1)) {
      return BDF_ERR_FORMAT;
    }
  }
  if (d->n_comp > 1 && total_hv > 10) {
    return BDF_ERR_FORMAT;
  }

  /* The spectral selection (Ss, Se) and the successive approximation (Ah,
   * Al): 0, 63, 0, 0 unless the image is progressive. */
  int32_t zig_start = 0, zig_end = BDFI_JPEG_BLOCK_SIZE - 1;
  uint32_t ah = 0, al = 0;
  if (d->progressive) {
    zig_start = (int32_t)d->tmp[1 + 2 * n_comp];
    zig_end = (int32_t)d->tmp[2 + 2 * n_comp];
    ah = (uint32_t)(d->tmp[3 + 2 * n_comp] >> 4);
    al = (uint32_t)(d->tmp[3 + 2 * n_comp] & 0x0f);
    if ((zig_start == 0 && zig_end != 0) || zig_start > zig_end ||
        BDFI_JPEG_BLOCK_SIZE <= zig_end) {
      return BDF_ERR_FORMAT;
    }
    if (zig_start != 0 && n_comp != 1) {
      return BDF_ERR_FORMAT; /* AC coefficients of several components */
    }
    if (ah != 0 && ah != al + 1) {
      return BDF_ERR_FORMAT;
    }
  }

  /* the image in MCUs */
  int64_t mxx = ((int64_t)d->width + 8 * d->max_h - 1) / (8 * d->max_h);
  int64_t myy = ((int64_t)d->height + 8 * d->max_v - 1) / (8 * d->max_v);
  if (!d->has_img) {
    bdf_status st = bdfi_jpeg_make_img(d, mxx, myy);
    if (st != BDF_OK) {
      return st;
    }
  }
  if (d->progressive) {
    for (int i = 0; i < n_comp; i++) {
      int comp_index = scan[i].comp_index;
      if (!d->prog_coeffs[comp_index]) {
        size_t blocks = (size_t)(mxx * myy * d->comp[comp_index].h * d->comp[comp_index].v);
        if (blocks > SIZE_MAX / BDFI_JPEG_BLOCK_SIZE) {
          return BDF_ERR_NOMEM;
        }
        d->prog_coeffs[comp_index] =
            bdfi_calloc_array(blocks * BDFI_JPEG_BLOCK_SIZE, sizeof(int32_t));
        if (!d->prog_coeffs[comp_index]) {
          return BDF_ERR_NOMEM;
        }
      }
    }
  }

  d->bits_a = 0;
  d->bits_m = 0;
  d->bits_n = 0;
  int64_t mcu = 0;
  uint8_t expected_rst = BDFI_JPEG_RST0;
  int32_t block[BDFI_JPEG_BLOCK_SIZE]; /* the coefficients of a block that is not kept */
  int32_t dc[BDFI_JPEG_MAX_COMPONENTS];
  memset(dc, 0, sizeof dc);
  int64_t bx = 0, by = 0, block_count = 0;
  for (int64_t my = 0; my < myy; my++) {
    for (int64_t mx = 0; mx < mxx; mx++) {
      for (int i = 0; i < n_comp; i++) {
        int comp_index = scan[i].comp_index;
        int64_t hi = d->comp[comp_index].h;
        int64_t vi = d->comp[comp_index].v;
        for (int64_t j = 0; j < hi * vi; j++) {
          /* The blocks of an interleaved scan come MCU by MCU. A scan of one
           * component has them row by row, and has none for the blocks that
           * are outside the image, which Go tells by the block's place as if
           * the component were not subsampled. */
          if (n_comp != 1) {
            bx = hi * mx + j % hi;
            by = vi * my + j / hi;
          } else {
            int64_t q = mxx * hi;
            bx = block_count % q;
            by = block_count / q;
            block_count++;
            if (bx * 8 >= d->width || by * 8 >= d->height) {
              continue;
            }
          }

          /* What the scans before decoded of the block, if they are kept:
           * Go copies the block out and back, which comes to the same, an
           * error in between ending the decoding. */
          int32_t *b = block;
          if (d->progressive) {
            b = d->prog_coeffs[comp_index] + (size_t)(by * mxx * hi + bx) * BDFI_JPEG_BLOCK_SIZE;
          } else {
            memset(block, 0, sizeof block);
          }

          if (ah != 0) {
            if (bdfi_jpeg_refine(d, b, &d->t->huff[BDFI_JPEG_AC_TABLE][scan[i].ta], zig_start,
                                 zig_end, (int32_t)1 << al) != BDF_OK) {
              return BDF_ERR_FORMAT;
            }
          } else {
            int32_t zig = zig_start;
            if (zig == 0) {
              zig++;
              /* the DC coefficient: a difference from the one before */
              uint8_t value = 0;
              if (bdfi_jpeg_decode_huffman(d, &d->t->huff[BDFI_JPEG_DC_TABLE][scan[i].td],
                                           &value) != BDF_OK) {
                return BDF_ERR_FORMAT;
              }
              if (value > 16) {
                return BDF_ERR_FORMAT;
              }
              int32_t dc_delta = 0;
              if (bdfi_jpeg_receive_extend(d, value, &dc_delta) != BDF_OK) {
                return BDF_ERR_FORMAT;
              }
              dc[comp_index] = bdfi_jpeg_add(dc[comp_index], dc_delta);
              b[0] = bdfi_jpeg_shl(dc[comp_index], al);
            }

            /* The run of blocks without more coefficients is the decoder's:
             * it outlives the scan, and a sequential scan counts it too. */
            if (zig <= zig_end && d->eob_run > 0) {
              d->eob_run--;
            } else {
              /* the AC coefficients */
              const bdfi_jpeg_huffman *huff = &d->t->huff[BDFI_JPEG_AC_TABLE][scan[i].ta];
              for (; zig <= zig_end; zig++) {
                uint8_t value = 0;
                if (bdfi_jpeg_decode_huffman(d, huff, &value) != BDF_OK) {
                  return BDF_ERR_FORMAT;
                }
                uint8_t val0 = (uint8_t)(value >> 4);
                uint8_t val1 = (uint8_t)(value & 0x0f);
                if (val1 != 0) {
                  zig += (int32_t)val0;
                  if (zig > zig_end) {
                    break;
                  }
                  int32_t ac = 0;
                  if (bdfi_jpeg_receive_extend(d, val1, &ac) != BDF_OK) {
                    return BDF_ERR_FORMAT;
                  }
                  b[bdfi_jpeg_unzig[zig]] = bdfi_jpeg_shl(ac, al);
                } else {
                  if (val0 != 0x0f) {
                    d->eob_run = (uint16_t)(1u << val0);
                    if (val0 != 0) {
                      uint32_t bits = 0;
                      if (bdfi_jpeg_decode_bits(d, (int32_t)val0, &bits) != BDF_OK) {
                        return BDF_ERR_FORMAT;
                      }
                      d->eob_run = (uint16_t)(d->eob_run | bits);
                    }
                    d->eob_run--;
                    break;
                  }
                  zig += 0x0f;
                }
              }
            }
          }

          /* a progressive image is transformed when all its scans are read */
          if (!d->progressive) {
            bdfi_jpeg_reconstruct_block(d, b, bx, by, comp_index);
          }
        }
      }
      mcu++;
      if (d->ri > 0 && mcu % d->ri == 0 && mcu < mxx * myy) {
        /* A restart marker follows, or the data is damaged and findRST
         * looks for it. */
        if (bdfi_jpeg_read_full(d, d->tmp, 2) != BDF_OK) {
          return BDF_ERR_FORMAT;
        }
        if (d->tmp[0] != 0xff || d->tmp[1] != expected_rst) {
          if (bdfi_jpeg_find_rst(d, expected_rst) != BDF_OK) {
            return BDF_ERR_FORMAT;
          }
        }
        expected_rst++;
        if (expected_rst == BDFI_JPEG_RST7 + 1) {
          expected_rst = BDFI_JPEG_RST0;
        }
        d->bits_a = 0;
        d->bits_m = 0;
        d->bits_n = 0;
        memset(dc, 0, sizeof dc);
        d->eob_run = 0;
      }
    }
  }
  return BDF_OK;
}

/* decoder.reconstructProgressiveImage: transforms the blocks the scans of a
 * progressive image left, those of the components that had a scan. */
static void bdfi_jpeg_reconstruct_progressive_image(bdfi_jpeg_decoder *d) {
  int64_t mxx = ((int64_t)d->width + 8 * d->max_h - 1) / (8 * d->max_h);
  for (int i = 0; i < d->n_comp; i++) {
    if (!d->prog_coeffs[i]) {
      continue;
    }
    int64_t v = 8 * d->max_v / d->comp[i].v;
    int64_t h = 8 * d->max_h / d->comp[i].h;
    int64_t stride = mxx * d->comp[i].h;
    for (int64_t by = 0; by * v < d->height; by++) {
      for (int64_t bx = 0; bx * h < d->width; bx++) {
        bdfi_jpeg_reconstruct_block(
            d, d->prog_coeffs[i] + (size_t)(by * stride + bx) * BDFI_JPEG_BLOCK_SIZE, bx, by, i);
      }
    }
  }
}

/* ---- The segments of a file (decoder.decode) ----------------------------- */

/*
 * decoder.decode up to the End Of Image marker: BDF_OK where Go goes on to
 * make the image, or, when only the size is asked for (config_only), where
 * Go returns without an error: at the frame header of a JFIF image and at
 * the first scan otherwise.
 */
static bdf_status bdfi_jpeg_read_segments(bdfi_jpeg_decoder *d, int config_only) {
  /* the Start Of Image marker */
  if (bdfi_jpeg_read_full(d, d->tmp, 2) != BDF_OK) {
    return BDF_ERR_FORMAT;
  }
  if (d->tmp[0] != 0xff || d->tmp[1] != BDFI_JPEG_SOI) {
    return BDF_ERR_FORMAT;
  }

  for (;;) {
    if (bdfi_jpeg_read_full(d, d->tmp, 2) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    while (d->tmp[0] != 0xff) {
      /* what is between the segments is passed over, as libjpeg does */
      d->tmp[0] = d->tmp[1];
      if (bdfi_jpeg_read_byte(d, &d->tmp[1]) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
    }
    uint8_t marker = d->tmp[1];
    if (marker == 0) {
      /* a stuffed 0xff that the scan left */
      continue;
    }
    while (marker == 0xff) {
      /* fill bytes before a marker */
      if (bdfi_jpeg_read_byte(d, &marker) != BDF_OK) {
        return BDF_ERR_FORMAT;
      }
    }
    if (marker == BDFI_JPEG_EOI) {
      break;
    }
    if (BDFI_JPEG_RST0 <= marker && marker <= BDFI_JPEG_RST7) {
      /* a restart marker after the last interval: it has no length */
      continue;
    }

    /* the length of the segment, itself included */
    if (bdfi_jpeg_read_full(d, d->tmp, 2) != BDF_OK) {
      return BDF_ERR_FORMAT;
    }
    int32_t n = ((int32_t)d->tmp[0] << 8) + (int32_t)d->tmp[1] - 2;
    if (n < 0) {
      return BDF_ERR_FORMAT;
    }

    bdf_status st = BDF_OK;
    switch (marker) {
    case BDFI_JPEG_SOF0:
    case BDFI_JPEG_SOF1:
    case BDFI_JPEG_SOF2:
      d->baseline = marker == BDFI_JPEG_SOF0;
      d->progressive = marker == BDFI_JPEG_SOF2;
      st = bdfi_jpeg_process_sof(d, n);
      if (config_only && d->jfif) {
        return st;
      }
      break;
    case BDFI_JPEG_DHT:
      st = config_only ? bdfi_jpeg_ignore(d, n) : bdfi_jpeg_process_dht(d, n);
      break;
    case BDFI_JPEG_DQT:
      st = config_only ? bdfi_jpeg_ignore(d, n) : bdfi_jpeg_process_dqt(d, n);
      break;
    case BDFI_JPEG_SOS:
      if (config_only) {
        return BDF_OK;
      }
      st = bdfi_jpeg_process_sos(d, n);
      break;
    case BDFI_JPEG_DRI:
      st = config_only ? bdfi_jpeg_ignore(d, n) : bdfi_jpeg_process_dri(d, n);
      break;
    case BDFI_JPEG_APP0:
      st = bdfi_jpeg_process_app0(d, n);
      break;
    case BDFI_JPEG_APP14:
      st = bdfi_jpeg_process_app14(d, n);
      break;
    default:
      if ((BDFI_JPEG_APP0 <= marker && marker <= BDFI_JPEG_APP15) || marker == BDFI_JPEG_COM) {
        st = bdfi_jpeg_ignore(d, n);
      } else {
        /* arithmetic coding, lossless and hierarchical frames, and whatever
         * else is not above */
        st = BDF_ERR_FORMAT;
      }
      break;
    }
    if (st != BDF_OK) {
      return st;
    }
  }

  /* Without a scan there is no image, and with only the size asked for no
   * scan was read: "missing SOS marker". */
  if (config_only) {
    return BDF_ERR_FORMAT;
  }
  return BDF_OK;
}

/* ---- The image as draw.Draw converts it ---------------------------------- */

/* color.YCbCrToRGB as imageutil.DrawYCbCr has it inline: p[0..2] get R, G
 * and B. (YCbCr.RGBA, which draw.Draw goes through for the ratios DrawYCbCr
 * does not know, gives the same 8 bits.) */
static void bdfi_jpeg_ycbcr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t *p) {
  int32_t yy1 = (int32_t)y * 0x10101;
  int32_t cb1 = (int32_t)cb - 128;
  int32_t cr1 = (int32_t)cr - 128;

  /* Go: if uint32(r)&0xff000000 == 0 { r >>= 16 } else { r = ^(r >> 31) },
   * and of that the low 8 bits: 0 below the range, 255 above it. */
  int32_t r = yy1 + 91881 * cr1;
  if (((uint32_t)r & 0xff000000u) == 0) {
    r >>= 16;
  } else {
    r = r < 0 ? 0 : 255;
  }
  int32_t g = yy1 - 22554 * cb1 - 46802 * cr1;
  if (((uint32_t)g & 0xff000000u) == 0) {
    g >>= 16;
  } else {
    g = g < 0 ? 0 : 255;
  }
  int32_t b = yy1 + 116130 * cb1;
  if (((uint32_t)b & 0xff000000u) == 0) {
    b >>= 16;
  } else {
    b = b < 0 ? 0 : 255;
  }
  p[0] = (uint8_t)r;
  p[1] = (uint8_t)g;
  p[2] = (uint8_t)b;
}

/* color.CMYKToRGB as drawCMYK calls it: p[0..2] get R, G and B. */
static void bdfi_jpeg_cmyk_to_rgb(uint8_t c, uint8_t m, uint8_t y, uint8_t k, uint8_t *p) {
  uint32_t w = 0xffff - (uint32_t)k * 0x101;
  uint32_t r = (0xffff - (uint32_t)c * 0x101) * w / 0xffff;
  uint32_t g = (0xffff - (uint32_t)m * 0x101) * w / 0xffff;
  uint32_t b = (0xffff - (uint32_t)y * 0x101) * w / 0xffff;
  p[0] = (uint8_t)(r >> 8);
  p[1] = (uint8_t)(g >> 8);
  p[2] = (uint8_t)(b >> 8);
}

/* How far apart the chroma samples of d.img3 are: YCbCr.COffset is
 * (y >> *vshift) * stride + (x >> *hshift) for an image that starts at
 * (0, 0). */
static void bdfi_jpeg_chroma_shifts(const bdfi_jpeg_decoder *d, uint32_t *hshift,
                                    uint32_t *vshift) {
  *hshift = 0;
  *vshift = 0;
  switch (d->ratio) {
  case BDFI_JPEG_RATIO_422:
    *hshift = 1;
    break;
  case BDFI_JPEG_RATIO_420:
    *hshift = 1;
    *vshift = 1;
    break;
  case BDFI_JPEG_RATIO_440:
    *vshift = 1;
    break;
  case BDFI_JPEG_RATIO_411:
    *hshift = 2;
    break;
  case BDFI_JPEG_RATIO_410:
    *hshift = 2;
    *vshift = 1;
    break;
  default:
    break;
  }
}

/* drawGray in image/draw: the *image.Gray of a one-component image. */
static void bdfi_jpeg_draw_gray(const bdfi_jpeg_decoder *d, uint8_t *pix) {
  size_t w = (size_t)d->width, h = (size_t)d->height;
  for (size_t y = 0; y < h; y++) {
    const uint8_t *src = d->plane[BDFI_JPEG_PLANE_Y] + y * d->stride[BDFI_JPEG_PLANE_Y];
    uint8_t *dst = pix + 4 * w * y;
    for (size_t x = 0; x < w; x++) {
      dst[4 * x + 0] = src[x];
      dst[4 * x + 1] = src[x];
      dst[4 * x + 2] = src[x];
      dst[4 * x + 3] = 255;
    }
  }
}

/*
 * The *image.YCbCr of a three-component image, d.img3: imageutil.DrawYCbCr
 * for 4:4:4, 4:2:2, 4:2:0 and 4:4:0, and for 4:1:1 and 4:1:0, which it
 * leaves to drawRGBA, RGBA64At of every pixel and the high bytes of that,
 * which are the same. Alpha is 255.
 */
static void bdfi_jpeg_draw_ycbcr(const bdfi_jpeg_decoder *d, uint8_t *pix) {
  size_t w = (size_t)d->width, h = (size_t)d->height;
  uint32_t hshift, vshift;
  bdfi_jpeg_chroma_shifts(d, &hshift, &vshift);
  for (size_t y = 0; y < h; y++) {
    const uint8_t *sy = d->plane[BDFI_JPEG_PLANE_Y] + y * d->stride[BDFI_JPEG_PLANE_Y];
    size_t co = (y >> vshift) * d->stride[BDFI_JPEG_PLANE_CB];
    const uint8_t *scb = d->plane[BDFI_JPEG_PLANE_CB] + co;
    const uint8_t *scr = d->plane[BDFI_JPEG_PLANE_CR] + co;
    uint8_t *dst = pix + 4 * w * y;
    for (size_t x = 0; x < w; x++) {
      size_t ci = x >> hshift;
      bdfi_jpeg_ycbcr_to_rgb(sy[x], scb[ci], scr[ci], dst + 4 * x);
      dst[4 * x + 3] = 255;
    }
  }
}

/* What decoder.convertToRGB asks of the sampling factors: G and B sampled
 * alike, and R a whole number of times as much. */
static int bdfi_jpeg_rgb_sampling_ok(const bdfi_jpeg_decoder *d) {
  int32_t h0 = d->comp[0].h, h1 = d->comp[1].h, h2 = d->comp[2].h;
  int32_t v0 = d->comp[0].v, v1 = d->comp[1].v, v2 = d->comp[2].v;
  return !(h1 != h2 || h0 % h1 != 0 || v1 != v2 || v0 % v1 != 0);
}

/*
 * decoder.convertToRGB: the *image.RGBA of a three-component image whose
 * components are R, G and B, which the reference takes as it is. The G and
 * B samples are found as Go finds them: by the row of the chroma planes
 * (COffset) and the ratio of the horizontal sampling factors, also where
 * the planes are the spread ones of flex mode and that is not where the
 * sample is.
 */
static void bdfi_jpeg_convert_to_rgb(const bdfi_jpeg_decoder *d, uint8_t *pix) {
  size_t c_scale = (size_t)(d->comp[0].h / d->comp[1].h);
  size_t w = (size_t)d->width, h = (size_t)d->height;
  uint32_t hshift, vshift;
  bdfi_jpeg_chroma_shifts(d, &hshift, &vshift);
  for (size_t y = 0; y < h; y++) {
    const uint8_t *sy = d->plane[BDFI_JPEG_PLANE_Y] + y * d->stride[BDFI_JPEG_PLANE_Y];
    size_t co = (y >> vshift) * d->stride[BDFI_JPEG_PLANE_CB];
    const uint8_t *scb = d->plane[BDFI_JPEG_PLANE_CB] + co;
    const uint8_t *scr = d->plane[BDFI_JPEG_PLANE_CR] + co;
    uint8_t *dst = pix + 4 * w * y;
    for (size_t x = 0; x < w; x++) {
      dst[4 * x + 0] = sy[x];
      dst[4 * x + 1] = scb[x / c_scale];
      dst[4 * x + 2] = scr[x / c_scale];
      dst[4 * x + 3] = 255;
    }
  }
}

/*
 * decoder.applyBlack and drawCMYK: the *image.CMYK of a four-component
 * image. Adobe's segment says whether the components are YCbCrK (Y, Cb and
 * Cr converted to RGB are C, M and Y, and K is inverted) or CMYK (all four
 * inverted, M and Y at half the resolution when C and K have two samples
 * each way). Without the segment Go refuses the image: bdfi_jpeg_finish
 * has seen to that.
 */
static void bdfi_jpeg_apply_black(const bdfi_jpeg_decoder *d, uint8_t *pix) {
  size_t w = (size_t)d->width, h = (size_t)d->height;
  const uint8_t *py = d->plane[BDFI_JPEG_PLANE_Y], *pcb = d->plane[BDFI_JPEG_PLANE_CB];
  const uint8_t *pcr = d->plane[BDFI_JPEG_PLANE_CR], *pk = d->plane[BDFI_JPEG_PLANE_K];
  size_t ys = d->stride[BDFI_JPEG_PLANE_Y], cs = d->stride[BDFI_JPEG_PLANE_CB];
  size_t ks = d->stride[BDFI_JPEG_PLANE_K];

  if (d->adobe_transform != 0) {
    /* YCbCrK: imageutil.DrawYCbCr of d.img3, which is 4:4:4 or 4:2:0 */
    uint32_t hshift, vshift;
    bdfi_jpeg_chroma_shifts(d, &hshift, &vshift);
    for (size_t y = 0; y < h; y++) {
      size_t co = (y >> vshift) * cs;
      uint8_t *dst = pix + 4 * w * y;
      for (size_t x = 0; x < w; x++) {
        uint8_t cmy[3];
        size_t ci = co + (x >> hshift);
        bdfi_jpeg_ycbcr_to_rgb(py[y * ys + x], pcb[ci], pcr[ci], cmy);
        uint8_t k = (uint8_t)(255 - pk[y * ks + x]);
        bdfi_jpeg_cmyk_to_rgb(cmy[0], cmy[1], cmy[2], k, dst + 4 * x);
        dst[4 * x + 3] = 255;
      }
    }
    return;
  }

  /* CMYK: a component sampled differently from the first has half of it */
  uint32_t sub1 = (d->comp[1].h != d->comp[0].h || d->comp[1].v != d->comp[0].v) ? 1 : 0;
  uint32_t sub2 = (d->comp[2].h != d->comp[0].h || d->comp[2].v != d->comp[0].v) ? 1 : 0;
  uint32_t sub3 = (d->comp[3].h != d->comp[0].h || d->comp[3].v != d->comp[0].v) ? 1 : 0;
  for (size_t y = 0; y < h; y++) {
    uint8_t *dst = pix + 4 * w * y;
    for (size_t x = 0; x < w; x++) {
      uint8_t c = (uint8_t)(255 - py[y * ys + x]);
      uint8_t m = (uint8_t)(255 - pcb[(y >> sub1) * cs + (x >> sub1)]);
      uint8_t yl = (uint8_t)(255 - pcr[(y >> sub2) * cs + (x >> sub2)]);
      uint8_t k = (uint8_t)(255 - pk[(y >> sub3) * ks + (x >> sub3)]);
      bdfi_jpeg_cmyk_to_rgb(c, m, yl, k, dst + 4 * x);
      dst[4 * x + 3] = 255;
    }
  }
}

/* decoder.isRGB: whether three components are R, G and B and not Y, Cb and
 * Cr. A JFIF image never is; Adobe's segment says so with transform 0; and
 * otherwise the identifiers do. */
static int bdfi_jpeg_is_rgb(const bdfi_jpeg_decoder *d) {
  if (d->jfif) {
    return 0;
  }
  if (d->adobe_transform_valid && d->adobe_transform == 0) {
    return 1;
  }
  return d->comp[0].c == 'R' && d->comp[1].c == 'G' && d->comp[2].c == 'B';
}

/* The end of decoder.decode, after the End Of Image marker, and the
 * conversion of the image it returns. */
static bdf_status bdfi_jpeg_finish(bdfi_jpeg_decoder *d, bdfi_bitmap *out) {
  if (d->progressive) {
    bdfi_jpeg_reconstruct_progressive_image(d);
  }
  if (!d->has_img) {
    return BDF_ERR_FORMAT; /* no scan */
  }
  /* what applyBlack and convertToRGB refuse */
  int rgb = d->n_comp == 3 && bdfi_jpeg_is_rgb(d);
  if (d->n_comp == 4 && !d->adobe_transform_valid) {
    return BDF_ERR_FORMAT;
  }
  if (rgb && !bdfi_jpeg_rgb_sampling_ok(d)) {
    return BDF_ERR_FORMAT;
  }

  uint8_t *pix = bdfi_malloc_array((size_t)d->width * (size_t)d->height, 4);
  if (!pix) {
    return BDF_ERR_NOMEM;
  }
  if (d->n_comp == 1) {
    bdfi_jpeg_draw_gray(d, pix);
  } else if (d->n_comp == 4) {
    bdfi_jpeg_apply_black(d, pix);
  } else if (rgb) {
    bdfi_jpeg_convert_to_rgb(d, pix);
  } else {
    bdfi_jpeg_draw_ycbcr(d, pix);
  }
  out->w = d->width;
  out->h = d->height;
  out->pix = pix;
  return BDF_OK;
}

/* ---- The decoder --------------------------------------------------------- */

/* jpeg.DecodeConfig: the size the frame header states. */
BDF_INTERNAL int bdfi_jpeg_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h) {
  bdfi_jpeg_decoder d;
  memset(&d, 0, sizeof d);
  d.data = data;
  d.len = len;
  *w = 0;
  *h = 0;
  /* a frame header was read where n_comp is set: "missing SOF marker"
   * otherwise */
  if (bdfi_jpeg_read_segments(&d, 1) != BDF_OK || d.n_comp == 0) {
    return 0;
  }
  *w = d.width;
  *h = d.height;
  return 1;
}

/* imgconv.Decode of the reference with jpeg.Decode, and the conversion of
 * the image. */
BDF_INTERNAL bdf_status bdfi_jpeg_decode(const uint8_t *data, size_t len, bdfi_bitmap *out) {
  memset(out, 0, sizeof *out);

  /* The size check is on what DecodeConfig reads, whatever Decode would
   * make of the file. (Where DecodeConfig fails, Decode fails before it
   * allocates an image: it reads the same segments more strictly.) */
  int32_t w = 0, h = 0;
  if (bdfi_jpeg_config(data, len, &w, &h)) {
    if (w <= 0 || h <= 0 || (int64_t)w > BDFI_MAX_DECODE_PIXELS / (int64_t)h) {
      return BDF_ERR_TOO_LARGE;
    }
  }

  bdfi_jpeg_decoder d;
  memset(&d, 0, sizeof d);
  d.data = data;
  d.len = len;
  d.t = bdfi_calloc_array(1, sizeof *d.t);
  if (!d.t) {
    return BDF_ERR_NOMEM;
  }
  bdf_status st = bdfi_jpeg_read_segments(&d, 0);
  if (st == BDF_OK) {
    st = bdfi_jpeg_finish(&d, out);
  }
  for (int i = 0; i < BDFI_JPEG_MAX_COMPONENTS; i++) {
    free(d.plane[i]);
    free(d.prog_coeffs[i]);
  }
  free(d.t);
  return st;
}
