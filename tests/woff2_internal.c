/* Tests of the WOFF2 transforms on uncompressed streams: UIntBase128, the
 * hmtx transform (which the reference's encoder never writes, so no test
 * document holds one) and the glyf transform's checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

static int failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                     \
    }                                                                                              \
  } while (0)

static void test_base128(void) {
  /* TestBase128And255 of the reference: values and their encodings */
  const struct {
    uint32_t v;
    uint8_t enc[5];
    size_t n;
  } cases[] = {
      {0, {0x00}, 1},
      {127, {0x7f}, 1},
      {128, {0x81, 0x00}, 2},
      {16383, {0xff, 0x7f}, 2},
      {16384, {0x81, 0x80, 0x00}, 3},
      {1u << 30, {0x84, 0x80, 0x80, 0x80, 0x00}, 5},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    size_t pos = 0;
    uint32_t v = 0;
    CHECK(bdfi_woff2_read_base128(cases[i].enc, cases[i].n, &pos, &v) == 0 && v == cases[i].v &&
          pos == cases[i].n);
  }
  size_t pos = 0;
  uint32_t v;
  const uint8_t leading_zero[] = {0x80, 0x01};
  CHECK(bdfi_woff2_read_base128(leading_zero, 2, &pos, &v) < 0);
  pos = 0;
  const uint8_t too_long[] = {0x81, 0x81, 0x81, 0x81, 0x81, 0x01};
  CHECK(bdfi_woff2_read_base128(too_long, 6, &pos, &v) < 0);
  pos = 0;
  const uint8_t over[] = {0x84, 0x80, 0x80, 0x80, 0x01}; /* 2^30 + 1 */
  CHECK(bdfi_woff2_read_base128(over, 5, &pos, &v) < 0);
  pos = 0;
  const uint8_t cut[] = {0x81};
  CHECK(bdfi_woff2_read_base128(cut, 1, &pos, &v) < 0);
}

static void put16(uint8_t *p, unsigned v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void test_hmtx(void) {
  /* three glyphs: an empty one, one with xMin 5 and one with xMin -300;
   * two of them have advances */
  uint8_t glyf[24] = {0};
  put16(glyf + 0, 1); /* glyph 1: nContours */
  put16(glyf + 2, 5); /* xMin */
  put16(glyf + 12, 1);
  put16(glyf + 14, (unsigned)(uint16_t)-300);
  uint8_t loca[16];
  put32(loca, 0);
  put32(loca + 4, 0); /* glyph 0 is empty */
  put32(loca + 8, 12);
  put32(loca + 12, 24);
  uint8_t hhea[36] = {0};
  put16(hhea + 34, 2); /* numberOfHMetrics */
  uint8_t maxp[6] = {0};
  put16(maxp + 4, 3); /* numGlyphs */
  uint8_t *out;
  size_t n;

  /* flags 1: the first numH left side bearings come from glyf; the rest are stored */
  const uint8_t t1[] = {1, 0x01, 0xf4, 0x02, 0x58, 0xff, 0xf9}; /* adv 500, 600; lsb[2] = -7 */
  CHECK(bdfi_woff2_rebuild_hmtx(t1, sizeof t1, hhea, sizeof hhea, maxp, sizeof maxp, glyf,
                                sizeof glyf, loca, sizeof loca, &out, &n) == BDF_OK);
  const uint8_t want1[] = {0x01, 0xf4, 0x00, 0x00, 0x02, 0x58, 0x00, 0x05, 0xff, 0xf9};
  CHECK(n == sizeof want1 && memcmp(out, want1, n) == 0);
  free(out);

  /* flags 2: the first ones are stored, the rest come from glyf */
  const uint8_t t2[] = {2, 0x01, 0xf4, 0x02, 0x58, 0x00, 0x0a, 0x00, 0x0b}; /* lsb[0] 10, lsb[1] 11 */
  CHECK(bdfi_woff2_rebuild_hmtx(t2, sizeof t2, hhea, sizeof hhea, maxp, sizeof maxp, glyf,
                                sizeof glyf, loca, sizeof loca, &out, &n) == BDF_OK);
  const uint8_t want2[] = {0x01, 0xf4, 0x00, 0x0a, 0x02, 0x58, 0x00, 0x0b, 0xfe, 0xd4};
  CHECK(n == sizeof want2 && memcmp(out, want2, n) == 0);
  free(out);

  /* flags 3 with no glyf at all: every bearing is 0 */
  const uint8_t t3[] = {3, 0x01, 0xf4, 0x02, 0x58};
  CHECK(bdfi_woff2_rebuild_hmtx(t3, sizeof t3, hhea, sizeof hhea, maxp, sizeof maxp, NULL, 0, NULL,
                                0, &out, &n) == BDF_OK);
  const uint8_t want3[] = {0x01, 0xf4, 0x00, 0x00, 0x02, 0x58, 0x00, 0x00, 0x00, 0x00};
  CHECK(n == sizeof want3 && memcmp(out, want3, n) == 0);
  free(out);

  /* short streams, and metrics the font cannot have */
  CHECK(bdfi_woff2_rebuild_hmtx(t1, sizeof t1 - 1, hhea, sizeof hhea, maxp, sizeof maxp, glyf,
                                sizeof glyf, loca, sizeof loca, &out, &n) == BDF_ERR_FORMAT);
  CHECK(bdfi_woff2_rebuild_hmtx(t1, 0, hhea, sizeof hhea, maxp, sizeof maxp, glyf, sizeof glyf,
                                loca, sizeof loca, &out, &n) == BDF_ERR_FORMAT);
  put16(hhea + 34, 4); /* more metrics than glyphs */
  CHECK(bdfi_woff2_rebuild_hmtx(t1, sizeof t1, hhea, sizeof hhea, maxp, sizeof maxp, glyf,
                                sizeof glyf, loca, sizeof loca, &out, &n) == BDF_ERR_FORMAT);
  put16(hhea + 34, 0);
  CHECK(bdfi_woff2_rebuild_hmtx(t1, sizeof t1, hhea, sizeof hhea, maxp, sizeof maxp, glyf,
                                sizeof glyf, loca, sizeof loca, &out, &n) == BDF_ERR_FORMAT);
  CHECK(bdfi_woff2_rebuild_hmtx(t1, sizeof t1, hhea, 10, maxp, sizeof maxp, glyf, sizeof glyf,
                                loca, sizeof loca, &out, &n) == BDF_ERR_FORMAT);
}

/* transformed builds a transformed glyf table of the given streams. */
static uint8_t *transformed(unsigned num_glyphs, unsigned option_flags, const uint8_t *const s[7],
                            const size_t n[7], const uint8_t *overlap, size_t *out_len) {
  size_t total = 36;
  for (int i = 0; i < 7; i++) {
    total += n[i];
  }
  size_t olen = (option_flags & 1) ? (num_glyphs + 7) >> 3 : 0;
  uint8_t *t = calloc(total + olen, 1);
  put16(t + 2, option_flags);
  put16(t + 4, num_glyphs);
  size_t pos = 36;
  for (int i = 0; i < 7; i++) {
    put32(t + 8 + 4 * i, (uint32_t)n[i]);
    if (n[i] > 0) { /* an empty stream has no bytes to copy, and may be NULL */
      memcpy(t + pos, s[i], n[i]);
    }
    pos += n[i];
  }
  if (olen) {
    memcpy(t + pos, overlap, olen);
  }
  *out_len = total + olen;
  return t;
}

static void test_glyf(void) {
  /* one simple glyph: a triangle of three on-curve points, no bbox stored,
   * no instructions; and one empty glyph */
  const uint8_t n_contour[] = {0, 1, 0, 0};
  const uint8_t n_points[] = {3};
  /* flags: triplet class 20..83 holds dx, dy in 1..64 each; 0x80 clear = on curve.
   * flag 20 + xySign (bit 0: x positive, bit 1: y positive) with the size
   * bits 0 → dx = ±(1 + (b1 >> 4)), dy = ±(1 + (b1 & 15)) */
  const uint8_t flags[] = {23, 23, 22};               /* the last: dx negative, dy positive */
  const uint8_t glyphs[] = {0x00, 0x90, 0x09, 0x00}; /* (1,1), (10,1), (-1,10), then 0 instructions */
  const uint8_t bbox[] = {0, 0, 0, 0};                /* bitmap of 2 glyphs: no bbox */
  const uint8_t *s[7] = {n_contour, n_points, flags, glyphs, NULL, bbox, NULL};
  const size_t n[7] = {sizeof n_contour, sizeof n_points, sizeof flags, sizeof glyphs, 0, sizeof bbox, 0};
  size_t tlen;
  uint8_t *t = transformed(2, 0, s, n, NULL, &tlen);
  uint8_t *glyf, *loca;
  size_t glyf_len, loca_len;
  CHECK(bdfi_woff2_rebuild_glyf(t, tlen, &glyf, &glyf_len, &loca, &loca_len) == BDF_OK);
  CHECK(loca_len == 12);
  /* points: (1,1) → (11,2) → (10,12): bbox 1,1,11,12 */
  const uint8_t want[] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0b, 0x00, 0x0c, /* nc, bbox */
                          0x00, 0x02,                                                 /* end point 2 */
                          0x00, 0x00,                                                 /* 0 instructions */
                          0x01, 0x01, 0x01,                                           /* flags */
                          0x00, 0x01, 0x00, 0x0a, 0xff, 0xff,                         /* dx */
                          0x00, 0x01, 0x00, 0x01, 0x00, 0x0a,                         /* dy */
                          0x00, 0x00, 0x00};                                          /* pad to 32 */
  CHECK(glyf_len == sizeof want && memcmp(glyf, want, glyf_len) == 0);
  const uint8_t want_loca[] = {0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 32};
  CHECK(memcmp(loca, want_loca, 12) == 0);
  free(glyf);
  free(loca);
  free(t);

  /* a glyph with contours but a stream that ended is malformed */
  const uint8_t n_contour2[] = {0, 1};
  const uint8_t *s2[7] = {n_contour2, NULL, NULL, NULL, NULL, bbox, NULL};
  const size_t n2[7] = {2, 0, 0, 0, 0, 4, 0};
  t = transformed(1, 0, s2, n2, NULL, &tlen);
  CHECK(bdfi_woff2_rebuild_glyf(t, tlen, &glyf, &glyf_len, &loca, &loca_len) == BDF_ERR_FORMAT);
  free(t);
  /* an empty glyph may not have a bbox; a composite must */
  const uint8_t bbox_set[] = {0x80, 0, 0, 0};
  const uint8_t *s3[7] = {n_contour2 + 0, NULL, NULL, NULL, NULL, bbox_set, NULL};
  const uint8_t zero[] = {0, 0};
  s3[0] = zero;
  t = transformed(1, 0, s3, n2, NULL, &tlen);
  CHECK(bdfi_woff2_rebuild_glyf(t, tlen, &glyf, &glyf_len, &loca, &loca_len) == BDF_ERR_FORMAT);
  free(t);
  const uint8_t composite[] = {0xff, 0xff};
  const uint8_t *s4[7] = {composite, NULL, NULL, NULL, NULL, bbox, NULL};
  t = transformed(1, 0, s4, n2, NULL, &tlen);
  CHECK(bdfi_woff2_rebuild_glyf(t, tlen, &glyf, &glyf_len, &loca, &loca_len) == BDF_ERR_FORMAT);
  free(t);
  /* no glyphs at all */
  const uint8_t *s5[7] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
  const size_t n5[7] = {0, 0, 0, 0, 0, 0, 0};
  t = transformed(0, 0, s5, n5, NULL, &tlen);
  CHECK(bdfi_woff2_rebuild_glyf(t, tlen, &glyf, &glyf_len, &loca, &loca_len) == BDF_OK &&
        glyf_len == 0 && loca_len == 4);
  free(glyf);
  free(loca);
  free(t);
  /* shorter than a header */
  CHECK(bdfi_woff2_rebuild_glyf(bbox, 4, &glyf, &glyf_len, &loca, &loca_len) == BDF_ERR_FORMAT);
}

int main(void) {
  test_base128();
  test_hmtx();
  test_glyf();
  if (failures) {
    fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
