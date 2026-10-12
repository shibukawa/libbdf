/*
 * Brotli decompression for WOFF2 through the Brotli decoder (the vendored
 * copy under third_party/brotli, or a system libbrotlidec in the
 * single-header build with BDF_HAVE_BROTLI). The stream is read up to the
 * size the font's directory states and one byte beyond, as the reference's
 * decompress does: a stream longer than the tables is fine, a shorter one or
 * one that ends without its final block is not, and nothing past that point
 * is looked at.
 */
#include <stdlib.h>

#include <brotli/decode.h>

#include "bdf_internal.h"

int bdf_woff2_available(void) { return 1; }

bdf_status bdfi_brotli_decompress(const uint8_t *in, size_t in_len, size_t size, uint8_t **out,
                                  bdf_error *err) {
  *out = NULL;
  if (size == SIZE_MAX) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  uint8_t *buf = malloc(size + 1);
  if (!buf) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  BrotliDecoderState *st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
  if (!st) {
    free(buf);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  size_t avail_in = in_len, avail_out = size + 1;
  const uint8_t *next_in = in;
  uint8_t *next_out = buf;
  BrotliDecoderResult r =
      BrotliDecoderDecompressStream(st, &avail_in, &next_in, &avail_out, &next_out, NULL);
  BrotliDecoderDestroyInstance(st);
  size_t produced = size + 1 - avail_out;
  const char *msg = NULL;
  /*
   * The outcomes of the reference's reader, which reads size + 1 bytes and
   * stops: once it has them nothing after matters (NEEDS_MORE_OUTPUT, or
   * NEEDS_MORE_INPUT with the byte beyond size already out); a stream that
   * ends sooner must have given the tables, and must end with its input
   * (its reader refuses input after the end of the stream).
   */
  switch (r) {
  case BROTLI_DECODER_RESULT_SUCCESS:
    if (produced < size) {
      msg = "the Brotli stream ends before the tables";
    } else if (avail_in > 0) {
      msg = "data after the end of the Brotli stream";
    }
    break;
  case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
    break;
  case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
    if (produced <= size) {
      msg = "the Brotli stream ends before the tables";
    }
    break;
  default:
    msg = "corrupt Brotli stream";
    break;
  }
  if (msg) {
    free(buf);
    bdfi_error_set(err, BDF_ERR_FORMAT, (size_t)(next_in - in), msg);
    return BDF_ERR_FORMAT;
  }
  *out = buf;
  return BDF_OK;
}
