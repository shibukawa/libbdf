/* Unit tests of the single-file header checks (spec §3.3). */
#include <stdio.h>
#include <string.h>

#include "bdf/bdf.h"

static int failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                     \
    }                                                                                              \
  } while (0)

static void put_u64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)(v >> (8 * i));
  }
}

static void valid(uint8_t h[32]) {
  memset(h, 0, 32);
  memcpy(h, "bdf\0", 4);
  h[4] = 1; /* version */
  put_u64(h + 8, 32);  /* manifestOff */
  put_u64(h + 16, 100); /* manifestLen */
  h[24] = 1; /* deflate-raw */
}

static const char *parse(const uint8_t *h, size_t len, uint64_t size, bdf_header *out) {
  bdf_error err;
  if (bdf_header_parse(h, len, size, out, &err) == BDF_OK) {
    return NULL;
  }
  return err.message;
}

int main(void) {
  uint8_t h[32];
  bdf_header hd;
  valid(h);
  CHECK(parse(h, 32, 1000, &hd) == NULL);
  CHECK(hd.version == 1 && hd.flags == 0 && hd.manifest_off == 32 && hd.manifest_len == 100 &&
        hd.manifest_enc == 1);
  CHECK(parse(h, 64, 1000, &hd) == NULL); /* more than the header is fine */
  CHECK(parse(h, 132, 132, &hd) == NULL); /* the manifest ends at the file's end */

  const char *m = parse(h, 31, 1000, &hd);
  CHECK(m && strcmp(m, "short header") == 0);

  valid(h);
  h[0] = 'B';
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "bad magic") == 0);

  valid(h);
  h[4] = 2;
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "unsupported format version") == 0);

  valid(h);
  h[6] = 1; /* encrypted */
  CHECK(parse(h, 32, 1000, &hd) == NULL && hd.flags == BDF_FLAG_ENCRYPTED);
  h[6] = 2;
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "unsupported header flags") == 0);

  valid(h);
  h[24] = 2;
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "unknown manifest encoding") == 0);

  valid(h);
  h[31] = 1;
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "non-zero reserved header bytes") == 0);

  valid(h);
  m = parse(h, 32, 131, &hd);
  CHECK(m && strcmp(m, "manifest out of range") == 0);
  put_u64(h + 8, 2000);
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "manifest out of range") == 0);
  put_u64(h + 8, UINT64_MAX);
  put_u64(h + 16, 2);
  m = parse(h, 32, 1000, &hd);
  CHECK(m && strcmp(m, "manifest out of range") == 0);

  valid(h);
  put_u64(h + 16, (uint64_t)BDF_MAX_MANIFEST_SIZE + 1);
  m = parse(h, 32, UINT64_MAX, &hd);
  CHECK(m && strcmp(m, "manifest too large") == 0);
  put_u64(h + 16, BDF_MAX_MANIFEST_SIZE);
  CHECK(parse(h, 32, UINT64_MAX, &hd) == NULL);

  bdf_error err;
  CHECK(bdf_header_parse(h, 32, 1000, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_header_parse(NULL, 32, 1000, &hd, &err) == BDF_ERR_ARG);
  CHECK(bdf_header_parse(NULL, 0, 1000, &hd, &err) == BDF_ERR_FORMAT);

  if (failures) {
    fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
