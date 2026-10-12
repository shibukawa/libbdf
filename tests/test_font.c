/* Tests of the font helpers through the public API: WOFF2 fonts of the
 * reference's testdata against what its decoder makes of them, the bounded
 * inputs of its tests, and the sfnt passthrough. */
#include "testutil.h"

#include "woff2_vectors.h"

static uint64_t fnv(const uint8_t *b, size_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static void expect_font(const uint8_t *woff2, size_t len, size_t want_len, uint64_t want_fnv,
                        unsigned want_tables) {
  uint8_t *out = NULL;
  size_t n = 0;
  bdf_error err;
  bdf_status st = bdf_woff2_decode(woff2, len, &out, &n, &err);
  if (!bdf_woff2_available()) {
    CHECK(st == BDF_ERR_UNSUPPORTED && out == NULL);
    return;
  }
  CHECK(st == BDF_OK);
  if (st != BDF_OK) {
    fprintf(stderr, "decode: %s at %zu\n", err.message, err.offset);
    return;
  }
  CHECK(n == want_len);
  CHECK(fnv(out, n) == want_fnv);
  CHECK(n >= 12 && ((unsigned)out[4] << 8 | out[5]) == want_tables);
  /* the tables come sorted by tag, 4-byte aligned, and a rebuilt loca is
   * long (a CFF font has no loca, and its head is left alone) */
  unsigned ntab = (unsigned)out[4] << 8 | out[5];
  int sorted = 1, aligned = 1, has_head = 0, has_glyf = 0;
  size_t head_off = 0;
  for (unsigned i = 0; i < ntab; i++) {
    const uint8_t *rec = out + 12 + 16 * i;
    if (i > 0 && memcmp(rec - 16, rec, 4) >= 0) {
      sorted = 0;
    }
    uint32_t off = (uint32_t)rec[8] << 24 | (uint32_t)rec[9] << 16 | (uint32_t)rec[10] << 8 | rec[11];
    if (off % 4 != 0) {
      aligned = 0;
    }
    if (memcmp(rec, "head", 4) == 0) {
      has_head = 1;
      head_off = off;
    }
    if (memcmp(rec, "glyf", 4) == 0) {
      has_glyf = 1;
    }
  }
  CHECK(sorted && aligned && has_head);
  if (has_glyf) {
    CHECK(head_off + 54 <= n && out[head_off + 50] == 0 && out[head_off + 51] == 1);
  }
  /* through bdf_font_sfnt it is the same, and the result passes through as it is */
  uint8_t *again = NULL;
  size_t m = 0;
  CHECK(bdf_font_sfnt(woff2, len, &again, &m, &err) == BDF_OK && m == n && memcmp(again, out, n) == 0);
  bdf_free(again);
  CHECK(bdf_font_sfnt(out, n, &again, &m, &err) == BDF_OK && m == n && memcmp(again, out, n) == 0);
  bdf_free(again);
  bdf_free(out);
  /* every proper prefix fails */
  for (size_t cut = 0; cut < len; cut += len / 37 + 1) {
    if (bdf_woff2_decode(woff2, cut, &out, &n, &err) == BDF_OK) {
      failures++;
      fprintf(stderr, "a prefix of %zu bytes decoded\n", cut);
      bdf_free(out);
    }
  }
}

int main(void) {
  uint8_t *out = NULL;
  size_t n = 0;
  bdf_error err;

  expect_font(woff2_ttf, sizeof woff2_ttf, WOFF2_TTF_SFNT_LEN, WOFF2_TTF_SFNT_FNV, WOFF2_TTF_TABLES);
  expect_font(woff2_cff, sizeof woff2_cff, WOFF2_CFF_SFNT_LEN, WOFF2_CFF_SFNT_FNV, WOFF2_CFF_TABLES);

  /* TestDecodeRejectsGarbage */
  CHECK(bdf_woff2_decode(NULL, 0, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_woff2_decode((const uint8_t *)"wOF2", 4, &out, &n, &err) == BDF_ERR_FORMAT);
  uint8_t zeros[64] = {'w', 'O', 'F', '2'};
  CHECK(bdf_woff2_decode(zeros, sizeof zeros, &out, &n, &err) != BDF_OK && out == NULL);
  CHECK(bdf_woff2_decode((const uint8_t *)"OTTO0000", 8, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "not a WOFF2 font");
  uint8_t ttc[48] = {'w', 'O', 'F', '2', 't', 't', 'c', 'f'};
  CHECK(bdf_woff2_decode(ttc, sizeof ttc, &out, &n, &err) == BDF_ERR_UNSUPPORTED);

  if (bdf_woff2_available()) {
    /* TestDecodeBounded: a stream longer than its tables decodes, tables
     * beyond 256 MiB and glyphs of streams that ended do not */
    CHECK(bdf_woff2_decode(woff2_long_stream, sizeof woff2_long_stream, &out, &n, &err) == BDF_OK);
    CHECK(n == WOFF2_LONG_STREAM_SFNT_LEN);
    bdf_free(out);
    CHECK(bdf_woff2_decode(woff2_too_large, sizeof woff2_too_large, &out, &n, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "the tables of the font are too large");
    CHECK(bdf_woff2_decode(woff2_ended_streams, sizeof woff2_ended_streams, &out, &n, &err) ==
          BDF_ERR_FORMAT);
    CHECK_MSG(err, "malformed WOFF2 font");
  }

  /* bdf_font_sfnt: sfnt flavours pass through, WOFF 1.0 and other data do not */
  const char *sfnts[] = {"\x00\x01\x00\x00rest", "OTTOrest", "truerest", "ttcfrest"};
  for (size_t i = 0; i < 4; i++) {
    CHECK(bdf_font_sfnt((const uint8_t *)sfnts[i], 8, &out, &n, &err) == BDF_OK && n == 8 &&
          memcmp(out, sfnts[i], 8) == 0);
    bdf_free(out);
  }
  CHECK(bdf_font_sfnt((const uint8_t *)"wOFFrest", 8, &out, &n, &err) == BDF_ERR_UNSUPPORTED);
  CHECK(bdf_font_sfnt((const uint8_t *)"\x89PNG", 4, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_font_sfnt(NULL, 0, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_font_sfnt(NULL, 0, NULL, &n, &err) == BDF_ERR_ARG);
  return finish();
}
