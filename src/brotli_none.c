/* A build without a Brotli decoder: WOFF2 fonts cannot be opened. */
#include "bdf_internal.h"

int bdf_woff2_available(void) { return 0; }

bdf_status bdfi_brotli_decompress(const uint8_t *in, size_t in_len, size_t size, uint8_t **out,
                                  bdf_error *err) {
  (void)in;
  (void)in_len;
  (void)size;
  *out = NULL;
  bdfi_error_set(err, BDF_ERR_UNSUPPORTED, 0, "this build has no Brotli decoder for WOFF2 fonts");
  return BDF_ERR_UNSUPPORTED;
}
