/* A libFuzzer target over the search of a text index. The input is a byte of
 * options (bit 0: case-sensitive; the next three: the context), the length
 * of the query in a byte, the query, and a text index part. Every hit is
 * checked against the runs it names. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bdf/bdf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2) {
    return 0;
  }
  int case_sensitive = data[0] & 1;
  size_t context = (data[0] >> 1) & 7;
  size_t qlen = data[1];
  data += 2;
  size -= 2;
  if (qlen > size) {
    qlen = size;
  }
  const char *query = (const char *)data;
  bdf_error err;
  char *out;
  size_t n;
  if (bdf_text_search_normalize(query, qlen, case_sensitive, &out, &n, &err) == BDF_OK) {
    bdf_free(out);
  }
  bdf_text_index *index;
  if (bdf_text_index_decode(data + qlen, size - qlen, &index, &err) != BDF_OK) {
    return 0;
  }
  bdf_text_search *search;
  if (bdf_text_search_new(index, case_sensitive, &search, &err) == BDF_OK) {
    if (bdf_text_search_plain(search, &out, &n, &err) == BDF_OK) {
      bdf_free(out);
    }
    bdf_text_hits *hits;
    if (bdf_text_search_find(search, query, qlen, 16, context, &hits, &err) == BDF_OK) {
      for (size_t i = 0; i < bdf_text_hits_count(hits); i++) {
        bdf_text_hit h;
        bdf_text_hits_at(hits, i, &h);
        if (h.nsegments == 0 || strlen(h.context) != h.context_len) {
          abort();
        }
        size_t total = 0;
        for (size_t k = 0; k < h.nsegments; k++) {
          bdf_text_hit_segment seg;
          bdf_index_run run;
          bdf_text_hits_segment(hits, i, k, &seg);
          if (bdf_text_index_run(index, seg.run, &run) != BDF_OK || seg.start >= seg.end ||
              seg.end > run.text_len) {
            abort();
          }
          total += seg.end - seg.start;
        }
        if (h.text_len < total) { /* the text of the segments, and the spaces between them */
          abort();
        }
      }
      bdf_text_hits_free(hits);
    }
    bdf_text_search_free(search);
  }
  bdf_text_index_free(index);
  return 0;
}
