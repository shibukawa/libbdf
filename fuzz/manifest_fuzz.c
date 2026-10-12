/* A libFuzzer target over the manifest reader, through the split form with a
 * store that has no parts. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bdf/bdf.h"

static int no_part(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *size) {
  (void)ctx;
  (void)name;
  (void)buf;
  (void)cap;
  (void)size;
  return -1;
}

static void touch_doc(const bdf_doc *doc) {
  size_t len;
  bdf_meta meta;
  bdf_doc_meta(doc, &meta);
  bdf_doc_unit(doc, &len);
  bdf_doc_dc(doc, "title", 0, &len);
  bdf_segment seg;
  bdf_doc_segment(doc, &seg);
  bdf_doc_cipher(doc, &len);
  for (size_t i = 0; i < bdf_doc_key_count(doc); i++) {
    bdf_key_slot k;
    bdf_doc_key(doc, i, &k);
  }
  for (size_t v = 0; v < bdf_doc_view_count(doc); v++) {
    bdf_view view;
    bdf_doc_view(doc, v, &view);
    for (size_t p = 0; p < view.npages; p++) {
      bdf_page page;
      bdf_doc_page(doc, v, p, &page);
      for (size_t l = 0; l < page.nlayers; l++) {
        bdf_layer layer;
        bdf_doc_layer(doc, v, p, l, &layer);
      }
    }
    float run[2];
    for (size_t i = 0; i < view.ncols; i++) {
      bdf_doc_view_run(doc, v, 0, i, run);
    }
    for (size_t i = 0; i < view.nrows; i++) {
      bdf_doc_view_run(doc, v, 1, i, run);
    }
    for (size_t i = 0; i < view.ntiles; i++) {
      bdf_tile t;
      uint32_t tx, ty;
      bdf_doc_tile(doc, v, i, &t);
      bdf_tile_key_parse(t.key, t.key_len, &tx, &ty);
    }
  }
  for (size_t i = 0; i < bdf_doc_part_count(doc); i++) {
    bdf_part_entry e;
    bdf_doc_part_entry(doc, i, &e);
    bdf_doc_part_find(doc, e.hash, NULL);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bdf_source src;
  memset(&src, 0, sizeof src);
  src.read_part = no_part;
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_split(data, size, &src, &doc, &err) == BDF_OK) {
    touch_doc(doc);
    if (bdf_doc_part_count(doc) > 0) {
      bdf_part_entry e;
      uint8_t *out;
      size_t n;
      bdf_doc_part_entry(doc, 0, &e);
      if (bdf_doc_part(doc, e.hash, &out, &n, &err) == BDF_OK) {
        bdf_free(out);
      }
    }
    bdf_doc_free(doc);
  }
  return 0;
}
