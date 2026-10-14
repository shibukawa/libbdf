/* Tests of drawing through a canvas (bdf_play_page, bdf_play_part): a canvas
 * that writes down what it is asked to do, and hand-built pages. */
#include <stdarg.h>

#include "testutil.h"

#include "render_vectors.h"

/* ---- A canvas that writes down its calls --------------------------------- */

typedef struct rec {
  char *s;
  size_t n, cap;
  int depth;        /* saves and layers that are open */
  int text;         /* 0: no text function; 1: it draws; 2: it declines */
  uint64_t ids[8];  /* the bitmaps with an id, in the order they came */
  uint32_t first[8]; /* and their first pixel */
  size_t nids;
} rec;

static void rec_f(rec *r, const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  if ((size_t)n >= sizeof line) {
    n = (int)sizeof line - 1;
  }
  if (r->n + (size_t)n + 1 > r->cap) {
    r->cap = 2 * (r->n + (size_t)n) + 256;
    r->s = realloc(r->s, r->cap);
  }
  memcpy(r->s + r->n, line, (size_t)n + 1);
  r->n += (size_t)n;
}

static void rec_path(rec *r, const bdf_canvas_path *p) {
  static const char names[] = "MLQCZ";
  static const int counts[] = {2, 2, 4, 6, 0};
  size_t c = 0;
  for (size_t i = 0; i < p->nverbs; i++) {
    uint8_t v = p->verbs[i];
    if (v > BDF_VERB_CLOSE) {
      rec_f(r, " ?%d", v);
      continue;
    }
    rec_f(r, " %c", names[v]);
    for (int k = 0; k < counts[v] && c < p->ncoords; k++) {
      rec_f(r, "%s%g", k ? "," : "", p->coords[c++]);
    }
  }
  if (c != p->ncoords) {
    rec_f(r, " (%zu numbers left over)", p->ncoords - c);
  }
}

/* The n of a bitmap: bitmaps with an id are numbered as they come. */
static void rec_bitmap(rec *r, const bdf_canvas_bitmap *b) {
  uint32_t px = (uint32_t)b->pixels[0] << 24 | (uint32_t)b->pixels[1] << 16 |
                (uint32_t)b->pixels[2] << 8 | b->pixels[3];
  if (b->id == 0) {
    /* a bitmap of this call alone: its first row from end to end */
    const uint8_t *e = b->pixels + 4 * ((size_t)b->width - 1);
    rec_f(r, "bitmap %dx%d %08x..%08x", b->width, b->height, px,
          (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3]);
    return;
  }
  size_t i = 0;
  while (i < r->nids && r->ids[i] != b->id) {
    i++;
  }
  if (i == r->nids && r->nids < 8) {
    r->ids[r->nids] = b->id;
    r->first[r->nids++] = px;
  }
  /* the same id: the same pixels */
  CHECK(i < 8 && r->first[i] == px);
  rec_f(r, "bitmap#%zu %dx%d %08x", i + 1, b->width, b->height, px);
}

static void rec_style(rec *r, const bdf_canvas_style *s) {
  if (s->paint) {
    const bdf_canvas_paint *p = s->paint;
    if (p->kind == BDF_PAINT_PATTERN) {
      rec_f(r, " pattern(");
      rec_bitmap(r, p->image);
      rec_f(r, " repeat=%d [%g %g %g %g %g %g])", p->repeat, p->matrix[0], p->matrix[1],
            p->matrix[2], p->matrix[3], p->matrix[4], p->matrix[5]);
    } else {
      rec_f(r, " gradient%d(", p->kind);
      for (size_t i = 0; i < p->ncoords; i++) {
        rec_f(r, "%s%g", i ? "," : "", (double)p->coords[i]);
      }
      for (size_t i = 0; i < p->nstops; i++) {
        rec_f(r, " %g:%08x", (double)p->stops[i].offset, p->stops[i].color);
      }
      rec_f(r, ")");
    }
  } else {
    rec_f(r, " %08x", s->color);
  }
  if (s->alpha != 1) {
    rec_f(r, " alpha=%g", (double)s->alpha);
  }
  if (s->blend != BDF_BLEND_SOURCE_OVER) {
    rec_f(r, " blend=%d", s->blend);
  }
  if ((s->shadow_color & 0xff) != 0 && (s->shadow_blur != 0 || s->shadow_dx != 0 || s->shadow_dy != 0)) {
    rec_f(r, " shadow=%08x,%g,%g,%g", s->shadow_color, s->shadow_blur, s->shadow_dx, s->shadow_dy);
  }
}

static void rec_line(rec *r, const bdf_canvas_line *l) {
  rec_f(r, " width=%g", l->width);
  if (l->cap != BDF_CAP_BUTT || l->join != BDF_JOIN_MITER) {
    rec_f(r, " cap=%d join=%d", l->cap, l->join);
  }
  if (l->miter != 10) {
    rec_f(r, " miter=%g", l->miter);
  }
  if (l->dash) {
    rec_f(r, " dash=");
    for (size_t i = 0; i < l->ndash; i++) {
      rec_f(r, "%s%g", i ? "," : "", (double)l->dash[i]);
    }
    rec_f(r, "+%g", l->dash_offset);
  }
}

static void rec_save(void *ctx) {
  rec *r = ctx;
  r->depth++;
  rec_f(r, "save\n");
}

static void rec_restore(void *ctx) {
  rec *r = ctx;
  r->depth--;
  CHECK(r->depth >= 0);
  rec_f(r, "restore\n");
}

static void rec_transform(void *ctx, const double m[6]) {
  /* + 0: a zero is written without its sign */
  rec_f(ctx, "transform %g %g %g %g %g %g\n", m[0] + 0, m[1] + 0, m[2] + 0, m[3] + 0, m[4] + 0,
        m[5] + 0);
}

static void rec_clip(void *ctx, const bdf_canvas_path *path, uint8_t rule) {
  rec_f(ctx, "clip%s", rule == BDF_FILL_EVENODD ? " evenodd" : "");
  rec_path(ctx, path);
  rec_f(ctx, "\n");
}

static void rec_fill(void *ctx, const bdf_canvas_path *path, uint8_t rule,
                     const bdf_canvas_style *style) {
  rec_f(ctx, "fill%s", rule == BDF_FILL_EVENODD ? " evenodd" : "");
  rec_path(ctx, path);
  rec_f(ctx, " with");
  rec_style(ctx, style);
  rec_f(ctx, "\n");
}

static void rec_stroke(void *ctx, const bdf_canvas_path *path, const bdf_canvas_line *line,
                       const bdf_canvas_style *style) {
  rec_f(ctx, "stroke");
  rec_path(ctx, path);
  rec_f(ctx, " with");
  rec_style(ctx, style);
  rec_line(ctx, line);
  rec_f(ctx, "\n");
}

static void rec_image(void *ctx, const bdf_canvas_bitmap *bitmap, const double src[4],
                      const double dst[4], const bdf_canvas_style *style) {
  rec_f(ctx, "image ");
  rec_bitmap(ctx, bitmap);
  rec_f(ctx, " %g,%g,%g,%g to %g,%g,%g,%g", src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2],
        dst[3]);
  if (style->alpha != 1) {
    rec_f(ctx, " alpha=%g", (double)style->alpha);
  }
  if (style->blend != BDF_BLEND_SOURCE_OVER) {
    rec_f(ctx, " blend=%d", style->blend);
  }
  rec_f(ctx, "%s\n", style->smoothing ? " smooth" : "");
}

static void rec_begin_layer(void *ctx, const double bounds[4], const bdf_canvas_style *s) {
  rec *r = ctx;
  r->depth++;
  rec_f(r, "begin_layer %g,%g,%g,%g alpha=%g blend=%d", bounds[0], bounds[1], bounds[2],
        bounds[3], (double)s->alpha, s->blend);
  if ((s->shadow_color & 0xff) != 0 && (s->shadow_blur != 0 || s->shadow_dx != 0 || s->shadow_dy != 0)) {
    rec_f(r, " shadow=%08x,%g,%g,%g", s->shadow_color, s->shadow_blur, s->shadow_dx, s->shadow_dy);
  }
  rec_f(r, "\n");
}

static void rec_end_layer(void *ctx) {
  rec *r = ctx;
  r->depth--;
  CHECK(r->depth >= 0);
  rec_f(r, "end_layer\n");
}

static int rec_text(void *ctx, const bdf_canvas_text *t, const bdf_canvas_line *line,
                    const bdf_canvas_style *style) {
  rec *r = ctx;
  rec_f(r, "text%s \"%.*s\" at %g,%g advance=%g size=%g", t->stroke ? " stroked" : "", (int)t->len,
        t->text, t->x, t->y, t->advance, t->size);
  if (t->font) {
    rec_f(r, " font=%s/%d/%d", t->font->family ? t->font->family : "", t->font->weight,
          t->font->style);
  }
  if (t->font_data) {
    /* an sfnt file, and an id to keep what is made of it by */
    CHECK(t->font_len > 12 && t->font_id != 0);
    rec_f(r, " embedded");
  }
  rec_f(r, " align=%d baseline=%d dir=%d spacing=%g with", t->align, t->baseline, t->dir,
        t->spacing);
  rec_style(r, style);
  if (t->stroke) {
    rec_line(r, line);
  }
  rec_f(r, "%s\n", r->text == 2 ? " (declined)" : "");
  return r->text == 1;
}

static bdf_canvas rec_canvas(rec *r, int text) {
  bdf_canvas c;
  memset(&c, 0, sizeof c);
  free(r->s);
  memset(r, 0, sizeof *r);
  r->text = text;
  c.ctx = r;
  c.save = rec_save;
  c.restore = rec_restore;
  c.transform = rec_transform;
  c.clip = rec_clip;
  c.fill = rec_fill;
  c.stroke = rec_stroke;
  c.image = rec_image;
  c.begin_layer = rec_begin_layer;
  c.end_layer = rec_end_layer;
  c.text = text ? rec_text : NULL;
  return c;
}

/* What was written down is what was expected, and the canvas is as it was. */
static int rec_is(const rec *r, const char *want) {
  const char *got = r->s ? r->s : "";
  if (r->depth == 0 && strcmp(got, want) == 0) {
    return 1;
  }
  fprintf(stderr, "---- the canvas was asked (%d left open):\n%s---- instead of:\n%s", r->depth, got,
          want);
  return 0;
}

/* ---- Objects ------------------------------------------------------------- */

static void ob_stroke_color(ob *o, uint32_t rgba) {
  ob_op(o, BDF_OP_STROKE_COLOR);
  w_u32(&o->ops, rgba);
}

static void ob_line(ob *o, float width, uint8_t cap, uint8_t join, float miter) {
  ob_op(o, BDF_OP_LINE);
  w_f32(&o->ops, width);
  w_u8(&o->ops, cap);
  w_u8(&o->ops, join);
  w_f32(&o->ops, miter);
}

static void ob_group_begin(ob *o, float alpha, uint8_t blend, float x, float y, float w, float h) {
  ob_op(o, BDF_OP_GROUP_BEGIN);
  w_f32(&o->ops, alpha);
  w_u8(&o->ops, blend);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, w);
  w_f32(&o->ops, h);
}

/* A path of a triangle, a quadratic and a cubic curve, and a point alone. */
static size_t ob_curves(ob *o) {
  static const float n[] = {0, 0, 4, 0, 4, 4, /* M L L Z */ 1,  1, 2, 0, 3, 1, /* M Q */
                            3, 2, 2, 3, 1, 2, /* C */       9,  9 /* M, and nothing from it */};
  static const uint8_t verbs[] = {BDF_VERB_MOVE, BDF_VERB_LINE, BDF_VERB_LINE,  BDF_VERB_CLOSE,
                                  BDF_VERB_MOVE, BDF_VERB_QUAD, BDF_VERB_CUBIC, BDF_VERB_MOVE};
  w_u8(&o->paths, 0);
  w_varuint(&o->paths, sizeof verbs);
  w_bytes(&o->paths, verbs, sizeof verbs);
  for (size_t i = 0; i < sizeof n / sizeof n[0]; i++) {
    w_f32(&o->paths, n[i]);
  }
  return o->npaths++;
}

static size_t ob_linear(ob *o) {
  w_u8(&o->paints, BDF_PAINT_LINEAR);
  w_f32(&o->paints, 0);
  w_f32(&o->paints, 0);
  w_f32(&o->paints, 8);
  w_f32(&o->paints, 0);
  w_varuint(&o->paints, 2);
  w_f32(&o->paints, 0);
  w_u32(&o->paints, 0xff0000ff);
  w_f32(&o->paints, 1);
  w_u32(&o->paints, 0x0000ff80);
  return o->npaints++;
}

/* 2x2: red, green; blue, white at half alpha (as in test_render.c) */
static const uint8_t png_2x2[76] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78, 0x9c, 0x51, 0x17, 0x92,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static void add_part(docb *d, uint32_t id, const char *type, const void *bytes, size_t n) {
  wr w = {0};
  w_bytes(&w, bytes, n);
  docb_add(d, id, type, &w);
}

/* A document of one fixed view whose pages are w by h and draw the objects
 * first, first + 1, ... */
static bdf_doc *pages_of(docb *d, uint32_t first, int n, int w, int h, wr *keep) {
  char views[2048], name[33];
  size_t at = (size_t)snprintf(views, sizeof views, "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":[");
  for (int i = 0; i < n; i++) {
    part_name(first + (uint32_t)i, name);
    at += (size_t)snprintf(views + at, sizeof views - at,
                           "%s{\"w\":%d,\"h\":%d,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}",
                           i ? "," : "", w, h, name);
  }
  snprintf(views + at, sizeof views - at, "]}]");
  return docb_open(d, views, keep);
}

/* ---- Tests --------------------------------------------------------------- */

/* Shapes: the state is the renderer's and every drawing carries its style;
 * the transform and the clip are the canvas's. */
static void test_shapes(void) {
  docb d = {0};
  wr keep;
  bdf_error err;
  rec r = {0};

  /* 20: a rectangle, a path, a stroke with its line, a clip and a transform
   * kept and restored */
  ob a = {0};
  size_t curves = ob_curves(&a);
  ob_fill_color(&a, 0xff0000ff);
  ob_rect(&a, BDF_OP_FILL_RECT, 1, 2, 3, 4);
  ob_op(&a, BDF_OP_SAVE);
  ob_rect(&a, BDF_OP_CLIP_RECT, 0, 0, 5, 5);
  ob_translate(&a, 2, 1);
  ob_scale(&a, 2, 2);
  ob_op(&a, BDF_OP_ALPHA);
  w_f32(&a.ops, 0.5f);
  ob_op(&a, BDF_OP_BLEND);
  w_u8(&a.ops, BDF_BLEND_MULTIPLY);
  ob_op(&a, BDF_OP_FILL_PATH);
  w_varuint(&a.ops, curves);
  w_u8(&a.ops, BDF_FILL_EVENODD);
  ob_op(&a, BDF_OP_RESTORE);
  /* the alpha and the blend mode went with the state; the stroke has its own colour */
  ob_stroke_color(&a, 0x00ff0080);
  ob_line(&a, 1.5f, BDF_CAP_ROUND, BDF_JOIN_BEVEL, 4);
  ob_op(&a, BDF_OP_DASH);
  w_varuint(&a.ops, 2);
  w_f32(&a.ops, 2);
  w_f32(&a.ops, 1);
  w_f32(&a.ops, 0.5f);
  ob_rect(&a, BDF_OP_STROKE_RECT, 0, 0, 6, 6);
  /* a RESTORE that has no SAVE does nothing */
  ob_op(&a, BDF_OP_RESTORE);
  /* and a SAVE that is not restored is restored at the end */
  ob_op(&a, BDF_OP_SAVE);
  docb_object(&d, 20, &a);

  /* 21: a gradient, a shadow, and an object drawn by another, at a place */
  ob b = {0};
  ob_fill_paint(&b, ob_linear(&b));
  ob_op(&b, BDF_OP_SHADOW);
  w_u32(&b.ops, 0x00000080);
  w_f32(&b.ops, 2);
  w_f32(&b.ops, 1);
  w_f32(&b.ops, -1);
  ob_rect(&b, BDF_OP_FILL_RECT, 0, 0, 8, 2);
  ob_use_at(&b, ob_child(&b, 30), 3, 4);
  /* the styles are as they were before the object was drawn */
  ob_rect(&b, BDF_OP_FILL_RECT, 0, 6, 1, 1);
  /* a group under the shadow casts it as a whole: what is in it has none */
  ob_group_begin(&b, 0.5f, BDF_BLEND_SOURCE_OVER, 2, 6, 2, 2);
  ob_rect(&b, BDF_OP_FILL_RECT, 2, 6, 2, 2);
  ob_op(&b, BDF_OP_GROUP_END);
  docb_object(&d, 21, &b);
  ob child = {0};
  ob_fill_color(&child, 0x0000ffff);
  ob_rect(&child, BDF_OP_FILL_RECT, 0, 0, 1, 1);
  docb_object(&d, 30, &child);

  bdf_doc *doc = pages_of(&d, 20, 2, 8, 8, &keep);
  bdf_renderer *rn = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &rn, &err) == BDF_OK);
  bdf_canvas c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L8,0 L8,8 L0,8 Z\n"
               "save\n"
               "fill M1,2 L4,2 L4,6 L1,6 Z with ff0000ff\n"
               "save\n"
               "clip M0,0 L5,0 L5,5 L0,5 Z\n"
               "transform 1 0 0 1 2 1\n"
               "transform 2 0 0 2 0 0\n"
               "fill evenodd M0,0 L4,0 L4,4 Z M1,1 Q2,0,3,1 C3,2,2,3,1,2 with ff0000ff alpha=0.5 blend=1\n"
               "restore\n"
               "stroke M0,0 L6,0 L6,6 L0,6 Z with 00ff0080 width=1.5 cap=1 join=2 miter=4 dash=2,1+0.5\n"
               "save\n"
               "restore\n"
               "restore\n"
               "restore\n"));
  c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 1, 1, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L8,0 L8,8 L0,8 Z\n"
               "save\n"
               "fill M0,0 L8,0 L8,2 L0,2 Z with gradient0(0,0,8,0 0:ff0000ff 1:0000ff80) shadow=00000080,2,1,-1\n"
               "save\n"
               "transform 1 0 0 1 3 4\n"
               "fill M0,0 L1,0 L1,1 L0,1 Z with 0000ffff shadow=00000080,2,1,-1\n"
               "restore\n"
               "fill M0,6 L1,6 L1,7 L0,7 Z with gradient0(0,0,8,0 0:ff0000ff 1:0000ff80) shadow=00000080,2,1,-1\n"
               "begin_layer 2,6,2,2 alpha=0.5 blend=0 shadow=00000080,2,1,-1\n"
               "clip M2,6 L4,6 L4,8 L2,8 Z\n"
               "fill M2,6 L4,6 L4,8 L2,8 Z with gradient0(0,0,8,0 0:ff0000ff 1:0000ff80)\n"
               "end_layer\n"
               "restore\n"
               "restore\n"));
  CHECK(bdf_renderer_warning_count(rn) == 0);

  /* an object part on its own: in its own coordinates, without the clip of a page */
  uint8_t hash[16] = {0, 0, 0, 30};
  c = rec_canvas(&r, 0);
  CHECK(bdf_play_part(rn, hash, 2, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "fill M0,0 L1,0 L1,1 L0,1 Z with 0000ffff\n"
               "restore\n"));

  /* what cannot be played */
  c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 2, 1, &c, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "no such page");
  CHECK(bdf_play_page(rn, 3, 0, 1, &c, &err) == BDF_ERR_ARG);
  hash[3] = 99;
  CHECK(bdf_play_part(rn, hash, 1, &c, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "no such object");
  CHECK(bdf_play_part(rn, NULL, 1, &c, &err) == BDF_ERR_ARG);
  CHECK(bdf_play_page(NULL, 0, 0, 1, &c, &err) == BDF_ERR_ARG);
  CHECK(bdf_play_page(rn, 0, 0, 1, NULL, &err) == BDF_ERR_ARG);
  CHECK(rec_is(&r, "" /* nothing was asked */));
  /* a canvas that lacks a function it must have */
  c.end_layer = NULL;
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "bad argument");
  CHECK(rec_is(&r, "" /* nothing was asked */));

  free(r.s);
  bdf_renderer_free(rn);
  bdf_doc_free(doc);
  free(keep.b);
}

/* Images come decoded, with an id to keep them by; groups are layers; a
 * soft mask is an image that what the group has drawn is multiplied by. */
static void test_images_and_layers(void) {
  docb d = {0};
  wr keep;
  bdf_error err;
  rec r = {0};
  add_part(&d, 1, "img", png_2x2, sizeof png_2x2);
  add_part(&d, 2, "img", "what is this", 12);

  /* 20: an image, a part of it, the image as a pattern, one that cannot be decoded */
  ob a = {0};
  size_t im = ob_image(&a, 1), bad = ob_image(&a, 2);
  ob_draw_image(&a, im, 1, 1, 4, 2);
  ob_smoothing(&a, 0);
  const float green[4] = {1, 0, 1, 1};
  ob_draw_image_sub(&a, im, green, 0, 4, 2, 2);
  ob_fill_paint(&a, ob_pattern(&a, im, BDF_REPEAT_X, 2, 1, 0));
  ob_rect(&a, BDF_OP_FILL_RECT, 0, 0, 8, 8);
  ob_draw_image(&a, bad, 0, 0, 1, 1);
  docb_object(&d, 20, &a);

  /* 21: a group at half its alpha, and one a soft mask shows the left half of */
  ob b = {0};
  ob_fill_color(&b, 0x00ff00ff);
  ob_group_begin(&b, 0.5f, BDF_BLEND_SCREEN, 1, 1, 4, 4);
  ob_rect(&b, BDF_OP_FILL_RECT, 0, 0, 8, 8);
  ob_op(&b, BDF_OP_GROUP_END);
  ob_group_begin(&b, 1, BDF_BLEND_SOURCE_OVER, 0, 0, 4, 2);
  ob_rect(&b, BDF_OP_FILL_RECT, 0, 0, 4, 2);
  ob_mask_begin(&b);
  ob_fill_color(&b, 0x000000ff);
  ob_rect(&b, BDF_OP_FILL_RECT, 0, 0, 2, 2);
  ob_op(&b, BDF_OP_MASK_END);
  ob_op(&b, BDF_OP_GROUP_END);
  /* a group that is not ended is ended with its object */
  ob_group_begin(&b, 1, BDF_BLEND_SOURCE_OVER, 0, 0, 1, 1);
  ob_op(&b, BDF_OP_SAVE);
  docb_object(&d, 21, &b);

  bdf_doc *doc = pages_of(&d, 20, 2, 8, 8, &keep);
  bdf_renderer *rn = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &rn, &err) == BDF_OK);
  bdf_canvas c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L8,0 L8,8 L0,8 Z\n"
               "save\n"
               "save\n"
               "transform 2 0 0 1 1 1\n"
               "image bitmap#1 2x2 ff0000ff 0,0,2,2 to 0,0,2,2 smooth\n"
               "restore\n"
               "save\n"
               "transform 2 0 0 2 -2 4\n"
               "image bitmap#1 2x2 ff0000ff 1,0,1,1 to 1,0,1,1\n"
               "restore\n"
               "fill M0,0 L8,0 L8,8 L0,8 Z with pattern(bitmap#1 2x2 ff0000ff repeat=1 [2 0 0 2 1 0])\n"
               "restore\n"
               "restore\n"));
  CHECK(bdf_renderer_warning_count(rn) == 1);
  CHECK(strstr(bdf_renderer_warning(rn, 0), "cannot be decoded") != NULL);
  /* drawn again, the image has the id it had */
  uint64_t id = r.ids[0];
  c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  CHECK(id != 0 && r.nids == 1 && r.ids[0] == id);

  /* the mask has the pixels of the scale it is played at */
  c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 1, 2, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L8,0 L8,8 L0,8 Z\n"
               "save\n"
               "begin_layer 1,1,4,4 alpha=0.5 blend=2\n"
               "clip M1,1 L5,1 L5,5 L1,5 Z\n"
               "fill M0,0 L8,0 L8,8 L0,8 Z with 00ff00ff\n"
               "end_layer\n"
               "begin_layer 0,0,4,2 alpha=1 blend=0\n"
               "clip M0,0 L4,0 L4,2 L0,2 Z\n"
               "fill M0,0 L4,0 L4,2 L0,2 Z with 00ff00ff\n"
               "save\n"
               "transform 1 0 0 1 0 0\n"
               "image bitmap 8x4 000000ff..00000000 0,0,8,4 to 0,0,4,2 blend=17\n"
               "restore\n"
               "end_layer\n"
               "begin_layer 0,0,1,1 alpha=1 blend=0\n"
               "clip M0,0 L1,0 L1,1 L0,1 Z\n"
               "save\n"
               "restore\n"
               "end_layer\n"
               "restore\n"
               "restore\n"));

  free(r.s);
  bdf_renderer_free(rn);
  bdf_doc_free(doc);
  free(keep.b);
}

/* Text is the outlines of its glyphs, or the canvas's own to draw. */
static void test_text(void) {
  docb d = {0};
  wr keep;
  bdf_error err;
  rec r = {0};
  add_part(&d, 1, "font", rv_font_boxes, sizeof rv_font_boxes);
  ob a = {0};
  ob_set_font(&a, ob_font_part(&a, 1, 400, BDF_STYLE_NORMAL), 10);
  ob_fill_color(&a, 0x112233ff);
  ob_text(&a, "AB", 1, 12, 0);
  ob_text_style(&a, BDF_ALIGN_RIGHT, BDF_BASELINE_TOP, BDF_DIR_INHERIT, 1);
  ob_stroke_color(&a, 0x445566ff);
  ob_line(&a, 0.5f, BDF_CAP_BUTT, BDF_JOIN_MITER, 10);
  ob_stroke_text(&a, "A", 20, 14, 8);
  /* a family by name, which the renderer has no font for */
  ob_set_font(&a, ob_font(&a, "Nowhere, serif"), 6);
  ob_text(&a, "B", 0, 30, 0);
  docb_object(&d, 20, &a);
  bdf_doc *doc = pages_of(&d, 20, 1, 32, 32, &keep);
  bdf_renderer *rn = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &rn, &err) == BDF_OK);

  /* a canvas without text: the outlines, and nothing for the font that is not there */
  bdf_canvas c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L32,0 L32,32 L0,32 Z\n"
               "save\n"
               "fill M2,12 L2,5 L10,5 L10,12 Z M11,14 L11,4 L16,4 L16,14 Z with 112233ff\n"
               "stroke M12.7273,22 L12.7273,15 L18.5455,15 L18.5455,22 Z with 445566ff width=0.5\n"
               "restore\n"
               "restore\n"));
  CHECK(bdf_renderer_warning_count(rn) == 1);
  CHECK(strstr(bdf_renderer_warning(rn, 0), "no font for") != NULL);
  bdf_renderer_free(rn);

  /* a canvas that draws text: the strings, with the font the document carries */
  CHECK(bdf_renderer_new(doc, NULL, &rn, &err) == BDF_OK);
  c = rec_canvas(&r, 1);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L32,0 L32,32 L0,32 Z\n"
               "save\n"
               "text \"AB\" at 1,12 advance=0 size=10 font=/400/0 embedded align=0 baseline=0 dir=0 spacing=0 with 112233ff\n"
               "text stroked \"A\" at 20,14 advance=8 size=10 font=/400/0 embedded align=1 baseline=1 dir=0 spacing=1 with 445566ff width=0.5\n"
               "text \"B\" at 0,30 advance=0 size=6 font=Nowhere, serif/400/0 align=1 baseline=1 dir=0 spacing=1 with 112233ff\n"
               "restore\n"
               "restore\n"));
  /* it has fonts of its own: nothing is missing */
  CHECK(bdf_renderer_warning_count(rn) == 0);

  /* a canvas that declines a string gets its outlines */
  c = rec_canvas(&r, 2);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L32,0 L32,32 L0,32 Z\n"
               "save\n"
               "text \"AB\" at 1,12 advance=0 size=10 font=/400/0 embedded align=0 baseline=0 dir=0 spacing=0 with 112233ff (declined)\n"
               "fill M2,12 L2,5 L10,5 L10,12 Z M11,14 L11,4 L16,4 L16,14 Z with 112233ff\n"
               "text stroked \"A\" at 20,14 advance=8 size=10 font=/400/0 embedded align=1 baseline=1 dir=0 spacing=1 with 445566ff width=0.5 (declined)\n"
               "stroke M12.7273,22 L12.7273,15 L18.5455,15 L18.5455,22 Z with 445566ff width=0.5\n"
               "text \"B\" at 0,30 advance=0 size=6 font=Nowhere, serif/400/0 align=1 baseline=1 dir=0 spacing=1 with 112233ff (declined)\n"
               "restore\n"
               "restore\n"));

  free(r.s);
  bdf_renderer_free(rn);
  bdf_doc_free(doc);
  free(keep.b);
}

/* An SVG image comes as a raster of it, as large as it is shown: one for
 * each scale, kept by its id. */
static void test_svg_images(void) {
  static const char stripes[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"2\">"
                                "<rect width=\"1\" height=\"2\" fill=\"#f00\"/></svg>";
  docb d = {0};
  wr keep;
  bdf_error err;
  rec r = {0};
  add_part(&d, 1, "img", stripes, sizeof stripes - 1);
  ob a = {0};
  size_t im = ob_image(&a, 1);
  ob_draw_image(&a, im, 0, 0, 8, 8);
  ob_fill_paint(&a, ob_pattern(&a, im, BDF_REPEAT, 1, 0, 0));
  ob_rect(&a, BDF_OP_FILL_RECT, 0, 8, 8, 4);
  docb_object(&d, 20, &a);
  bdf_doc *doc = pages_of(&d, 20, 1, 8, 12, &keep);
  bdf_renderer *rn = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &rn, &err) == BDF_OK);
  bdf_canvas c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 0, 1, &c, &err) == BDF_OK);
  /* drawn four times its size: a raster of 8 by 8, of which a unit of the
   * image is four pixels; as a pattern at its own size, one of 2 by 2 */
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L8,0 L8,12 L0,12 Z\n"
               "save\n"
               "save\n"
               "transform 1 0 0 1 0 0\n"
               "image bitmap#1 8x8 ff0000ff 0,0,8,8 to 0,0,8,8 smooth\n"
               "restore\n"
               "fill M0,8 L8,8 L8,12 L0,12 Z with pattern(bitmap#2 2x2 ff0000ff repeat=0 [1 0 0 1 0 0])\n"
               "restore\n"
               "restore\n"));
  /* at twice the scale the rasters are twice as large, and others */
  c = rec_canvas(&r, 0);
  CHECK(bdf_play_page(rn, 0, 0, 2, &c, &err) == BDF_OK);
  CHECK(rec_is(&r,
               "save\n"
               "clip M0,0 L8,0 L8,12 L0,12 Z\n"
               "save\n"
               "save\n"
               "transform 0.5 0 0 0.5 0 0\n"
               "image bitmap#1 16x16 ff0000ff 0,0,16,16 to 0,0,16,16 smooth\n"
               "restore\n"
               "fill M0,8 L8,8 L8,12 L0,12 Z with pattern(bitmap#2 4x4 ff0000ff repeat=0 [0.5 0 0 0.5 0 0])\n"
               "restore\n"
               "restore\n"));
  CHECK(bdf_renderer_warning_count(rn) == 0);
  free(r.s);
  bdf_renderer_free(rn);
  bdf_doc_free(doc);
  free(keep.b);
}

int main(void) {
  test_shapes();
  test_images_and_layers();
  test_text();
  test_svg_images();
  return finish();
}
