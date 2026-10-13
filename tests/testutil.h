/* Helpers shared by the tests: a check macro, a small byte writer, and
 * builders of single-file documents. */
#ifndef BDF_TESTUTIL_H
#define BDF_TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>
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

#define CHECK_MSG(err, msg)                                                                        \
  do {                                                                                             \
    if (strcmp((err).message, (msg)) != 0) {                                                       \
      failures++;                                                                                  \
      fprintf(stderr, "%s:%d: message %s, want %s\n", __FILE__, __LINE__, (err).message, (msg));   \
    }                                                                                              \
  } while (0)

typedef struct wr {
  uint8_t *b;
  size_t n, cap;
} wr;

static inline void w_bytes(wr *w, const void *p, size_t n) {
  if (w->n + n > w->cap) {
    w->cap = (w->n + n) * 2 + 64;
    w->b = realloc(w->b, w->cap);
    if (!w->b) {
      abort();
    }
  }
  if (n > 0) {
    memcpy(w->b + w->n, p, n);
  }
  w->n += n;
}

static inline void w_u8(wr *w, uint8_t v) { w_bytes(w, &v, 1); }

static inline void w_u16(wr *w, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  w_bytes(w, b, 2);
}

static inline void w_u32(wr *w, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  w_bytes(w, b, 4);
}

static inline void w_u64(wr *w, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    w_u8(w, (uint8_t)(v >> (8 * i)));
  }
}

static inline void w_f32(wr *w, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  w_u32(w, u);
}

static inline void w_varuint(wr *w, uint64_t v) {
  while (v >= 0x80) {
    w_u8(w, (uint8_t)(v | 0x80));
    v >>= 7;
  }
  w_u8(w, (uint8_t)v);
}

static inline void w_str(wr *w, const char *s) {
  w_varuint(w, strlen(s));
  w_bytes(w, s, strlen(s));
}

static inline void w_text(wr *w, const char *s) { w_bytes(w, s, strlen(s)); }

/* w_header writes a single-file header with the manifest at offset 32. */
static inline void w_header(wr *w, uint16_t flags, uint64_t moff, uint64_t mlen, uint8_t menc) {
  w_bytes(w, "bdf\0", 4);
  w_u16(w, BDF_FORMAT_VERSION);
  w_u16(w, flags);
  w_u64(w, moff);
  w_u64(w, mlen);
  w_u8(w, menc);
  for (int i = 0; i < 7; i++) {
    w_u8(w, 0);
  }
}

/* w_stored writes bytes as one stored deflate block: valid deflate-raw data
 * without a compressor. */
static inline void w_stored(wr *w, const void *p, size_t n) {
  w_u8(w, 0x01); /* final block, stored */
  w_u16(w, (uint16_t)n);
  w_u16(w, (uint16_t)~n);
  w_bytes(w, p, n);
}

/* single builds a single-file document: the header, the manifest JSON as it
 * is, and the stored bytes of the parts after it. */
static inline wr single(const char *manifest, const void *parts, size_t parts_len) {
  wr w = {0};
  w_header(&w, 0, BDF_HEADER_SIZE, strlen(manifest), 0);
  w_text(&w, manifest);
  w_bytes(&w, parts, parts_len);
  return w;
}

/* open_single opens such a document and returns the status; the error is
 * left in *err and the document in *doc. */
static inline bdf_status open_single(const char *manifest, const void *parts, size_t parts_len,
                              bdf_doc **doc, bdf_error *err, wr *keep) {
  *keep = single(manifest, parts, parts_len);
  return bdf_doc_open_memory(keep->b, keep->n, doc, err);
}

/* manifest_ok reports whether a manifest alone opens, freeing what it made. */
static inline int manifest_opens(const char *manifest, bdf_error *err) {
  wr w;
  bdf_doc *doc = NULL;
  bdf_status st = open_single(manifest, NULL, 0, &doc, err, &w);
  bdf_doc_free(doc);
  free(w.b);
  return st == BDF_OK;
}

/* w_index_run writes a run of a text index part (spec §7.9). */
static inline void w_index_run(wr *w, uint32_t a, uint32_t b, uint32_t ordinal, uint8_t sep,
                               const char *text) {
  w_varuint(w, a);
  w_varuint(w, b);
  w_varuint(w, ordinal);
  w_u8(w, sep);
  w_str(w, text);
}

/* ---- An object builder --------------------------------------------------- */

/* ob assembles an Object part: its tables and its op stream. */
typedef struct ob {
  wr strings, paths, paints, fonts, images, objects, ops;
  size_t nstrings, npaths, npaints, nfonts, nimages, nobjects;
  float bbox[4];
} ob;

static inline size_t ob_str(ob *o, const char *s) {
  w_str(&o->strings, s);
  return o->nstrings++;
}

static inline size_t ob_font(ob *o, const char *family) {
  w_u8(&o->fonts, BDF_FONT_SYSTEM);
  w_str(&o->fonts, family);
  w_u16(&o->fonts, 400);
  w_u8(&o->fonts, BDF_STYLE_NORMAL);
  return o->nfonts++;
}

/* ob_font_part adds a font that is the part id (see ob_child), to be drawn
 * at a weight and in a style. */
static inline size_t ob_font_part(ob *o, uint32_t id, uint16_t weight, uint8_t style) {
  uint8_t h[16] = {0};
  h[0] = (uint8_t)(id >> 24);
  h[1] = (uint8_t)(id >> 16);
  h[2] = (uint8_t)(id >> 8);
  h[3] = (uint8_t)id;
  w_u8(&o->fonts, BDF_FONT_EMBEDDED);
  w_bytes(&o->fonts, h, sizeof h);
  w_str(&o->fonts, "");
  w_u16(&o->fonts, weight);
  w_u8(&o->fonts, style);
  return o->nfonts++;
}

/* ob_rect_path adds an inline path of one unit square. */
static inline size_t ob_rect_path(ob *o) {
  w_u8(&o->paths, 0);
  w_varuint(&o->paths, 1);
  w_u8(&o->paths, BDF_VERB_RECT);
  w_f32(&o->paths, 0);
  w_f32(&o->paths, 0);
  w_f32(&o->paths, 1);
  w_f32(&o->paths, 1);
  return o->npaths++;
}

/* ob_child adds a child object whose part name is id as four bytes and zeros. */
static inline size_t ob_child(ob *o, uint32_t id) {
  uint8_t h[16] = {0};
  h[0] = (uint8_t)(id >> 24);
  h[1] = (uint8_t)(id >> 16);
  h[2] = (uint8_t)(id >> 8);
  h[3] = (uint8_t)id;
  w_bytes(&o->objects, h, sizeof h);
  return o->nobjects++;
}

/* ob_image adds an image whose part name is id as four bytes and zeros. */
static inline size_t ob_image(ob *o, uint32_t id) {
  uint8_t h[16] = {0};
  h[0] = (uint8_t)(id >> 24);
  h[1] = (uint8_t)(id >> 16);
  h[2] = (uint8_t)(id >> 8);
  h[3] = (uint8_t)id;
  w_bytes(&o->images, h, sizeof h);
  return o->nimages++;
}

/* ob_pattern adds a pattern paint of an image, scaled by s and moved. */
static inline size_t ob_pattern(ob *o, size_t image, uint8_t repeat, float s, float dx, float dy) {
  w_u8(&o->paints, BDF_PAINT_PATTERN);
  w_varuint(&o->paints, image);
  w_u8(&o->paints, repeat);
  w_f32(&o->paints, s);
  w_f32(&o->paints, 0);
  w_f32(&o->paints, 0);
  w_f32(&o->paints, s);
  w_f32(&o->paints, dx);
  w_f32(&o->paints, dy);
  return o->npaints++;
}

static inline void ob_op(ob *o, uint8_t op) { w_u8(&o->ops, op); }

static inline void ob_fill_paint(ob *o, size_t paint) {
  ob_op(o, BDF_OP_FILL_PAINT);
  w_varuint(&o->ops, paint);
}

static inline void ob_smoothing(ob *o, int on) {
  ob_op(o, BDF_OP_SMOOTHING);
  w_u8(&o->ops, on ? 1 : 0);
  w_u8(&o->ops, 0);
}

/* ob_draw_image draws an image into a rectangle. */
static inline void ob_draw_image(ob *o, size_t image, float x, float y, float w, float h) {
  ob_op(o, BDF_OP_IMAGE);
  w_varuint(&o->ops, image);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, w);
  w_f32(&o->ops, h);
}

/* ob_draw_image_sub draws the part src (x, y, w, h in its pixels) of an
 * image into a rectangle. */
static inline void ob_draw_image_sub(ob *o, size_t image, const float src[4], float x, float y,
                                     float w, float h) {
  ob_op(o, BDF_OP_IMAGE_SUB);
  w_varuint(&o->ops, image);
  for (int i = 0; i < 4; i++) {
    w_f32(&o->ops, src[i]);
  }
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, w);
  w_f32(&o->ops, h);
}

static inline void ob_set_font(ob *o, size_t font, float size) {
  ob_op(o, BDF_OP_FONT);
  w_varuint(&o->ops, font);
  w_f32(&o->ops, size);
}

static inline void ob_text_style(ob *o, uint8_t align, uint8_t baseline, uint8_t dir, float spacing) {
  ob_op(o, BDF_OP_TEXT_STYLE);
  w_u8(&o->ops, align);
  w_u8(&o->ops, baseline);
  w_u8(&o->ops, dir);
  w_f32(&o->ops, spacing);
}

/* ob_stroke_text is ob_text with the outlines of the glyphs. */
static inline void ob_stroke_text(ob *o, const char *text, float x, float y, float advance) {
  size_t s = ob_str(o, text);
  ob_op(o, BDF_OP_STROKE_TEXT);
  w_varuint(&o->ops, s);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, advance);
}

static inline void ob_align(ob *o, uint8_t align) {
  ob_op(o, BDF_OP_TEXT_STYLE);
  w_u8(&o->ops, align);
  w_u8(&o->ops, 0);
  w_u8(&o->ops, 0);
  w_f32(&o->ops, 0);
}

static inline void ob_mark(ob *o, uint8_t kind, const char *payload) {
  size_t s = ob_str(o, payload);
  ob_op(o, BDF_OP_MARK);
  w_u8(&o->ops, kind);
  w_varuint(&o->ops, s);
}

static inline void ob_text(ob *o, const char *text, float x, float y, float advance) {
  size_t s = ob_str(o, text);
  ob_op(o, BDF_OP_FILL_TEXT);
  w_varuint(&o->ops, s);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, advance);
}

static inline void ob_path_at(ob *o, size_t path, float x, float y) {
  ob_op(o, BDF_OP_FILL_PATH_AT);
  w_varuint(&o->ops, path);
  w_u8(&o->ops, BDF_FILL_NONZERO);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
}

static inline void ob_translate(ob *o, float x, float y) {
  ob_op(o, BDF_OP_TRANSLATE);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
}

static inline void ob_scale(ob *o, float x, float y) {
  ob_op(o, BDF_OP_SCALE);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
}

static inline void ob_use(ob *o, size_t child) {
  ob_op(o, BDF_OP_USE);
  w_varuint(&o->ops, child);
}

static inline void ob_use_at(ob *o, size_t child, float x, float y) {
  ob_op(o, BDF_OP_USE_AT);
  w_varuint(&o->ops, child);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
}

static inline void ob_link(ob *o, float x, float y, float w, float h, const char *url) {
  size_t s = ob_str(o, url);
  ob_op(o, BDF_OP_LINK);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, w);
  w_f32(&o->ops, h);
  w_varuint(&o->ops, s);
}

static inline void ob_rect(ob *o, uint8_t op, float x, float y, float w, float h) {
  ob_op(o, op); /* FILL_RECT, STROKE_RECT, CLIP_RECT, CLEAR_RECT */
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, w);
  w_f32(&o->ops, h);
}

static inline void ob_mask_begin(ob *o) {
  ob_op(o, BDF_OP_MASK_BEGIN);
  w_u8(&o->ops, BDF_MASK_ALPHA);
  w_u32(&o->ops, 0);
  w_varuint(&o->ops, 0);
}

/* ob_encode writes the part and frees the builder. */
static inline wr ob_encode(ob *o) {
  wr w = {0};
  w_bytes(&w, "BOBJ", 4);
  w_u16(&w, BDF_OPSET_VERSION);
  w_u16(&w, 0);
  for (int i = 0; i < 4; i++) {
    w_f32(&w, o->bbox[i]);
  }
  w_varuint(&w, o->nstrings);
  w_bytes(&w, o->strings.b, o->strings.n);
  w_varuint(&w, o->npaths);
  w_bytes(&w, o->paths.b, o->paths.n);
  w_varuint(&w, o->npaints);
  w_bytes(&w, o->paints.b, o->paints.n);
  w_varuint(&w, o->nfonts);
  w_bytes(&w, o->fonts.b, o->fonts.n);
  w_varuint(&w, o->nimages);
  w_bytes(&w, o->images.b, o->images.n);
  w_varuint(&w, o->nobjects);
  w_bytes(&w, o->objects.b, o->objects.n);
  w_varuint(&w, o->ops.n);
  w_bytes(&w, o->ops.b, o->ops.n);
  free(o->strings.b);
  free(o->paths.b);
  free(o->paints.b);
  free(o->fonts.b);
  free(o->images.b);
  free(o->objects.b);
  free(o->ops.b);
  memset(o, 0, sizeof *o);
  return w;
}

/* ob_decode encodes and decodes: the object, or NULL. */
static inline bdf_object *ob_decode(ob *o) {
  wr w = ob_encode(o);
  bdf_object *obj = NULL;
  bdf_error err;
  if (bdf_object_decode(w.b, w.n, &obj, &err) != BDF_OK) {
    fprintf(stderr, "ob_decode: %s at %zu\n", err.message, err.offset);
  }
  free(w.b);
  return obj;
}

static inline void ob_fill_color(ob *o, uint32_t rgba) {
  ob_op(o, BDF_OP_FILL_COLOR);
  w_u32(&o->ops, rgba);
}

/* ---- A document builder -------------------------------------------------- */

/* docb assembles the parts of a single-file document. */
typedef struct docb {
  wr parts, entries;
} docb;

/* part_name writes the 32-digit name of part id, the name ob_child refers to. */
static inline void part_name(uint32_t id, char out[33]) { snprintf(out, 33, "%08x000000000000000000000000", id); }

static inline void docb_add(docb *d, uint32_t id, const char *type, wr *data) {
  char name[33], e[256];
  part_name(id, name);
  snprintf(e, sizeof e, "%s{\"h\":\"%s\",\"t\":\"%s\",\"enc\":\"identity\",\"len\":%zu,\"size\":%zu,\"off\":%zu}",
           d->entries.n ? "," : "", name, type, data->n, data->n, d->parts.n);
  w_text(&d->entries, e);
  w_bytes(&d->parts, data->b, data->n);
  free(data->b);
  data->b = NULL;
}

static inline void docb_object(docb *d, uint32_t id, ob *o) {
  wr w = ob_encode(o);
  docb_add(d, id, "obj", &w);
}

/* docb_open closes the manifest around views (a JSON array) and opens it. */
static inline bdf_doc *docb_open(docb *d, const char *views, wr *keep) {
  wr m = {0};
  w_text(&m, "{\"bdf\":1,\"opset\":1,\"views\":");
  w_text(&m, views);
  w_text(&m, ",\"parts\":[");
  w_bytes(&m, d->entries.b, d->entries.n);
  w_text(&m, "]}");
  w_u8(&m, 0);
  *keep = single((const char *)m.b, d->parts.b, d->parts.n);
  free(m.b);
  free(d->parts.b);
  free(d->entries.b);
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(keep->b, keep->n, &doc, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "docb_open: %s\n", err.message);
  }
  return doc;
}

static inline int finish(void) {
  if (failures) {
    fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}

#endif /* BDF_TESTUTIL_H */
