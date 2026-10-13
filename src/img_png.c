/*
 * The PNG decoder of the rasterizer: a port of Go's image/png (BSD license,
 * see third_party/go-image/LICENSE), which the reference renderer decodes
 * with, so that an image is the same pixels here and is refused here when
 * it is refused there. What the port keeps on purpose: chunks are read in
 * the reference's order with its checks, the checksum of every chunk read
 * is verified, and the image data is taken from its chunks in the
 * stretches a buffered reader takes them, because whether bytes are left
 * in the last chunk afterwards decides whether the image is refused.
 *
 * The pixels come out as the reference converts them (draw.Draw into an
 * *image.RGBA): premultiplied, 8 bits a channel.
 */
#include <stdlib.h>
#include <string.h>

#include "image_internal.h"

/* A combination of colour type and bit depth (cb in reader.go). */
enum {
  BDFI_PNG_INVALID,
  BDFI_PNG_G1,
  BDFI_PNG_G2,
  BDFI_PNG_G4,
  BDFI_PNG_G8,
  BDFI_PNG_GA8,
  BDFI_PNG_TC8,
  BDFI_PNG_P1,
  BDFI_PNG_P2,
  BDFI_PNG_P4,
  BDFI_PNG_P8,
  BDFI_PNG_TCA8,
  BDFI_PNG_G16,
  BDFI_PNG_GA16,
  BDFI_PNG_TC16,
  BDFI_PNG_TCA16
};

/* The decoding stages: the chunks have an order. */
enum {
  BDFI_PNG_START,
  BDFI_PNG_SEEN_IHDR,
  BDFI_PNG_SEEN_PLTE,
  BDFI_PNG_SEEN_TRNS,
  BDFI_PNG_SEEN_IDAT,
  BDFI_PNG_SEEN_IEND
};

typedef struct bdfi_png {
  const uint8_t *data; /* the file */
  size_t len, pos;
  uint32_t crc; /* of the chunk being read, not yet inverted */
  int32_t width, height;
  int depth, cb, stage, interlace;
  int palette_len;         /* the entries of PLTE, grown by tRNS */
  uint8_t palette[256][4]; /* r, g, b and alpha, not premultiplied */
  int use_transparent;
  uint8_t transparent[6];
  uint32_t idat_length; /* what is left of the IDAT chunk being read */
  bdfi_bitmap img;
  bdf_status status; /* why decoding failed, when it is not the format */
} bdfi_png;

static const uint32_t bdfi_png_crc_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u,
    0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu,
    0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du,
    0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u, 0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu,
    0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u,
    0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
    0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u, 0x26d930acu,
    0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu,
    0xb6662d3du, 0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu,
    0x9fbfe4a5u, 0xe8b8d433u, 0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu,
    0x086d3d2du, 0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
    0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau,
    0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u, 0x4db26158u, 0x3ab551ceu,
    0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au,
    0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u,
    0xce61e49fu, 0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
    0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u,
    0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u,
    0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u, 0xf00f9344u, 0x8708a3d2u, 0x1e01f268u,
    0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u,
    0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u,
    0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu,
    0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u,
    0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u,
    0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du, 0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au,
    0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu,
    0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
    0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u, 0x88085ae6u,
    0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du,
    0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u,
    0x47b2cf7fu, 0x30b5ffe9u, 0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u,
    0xcdd70693u, 0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
    0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static uint32_t bdfi_png_crc(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc = bdfi_png_crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }
  return crc;
}

static uint32_t bdfi_png_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* bdfi_png_take returns the next n bytes of the file, or NULL when it has
 * fewer (io.ReadFull failing). */
static const uint8_t *bdfi_png_take(bdfi_png *d, size_t n) {
  if (d->len - d->pos < n) {
    return NULL;
  }
  const uint8_t *p = d->data + d->pos;
  d->pos += n;
  return p;
}

/* decoder.verifyChecksum */
static int bdfi_png_verify(bdfi_png *d) {
  const uint8_t *p = bdfi_png_take(d, 4);
  return p && bdfi_png_be32(p) == (d->crc ^ 0xffffffffu);
}

/* bdfi_png_data reads the n data bytes of a chunk into its checksum. */
static const uint8_t *bdfi_png_data(bdfi_png *d, size_t n) {
  const uint8_t *p = bdfi_png_take(d, n);
  if (p) {
    d->crc = bdfi_png_crc(d->crc, p, n);
  }
  return p;
}

static int bdfi_png_paletted(int cb) {
  return BDFI_PNG_P1 <= cb && cb <= BDFI_PNG_P8;
}

static int bdfi_png_true_color(int cb) {
  return cb == BDFI_PNG_TC8 || cb == BDFI_PNG_TC16;
}

/* decoder.parseIHDR */
static int bdfi_png_ihdr(bdfi_png *d, uint32_t length) {
  static const uint8_t cbs[5][7] = {
      /* by depth (1, 2, 4, 8, 16) and colour type (0 to 6) */
      {BDFI_PNG_G1, 0, 0, BDFI_PNG_P1, 0, 0, 0},
      {BDFI_PNG_G2, 0, 0, BDFI_PNG_P2, 0, 0, 0},
      {BDFI_PNG_G4, 0, 0, BDFI_PNG_P4, 0, 0, 0},
      {BDFI_PNG_G8, 0, BDFI_PNG_TC8, BDFI_PNG_P8, BDFI_PNG_GA8, 0, BDFI_PNG_TCA8},
      {BDFI_PNG_G16, 0, BDFI_PNG_TC16, 0, BDFI_PNG_GA16, 0, BDFI_PNG_TCA16},
  };
  if (length != 13) {
    return 0;
  }
  const uint8_t *p = bdfi_png_data(d, 13);
  if (!p || p[10] != 0 || p[11] != 0 || p[12] > 1) {
    return 0;
  }
  d->interlace = p[12];
  int32_t w = (int32_t)bdfi_png_be32(p), h = (int32_t)bdfi_png_be32(p + 4);
  if (w <= 0 || h <= 0) {
    return 0;
  }
  d->cb = BDFI_PNG_INVALID;
  d->depth = p[8];
  int row = p[8] == 1 ? 0 : p[8] == 2 ? 1 : p[8] == 4 ? 2 : p[8] == 8 ? 3 : p[8] == 16 ? 4 : -1;
  if (row >= 0 && p[9] <= 6) {
    d->cb = cbs[row][p[9]];
  }
  if (d->cb == BDFI_PNG_INVALID) {
    return 0;
  }
  d->width = w;
  d->height = h;
  return bdfi_png_verify(d);
}

/* decoder.parsePLTE */
static int bdfi_png_plte(bdfi_png *d, uint32_t length) {
  uint32_t np = length / 3;
  if (length % 3 != 0 || np == 0 || np > 256 || np > (uint32_t)1 << d->depth) {
    return 0;
  }
  const uint8_t *p = bdfi_png_data(d, 3 * (size_t)np);
  if (!p) {
    return 0;
  }
  switch (d->cb) {
  case BDFI_PNG_P1:
  case BDFI_PNG_P2:
  case BDFI_PNG_P4:
  case BDFI_PNG_P8:
    /* the entries past the chunk's are opaque black: pixels may name them */
    for (uint32_t i = 0; i < 256; i++) {
      d->palette[i][0] = (uint8_t)(i < np ? p[3 * i] : 0);
      d->palette[i][1] = (uint8_t)(i < np ? p[3 * i + 1] : 0);
      d->palette[i][2] = (uint8_t)(i < np ? p[3 * i + 2] : 0);
      d->palette[i][3] = 0xff;
    }
    d->palette_len = (int)np;
    break;
  case BDFI_PNG_TC8:
  case BDFI_PNG_TCA8:
  case BDFI_PNG_TC16:
  case BDFI_PNG_TCA16:
    break; /* a palette that true colour has no use for */
  default:
    return 0;
  }
  return bdfi_png_verify(d);
}

/* decoder.parsetRNS */
static int bdfi_png_trns(bdfi_png *d, uint32_t length) {
  const uint8_t *p;
  switch (d->cb) {
  case BDFI_PNG_G1:
  case BDFI_PNG_G2:
  case BDFI_PNG_G4:
  case BDFI_PNG_G8:
  case BDFI_PNG_G16:
    if (length != 2 || !(p = bdfi_png_data(d, 2))) {
      return 0;
    }
    d->transparent[0] = p[0];
    d->transparent[1] = p[1];
    /* the sample is stretched to 8 bits as the pixels are, in a byte */
    if (d->cb == BDFI_PNG_G1) {
      d->transparent[1] = (uint8_t)(p[1] * 0xff);
    } else if (d->cb == BDFI_PNG_G2) {
      d->transparent[1] = (uint8_t)(p[1] * 0x55);
    } else if (d->cb == BDFI_PNG_G4) {
      d->transparent[1] = (uint8_t)(p[1] * 0x11);
    }
    d->use_transparent = 1;
    break;
  case BDFI_PNG_TC8:
  case BDFI_PNG_TC16:
    if (length != 6 || !(p = bdfi_png_data(d, 6))) {
      return 0;
    }
    memcpy(d->transparent, p, 6);
    d->use_transparent = 1;
    break;
  case BDFI_PNG_P1:
  case BDFI_PNG_P2:
  case BDFI_PNG_P4:
  case BDFI_PNG_P8:
    if (length > 256 || !(p = bdfi_png_data(d, length))) {
      return 0;
    }
    if (d->palette_len < (int)length) {
      d->palette_len = (int)length;
    }
    for (uint32_t i = 0; i < length; i++) {
      d->palette[i][3] = p[i];
    }
    break;
  default:
    return 0;
  }
  return bdfi_png_verify(d);
}

/*
 * bdfi_png_more is decoder.Read behind the bufio.Reader that zlib wraps it
 * in: the next stretch of image data, which is what is left of the IDAT
 * chunk being read, or want bytes of it. At the end of a chunk its checksum
 * is verified and the next chunk has to be an IDAT chunk.
 */
static int bdfi_png_more(bdfi_pull *in, size_t want) {
  bdfi_png *d = in->ctx;
  while (d->idat_length == 0) {
    if (!bdfi_png_verify(d)) {
      return 0;
    }
    const uint8_t *p = bdfi_png_take(d, 8);
    if (!p) {
      return 0;
    }
    d->idat_length = bdfi_png_be32(p);
    if (memcmp(p + 4, "IDAT", 4) != 0) {
      return 0;
    }
    d->crc = bdfi_png_crc(0xffffffffu, p + 4, 4);
  }
  size_t n = want;
  if (n > d->idat_length) {
    n = d->idat_length;
  }
  if (n > d->len - d->pos) {
    n = d->len - d->pos;
  }
  if (n == 0) {
    return 0;
  }
  in->p = bdfi_png_data(d, n);
  in->n = n;
  in->pos = 0;
  d->idat_length -= (uint32_t)n;
  return 1;
}

/* bdfi_png_pull_byte reads a byte as io.ReadFull through the buffer does. */
static int bdfi_png_pull_byte(bdfi_pull *in) {
  if (in->pos == in->n && !in->more(in, BDFI_PULL_BUFFER)) {
    return -1;
  }
  return in->p[in->pos++];
}

/* The placement and size of the passes of Adam7 interlacing. */
static const uint8_t bdfi_png_interlacing[7][4] = {
    /* xFactor, yFactor, xOffset, yOffset */
    {8, 8, 0, 0}, {8, 8, 4, 0}, {4, 8, 0, 4}, {4, 4, 2, 0},
    {2, 4, 0, 2}, {2, 2, 1, 0}, {1, 2, 0, 1},
};

static int bdfi_png_bits_per_pixel(const bdfi_png *d) {
  switch (d->cb) {
  case BDFI_PNG_GA8:
    return 16;
  case BDFI_PNG_TC8:
    return 24;
  case BDFI_PNG_TCA8:
    return 32;
  case BDFI_PNG_G16:
    return 16;
  case BDFI_PNG_GA16:
    return 32;
  case BDFI_PNG_TC16:
    return 48;
  case BDFI_PNG_TCA16:
    return 64;
  default:
    return d->depth;
  }
}

/* bdfi_png_pass gives the size of a pass (the whole image when it is not
 * interlaced) and its placement. A pass may be empty. */
static void bdfi_png_pass(const bdfi_png *d, int pass, int32_t *w, int32_t *h, const uint8_t **at) {
  static const uint8_t whole[4] = {1, 1, 0, 0};
  *at = whole;
  *w = d->width;
  *h = d->height;
  if (d->interlace) {
    const uint8_t *p = bdfi_png_interlacing[pass];
    *at = p;
    *w = (d->width - p[2] + p[0] - 1) / p[0];
    *h = (d->height - p[3] + p[1] - 1) / p[1];
    if (*w == 0 || *h == 0) {
      *w = 0;
      *h = 0;
    }
  }
}

static size_t bdfi_png_row_size(const bdfi_png *d, int32_t w) {
  return 1 + (size_t)(((int64_t)bdfi_png_bits_per_pixel(d) * w + 7) / 8);
}

/* filterPaeth in paeth.go */
static void bdfi_png_paeth(uint8_t *cdat, const uint8_t *pdat, size_t n, size_t bpp) {
  for (size_t i = 0; i < bpp; i++) {
    int a = 0, c = 0;
    for (size_t j = i; j < n; j += bpp) {
      int b = pdat[j];
      int pa = b - c, pb = a - c;
      int pc = abs(pa + pb);
      pa = abs(pa);
      pb = abs(pb);
      if (pa <= pb && pa <= pc) {
        /* a stays */
      } else if (pb <= pc) {
        a = b;
      } else {
        a = c;
      }
      a = (a + cdat[j]) & 0xff;
      cdat[j] = (uint8_t)a;
      c = b;
    }
  }
}

/* bdfi_png_unfilter undoes the filter of a row in place; pdat is the row
 * before it (zeroes for the first of a pass). 0 for a filter there is not. */
static int bdfi_png_unfilter(int filter, uint8_t *cdat, const uint8_t *pdat, size_t n, size_t bpp) {
  switch (filter) {
  case 0:
    break;
  case 1:
    for (size_t i = bpp; i < n; i++) {
      cdat[i] = (uint8_t)(cdat[i] + cdat[i - bpp]);
    }
    break;
  case 2:
    for (size_t i = 0; i < n; i++) {
      cdat[i] = (uint8_t)(cdat[i] + pdat[i]);
    }
    break;
  case 3:
    for (size_t i = 0; i < bpp; i++) {
      cdat[i] = (uint8_t)(cdat[i] + pdat[i] / 2);
    }
    for (size_t i = bpp; i < n; i++) {
      cdat[i] = (uint8_t)(cdat[i] + (cdat[i - bpp] + pdat[i]) / 2);
    }
    break;
  case 4:
    bdfi_png_paeth(cdat, pdat, n, bpp);
    break;
  default:
    return 0;
  }
  return 1;
}

/* What draw.Draw makes of a colour that is not premultiplied, 8 bits a
 * channel (drawNRGBASrc, and NRGBA.RGBA for a palette's colours). */
static void bdfi_png_put8(uint8_t *o, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  uint32_t sa = a * 0x101;
  o[0] = (uint8_t)((r * sa / 0xff) >> 8);
  o[1] = (uint8_t)((g * sa / 0xff) >> 8);
  o[2] = (uint8_t)((b * sa / 0xff) >> 8);
  o[3] = (uint8_t)a;
}

/* And of one of 16 bits a channel (NRGBA64.RGBA, then the high bytes). */
static void bdfi_png_put16(uint8_t *o, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  o[0] = (uint8_t)((r * a / 0xffff) >> 8);
  o[1] = (uint8_t)((g * a / 0xffff) >> 8);
  o[2] = (uint8_t)((b * a / 0xffff) >> 8);
  o[3] = (uint8_t)(a >> 8);
}

static uint32_t bdfi_png_be16(const uint8_t *p) {
  return (uint32_t)p[0] << 8 | p[1];
}

/*
 * bdfi_png_row converts an unfiltered row of w pixels to colours (the
 * second half of readImagePass) and writes them where the pass puts them:
 * o is the first pixel of the row in the image and step the bytes from one
 * pixel of the pass to the next.
 */
static void bdfi_png_row(const bdfi_png *d, const uint8_t *cdat, int32_t w, uint8_t *o,
                         size_t step) {
  const uint8_t *t = d->transparent;
  switch (d->cb) {
  case BDFI_PNG_G1:
  case BDFI_PNG_G2:
  case BDFI_PNG_G4: {
    int depth = d->depth;
    uint32_t scale = depth == 1 ? 0xff : depth == 2 ? 0x55 : 0x11;
    for (int32_t x = 0; x < w; x++, o += step) {
      uint32_t v = (uint32_t)cdat[(int64_t)x * depth / 8] >> (8 - depth - x * depth % 8);
      uint32_t y = (v & ((1u << depth) - 1)) * scale;
      bdfi_png_put8(o, y, y, y, d->use_transparent && y == t[1] ? 0 : 0xff);
    }
    break;
  }
  case BDFI_PNG_G8:
    for (int32_t x = 0; x < w; x++, o += step) {
      uint32_t y = cdat[x];
      bdfi_png_put8(o, y, y, y, d->use_transparent && y == t[1] ? 0 : 0xff);
    }
    break;
  case BDFI_PNG_GA8:
    for (int32_t x = 0; x < w; x++, o += step) {
      uint32_t y = cdat[2 * (size_t)x];
      bdfi_png_put8(o, y, y, y, cdat[2 * (size_t)x + 1]);
    }
    break;
  case BDFI_PNG_TC8:
    for (int32_t x = 0; x < w; x++, o += step) {
      const uint8_t *s = cdat + 3 * (size_t)x;
      int clear = d->use_transparent && s[0] == t[1] && s[1] == t[3] && s[2] == t[5];
      bdfi_png_put8(o, s[0], s[1], s[2], clear ? 0 : 0xff);
    }
    break;
  case BDFI_PNG_P1:
  case BDFI_PNG_P2:
  case BDFI_PNG_P4:
  case BDFI_PNG_P8: {
    int depth = d->depth;
    for (int32_t x = 0; x < w; x++, o += step) {
      uint32_t v = (uint32_t)cdat[(int64_t)x * depth / 8] >> (8 - depth - x * depth % 8);
      const uint8_t *c = d->palette[v & ((1u << depth) - 1)];
      bdfi_png_put8(o, c[0], c[1], c[2], c[3]);
    }
    break;
  }
  case BDFI_PNG_TCA8:
    for (int32_t x = 0; x < w; x++, o += step) {
      const uint8_t *s = cdat + 4 * (size_t)x;
      bdfi_png_put8(o, s[0], s[1], s[2], s[3]);
    }
    break;
  case BDFI_PNG_G16:
    for (int32_t x = 0; x < w; x++, o += step) {
      uint32_t y = bdfi_png_be16(cdat + 2 * (size_t)x);
      bdfi_png_put16(o, y, y, y, d->use_transparent && y == bdfi_png_be16(t) ? 0 : 0xffff);
    }
    break;
  case BDFI_PNG_GA16:
    for (int32_t x = 0; x < w; x++, o += step) {
      uint32_t y = bdfi_png_be16(cdat + 4 * (size_t)x);
      bdfi_png_put16(o, y, y, y, bdfi_png_be16(cdat + 4 * (size_t)x + 2));
    }
    break;
  case BDFI_PNG_TC16:
    for (int32_t x = 0; x < w; x++, o += step) {
      const uint8_t *s = cdat + 6 * (size_t)x;
      uint32_t r = bdfi_png_be16(s), g = bdfi_png_be16(s + 2), b = bdfi_png_be16(s + 4);
      int clear = d->use_transparent && r == bdfi_png_be16(t) && g == bdfi_png_be16(t + 2) &&
                  b == bdfi_png_be16(t + 4);
      bdfi_png_put16(o, r, g, b, clear ? 0 : 0xffff);
    }
    break;
  case BDFI_PNG_TCA16:
    for (int32_t x = 0; x < w; x++, o += step) {
      const uint8_t *s = cdat + 8 * (size_t)x;
      bdfi_png_put16(o, bdfi_png_be16(s), bdfi_png_be16(s + 2), bdfi_png_be16(s + 4),
                     bdfi_png_be16(s + 6));
    }
    break;
  default:
    break;
  }
}

static uint32_t bdfi_png_adler32(const uint8_t *p, size_t n) {
  uint32_t a = 1, b = 0;
  while (n > 0) {
    size_t run = n < 5552 ? n : 5552; /* the most bytes the sums hold before a remainder */
    for (size_t i = 0; i < run; i++) {
      a += p[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
    p += run;
    n -= run;
  }
  return b << 16 | a;
}

/*
 * bdfi_png_idat is decoder.parseIDAT: it inflates the image data, which
 * begins in the chunk of the given length and goes on through the IDAT
 * chunks after it, and makes the image of it. 0 when the image is refused
 * (d->status says so when the reason is not its format).
 */
static int bdfi_png_idat(bdfi_png *d, uint32_t length) {
  d->idat_length = length;
  if ((int64_t)d->width * d->height > BDFI_MAX_DECODE_PIXELS) {
    d->status = BDF_ERR_TOO_LARGE;
    return 0;
  }
  /* what the rows of all the passes take, with their filter bytes */
  size_t total = 0;
  int passes = d->interlace ? 7 : 1;
  for (int pass = 0; pass < passes; pass++) {
    int32_t w, h;
    const uint8_t *at;
    bdfi_png_pass(d, pass, &w, &h, &at);
    if (w > 0) {
      total += bdfi_png_row_size(d, w) * (size_t)h;
    }
  }

  /* zlib.NewReader: the two bytes of the header, and the checksum of a
   * dictionary there is none of */
  bdfi_pull in;
  memset(&in, 0, sizeof in);
  in.more = bdfi_png_more;
  in.ctx = d;
  int cmf = bdfi_png_pull_byte(&in), flg = cmf < 0 ? -1 : bdfi_png_pull_byte(&in);
  if (flg < 0 || (cmf & 0x0f) != 8 || cmf >> 4 > 7 || (cmf << 8 | flg) % 31 != 0) {
    return 0;
  }
  if (flg & 0x20) {
    uint32_t id = 0;
    for (int i = 0; i < 4; i++) {
      int b = bdfi_png_pull_byte(&in);
      if (b < 0) {
        return 0;
      }
      id = id << 8 | (uint32_t)b;
    }
    if (id != 1) { /* the checksum of no bytes */
      return 0;
    }
  }
  uint8_t *raw = NULL;
  size_t raw_len = 0;
  bdf_status st = bdfi_inflate_pull(&in, total, &raw, &raw_len);
  if (st != BDF_OK) {
    d->status = st == BDF_ERR_NOMEM ? st : BDF_ERR_FORMAT;
    return 0;
  }
  uint32_t sum = 0;
  int ok = raw_len == total;
  for (int i = 0; ok && i < 4; i++) {
    int b = bdfi_png_pull_byte(&in);
    ok = b >= 0;
    sum = sum << 8 | (uint32_t)(b & 0xff);
  }
  /* nothing of the chunk the data ends in may be left unread: the
   * reference's "too much pixel data" */
  ok = ok && sum == bdfi_png_adler32(raw, raw_len) && d->idat_length == 0;
  if (!ok) {
    free(raw);
    return 0;
  }

  size_t width = (size_t)d->width, height = (size_t)d->height;
  size_t bpp = (size_t)(bdfi_png_bits_per_pixel(d) + 7) / 8;
  uint8_t *pix = bdfi_malloc_array(width * height, 4);
  uint8_t *zero = calloc(1, bdfi_png_row_size(d, d->width));
  if (!pix || !zero) {
    free(pix);
    free(zero);
    free(raw);
    d->status = BDF_ERR_NOMEM;
    return 0;
  }
  uint8_t *row = raw;
  for (int pass = 0; ok && pass < passes; pass++) {
    int32_t w, h;
    const uint8_t *at;
    bdfi_png_pass(d, pass, &w, &h, &at);
    if (w == 0) {
      continue;
    }
    size_t n = bdfi_png_row_size(d, w) - 1;
    const uint8_t *before = zero;
    for (int32_t y = 0; y < h; y++) {
      if (!bdfi_png_unfilter(row[0], row + 1, before, n, bpp)) {
        ok = 0;
        break;
      }
      uint8_t *o = pix + 4 * (((size_t)y * at[1] + at[3]) * width + at[2]);
      bdfi_png_row(d, row + 1, w, o, 4 * (size_t)at[0]);
      before = row + 1;
      row += n + 1;
    }
  }
  free(zero);
  free(raw);
  if (!ok) {
    free(pix);
    return 0;
  }
  free(d->img.pix); /* there is one IDAT stage, but nothing may leak */
  d->img.w = d->width;
  d->img.h = d->height;
  d->img.pix = pix;
  return bdfi_png_verify(d);
}

/* decoder.parseChunk: 0 when the chunk is refused. */
static int bdfi_png_chunk(bdfi_png *d, int config_only) {
  const uint8_t *p = bdfi_png_take(d, 8);
  if (!p) {
    return 0;
  }
  uint32_t length = bdfi_png_be32(p);
  d->crc = bdfi_png_crc(0xffffffffu, p + 4, 4);
  if (memcmp(p + 4, "IHDR", 4) == 0) {
    if (d->stage != BDFI_PNG_START) {
      return 0;
    }
    d->stage = BDFI_PNG_SEEN_IHDR;
    return bdfi_png_ihdr(d, length);
  }
  if (memcmp(p + 4, "PLTE", 4) == 0) {
    if (d->stage != BDFI_PNG_SEEN_IHDR) {
      return 0;
    }
    d->stage = BDFI_PNG_SEEN_PLTE;
    return bdfi_png_plte(d, length);
  }
  if (memcmp(p + 4, "tRNS", 4) == 0) {
    if (bdfi_png_paletted(d->cb)) {
      if (d->stage != BDFI_PNG_SEEN_PLTE) {
        return 0;
      }
    } else if (bdfi_png_true_color(d->cb)) {
      if (d->stage != BDFI_PNG_SEEN_IHDR && d->stage != BDFI_PNG_SEEN_PLTE) {
        return 0;
      }
    } else if (d->stage != BDFI_PNG_SEEN_IHDR) {
      return 0;
    }
    d->stage = BDFI_PNG_SEEN_TRNS;
    return bdfi_png_trns(d, length);
  }
  if (memcmp(p + 4, "IDAT", 4) == 0) {
    if (d->stage < BDFI_PNG_SEEN_IHDR || d->stage > BDFI_PNG_SEEN_IDAT ||
        (d->stage == BDFI_PNG_SEEN_IHDR && bdfi_png_paletted(d->cb))) {
      return 0;
    }
    if (d->stage != BDFI_PNG_SEEN_IDAT) {
      d->stage = BDFI_PNG_SEEN_IDAT;
      return config_only ? 1 : bdfi_png_idat(d, length);
    }
    /* IDAT chunks after the image data are passed over like any other */
  } else if (memcmp(p + 4, "IEND", 4) == 0) {
    if (d->stage != BDFI_PNG_SEEN_IDAT) {
      return 0;
    }
    d->stage = BDFI_PNG_SEEN_IEND;
    return length == 0 && bdfi_png_verify(d);
  }
  /* a chunk that is not read, which still has to be whole */
  if (length > 0x7fffffff || !bdfi_png_data(d, length)) {
    return 0;
  }
  return bdfi_png_verify(d);
}

/* decoder.checkHeader */
static int bdfi_png_begin(bdfi_png *d, const uint8_t *data, size_t len) {
  memset(d, 0, sizeof *d);
  d->data = data;
  d->len = len;
  d->status = BDF_ERR_FORMAT;
  const uint8_t *p = bdfi_png_take(d, 8);
  return p && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0;
}

/* bdfi_png_read_config is DecodeConfig on a decoder that has begun. */
static int bdfi_png_read_config(bdfi_png *d) {
  for (;;) {
    if (!bdfi_png_chunk(d, 1)) {
      return 0;
    }
    /* a palette is part of what DecodeConfig returns: it reads on to it */
    if (d->stage >= (bdfi_png_paletted(d->cb) ? BDFI_PNG_SEEN_TRNS : BDFI_PNG_SEEN_IHDR)) {
      return 1;
    }
  }
}

BDF_INTERNAL int bdfi_png_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h) {
  *w = 0;
  *h = 0;
  bdfi_png *d = malloc(sizeof *d);
  if (!d) {
    return 0;
  }
  int ok = bdfi_png_begin(d, data, len) && bdfi_png_read_config(d);
  if (ok) {
    *w = d->width;
    *h = d->height;
  }
  free(d);
  return ok;
}

BDF_INTERNAL bdf_status bdfi_png_decode(const uint8_t *data, size_t len, bdfi_bitmap *out) {
  memset(out, 0, sizeof *out);
  int32_t w, h;
  if (bdfi_png_config(data, len, &w, &h) && (int64_t)w * h > BDFI_MAX_DECODE_PIXELS) {
    return BDF_ERR_TOO_LARGE;
  }
  bdfi_png *d = malloc(sizeof *d);
  if (!d) {
    return BDF_ERR_NOMEM;
  }
  int ok = bdfi_png_begin(d, data, len);
  while (ok && d->stage != BDFI_PNG_SEEN_IEND) {
    ok = bdfi_png_chunk(d, 0);
  }
  bdf_status st = BDF_OK;
  if (ok) {
    *out = d->img;
  } else {
    free(d->img.pix);
    st = d->status;
  }
  free(d);
  return st;
}
