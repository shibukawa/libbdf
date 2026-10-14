/* See png_write.h. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) {
      c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    crc_table[n] = c;
  }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* A chunk: its length, its type, its data and the CRC of the last two. */
static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
  uint8_t head[8], tail[4];
  put32(head, (uint32_t)len);
  memcpy(head + 4, type, 4);
  uint32_t crc = crc_update(0xFFFFFFFFu, head + 4, 4);
  crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
  put32(tail, crc);
  return fwrite(head, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
                 fwrite(tail, 1, 4, f) == 4
             ? 0
             : -1;
}

int png_write_rgba(const char *path, const uint8_t *pixels, int width, int height,
                   int premultiplied) {
  if (width <= 0 || height <= 0) {
    return -1;
  }
  crc_init();
  /* the scanlines, each after its filter byte (0: none) */
  size_t row = (size_t)width * 4 + 1;
  size_t raw_len = row * (size_t)height;
  uint8_t *raw = malloc(raw_len);
  /* zlib: a header, stored blocks of at most 65535 bytes, the Adler-32 */
  size_t blocks = raw_len / 65535 + 1;
  size_t z_len = 2 + raw_len + blocks * 5 + 4;
  uint8_t *z = malloc(z_len);
  if (!raw || !z) {
    free(raw);
    free(z);
    return -1;
  }
  for (int y = 0; y < height; y++) {
    uint8_t *out = raw + row * (size_t)y;
    const uint8_t *in = pixels + (size_t)width * 4 * (size_t)y;
    out[0] = 0;
    for (int x = 0; x < width; x++) {
      const uint8_t *p = in + 4 * x;
      uint8_t *q = out + 1 + 4 * x;
      unsigned a = p[3];
      for (int c = 0; c < 3; c++) {
        q[c] = premultiplied && a != 0 && a != 255 ? (uint8_t)((p[c] * 255u + a / 2) / a) : p[c];
      }
      q[3] = (uint8_t)a;
    }
  }
  size_t w = 0;
  z[w++] = 0x78;
  z[w++] = 0x01;
  uint32_t s1 = 1, s2 = 0;
  for (size_t off = 0; off < raw_len || off == 0;) {
    size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
    int last = off + n == raw_len;
    z[w++] = (uint8_t)last;
    z[w++] = (uint8_t)n;
    z[w++] = (uint8_t)(n >> 8);
    z[w++] = (uint8_t)~n;
    z[w++] = (uint8_t)(~n >> 8);
    memcpy(z + w, raw + off, n);
    for (size_t i = 0; i < n; i++) {
      s1 = (s1 + raw[off + i]) % 65521;
      s2 = (s2 + s1) % 65521;
    }
    w += n;
    off += n;
    if (last) {
      break;
    }
  }
  put32(z + w, (s2 << 16) | s1);
  w += 4;

  uint8_t ihdr[13];
  put32(ihdr, (uint32_t)width);
  put32(ihdr + 4, (uint32_t)height);
  ihdr[8] = 8;  /* bits a channel */
  ihdr[9] = 6;  /* RGBA */
  ihdr[10] = 0; /* deflate */
  ihdr[11] = 0; /* adaptive filters */
  ihdr[12] = 0; /* not interlaced */
  FILE *f = fopen(path, "wb");
  int rc = -1;
  if (f) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    rc = fwrite(signature, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, sizeof ihdr) == 0 &&
                 chunk(f, "IDAT", z, w) == 0 && chunk(f, "IEND", NULL, 0) == 0
             ? 0
             : -1;
    if (fclose(f) != 0) {
      rc = -1;
    }
  }
  free(raw);
  free(z);
  return rc;
}
