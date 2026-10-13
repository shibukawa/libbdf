/* A libFuzzer target over encrypted documents and segments. The input is two
 * documents one after the other, the first 4 bytes the length of the first
 * (little-endian): each is opened and unlocked with the password "pass" and
 * with a fixed private key, read through, and the second is then put into
 * the first as a segment. The seeds are documents the reference wrote with
 * that password or sealed for that key, so that the fuzzer gets past the
 * unlock into the sealed manifest and the sealed parts.
 *
 * The fuzz build lowers the iterations a document may ask for
 * (BDF_MAX_KDF_ITERATIONS), so that an input is no test of patience. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bdf/bdf.h"

/* the private key of go/genvectors */
static const uint8_t private_key[32] = {0xc9, 0xaf, 0xa9, 0xd8, 0x45, 0xba, 0x75, 0x16,
                                        0x6b, 0x5c, 0x21, 0x57, 0x67, 0xb1, 0xd6, 0x93,
                                        0x4e, 0x50, 0xc3, 0xdb, 0x36, 0xe8, 0x9b, 0x12,
                                        0x7b, 0x8a, 0x62, 0x2b, 0x12, 0x0f, 0x67, 0x21};

static bdf_doc *open_one(const uint8_t *data, size_t size) {
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(data, size, &doc, &err) != BDF_OK) {
    return NULL;
  }
  if (bdf_doc_locked(doc) && bdf_doc_unlock(doc, "pass", 4, &err) != BDF_OK) {
    bdf_doc_unlock_ecdh(doc, private_key, &err);
  }
  return doc;
}

static void read_through(const bdf_doc *doc) {
  bdf_error err;
  bdf_segment seg;
  bdf_doc_segment(doc, &seg);
  size_t n = bdf_doc_part_count(doc);
  for (size_t i = 0; i < n && i < 64; i++) {
    bdf_part_entry e;
    uint8_t *out;
    size_t len;
    if (bdf_doc_part_entry(doc, i, &e) == BDF_OK &&
        bdf_doc_part(doc, e.hash, &out, &len, &err) == BDF_OK) {
      bdf_free(out);
    }
  }
  for (size_t v = 0; v < bdf_doc_view_count(doc) && v < 4; v++) {
    bdf_view view;
    bdf_doc_view(doc, v, &view);
    for (size_t p = 0; p < view.npages && p < 16; p++) {
      bdf_page page;
      bdf_layer layer;
      bdf_text *text;
      if (bdf_doc_page(doc, v, p, &page) == BDF_OK && page.nlayers > 0 &&
          bdf_doc_layer(doc, v, p, 0, &layer) == BDF_OK &&
          bdf_doc_text(doc, layer.obj, &text, &err) == BDF_OK) {
        bdf_text_free(text);
      }
    }
  }
  bdf_search_text *st;
  if (bdf_doc_search_text(doc, &st, &err) == BDF_OK) {
    bdf_search_text_free(st);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 4) {
    return 0;
  }
  size_t first = (size_t)data[0] | (size_t)data[1] << 8 | (size_t)data[2] << 16 |
                 (size_t)data[3] << 24;
  data += 4;
  size -= 4;
  if (first > size) {
    first = size;
  }
  bdf_doc *a = open_one(data, first);
  bdf_doc *b = open_one(data + first, size - first);
  if (a) {
    read_through(a);
  }
  if (a && b) {
    bdf_error err;
    if (bdf_doc_add_segment(a, b, &err) == BDF_OK) {
      b = NULL; /* a's now */
      read_through(a);
    }
  }
  bdf_doc_free(b);
  bdf_doc_free(a);
  return 0;
}
