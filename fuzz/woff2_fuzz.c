/* A libFuzzer target over the WOFF2 decoder and the font passthrough. */
#include <stddef.h>
#include <stdint.h>

#include "bdf/bdf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  uint8_t *out;
  size_t n;
  bdf_error err;
  if (bdf_woff2_decode(data, size, &out, &n, &err) == BDF_OK) {
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < n; i++) {
      sink ^= out[i];
    }
    bdf_free(out);
  }
  if (bdf_font_sfnt(data, size, &out, &n, &err) == BDF_OK) {
    bdf_free(out);
  }
  return 0;
}
