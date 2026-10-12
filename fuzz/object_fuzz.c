/* A libFuzzer target over the part decoders: an Object part (its tables and
 * every instruction and operand), a path collection, and a file header. */
#include <stddef.h>
#include <stdint.h>

#include "bdf/bdf.h"

static void touch_object(const uint8_t *data, size_t size) {
  bdf_object *o = NULL;
  bdf_error err;
  if (bdf_object_decode(data, size, &o, &err) != BDF_OK) {
    return;
  }
  float bbox[4];
  bdf_object_bbox(o, bbox);
  size_t len;
  for (size_t i = 0; i < bdf_object_string_count(o); i++) {
    bdf_object_string(o, i, &len);
  }
  bdf_path p;
  for (size_t i = 0; i < bdf_object_path_count(o); i++) {
    bdf_object_path(o, i, &p);
  }
  bdf_paint pt;
  for (size_t i = 0; i < bdf_object_paint_count(o); i++) {
    bdf_object_paint(o, i, &pt);
  }
  bdf_font f;
  for (size_t i = 0; i < bdf_object_font_count(o); i++) {
    bdf_object_font(o, i, &f);
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
      case BDF_ARG_F32S: {
        const float *s = bdf_instr_f32s(it, i, &m);
        for (size_t k = 0; k < m; k++) {
          sink += s[k];
        }
        break;
      }
      case BDF_ARG_GLYPHS: {
        const bdf_glyph *g = bdf_instr_glyphs(it, i, &m);
        for (size_t k = 0; k < m; k++) {
          sink += g[k].x;
        }
        break;
      }
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
  touch_object(data, size);
  bdf_paths *ps = NULL;
  bdf_error err;
  if (bdf_paths_decode(data, size, &ps, &err) == BDF_OK) {
    bdf_path p;
    for (size_t i = 0; i < bdf_paths_count(ps); i++) {
      bdf_paths_get(ps, i, &p);
    }
    bdf_paths_free(ps);
  }
  bdf_header h;
  bdf_header_parse(data, size, size, &h, &err);
  return 0;
}
