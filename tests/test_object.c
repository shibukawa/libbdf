/* Unit tests of the Object part decoder and the instruction iterator. The
 * cases mirror bdf_test.go and malformed_test.go of the reference
 * implementation; the differential tests under go/ cover real documents. */
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

/* ---- A small encoder ---------------------------------------------------- */

typedef struct wr {
  uint8_t *b;
  size_t n, cap;
} wr;

static void w_bytes(wr *w, const void *p, size_t n) {
  if (w->n + n > w->cap) {
    w->cap = (w->n + n) * 2 + 64;
    w->b = realloc(w->b, w->cap);
    if (!w->b) {
      abort();
    }
  }
  memcpy(w->b + w->n, p, n);
  w->n += n;
}

static void w_u8(wr *w, uint8_t v) { w_bytes(w, &v, 1); }

static void w_u16(wr *w, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  w_bytes(w, b, 2);
}

static void w_u32(wr *w, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  w_bytes(w, b, 4);
}

static void w_f32(wr *w, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  w_u32(w, u);
}

static void w_varuint(wr *w, uint64_t v) {
  while (v >= 0x80) {
    w_u8(w, (uint8_t)(v | 0x80));
    v >>= 7;
  }
  w_u8(w, (uint8_t)v);
}

static void w_str(wr *w, const char *s) {
  w_varuint(w, strlen(s));
  w_bytes(w, s, strlen(s));
}

static void w_hash(wr *w, uint8_t fill) {
  uint8_t h[16];
  memset(h, fill, sizeof h);
  w_bytes(w, h, sizeof h);
}

static void w_object_header(wr *w, uint16_t opset, uint16_t flags) {
  w_bytes(w, "BOBJ", 4);
  w_u16(w, opset);
  w_u16(w, flags);
  w_f32(w, 0);
  w_f32(w, 0);
  w_f32(w, 40);
  w_f32(w, 30);
}

/* object assembles an object from encoded tables: each is a count followed
 * by its entries, or NULL for an empty table. */
static wr object(const wr *strings, const wr *paths, const wr *paints, const wr *fonts,
                 const wr *ops) {
  wr w = {0};
  w_object_header(&w, 1, 0);
  const wr *tables[] = {strings, paths, paints, fonts, NULL, NULL};
  for (size_t i = 0; i < 6; i++) {
    if (tables[i]) {
      w_bytes(&w, tables[i]->b, tables[i]->n);
    } else {
      w_varuint(&w, 0);
    }
  }
  w_varuint(&w, ops ? ops->n : 0);
  if (ops) {
    w_bytes(&w, ops->b, ops->n);
  }
  return w;
}

/* sample is the object of TestRoundTripObject, plus a paint, an image, a
 * child object and a MASK_BEGIN, so that every table and every signature
 * class is read. */
static wr sample(size_t *ops_len) {
  wr w = {0};
  w_object_header(&w, 1, 0);
  /* strings */
  w_varuint(&w, 1);
  w_str(&w, "hi");
  /* paths: one inline triangle */
  w_varuint(&w, 1);
  w_u8(&w, 0);
  w_varuint(&w, 4);
  w_u8(&w, BDF_VERB_MOVE);
  w_u8(&w, BDF_VERB_LINE);
  w_u8(&w, BDF_VERB_LINE);
  w_u8(&w, BDF_VERB_CLOSE);
  float args[] = {0, 0, 10, 0, 10, 10};
  for (size_t i = 0; i < 6; i++) {
    w_f32(&w, args[i]);
  }
  /* paints: one linear gradient */
  w_varuint(&w, 1);
  w_u8(&w, BDF_PAINT_LINEAR);
  w_f32(&w, 0);
  w_f32(&w, 0);
  w_f32(&w, 1);
  w_f32(&w, 1);
  w_varuint(&w, 2);
  w_f32(&w, 0);
  w_u32(&w, 0xFF0000FFu);
  w_f32(&w, 1);
  w_u32(&w, 0x0000FFFFu);
  /* fonts: one system font */
  w_varuint(&w, 1);
  w_u8(&w, BDF_FONT_SYSTEM);
  w_str(&w, "sans-serif");
  w_u16(&w, 400);
  w_u8(&w, BDF_STYLE_NORMAL);
  /* images, objects */
  w_varuint(&w, 1);
  w_hash(&w, 0x11);
  w_varuint(&w, 1);
  w_hash(&w, 0x22);
  /* ops */
  wr ops = {0};
  w_u8(&ops, BDF_OP_FILL_COLOR);
  w_u32(&ops, 0x010203FFu);
  w_u8(&ops, BDF_OP_FILL_PATH);
  w_varuint(&ops, 0);
  w_u8(&ops, BDF_FILL_NONZERO);
  w_u8(&ops, BDF_OP_DASH);
  w_varuint(&ops, 2);
  w_f32(&ops, 1);
  w_f32(&ops, 2);
  w_f32(&ops, 0.5f);
  w_u8(&ops, BDF_OP_FONT);
  w_varuint(&ops, 0);
  w_f32(&ops, 12);
  w_u8(&ops, BDF_OP_FILL_TEXT);
  w_varuint(&ops, 0);
  w_f32(&ops, 1);
  w_f32(&ops, 2);
  w_f32(&ops, 3);
  w_u8(&ops, BDF_OP_FILL_PATH_RUN);
  w_u8(&ops, BDF_FILL_EVENODD);
  w_varuint(&ops, 2);
  w_varuint(&ops, 0);
  w_f32(&ops, 20);
  w_f32(&ops, 20);
  w_varuint(&ops, 0);
  w_f32(&ops, 30);
  w_f32(&ops, 20);
  w_u8(&ops, BDF_OP_MASK_BEGIN);
  w_u8(&ops, BDF_MASK_ALPHA);
  w_u32(&ops, 0);
  w_varuint(&ops, 0);
  w_u8(&ops, BDF_OP_EXT);
  w_u32(&ops, 3);
  w_u8(&ops, 1);
  w_u8(&ops, 2);
  w_u8(&ops, 3);
  w_varuint(&w, ops.n);
  w_bytes(&w, ops.b, ops.n);
  *ops_len = ops.n;
  free(ops.b);
  return w;
}

/* ---- Tests -------------------------------------------------------------- */

static int count_instrs(bdf_object *o, bdf_error *err) {
  bdf_instr *it = bdf_instr_new(o);
  int n = 0, rc;
  while ((rc = bdf_instr_next(it, err)) > 0) {
    n++;
  }
  bdf_instr_free(it);
  return rc < 0 ? -1 : n;
}

static void test_sample(void) {
  size_t ops_len;
  wr w = sample(&ops_len);
  bdf_object *o = NULL;
  bdf_error err;
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_OK);
  CHECK(o != NULL);
  if (!o) {
    free(w.b);
    return;
  }
  CHECK(bdf_object_opset(o) == 1);
  float bbox[4];
  bdf_object_bbox(o, bbox);
  CHECK(bbox[0] == 0 && bbox[1] == 0 && bbox[2] == 40 && bbox[3] == 30);

  size_t len;
  CHECK(bdf_object_string_count(o) == 1);
  const char *s = bdf_object_string(o, 0, &len);
  CHECK(s && len == 2 && strcmp(s, "hi") == 0);
  CHECK(bdf_object_string(o, 1, &len) == NULL && len == 0);

  bdf_path p;
  CHECK(bdf_object_path_count(o) == 1);
  CHECK(bdf_object_path(o, 0, &p) == BDF_OK);
  CHECK(!p.external && p.nverbs == 4 && p.nargs == 6 && p.args[2] == 10 && p.hash == NULL);
  CHECK(p.verbs[3] == BDF_VERB_CLOSE);
  CHECK(bdf_object_path(o, 1, &p) == BDF_ERR_ARG);

  bdf_paint pt;
  CHECK(bdf_object_paint_count(o) == 1);
  CHECK(bdf_object_paint(o, 0, &pt) == BDF_OK);
  CHECK(pt.kind == BDF_PAINT_LINEAR && pt.ncoords == 4 && pt.coords[2] == 1);
  CHECK(pt.nstops == 2 && pt.stops[1].offset == 1 && pt.stops[1].color == 0x0000FFFFu);

  bdf_font f;
  CHECK(bdf_object_font_count(o) == 1);
  CHECK(bdf_object_font(o, 0, &f) == BDF_OK);
  CHECK(f.kind == BDF_FONT_SYSTEM && f.hash == NULL && f.weight == 400 && f.style == 0);
  CHECK(f.family_len == 10 && strcmp(f.family, "sans-serif") == 0);

  CHECK(bdf_object_image_count(o) == 1 && bdf_object_image(o, 0)[15] == 0x11);
  CHECK(bdf_object_object_count(o) == 1 && bdf_object_object(o, 0)[0] == 0x22);
  CHECK(bdf_object_image(o, 1) == NULL);
  CHECK(bdf_object_ops(o, &len) != NULL && len == ops_len);

  /* the instructions, operand by operand */
  bdf_instr *it = bdf_instr_new(o);
  CHECK(it != NULL);
  CHECK(bdf_instr_next(it, &err) == 1);
  CHECK(bdf_instr_op(it) == BDF_OP_FILL_COLOR && bdf_instr_offset(it) == 0);
  CHECK(strcmp(bdf_op_name(bdf_instr_op(it)), "FILL_COLOR") == 0);
  CHECK(bdf_instr_arg_count(it) == 1 && bdf_instr_arg_kind_at(it, 0) == BDF_ARG_U32);
  CHECK(bdf_instr_u64(it, 0) == 16909311);
  CHECK(bdf_instr_f32(it, 0) == 0); /* the wrong kind reads as zero */

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_FILL_PATH);
  CHECK(bdf_instr_offset(it) == 5);
  CHECK(bdf_instr_arg_kind_at(it, 0) == BDF_ARG_VARUINT && bdf_instr_u64(it, 0) == 0);
  CHECK(bdf_instr_arg_kind_at(it, 1) == BDF_ARG_U8 && bdf_instr_u64(it, 1) == 0);

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_DASH);
  size_t n;
  const float *segs = bdf_instr_f32s(it, 0, &n);
  CHECK(segs && n == 2 && segs[0] == 1 && segs[1] == 2);
  CHECK(bdf_instr_arg_kind_at(it, 1) == BDF_ARG_F32 && bdf_instr_f32(it, 1) == 0.5f);

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_FONT);
  CHECK(bdf_instr_u64(it, 0) == 0 && bdf_instr_f32(it, 1) == 12);

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_FILL_TEXT);
  s = bdf_instr_string(it, 0, &len);
  CHECK(s && len == 2 && strcmp(s, "hi") == 0);
  CHECK(bdf_instr_f32(it, 1) == 1 && bdf_instr_f32(it, 2) == 2 && bdf_instr_f32(it, 3) == 3);

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_FILL_PATH_RUN);
  CHECK(bdf_instr_u64(it, 0) == BDF_FILL_EVENODD);
  const bdf_glyph *g = bdf_instr_glyphs(it, 1, &n);
  CHECK(g && n == 2 && g[0].path == 0 && g[0].x == 20 && g[1].x == 30 && g[1].y == 20);

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_MASK_BEGIN);
  CHECK(bdf_instr_arg_count(it) == 3 && bdf_instr_arg_kind_at(it, 2) == BDF_ARG_BYTES);
  CHECK(bdf_instr_bytes(it, 2, &n) != NULL && n == 0);

  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_EXT);
  const uint8_t *b = bdf_instr_bytes(it, 0, &n);
  CHECK(b && n == 3 && b[0] == 1 && b[2] == 3);

  CHECK(bdf_instr_next(it, &err) == 0);
  CHECK(bdf_instr_next(it, &err) == 0);
  CHECK(bdf_instr_arg_count(it) == 0);
  bdf_instr_rewind(it);
  CHECK(bdf_instr_next(it, &err) == 1 && bdf_instr_op(it) == BDF_OP_FILL_COLOR);
  bdf_instr_free(it);
  CHECK(count_instrs(o, &err) == 8);

  bdf_object_free(o);
  free(w.b);
}

static void test_truncation(void) {
  size_t ops_len;
  wr w = sample(&ops_len);
  for (size_t n = 0; n < w.n; n++) {
    bdf_object *o = NULL;
    bdf_error err;
    bdf_status st = bdf_object_decode(w.b, n, &o, &err);
    if (st != BDF_ERR_FORMAT || o != NULL) {
      failures++;
      fprintf(stderr, "truncated to %zu bytes: status %d\n", n, (int)st);
      bdf_object_free(o);
    }
  }
  free(w.b);
}

static void test_unknown_opcode(void) {
  size_t ops_len;
  wr w = sample(&ops_len);
  /* EXT is the last 8 bytes of the stream: opcode, u32 length, 3 bytes */
  size_t ext = w.n - 8;
  w.b[ext] = 0x7E;
  bdf_object *o = NULL;
  bdf_error err;
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_OK);
  bdf_instr *it = bdf_instr_new(o);
  int n = 0, rc;
  while ((rc = bdf_instr_next(it, &err)) > 0) {
    n++;
  }
  CHECK(n == 7 && rc == -1);
  CHECK(err.status == BDF_ERR_FORMAT && err.offset == ops_len - 8);
  CHECK(strcmp(err.message, "unknown opcode") == 0);
  CHECK(bdf_instr_next(it, &err) == -1); /* the walk stays over */
  bdf_instr_free(it);
  bdf_object_free(o);
  free(w.b);
}

/* expect_decode_error builds an object from the tables and checks that
 * decoding it fails with the message. */
static void expect_decode_error(const char *name, const wr *strings, const wr *paths,
                                const wr *paints, const wr *fonts, const char *message) {
  wr w = object(strings, paths, paints, fonts, NULL);
  bdf_object *o = NULL;
  bdf_error err;
  bdf_status st = bdf_object_decode(w.b, w.n, &o, &err);
  if (st != BDF_ERR_FORMAT || strcmp(err.message, message) != 0) {
    failures++;
    fprintf(stderr, "%s: status %d, message %s\n", name, (int)st, err.message);
  }
  bdf_object_free(o);
  free(w.b);
}

/* expect_instr_error builds an object with the op stream and checks that
 * decoding succeeds and the walk fails with the message. */
static void expect_instr_error(const char *name, const wr *ops, const char *message) {
  wr w = object(NULL, NULL, NULL, NULL, ops);
  bdf_object *o = NULL;
  bdf_error err;
  if (bdf_object_decode(w.b, w.n, &o, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "%s: decode failed: %s\n", name, err.message);
  } else if (count_instrs(o, &err) != -1 || strcmp(err.message, message) != 0) {
    failures++;
    fprintf(stderr, "%s: message %s\n", name, err.message);
  }
  bdf_object_free(o);
  free(w.b);
}

static void test_huge_counts(void) {
  wr huge = {0};
  w_varuint(&huge, (uint64_t)1 << 62);
  uint8_t pad[64] = {0};
  w_bytes(&huge, pad, sizeof pad);

  wr t = {0};
  w_varuint(&t, 1);
  w_u8(&t, 0);
  w_bytes(&t, huge.b, huge.n);
  expect_decode_error("path verbs", NULL, &t, NULL, NULL, "bad path verb count");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_varuint(&t, 1);
  w_u8(&t, BDF_PAINT_LINEAR);
  w_bytes(&t, pad, 16);
  w_bytes(&t, huge.b, huge.n);
  expect_decode_error("paint stops", NULL, NULL, &t, NULL, "bad stop count");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_bytes(&t, huge.b, huge.n);
  expect_decode_error("strings", &t, NULL, NULL, NULL, "bad count");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_varuint(&t, 1);
  w_u8(&t, BDF_FONT_SYSTEM);
  w_bytes(&t, huge.b, huge.n);
  expect_decode_error("font family", NULL, NULL, NULL, &t, "bad string length");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_DASH);
  w_bytes(&t, huge.b, huge.n);
  expect_instr_error("dash", &t, "bad dash count");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_FILL_PATH_RUN);
  w_u8(&t, BDF_FILL_NONZERO);
  w_bytes(&t, huge.b, huge.n);
  expect_instr_error("glyph run", &t, "bad run count");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_MASK_BEGIN);
  w_u8(&t, 0);
  w_u32(&t, 0);
  w_bytes(&t, huge.b, huge.n);
  expect_instr_error("mask bytes", &t, "bad byte count");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_EXT);
  w_u32(&t, 0xFFFFFFFFu);
  w_bytes(&t, pad, sizeof pad);
  expect_instr_error("ext", &t, "unexpected end of data");
  free(t.b);

  bdf_paths *ps = NULL;
  bdf_error err;
  CHECK(bdf_paths_decode(huge.b, huge.n, &ps, &err) == BDF_ERR_FORMAT && ps == NULL);
  CHECK(strcmp(err.message, "bad path collection count") == 0);
  free(huge.b);
}

static void test_bad_values(void) {
  wr t = {0};
  w_varuint(&t, 1);
  w_u8(&t, 0);
  w_varuint(&t, 1);
  w_u8(&t, 9);
  expect_decode_error("verb", NULL, &t, NULL, NULL, "unknown path verb");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_varuint(&t, 1);
  w_u8(&t, 4);
  expect_decode_error("paint kind", NULL, NULL, &t, NULL, "unknown paint kind");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_FILL_TEXT);
  w_varuint(&t, 5);
  w_f32(&t, 0);
  w_f32(&t, 0);
  w_f32(&t, 0);
  expect_instr_error("string ref", &t, "bad string ref");
  free(t.b);

  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_FILL_RECT);
  w_f32(&t, 0);
  expect_instr_error("short operands", &t, "unexpected end of data");
  free(t.b);

  /* a varuint of 10 bytes with the last above 1 overflows */
  memset(&t, 0, sizeof t);
  w_u8(&t, BDF_OP_USE);
  for (int i = 0; i < 9; i++) {
    w_u8(&t, 0xFF);
  }
  w_u8(&t, 0x02);
  expect_instr_error("varuint overflow", &t, "bad varuint");
  free(t.b);
}

static void test_headers(void) {
  bdf_object *o = NULL;
  bdf_error err;
  wr w = {0};
  w_object_header(&w, 1, 0);
  w.b[3] = 'X';
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_ERR_FORMAT);
  CHECK(strcmp(err.message, "not an object part") == 0 && err.offset == 0);
  w.b[3] = 'J';
  w.b[4] = 2;
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_ERR_FORMAT);
  CHECK(strcmp(err.message, "unsupported opset") == 0);
  w.b[4] = 1;
  w.b[6] = 1;
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_ERR_FORMAT);
  CHECK(strcmp(err.message, "unsupported object flags") == 0);
  /* a header alone: the tables are missing */
  w.b[6] = 0;
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_ERR_FORMAT && o == NULL);
  CHECK(strcmp(err.message, "bad varuint") == 0 && err.offset == w.n);
  /* "BOBJ" cut after the magic reads opset 0, as the reference does */
  CHECK(bdf_object_decode(w.b, 4, &o, &err) == BDF_ERR_FORMAT);
  CHECK(strcmp(err.message, "unsupported opset") == 0);
  CHECK(bdf_object_decode(NULL, 0, &o, &err) == BDF_ERR_FORMAT);
  CHECK(strcmp(err.message, "not an object part") == 0);
  CHECK(bdf_object_decode(w.b, w.n, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_object_decode(NULL, 3, &o, &err) == BDF_ERR_ARG);
  free(w.b);

  /* the smallest object: empty tables and no instructions */
  wr e = object(NULL, NULL, NULL, NULL, NULL);
  CHECK(bdf_object_decode(e.b, e.n, &o, &err) == BDF_OK);
  CHECK(count_instrs(o, &err) == 0);
  bdf_object_free(o);
  /* trailing bytes after the op stream are ignored, as in the reference */
  w_u8(&e, 0xAB);
  CHECK(bdf_object_decode(e.b, e.n, &o, &err) == BDF_OK);
  bdf_object_free(o);
  free(e.b);
}

static void test_paths(void) {
  wr w = {0};
  w_varuint(&w, 2);
  w_varuint(&w, 1);
  w_u8(&w, BDF_VERB_RECT);
  w_f32(&w, 1);
  w_f32(&w, 2);
  w_f32(&w, 3);
  w_f32(&w, 4);
  w_varuint(&w, 0);
  bdf_paths *ps = NULL;
  bdf_error err;
  CHECK(bdf_paths_decode(w.b, w.n, &ps, &err) == BDF_OK);
  CHECK(bdf_paths_count(ps) == 2);
  bdf_path p;
  CHECK(bdf_paths_get(ps, 0, &p) == BDF_OK && p.nverbs == 1 && p.nargs == 4 && p.args[3] == 4);
  CHECK(bdf_paths_get(ps, 1, &p) == BDF_OK && p.nverbs == 0 && p.nargs == 0);
  CHECK(bdf_paths_get(ps, 2, &p) == BDF_ERR_ARG);
  bdf_paths_free(ps);
  /* the second path cut short */
  CHECK(bdf_paths_decode(w.b, w.n - 1, &ps, &err) == BDF_ERR_FORMAT && ps == NULL);
  free(w.b);
}

/* A byte order mark at the start of a string is not part of it (spec §6):
 * one is dropped, as a decoder of UTF-8 drops one, and no other. */
static void test_byte_order_mark(void) {
  wr strings = {0};
  w_varuint(&strings, 4);
  w_str(&strings, "\xEF\xBB\xBFmarked");
  w_str(&strings, "\xEF\xBB\xBF\xEF\xBB\xBFtwice");
  w_str(&strings, "in\xEF\xBB\xBFside");
  w_str(&strings, "\xEF\xBB\xBF");
  wr w = object(&strings, NULL, NULL, NULL, NULL);
  bdf_object *o = NULL;
  bdf_error err;
  CHECK(bdf_object_decode(w.b, w.n, &o, &err) == BDF_OK);
  if (o) {
    size_t len = 0;
    const char *s = bdf_object_string(o, 0, &len);
    CHECK(s && len == 6 && memcmp(s, "marked", 7) == 0);
    s = bdf_object_string(o, 1, &len);
    CHECK(s && len == 8 && memcmp(s, "\xEF\xBB\xBFtwice", 9) == 0);
    s = bdf_object_string(o, 2, &len);
    CHECK(s && len == 9 && memcmp(s, "in\xEF\xBB\xBFside", 10) == 0);
    s = bdf_object_string(o, 3, &len);
    CHECK(s && len == 0 && s[0] == 0);
  }
  bdf_object_free(o);
  free(strings.b);
  free(w.b);
}

static void test_misc(void) {
  CHECK(strcmp(bdf_version(), BDF_VERSION_STRING) == 0);
  CHECK(strcmp(bdf_op_name(BDF_OP_IMAGE_SUB), "IMAGE_SUB") == 0);
  CHECK(strcmp(bdf_op_name(0x00), "") == 0 && strcmp(bdf_op_name(0x7E), "") == 0);
  CHECK(bdf_verb_args(BDF_VERB_ELLIPSE) == 8 && bdf_verb_args(9) == -1);
  CHECK(bdf_instr_new(NULL) == NULL);
  CHECK(bdf_object_string_count(NULL) == 0 && bdf_object_ops(NULL, NULL) == NULL);
  bdf_object_free(NULL);
  bdf_instr_free(NULL);
  bdf_paths_free(NULL);
}

int main(void) {
  test_sample();
  test_truncation();
  test_unknown_opcode();
  test_huge_counts();
  test_bad_values();
  test_headers();
  test_paths();
  test_byte_order_mark();
  test_misc();
  if (failures) {
    fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
