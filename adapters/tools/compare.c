/* The comparison of an adapter with the rasterizer (compare.h), and the
 * pages it compares when it is given no document. */
#include "compare.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testutil.h"

#include "render_vectors.h" /* a font of boxes */

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 1 << 16, n = 0;
  uint8_t *buf = malloc(cap);
  while (buf) {
    n += fread(buf + n, 1, cap - n, f);
    if (n < cap) {
      break;
    }
    uint8_t *grown = realloc(buf, cap * 2);
    if (!grown) {
      free(buf);
      buf = NULL;
      break;
    }
    buf = grown;
    cap *= 2;
  }
  fclose(f);
  *len = n;
  return buf;
}

/* Writes premultiplied RGBA on white as a PPM file, for looking at. */
static void write_ppm(const char *path, const uint8_t *pix, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
    fwrite(pix + 4 * i, 1, 3, f);
  }
  fclose(f);
}

typedef struct comparison {
  const char *who;
  bdf_compare_draw draw;
  void *ctx;
  double mean_limit, far_limit;
  int pages, bad;
} comparison;

/* The pages of a document (at most max of each view), both ways. */
static void compare_doc(comparison *c, bdf_doc *doc, const char *name, const bdf_fonts *fonts,
                        size_t max) {
  const char *dump = getenv("BDF_COMPARE_DUMP");
  bdf_error err;
  bdf_render_options opts = {0, 0, fonts};
  bdf_renderer *soft = NULL, *through = NULL;
  if (bdf_renderer_new(doc, &opts, &soft, &err) != BDF_OK ||
      bdf_renderer_new(doc, &opts, &through, &err) != BDF_OK) {
    bdf_renderer_free(soft);
    c->bad++;
    return;
  }
  for (size_t v = 0; v < bdf_doc_view_count(doc); v++) {
    for (size_t p = 0; p < max; p++) {
      bdf_page page;
      bdf_image img;
      const double scale = 1.5;
      if (bdf_doc_page(doc, v, p, &page) != BDF_OK ||
          bdf_render_page(soft, v, p, scale, &img, &err) != BDF_OK) {
        break;
      }
      uint8_t *other = malloc(4 * (size_t)img.width * (size_t)img.height);
      if (other && c->draw(c->ctx, through, v, p, (double)img.width / (double)page.w,
                           (double)img.height / (double)page.h, img.width, img.height, other)) {
        size_t n = (size_t)img.width * (size_t)img.height, far = 0;
        double sum = 0;
        for (size_t i = 0; i < n; i++) {
          int worst = 0;
          for (int k = 0; k < 4; k++) {
            int d = (int)img.pixels[4 * i + k] - (int)other[4 * i + k];
            d = d < 0 ? -d : d;
            sum += d;
            worst = d > worst ? d : worst;
          }
          far += worst > 64;
        }
        double mean = sum / (4.0 * (double)n), share = (double)far / (double)n;
        int apart = mean > c->mean_limit || share > c->far_limit;
        printf("%s %s view %zu page %zu (%dx%d): mean %.3f, %.2f%% far%s\n", c->who, name, v, p,
               img.width, img.height, mean, 100 * share, apart ? "  <-- apart" : "");
        c->bad += apart;
        c->pages++;
        if (dump) {
          char path[1024];
          const char *base = strrchr(name, '/');
          base = base ? base + 1 : name;
          base = strcmp(base, "built in") == 0 ? "builtin" : base;
          snprintf(path, sizeof path, "%s/%s-%zu-%zu-libbdf.ppm", dump, base, v, p);
          write_ppm(path, img.pixels, img.width, img.height);
          snprintf(path, sizeof path, "%s/%s-%zu-%zu-%s.ppm", dump, base, v, p, c->who);
          write_ppm(path, other, img.width, img.height);
        }
      } else {
        printf("%s %s view %zu page %zu: not drawn  <-- apart\n", c->who, name, v, p);
        c->bad++;
      }
      free(other);
      bdf_image_free(&img);
    }
  }
  /* what the two could not draw is the same */
  if (bdf_renderer_warning_count(soft) != bdf_renderer_warning_count(through)) {
    printf("%s %s: %zu warnings, the rasterizer has %zu  <-- apart\n", c->who, name,
           bdf_renderer_warning_count(through), bdf_renderer_warning_count(soft));
    c->bad++;
  }
  bdf_renderer_free(soft);
  bdf_renderer_free(through);
}

/* ---- The pages built in -------------------------------------------------- */

#define PAGE 64

static void op_color(ob *o, uint8_t op, uint32_t rgba) {
  ob_op(o, op); /* FILL_COLOR, STROKE_COLOR */
  w_u32(&o->ops, rgba);
}

static void op_paint(ob *o, uint8_t op, size_t paint) {
  ob_op(o, op); /* FILL_PAINT, STROKE_PAINT */
  w_varuint(&o->ops, paint);
}

static void op_line(ob *o, float width, uint8_t cap, uint8_t join, float miter) {
  ob_op(o, BDF_OP_LINE);
  w_f32(&o->ops, width);
  w_u8(&o->ops, cap);
  w_u8(&o->ops, join);
  w_f32(&o->ops, miter);
}

static void op_dash(ob *o, const float *dash, size_t n, float offset) {
  ob_op(o, BDF_OP_DASH);
  w_varuint(&o->ops, n);
  for (size_t i = 0; i < n; i++) {
    w_f32(&o->ops, dash[i]);
  }
  w_f32(&o->ops, offset);
}

static void op_f32(ob *o, uint8_t op, float v) {
  ob_op(o, op); /* ALPHA */
  w_f32(&o->ops, v);
}

static void op_u8(ob *o, uint8_t op, uint8_t v) {
  ob_op(o, op); /* BLEND */
  w_u8(&o->ops, v);
}

static void op_group(ob *o, float alpha, uint8_t blend, float x, float y, float w, float h) {
  ob_op(o, BDF_OP_GROUP_BEGIN);
  w_f32(&o->ops, alpha);
  w_u8(&o->ops, blend);
  w_f32(&o->ops, x);
  w_f32(&o->ops, y);
  w_f32(&o->ops, w);
  w_f32(&o->ops, h);
}

static void op_mask(ob *o, uint8_t kind, uint32_t backdrop) {
  ob_op(o, BDF_OP_MASK_BEGIN);
  w_u8(&o->ops, kind);
  w_u32(&o->ops, backdrop);
  w_varuint(&o->ops, 0);
}

static void op_path(ob *o, uint8_t op, size_t path, uint8_t rule) {
  ob_op(o, op); /* FILL_PATH and CLIP_PATH (with a rule), STROKE_PATH */
  w_varuint(&o->ops, path);
  if (op != BDF_OP_STROKE_PATH) {
    w_u8(&o->ops, rule);
  }
}

static void op_transform(ob *o, double a, double b, double c, double d, double e, double f) {
  ob_op(o, BDF_OP_TRANSFORM);
  w_f32(&o->ops, (float)a);
  w_f32(&o->ops, (float)b);
  w_f32(&o->ops, (float)c);
  w_f32(&o->ops, (float)d);
  w_f32(&o->ops, (float)e);
  w_f32(&o->ops, (float)f);
}

static void op_shadow(ob *o, uint32_t rgba, float blur, float dx, float dy) {
  ob_op(o, BDF_OP_SHADOW);
  w_u32(&o->ops, rgba);
  w_f32(&o->ops, blur);
  w_f32(&o->ops, dx);
  w_f32(&o->ops, dy);
}

static size_t add_path(ob *o, const uint8_t *verbs, size_t nverbs, const float *args, size_t nargs) {
  w_u8(&o->paths, 0);
  w_varuint(&o->paths, nverbs);
  w_bytes(&o->paths, verbs, nverbs);
  for (size_t i = 0; i < nargs; i++) {
    w_f32(&o->paths, args[i]);
  }
  return o->npaths++;
}

#define PATH(o, verbs, args) add_path(o, verbs, sizeof verbs, args, sizeof args / sizeof args[0])

static size_t add_gradient(ob *o, uint8_t kind, const float *coords, const float *offsets,
                           const uint32_t *colors, size_t nstops) {
  static const size_t ncoords[] = {4, 6, 3};
  w_u8(&o->paints, kind);
  for (size_t i = 0; i < ncoords[kind]; i++) {
    w_f32(&o->paints, coords[i]);
  }
  w_varuint(&o->paints, nstops);
  for (size_t i = 0; i < nstops; i++) {
    w_f32(&o->paints, offsets[i]);
    w_u32(&o->paints, colors[i]);
  }
  return o->npaints++;
}

static size_t add_pattern(ob *o, size_t image, uint8_t repeat, float s, float dx, float dy) {
  return ob_pattern(o, image, repeat, s, dx, dy);
}

static const float thirds[3] = {0, 0.5f, 1};
static const uint32_t rgb[3] = {0xff0000ff, 0x00c000ff, 0x0000ffff};

/* Filled shapes: rectangles, colours that are not opaque, a path of lines
 * and curves filled by both rules, and the shapes a path has names for. */
static void page_shapes(ob *o) {
  static const uint8_t v1[] = {BDF_VERB_MOVE, BDF_VERB_LINE, BDF_VERB_LINE,  BDF_VERB_CLOSE,
                               BDF_VERB_MOVE, BDF_VERB_QUAD, BDF_VERB_CUBIC, BDF_VERB_CLOSE};
  static const float a1[] = {34, 34, 60, 34, 47, 60, 40, 38, 47, 30, 54, 38, 54, 46, 50, 50, 47, 54};
  static const uint8_t v2[] = {BDF_VERB_ELLIPSE, BDF_VERB_ROUND_RECT, BDF_VERB_RECT};
  static const float a2[] = {16, 48, 11, 6, 0.5f, 0, 6.2831855f, 0, /* an ellipse, turned */
                             4,  26, 22, 12, 4,   /* a rectangle with round corners */
                             8,  29, 14, 6 /* and a hole in it, by the even-odd rule */};
  op_color(o, BDF_OP_FILL_COLOR, 0xcc3333ff);
  ob_rect(o, BDF_OP_FILL_RECT, 4, 4, 24, 16);
  op_color(o, BDF_OP_FILL_COLOR, 0x3366ccb0);
  ob_rect(o, BDF_OP_FILL_RECT, 16.5f, 10.25f, 24, 12);
  op_color(o, BDF_OP_FILL_COLOR, 0x228833ff);
  op_path(o, BDF_OP_FILL_PATH, PATH(o, v1, a1), BDF_FILL_NONZERO);
  op_color(o, BDF_OP_FILL_COLOR, 0x8833aaff);
  op_path(o, BDF_OP_FILL_PATH, PATH(o, v2, a2), BDF_FILL_EVENODD);
  op_color(o, BDF_OP_FILL_COLOR, 0x000000ff);
  ob_rect(o, BDF_OP_FILL_RECT, 44, 4, 16, 16);
  ob_rect(o, BDF_OP_CLEAR_RECT, 48, 8, 8, 8);
}

/* Lines: widths, caps, joins, dashes, a line that is not opaque crossing itself. */
static void page_lines(ob *o) {
  static const uint8_t zig[] = {BDF_VERB_MOVE, BDF_VERB_LINE, BDF_VERB_LINE, BDF_VERB_LINE};
  static const float a1[] = {6, 6, 18, 18, 30, 6, 42, 18};
  static const float a2[] = {6, 22, 18, 34, 30, 22, 42, 34};
  static const float a3[] = {48, 6, 54, 34, 58, 6, 58, 30};
  static const uint8_t curve[] = {BDF_VERB_MOVE, BDF_VERB_CUBIC};
  static const float a4[] = {6, 52, 20, 30, 40, 74, 58, 46};
  static const uint8_t cross[] = {BDF_VERB_MOVE, BDF_VERB_LINE, BDF_VERB_LINE, BDF_VERB_LINE,
                                  BDF_VERB_CLOSE};
  static const float a5[] = {8, 40, 30, 60, 30, 40, 8, 60};
  static const float dash[] = {6, 3};
  op_color(o, BDF_OP_STROKE_COLOR, 0x000000ff);
  op_line(o, 4, BDF_CAP_ROUND, BDF_JOIN_ROUND, 10);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, zig, a1), 0);
  op_color(o, BDF_OP_STROKE_COLOR, 0x1155ccff);
  op_line(o, 3, BDF_CAP_SQUARE, BDF_JOIN_BEVEL, 10);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, zig, a2), 0);
  op_color(o, BDF_OP_STROKE_COLOR, 0xcc5511ff);
  op_line(o, 2.5f, BDF_CAP_BUTT, BDF_JOIN_MITER, 10);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, zig, a3), 0);
  op_color(o, BDF_OP_STROKE_COLOR, 0x117733ff);
  op_line(o, 2, BDF_CAP_BUTT, BDF_JOIN_MITER, 10);
  op_dash(o, dash, 2, 2);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, curve, a4), 0);
  op_dash(o, dash, 0, 0);
  op_color(o, BDF_OP_STROKE_COLOR, 0xaa00aa80);
  op_line(o, 5, BDF_CAP_BUTT, BDF_JOIN_ROUND, 10);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, cross, a5), 0);
  op_color(o, BDF_OP_STROKE_COLOR, 0x00000080);
  op_line(o, 1, BDF_CAP_BUTT, BDF_JOIN_MITER, 10);
  ob_rect(o, BDF_OP_STROKE_RECT, 38.5f, 40.5f, 20, 18);
}

/* Gradients: along a line, between two circles, around a point; with stops
 * that are not opaque; on a line. */
static void page_gradients(ob *o) {
  static const float along[] = {4, 0, 60, 0};
  static const float circles[] = {17, 35, 2, 17, 35, 13};
  static const float around[] = {0, 47, 35};
  static const float down[] = {0, 52, 0, 62};
  static const float ends[2] = {0, 1};
  static const uint32_t fade[2] = {0x000000ff, 0x00000000};
  static const uint8_t wave[] = {BDF_VERB_MOVE, BDF_VERB_QUAD, BDF_VERB_QUAD};
  static const float a1[] = {6, 57, 20, 48, 32, 57, 44, 66, 58, 57};
  op_paint(o, BDF_OP_FILL_PAINT, add_gradient(o, BDF_PAINT_LINEAR, along, thirds, rgb, 3));
  ob_rect(o, BDF_OP_FILL_RECT, 4, 4, 56, 14);
  op_paint(o, BDF_OP_FILL_PAINT, add_gradient(o, BDF_PAINT_RADIAL, circles, thirds, rgb, 3));
  ob_rect(o, BDF_OP_FILL_RECT, 4, 22, 26, 26);
  op_paint(o, BDF_OP_FILL_PAINT, add_gradient(o, BDF_PAINT_CONIC, around, thirds, rgb, 3));
  ob_rect(o, BDF_OP_FILL_RECT, 34, 22, 26, 26);
  op_paint(o, BDF_OP_FILL_PAINT, add_gradient(o, BDF_PAINT_LINEAR, down, ends, fade, 2));
  ob_rect(o, BDF_OP_FILL_RECT, 4, 52, 56, 10);
  op_paint(o, BDF_OP_STROKE_PAINT, add_gradient(o, BDF_PAINT_LINEAR, along, thirds, rgb, 3));
  op_line(o, 3, BDF_CAP_ROUND, BDF_JOIN_ROUND, 10);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, wave, a1), 0);
}

/* 2x2: red, green; blue, white at half alpha */
static const uint8_t png_2x2[76] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78, 0x9c, 0x51, 0x17, 0x92,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

/* Images: pixel for pixel, a part of one, turned, at an alpha, as patterns. */
static void page_images(ob *o) {
  size_t im = ob_image(o, 1);
  const float green[4] = {1, 0, 1, 1};
  ob_smoothing(o, 0);
  ob_draw_image(o, im, 4, 4, 24, 24);
  ob_draw_image_sub(o, im, green, 34, 4, 10, 10);
  ob_op(o, BDF_OP_SAVE);
  op_f32(o, BDF_OP_ALPHA, 0.5f);
  op_transform(o, 0.8660254, 0.5, -0.5, 0.8660254, 52, 6);
  ob_draw_image(o, im, 0, 0, 10, 10);
  ob_op(o, BDF_OP_RESTORE);
  op_paint(o, BDF_OP_FILL_PAINT, add_pattern(o, im, BDF_REPEAT, 4, 0, 2));
  ob_rect(o, BDF_OP_FILL_RECT, 4, 34, 24, 24);
  op_paint(o, BDF_OP_FILL_PAINT, add_pattern(o, im, BDF_REPEAT_X, 5, 34, 36));
  ob_rect(o, BDF_OP_FILL_RECT, 32, 32, 28, 14);
  op_paint(o, BDF_OP_FILL_PAINT, add_pattern(o, im, BDF_NO_REPEAT, 6, 40, 48));
  ob_rect(o, BDF_OP_FILL_RECT, 32, 46, 28, 16);
}

/* Groups, composite operations, a clip, a transform. */
static void page_groups(ob *o) {
  static const uint8_t oval[] = {BDF_VERB_ELLIPSE};
  static const float a1[] = {46, 46, 14, 9, 0, 0, 6.2831855f, 0};
  op_color(o, BDF_OP_FILL_COLOR, 0xffcc00ff);
  ob_rect(o, BDF_OP_FILL_RECT, 2, 2, 36, 36);
  /* two squares that overlap, at half the alpha as one */
  op_group(o, 0.5f, BDF_BLEND_SOURCE_OVER, 4, 4, 30, 30);
  op_color(o, BDF_OP_FILL_COLOR, 0xff0000ff);
  ob_rect(o, BDF_OP_FILL_RECT, 6, 6, 18, 18);
  op_color(o, BDF_OP_FILL_COLOR, 0x0000ffff);
  ob_rect(o, BDF_OP_FILL_RECT, 14, 14, 18, 18);
  ob_op(o, BDF_OP_GROUP_END);
  /* a shape that multiplies, and a group that screens */
  op_color(o, BDF_OP_FILL_COLOR, 0x33aa77ff);
  ob_rect(o, BDF_OP_FILL_RECT, 40, 4, 20, 26);
  op_u8(o, BDF_OP_BLEND, BDF_BLEND_MULTIPLY);
  op_color(o, BDF_OP_FILL_COLOR, 0xcc66ccff);
  ob_rect(o, BDF_OP_FILL_RECT, 34, 8, 16, 10);
  op_u8(o, BDF_OP_BLEND, BDF_BLEND_SOURCE_OVER);
  op_group(o, 1, BDF_BLEND_SCREEN, 44, 16, 18, 16);
  op_color(o, BDF_OP_FILL_COLOR, 0x804020ff);
  ob_rect(o, BDF_OP_FILL_RECT, 46, 18, 14, 10);
  ob_op(o, BDF_OP_GROUP_END);
  /* stripes turned, through an oval */
  ob_op(o, BDF_OP_SAVE);
  op_path(o, BDF_OP_CLIP_PATH, PATH(o, oval, a1), BDF_FILL_NONZERO);
  op_transform(o, 0.8660254, 0.5, -0.5, 0.8660254, 46, 46);
  for (int i = -4; i < 4; i++) {
    op_color(o, BDF_OP_FILL_COLOR, i & 1 ? 0x222222ff : 0xdd4444ff);
    ob_rect(o, BDF_OP_FILL_RECT, (float)(4 * i), -20, 4, 40);
  }
  ob_op(o, BDF_OP_RESTORE);
  /* a clip to a rectangle, left by its RESTORE */
  ob_op(o, BDF_OP_SAVE);
  ob_rect(o, BDF_OP_CLIP_RECT, 4, 42, 20, 10);
  op_color(o, BDF_OP_FILL_COLOR, 0x0077ccff);
  ob_rect(o, BDF_OP_FILL_RECT, 0, 40, 30, 30);
  ob_op(o, BDF_OP_RESTORE);
  ob_rect(o, BDF_OP_FILL_RECT, 4, 56, 20, 4);
}

/* Soft masks: by the alpha of what is drawn, and by its luminosity. */
static void page_masks(ob *o) {
  static const float along[] = {4, 0, 60, 0};
  static const float ends[2] = {0, 1};
  static const uint32_t white_black[2] = {0xffffffff, 0x000000ff};
  static const uint8_t oval[] = {BDF_VERB_ELLIPSE};
  static const float a1[] = {32, 17, 24, 10, 0, 0, 6.2831855f, 0};
  op_group(o, 1, BDF_BLEND_SOURCE_OVER, 4, 4, 56, 26);
  op_paint(o, BDF_OP_FILL_PAINT, add_gradient(o, BDF_PAINT_LINEAR, along, thirds, rgb, 3));
  ob_rect(o, BDF_OP_FILL_RECT, 4, 4, 56, 26);
  op_mask(o, BDF_MASK_ALPHA, 0);
  op_color(o, BDF_OP_FILL_COLOR, 0x000000ff);
  op_path(o, BDF_OP_FILL_PATH, PATH(o, oval, a1), BDF_FILL_NONZERO);
  op_color(o, BDF_OP_FILL_COLOR, 0x00000060);
  ob_rect(o, BDF_OP_FILL_RECT, 4, 4, 20, 26);
  ob_op(o, BDF_OP_MASK_END);
  ob_op(o, BDF_OP_GROUP_END);
  op_group(o, 0.8f, BDF_BLEND_SOURCE_OVER, 4, 34, 56, 26);
  op_color(o, BDF_OP_FILL_COLOR, 0x224488ff);
  ob_rect(o, BDF_OP_FILL_RECT, 4, 34, 56, 26);
  op_mask(o, BDF_MASK_LUMINOSITY, 0x000000ff);
  op_paint(o, BDF_OP_FILL_PAINT, add_gradient(o, BDF_PAINT_LINEAR, along, ends, white_black, 2));
  ob_rect(o, BDF_OP_FILL_RECT, 8, 38, 48, 18);
  ob_op(o, BDF_OP_MASK_END);
  ob_op(o, BDF_OP_GROUP_END);
}

/* Text in a font the document carries: filled, outlined, made bold and
 * slanted, fitted to a width, spaced. */
static void page_text(ob *o) {
  size_t plain = ob_font_part(o, 2, 400, BDF_STYLE_NORMAL);
  size_t bold = ob_font_part(o, 2, 700, BDF_STYLE_NORMAL);
  size_t slanted = ob_font_part(o, 2, 400, BDF_STYLE_ITALIC);
  op_color(o, BDF_OP_FILL_COLOR, 0x222222ff);
  ob_set_font(o, plain, 16);
  ob_text(o, "AB", 4, 22, 0);
  ob_set_font(o, bold, 16);
  ob_text(o, "A", 36, 22, 0);
  ob_set_font(o, slanted, 14);
  ob_text(o, "B", 52, 22, 0);
  ob_set_font(o, plain, 14);
  op_color(o, BDF_OP_STROKE_COLOR, 0xbb2222ff);
  op_line(o, 1.5f, BDF_CAP_BUTT, BDF_JOIN_MITER, 10);
  ob_stroke_text(o, "AB", 4, 44, 0);
  ob_text_style(o, BDF_ALIGN_RIGHT, BDF_BASELINE_TOP, BDF_DIR_INHERIT, 2);
  op_color(o, BDF_OP_FILL_COLOR, 0x2244aaff);
  ob_text(o, "BA", 60, 30, 0);
  ob_text_style(o, BDF_ALIGN_CENTER, BDF_BASELINE_ALPHABETIC, BDF_DIR_INHERIT, 0);
  ob_text(o, "AAB", 32, 60, 50);
}

/* An SVG image: a canvas is given a raster of it, as large as it is shown. */
static const char svg_image[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\" viewBox=\"0 0 16 16\">"
    "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#f60\"/>"
    "<stop offset=\"1\" stop-color=\"#06f\"/></linearGradient></defs>"
    "<rect width=\"16\" height=\"16\" rx=\"3\" fill=\"url(#g)\"/>"
    "<circle cx=\"8\" cy=\"8\" r=\"4\" fill=\"#fff\" opacity=\".7\"/>"
    "<path d=\"M3 13L8 3l5 10z\" fill=\"none\" stroke=\"#000\"/></svg>";

static void page_svg(ob *o) {
  size_t im = ob_image(o, 3);
  const float half[4] = {0, 0, 16, 32};
  ob_draw_image(o, im, 4, 4, 32, 32);
  ob_draw_image_sub(o, im, half, 40, 4, 20, 40);
  op_paint(o, BDF_OP_FILL_PAINT, add_pattern(o, im, BDF_REPEAT, 0.5f, 4, 44));
  ob_rect(o, BDF_OP_FILL_RECT, 4, 44, 32, 16);
}

/* Shadows that are not blurred: every library blurs in its own way. */
static void page_shadows(ob *o) {
  static const uint8_t zig[] = {BDF_VERB_MOVE, BDF_VERB_LINE, BDF_VERB_LINE};
  static const float a1[] = {8, 56, 30, 40, 54, 56};
  op_shadow(o, 0x00000080, 0, 4, 3);
  op_color(o, BDF_OP_FILL_COLOR, 0xdd8800ff);
  ob_rect(o, BDF_OP_FILL_RECT, 6, 6, 22, 18);
  op_shadow(o, 0x2222cc80, 0, -3, 4);
  op_color(o, BDF_OP_FILL_COLOR, 0x44aa4480);
  ob_rect(o, BDF_OP_FILL_RECT, 36, 6, 20, 18);
  op_shadow(o, 0x000000ff, 0, 2, 2);
  op_color(o, BDF_OP_STROKE_COLOR, 0xcc2255ff);
  op_line(o, 4, BDF_CAP_ROUND, BDF_JOIN_ROUND, 10);
  op_path(o, BDF_OP_STROKE_PATH, PATH(o, zig, a1), 0);
}

static bdf_doc *builtin_pages(unsigned flags, wr *keep) {
  void (*const pages[])(ob *) = {page_shapes, page_lines, page_gradients, page_images, page_groups,
                                 page_masks,  page_text,  page_svg,       page_shadows};
  size_t n = sizeof pages / sizeof pages[0];
  if (flags & BDF_COMPARE_NO_SHADOWS) {
    n--;
  }
  docb d = {0};
  wr image = {0}, font = {0};
  w_bytes(&image, png_2x2, sizeof png_2x2);
  docb_add(&d, 1, "img", &image);
  w_bytes(&font, rv_font_boxes, sizeof rv_font_boxes);
  docb_add(&d, 2, "font", &font);
  wr svg = {0};
  w_bytes(&svg, svg_image, sizeof svg_image - 1);
  docb_add(&d, 3, "img", &svg);
  char views[4096], name[33];
  size_t at = (size_t)snprintf(views, sizeof views, "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":[");
  for (size_t i = 0; i < n; i++) {
    ob o = {0};
    o.bbox[2] = PAGE;
    o.bbox[3] = PAGE;
    pages[i](&o);
    docb_object(&d, 16 + (uint32_t)i, &o);
    part_name(16 + (uint32_t)i, name);
    at += (size_t)snprintf(views + at, sizeof views - at,
                           "%s{\"w\":%d,\"h\":%d,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}",
                           i ? "," : "", PAGE, PAGE, name);
  }
  snprintf(views + at, sizeof views - at, "]}]");
  return docb_open(&d, views, keep);
}

int bdf_compare_main(int argc, char **argv, const char *who, bdf_compare_draw draw, void *ctx,
                     unsigned flags, double mean_limit, double far_limit) {
  comparison c = {who, draw, ctx, mean_limit, far_limit, 0, 0};
  bdf_error err;
  bdf_fonts *fonts = NULL;
  if (bdf_fonts_new(&fonts, &err) != BDF_OK) {
    return 1;
  }
  /* font files first: the fonts for the text that names its font */
  int first = 1;
  for (; first < argc; first++) {
    size_t n = strlen(argv[first]);
    if (n < 4 || (strcmp(argv[first] + n - 4, ".ttf") != 0 && strcmp(argv[first] + n - 4, ".otf") != 0)) {
      break;
    }
    if (bdf_fonts_add_file(fonts, argv[first], &err) != BDF_OK) {
      fprintf(stderr, "%s: %s\n", argv[first], err.message);
    }
  }
  if (first == argc) {
    wr keep = {0};
    bdf_doc *doc = builtin_pages(flags, &keep);
    if (doc) {
      compare_doc(&c, doc, "built in", fonts, 64);
      bdf_doc_free(doc);
    } else {
      c.bad++;
    }
    free(keep.b);
  }
  for (int a = first; a < argc; a++) {
    size_t len;
    uint8_t *data = read_file(argv[a], &len);
    bdf_doc *doc = NULL;
    if (!data || bdf_doc_open_memory(data, len, &doc, &err) != BDF_OK) {
      fprintf(stderr, "%s: cannot be opened\n", argv[a]);
      free(data);
      c.bad++;
      continue;
    }
    compare_doc(&c, doc, argv[a], fonts, 4);
    bdf_doc_free(doc);
    free(data);
  }
  bdf_fonts_free(fonts);
  printf("%s: %d pages, %d apart\n", who, c.pages, c.bad);
  return c.bad != 0 || failures != 0;
}
