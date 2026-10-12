/* Tests of the deflate-raw decoder against vectors zlib produced. */
#include "testutil.h"

#include "inflate_vectors.h"

static uint64_t fnv(const uint8_t *b, size_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static void expect_text(const uint8_t *packed, size_t packed_len, const char *plain) {
  uint8_t *out = NULL;
  size_t n = 0;
  bdf_error err;
  CHECK(bdf_inflate(packed, packed_len, strlen(plain), &out, &n, &err) == BDF_OK);
  CHECK(out && n == strlen(plain) && memcmp(out, plain, n) == 0);
  bdf_free(out);
  /* a generous limit changes nothing */
  CHECK(bdf_inflate(packed, packed_len, 1 << 20, &out, &n, &err) == BDF_OK && n == strlen(plain));
  bdf_free(out);
  /* one byte short of the data is "more than its stated size" */
  CHECK(bdf_inflate(packed, packed_len, strlen(plain) - 1, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK(out == NULL);
  CHECK_MSG(err, "data inflates to more than its stated size");
}

int main(void) {
  uint8_t *out;
  size_t n;
  bdf_error err;

  expect_text(hello_fixed, sizeof hello_fixed, HELLO_PLAIN);
  expect_text(hello_stored, sizeof hello_stored, HELLO_PLAIN);

  /* a dynamic-Huffman block of text */
  CHECK(bdf_inflate(text_dynamic, sizeof text_dynamic, TEXT_PLAIN_LEN, &out, &n, &err) == BDF_OK);
  CHECK(n == TEXT_PLAIN_LEN && fnv(out, n) == TEXT_PLAIN_FNV);
  bdf_free(out);

  /* a megabyte of zeros, the case of the reference's inflate limit test */
  CHECK(bdf_inflate(zeros_packed, sizeof zeros_packed, ZEROS_PLAIN_LEN, &out, &n, &err) == BDF_OK);
  CHECK(n == ZEROS_PLAIN_LEN);
  int zero = 1;
  for (size_t i = 0; i < n; i++) {
    zero &= out[i] == 0;
  }
  CHECK(zero);
  bdf_free(out);
  CHECK(bdf_inflate(zeros_packed, sizeof zeros_packed, 1024, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "data inflates to more than its stated size");

  /* every proper prefix ends too early */
  for (size_t cut = 0; cut < sizeof hello_fixed; cut++) {
    if (bdf_inflate(hello_fixed, cut, 1 << 20, &out, &n, &err) != BDF_ERR_FORMAT) {
      failures++;
      fprintf(stderr, "prefix of %zu bytes inflated\n", cut);
      bdf_free(out);
    }
  }
  for (size_t cut = 0; cut < sizeof text_dynamic; cut += 97) {
    if (bdf_inflate(text_dynamic, cut, 1 << 20, &out, &n, &err) != BDF_ERR_FORMAT) {
      failures++;
      fprintf(stderr, "prefix of %zu bytes inflated\n", cut);
      bdf_free(out);
    }
  }

  /* bytes after the final block are not read, as the reference's reader stops there */
  wr w = {0};
  w_bytes(&w, hello_fixed, sizeof hello_fixed);
  w_u8(&w, 0xFF);
  w_u8(&w, 0xFF);
  CHECK(bdf_inflate(w.b, w.n, 64, &out, &n, &err) == BDF_OK && n == 11);
  bdf_free(out);
  free(w.b);

  /* a stored block whose length and its complement disagree */
  uint8_t bad[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
  CHECK(bdf_inflate(bad, sizeof bad, 64, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "corrupt deflate stream");
  /* block type 3 */
  uint8_t type3[] = {0x07, 0x00};
  CHECK(bdf_inflate(type3, sizeof type3, 64, &out, &n, &err) == BDF_ERR_FORMAT);
  /* a stored block cut short */
  uint8_t cut[] = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e'};
  CHECK(bdf_inflate(cut, sizeof cut, 64, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "unexpected end of deflate stream");
  /* an empty stored block is an empty result */
  uint8_t empty[] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
  CHECK(bdf_inflate(empty, sizeof empty, 64, &out, &n, &err) == BDF_OK && n == 0 && out != NULL);
  bdf_free(out);
  /* a limit of zero: an empty block is fine, a byte is not */
  CHECK(bdf_inflate(empty, sizeof empty, 0, &out, &n, &err) == BDF_OK && n == 0);
  bdf_free(out);
  CHECK(bdf_inflate(hello_fixed, sizeof hello_fixed, 0, &out, &n, &err) == BDF_ERR_FORMAT);

  CHECK(bdf_inflate(NULL, 0, 64, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_inflate(hello_fixed, sizeof hello_fixed, 64, NULL, &n, &err) == BDF_ERR_ARG);
  return finish();
}
