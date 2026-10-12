/* A libFuzzer target over a whole single-file document: the header, the
 * manifest, every part, and the objects and path collections among them. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bdf/bdf.h"

static void touch_object(const uint8_t *data, size_t size) {
  bdf_object *o = NULL;
  bdf_error err;
  if (bdf_object_decode(data, size, &o, &err) != BDF_OK) {
    return;
  }
  bdf_instr *it = bdf_instr_new(o);
  volatile float sink = 0;
  while (bdf_instr_next(it, &err) > 0) {
    size_t n = bdf_instr_arg_count(it);
    for (size_t i = 0; i < n; i++) {
      size_t m;
      switch (bdf_instr_arg_kind_at(it, i)) {
      case BDF_ARG_F32:
        sink += bdf_instr_f32(it, i);
        break;
      case BDF_ARG_STRING:
        bdf_instr_string(it, i, &m);
        break;
      case BDF_ARG_BYTES:
        bdf_instr_bytes(it, i, &m);
        break;
      case BDF_ARG_F32S:
        bdf_instr_f32s(it, i, &m);
        break;
      case BDF_ARG_GLYPHS:
        bdf_instr_glyphs(it, i, &m);
        break;
      default:
        sink += (float)bdf_instr_u64(it, i);
        break;
      }
    }
  }
  bdf_instr_free(it);
  bdf_object_free(o);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(data, size, &doc, &err) != BDF_OK) {
    return 0;
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
        bdf_doc_part_find(doc, layer.obj, NULL);
      }
    }
  }
  size_t n = bdf_doc_part_count(doc);
  for (size_t i = 0; i < n && i < 64; i++) {
    bdf_part_entry e;
    uint8_t *out;
    size_t len;
    bdf_doc_part_entry(doc, i, &e);
    if (bdf_doc_part(doc, e.hash, &out, &len, &err) != BDF_OK) {
      continue;
    }
    if (e.type_len == 3 && memcmp(e.type, "obj", 3) == 0) {
      touch_object(out, len);
    } else if (e.type_len == 4 && memcmp(e.type, "path", 4) == 0) {
      bdf_paths *ps;
      if (bdf_paths_decode(out, len, &ps, &err) == BDF_OK) {
        bdf_paths_free(ps);
      }
    } else if (e.type_len == 3 && memcmp(e.type, "idx", 3) == 0) {
      uint8_t h[16];
      bdf_tiles_index_find(out, len, 0, 0, h);
    }
    bdf_free(out);
  }
  /* the text of the first views, and of the whole document for a search
   * index (the fuzz build lowers the limit that bounds these walks) */
  for (size_t v = 0; v < bdf_doc_view_count(doc) && v < 4; v++) {
    bdf_text_index *idx;
    if (bdf_doc_view_text(doc, v, 1, &idx, &err) == BDF_OK) {
      bdf_text_index_free(idx);
    }
    bdf_page page;
    bdf_layer layer;
    bdf_text *text;
    if (bdf_doc_page(doc, v, 0, &page) == BDF_OK && bdf_doc_layer(doc, v, 0, 0, &layer) == BDF_OK &&
        bdf_doc_text(doc, layer.obj, &text, &err) == BDF_OK) {
      bdf_text_free(text);
    }
  }
  bdf_search_text *st;
  if (bdf_doc_search_text(doc, &st, &err) == BDF_OK) {
    bdf_search_text_free(st);
  }
  bdf_doc_free(doc);
  return 0;
}
