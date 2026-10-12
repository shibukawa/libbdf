/* A libFuzzer target over the deflate-raw decoder. */
#include <stddef.h>
#include <stdint.h>

#include "bdf/bdf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  uint8_t *out;
  size_t n;
  bdf_error err;
  /* a limit that depends on the input keeps both outcomes reachable */
  size_t limit = size % 7 == 0 ? 64 : (size_t)1 << 20;
  if (bdf_inflate(data, size, limit, &out, &n, &err) == BDF_OK) {
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < n; i++) {
      sink ^= out[i];
    }
    bdf_free(out);
  }
  return 0;
}
