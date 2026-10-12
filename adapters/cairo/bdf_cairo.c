/* libbdf through cairo: see bdf_cairo.h. */
#include "bdf/bdf_cairo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The surface of a bitmap that has an id, kept while the renderer may give
 * the id again. */
typedef struct bdf_cairo_image {
  uint64_t id;
  cairo_surface_t *surface;
} bdf_cairo_image;

/* A layer that is open: what it is composited with when it ends. */
typedef struct bdf_cairo_layer {
  double alpha;
  cairo_operator_t op;
} bdf_cairo_layer;

struct bdf_cairo {
  bdf_renderer *renderer;
  cairo_t *cr;
  bdf_cairo_image *images;
  size_t nimages, cap_images;
  bdf_cairo_layer *layers;
  size_t nlayers, cap_layers;
};

bdf_cairo *bdf_cairo_new(bdf_renderer *renderer) {
  bdf_cairo *bc = calloc(1, sizeof *bc);
  if (bc) {
    bc->renderer = renderer;
  }
  return bc;
}

void bdf_cairo_free(bdf_cairo *bc) {
  if (!bc) {
    return;
  }
  for (size_t i = 0; i < bc->nimages; i++) {
    cairo_surface_destroy(bc->images[i].surface);
  }
  free(bc->images);
  free(bc->layers);
  free(bc);
}

/* A bitmap as a cairo surface: premultiplied, in cairo's order of bytes. */
static cairo_surface_t *bdf_cairo_surface(bdf_cairo *bc, const bdf_canvas_bitmap *b) {
  if (b->id != 0) {
    for (size_t i = 0; i < bc->nimages; i++) {
      if (bc->images[i].id == b->id) {
        return cairo_surface_reference(bc->images[i].surface);
      }
    }
  }
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, b->width, b->height);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    return s;
  }
  cairo_surface_flush(s);
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < b->height; y++) {
    const uint8_t *src = b->pixels + 4 * (size_t)y * (size_t)b->width;
    uint32_t *dst = (uint32_t *)(void *)(data + (size_t)y * (size_t)stride);
    for (int x = 0; x < b->width; x++) {
      dst[x] = (uint32_t)src[4 * x + 3] << 24 | (uint32_t)src[4 * x] << 16 |
               (uint32_t)src[4 * x + 1] << 8 | src[4 * x + 2];
    }
  }
  cairo_surface_mark_dirty(s);
  if (b->id != 0) {
    if (bc->nimages == bc->cap_images) {
      size_t cap = bc->cap_images ? 2 * bc->cap_images : 16;
      bdf_cairo_image *images = realloc(bc->images, cap * sizeof *images);
      if (!images) {
        return s; /* drawn, and made again the next time */
      }
      bc->images = images;
      bc->cap_images = cap;
    }
    bc->images[bc->nimages].id = b->id;
    bc->images[bc->nimages].surface = cairo_surface_reference(s);
    bc->nimages++;
  }
  return s;
}

static cairo_operator_t bdf_cairo_operator(uint8_t blend) {
  static const cairo_operator_t ops[] = {
      CAIRO_OPERATOR_OVER,          CAIRO_OPERATOR_MULTIPLY,      CAIRO_OPERATOR_SCREEN,
      CAIRO_OPERATOR_OVERLAY,       CAIRO_OPERATOR_DARKEN,        CAIRO_OPERATOR_LIGHTEN,
      CAIRO_OPERATOR_COLOR_DODGE,   CAIRO_OPERATOR_COLOR_BURN,    CAIRO_OPERATOR_HARD_LIGHT,
      CAIRO_OPERATOR_SOFT_LIGHT,    CAIRO_OPERATOR_DIFFERENCE,    CAIRO_OPERATOR_EXCLUSION,
      CAIRO_OPERATOR_HSL_HUE,       CAIRO_OPERATOR_HSL_SATURATION, CAIRO_OPERATOR_HSL_COLOR,
      CAIRO_OPERATOR_HSL_LUMINOSITY, CAIRO_OPERATOR_DEST_OVER,    CAIRO_OPERATOR_DEST_IN,
      CAIRO_OPERATOR_DEST_OUT,      CAIRO_OPERATOR_IN,            CAIRO_OPERATOR_OUT,
      CAIRO_OPERATOR_ATOP,          CAIRO_OPERATOR_DEST_ATOP,     CAIRO_OPERATOR_XOR,
      CAIRO_OPERATOR_SOURCE,        CAIRO_OPERATOR_ADD,
  };
  return blend < sizeof ops / sizeof ops[0] ? ops[blend] : CAIRO_OPERATOR_OVER;
}

static void bdf_cairo_path(cairo_t *cr, const bdf_canvas_path *p) {
  const double *c = p->coords;
  double x = 0, y = 0;
  cairo_new_path(cr);
  for (size_t i = 0; i < p->nverbs; i++) {
    switch (p->verbs[i]) {
    case BDF_VERB_MOVE:
      cairo_move_to(cr, c[0], c[1]);
      x = c[0];
      y = c[1];
      c += 2;
      break;
    case BDF_VERB_LINE:
      cairo_line_to(cr, c[0], c[1]);
      x = c[0];
      y = c[1];
      c += 2;
      break;
    case BDF_VERB_QUAD:
      /* the cubic curve of the same shape */
      cairo_curve_to(cr, x + 2 * (c[0] - x) / 3, y + 2 * (c[1] - y) / 3, c[2] + 2 * (c[0] - c[2]) / 3,
                     c[3] + 2 * (c[1] - c[3]) / 3, c[2], c[3]);
      x = c[2];
      y = c[3];
      c += 4;
      break;
    case BDF_VERB_CUBIC:
      cairo_curve_to(cr, c[0], c[1], c[2], c[3], c[4], c[5]);
      x = c[4];
      y = c[5];
      c += 6;
      break;
    default:
      cairo_close_path(cr);
      break;
    }
  }
}

static void bdf_cairo_stop(cairo_pattern_t *p, double offset, uint32_t color) {
  cairo_pattern_add_color_stop_rgba(p, offset < 0 ? 0 : offset > 1 ? 1 : offset,
                                    (color >> 24) / 255.0, (color >> 16 & 0xff) / 255.0,
                                    (color >> 8 & 0xff) / 255.0, (color & 0xff) / 255.0);
}

/* The colour of a gradient at t, between its stops (not premultiplied). */
static void bdf_cairo_color_at(const bdf_canvas_paint *paint, double t, double out[4]) {
  const bdf_stop *lo = NULL, *hi = NULL;
  for (size_t i = 0; i < paint->nstops; i++) {
    const bdf_stop *s = &paint->stops[i];
    double off = s->offset < 0 ? 0 : s->offset > 1 ? 1 : s->offset;
    double lo_off = lo ? (lo->offset < 0 ? 0 : lo->offset > 1 ? 1 : lo->offset) : -1;
    double hi_off = hi ? (hi->offset < 0 ? 0 : hi->offset > 1 ? 1 : hi->offset) : 2;
    if (off <= t && off >= lo_off) {
      lo = s;
    }
    if (off >= t && off < hi_off) {
      hi = s;
    }
  }
  if (!lo) {
    lo = hi;
  }
  if (!hi) {
    hi = lo;
  }
  double a = lo->offset < 0 ? 0 : lo->offset > 1 ? 1 : lo->offset;
  double b = hi->offset < 0 ? 0 : hi->offset > 1 ? 1 : hi->offset;
  double u = b > a ? (t - a) / (b - a) : 0;
  for (int k = 0; k < 4; k++) {
    double c0 = (lo->color >> (24 - 8 * k) & 0xff) / 255.0;
    double c1 = (hi->color >> (24 - 8 * k) & 0xff) / 255.0;
    out[k] = c0 + (c1 - c0) * u;
  }
}

/* A conic gradient, which cairo has no pattern for, as a mesh of wedges
 * around its centre, far enough out to cover anything. */
static cairo_pattern_t *bdf_cairo_conic(const bdf_canvas_paint *paint) {
  enum { wedges = 180 };
  double start = paint->coords[0], cx = paint->coords[1], cy = paint->coords[2];
  const double reach = 1e6;
  cairo_pattern_t *mesh = cairo_pattern_create_mesh();
  for (int i = 0; i < wedges; i++) {
    double t0 = (double)i / wedges, t1 = (double)(i + 1) / wedges;
    double a0 = start + 2 * M_PI * t0, a1 = start + 2 * M_PI * t1;
    double c0[4], c1[4];
    bdf_cairo_color_at(paint, t0, c0);
    bdf_cairo_color_at(paint, t1, c1);
    cairo_mesh_pattern_begin_patch(mesh);
    cairo_mesh_pattern_move_to(mesh, cx, cy);
    cairo_mesh_pattern_line_to(mesh, cx + reach * cos(a0), cy + reach * sin(a0));
    cairo_mesh_pattern_line_to(mesh, cx + reach * cos(a1), cy + reach * sin(a1));
    cairo_mesh_pattern_line_to(mesh, cx, cy);
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, (c0[0] + c1[0]) / 2, (c0[1] + c1[1]) / 2,
                                             (c0[2] + c1[2]) / 2, (c0[3] + c1[3]) / 2);
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, c0[0], c0[1], c0[2], c0[3]);
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, c1[0], c1[1], c1[2], c1[3]);
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, (c0[0] + c1[0]) / 2, (c0[1] + c1[1]) / 2,
                                             (c0[2] + c1[2]) / 2, (c0[3] + c1[3]) / 2);
    cairo_mesh_pattern_end_patch(mesh);
  }
  return mesh;
}

/* A copy of a surface with its first and last rows (pad_y) or columns
 * (pad_x) once more around it. */
static cairo_surface_t *bdf_cairo_padded(cairo_surface_t *s, int pad_x, int pad_y) {
  cairo_surface_t *out =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cairo_image_surface_get_width(s) + 2 * pad_x,
                                 cairo_image_surface_get_height(s) + 2 * pad_y);
  cairo_t *cr = cairo_create(out);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, s, pad_x, pad_y);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_destroy(cr);
  return out;
}

/*
 * Sets the source of a style: its colour, gradient or pattern. A pattern
 * that does not repeat along an axis shows in a band (or in its one cell),
 * with its pixels as they are up to the edge: cairo cannot say that, so
 * *band is set (1: it repeats along x, 2: along y, 3: not at all) and
 * band_matrix/band_size say the band, which the caller clips to.
 */
static void bdf_cairo_source(bdf_cairo *bc, const bdf_canvas_style *style, int *band,
                             cairo_matrix_t *band_matrix, double band_size[2]) {
  cairo_t *cr = bc->cr;
  const bdf_canvas_paint *paint = style->paint;
  *band = 0;
  if (!paint) {
    uint32_t c = style->color;
    cairo_set_source_rgba(cr, (c >> 24) / 255.0, (c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0,
                          (c & 0xff) / 255.0);
    return;
  }
  cairo_pattern_t *p = NULL;
  switch (paint->kind) {
  case BDF_PAINT_LINEAR:
    if (paint->ncoords >= 4) {
      p = cairo_pattern_create_linear(paint->coords[0], paint->coords[1], paint->coords[2],
                                      paint->coords[3]);
    }
    break;
  case BDF_PAINT_RADIAL:
    if (paint->ncoords >= 6) {
      p = cairo_pattern_create_radial(paint->coords[0], paint->coords[1], paint->coords[2],
                                      paint->coords[3], paint->coords[4], paint->coords[5]);
    }
    break;
  case BDF_PAINT_CONIC:
    if (paint->ncoords >= 3 && paint->nstops > 0) {
      p = bdf_cairo_conic(paint);
    }
    break;
  case BDF_PAINT_PATTERN:
    if (paint->image) {
      cairo_surface_t *s = bdf_cairo_surface(bc, paint->image);
      p = cairo_pattern_create_for_surface(s);
      cairo_surface_destroy(s);
      cairo_matrix_t m;
      cairo_matrix_init(&m, paint->matrix[0], paint->matrix[1], paint->matrix[2], paint->matrix[3],
                        paint->matrix[4], paint->matrix[5]);
      int pad_x = 0, pad_y = 0;
      if (paint->repeat != BDF_REPEAT) {
        *band = paint->repeat == BDF_REPEAT_X ? 1 : paint->repeat == BDF_REPEAT_Y ? 2 : 3;
        *band_matrix = m;
        band_size[0] = paint->image->width;
        band_size[1] = paint->image->height;
        /* along the axis it repeats on, the image with its edge once more
         * on both sides of the other, so that the edge is not mixed with
         * the far side */
        if (*band != 3) {
          pad_x = *band == 2;
          pad_y = *band == 1;
          cairo_surface_t *padded = bdf_cairo_padded(s, pad_x, pad_y);
          cairo_pattern_destroy(p);
          p = cairo_pattern_create_for_surface(padded);
          cairo_surface_destroy(padded);
        }
      }
      if (cairo_matrix_invert(&m) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(p);
        p = NULL;
        *band = 0;
        break;
      }
      cairo_matrix_t pad;
      cairo_matrix_init_translate(&pad, pad_x, pad_y);
      cairo_matrix_multiply(&m, &m, &pad);
      cairo_pattern_set_matrix(p, &m);
      cairo_pattern_set_extend(p, *band == 3 ? CAIRO_EXTEND_PAD : CAIRO_EXTEND_REPEAT);
      cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);
    }
    break;
  default:
    break;
  }
  if (!p) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    return;
  }
  if (paint->kind == BDF_PAINT_LINEAR || paint->kind == BDF_PAINT_RADIAL) {
    for (size_t i = 0; i < paint->nstops; i++) {
      bdf_cairo_stop(p, paint->stops[i].offset, paint->stops[i].color);
    }
    cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
  }
  cairo_set_source(cr, p);
  cairo_pattern_destroy(p);
}

/* Clips to the band, or the cell, a pattern that does not repeat along
 * both axes shows in. */
static void bdf_cairo_band(cairo_t *cr, int band, const cairo_matrix_t *m, const double size[2]) {
  const double far = 1e7;
  cairo_save(cr);
  cairo_transform(cr, m);
  cairo_new_path(cr);
  if (band == 1) {
    cairo_rectangle(cr, -far, 0, 2 * far, size[1]);
  } else if (band == 2) {
    cairo_rectangle(cr, 0, -far, size[0], 2 * far);
  } else {
    cairo_rectangle(cr, 0, 0, size[0], size[1]);
  }
  cairo_restore(cr);
  cairo_clip(cr);
}

static void bdf_cairo_save(void *ctx) { cairo_save(((bdf_cairo *)ctx)->cr); }

static void bdf_cairo_restore(void *ctx) { cairo_restore(((bdf_cairo *)ctx)->cr); }

static void bdf_cairo_transform(void *ctx, const double m[6]) {
  cairo_matrix_t cm;
  cairo_matrix_init(&cm, m[0], m[1], m[2], m[3], m[4], m[5]);
  cairo_transform(((bdf_cairo *)ctx)->cr, &cm);
}

static void bdf_cairo_clip(void *ctx, const bdf_canvas_path *path, uint8_t rule) {
  cairo_t *cr = ((bdf_cairo *)ctx)->cr;
  bdf_cairo_path(cr, path);
  cairo_set_fill_rule(cr, rule == BDF_FILL_EVENODD ? CAIRO_FILL_RULE_EVEN_ODD
                                                    : CAIRO_FILL_RULE_WINDING);
  cairo_clip(cr);
}

/* Fills or strokes the current path with a style. With an alpha below 1
 * the shape is drawn on a group of its own first, so that a stroke that
 * crosses itself, or a paint, is not composited twice. */
static void bdf_cairo_draw(bdf_cairo *bc, const bdf_canvas_style *style, int stroke) {
  cairo_t *cr = bc->cr;
  int band;
  cairo_matrix_t band_matrix;
  double band_size[2];
  cairo_operator_t op = bdf_cairo_operator(style->blend);
  cairo_path_t *path = cairo_copy_path(cr);
  cairo_save(cr);
  if (style->alpha < 1) {
    cairo_push_group(cr);
    cairo_new_path(cr);
    cairo_append_path(cr, path);
    bdf_cairo_source(bc, style, &band, &band_matrix, band_size);
    if (band) {
      cairo_path_t *again = cairo_copy_path(cr);
      bdf_cairo_band(cr, band, &band_matrix, band_size);
      cairo_new_path(cr);
      cairo_append_path(cr, again);
      cairo_path_destroy(again);
    }
    if (stroke) {
      cairo_stroke(cr);
    } else {
      cairo_fill(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, op);
    cairo_paint_with_alpha(cr, style->alpha);
  } else {
    bdf_cairo_source(bc, style, &band, &band_matrix, band_size);
    if (band) {
      bdf_cairo_band(cr, band, &band_matrix, band_size);
      cairo_new_path(cr);
      cairo_append_path(cr, path);
    }
    cairo_set_operator(cr, op);
    if (stroke) {
      cairo_stroke(cr);
    } else {
      cairo_fill(cr);
    }
  }
  cairo_restore(cr);
  cairo_new_path(cr);
  cairo_path_destroy(path);
}

static void bdf_cairo_fill(void *ctx, const bdf_canvas_path *path, uint8_t rule,
                           const bdf_canvas_style *style) {
  bdf_cairo *bc = ctx;
  bdf_cairo_path(bc->cr, path);
  cairo_set_fill_rule(bc->cr, rule == BDF_FILL_EVENODD ? CAIRO_FILL_RULE_EVEN_ODD
                                                        : CAIRO_FILL_RULE_WINDING);
  bdf_cairo_draw(bc, style, 0);
}

static void bdf_cairo_stroke(void *ctx, const bdf_canvas_path *path, const bdf_canvas_line *line,
                             const bdf_canvas_style *style) {
  bdf_cairo *bc = ctx;
  cairo_t *cr = bc->cr;
  cairo_save(cr);
  cairo_set_line_width(cr, line->width);
  cairo_set_line_cap(cr, line->cap == BDF_CAP_ROUND    ? CAIRO_LINE_CAP_ROUND
                         : line->cap == BDF_CAP_SQUARE ? CAIRO_LINE_CAP_SQUARE
                                                       : CAIRO_LINE_CAP_BUTT);
  cairo_set_line_join(cr, line->join == BDF_JOIN_ROUND   ? CAIRO_LINE_JOIN_ROUND
                          : line->join == BDF_JOIN_BEVEL ? CAIRO_LINE_JOIN_BEVEL
                                                         : CAIRO_LINE_JOIN_MITER);
  cairo_set_miter_limit(cr, line->miter);
  if (line->dash && line->ndash > 0 && line->ndash <= 64) {
    double dashes[64];
    for (size_t i = 0; i < line->ndash; i++) {
      dashes[i] = line->dash[i];
    }
    cairo_set_dash(cr, dashes, (int)line->ndash, line->dash_offset);
  } else {
    cairo_set_dash(cr, NULL, 0, 0);
  }
  bdf_cairo_path(cr, path);
  bdf_cairo_draw(bc, style, 1);
  cairo_restore(cr);
}

static void bdf_cairo_image_fn(void *ctx, const bdf_canvas_bitmap *bitmap, const double src[4],
                               const double dst[4], const bdf_canvas_style *style) {
  bdf_cairo *bc = ctx;
  cairo_t *cr = bc->cr;
  if (!(src[2] > 0 && src[3] > 0 && dst[2] > 0 && dst[3] > 0)) {
    return;
  }
  cairo_surface_t *whole = bdf_cairo_surface(bc, bitmap);
  /* the part alone, so that what lies beside it does not bleed into it:
   * whole pixels around it, held at their edges */
  double x0 = floor(src[0]), y0 = floor(src[1]);
  double x1 = ceil(src[0] + src[2]), y1 = ceil(src[1] + src[3]);
  cairo_surface_t *part = cairo_surface_create_for_rectangle(whole, x0, y0, x1 - x0, y1 - y0);
  cairo_pattern_t *p = cairo_pattern_create_for_surface(part);
  cairo_matrix_t m;
  /* user space to the pixels of the part */
  cairo_matrix_init(&m, src[2] / dst[2], 0, 0, src[3] / dst[3],
                    src[0] - x0 - dst[0] * src[2] / dst[2], src[1] - y0 - dst[1] * src[3] / dst[3]);
  cairo_pattern_set_matrix(p, &m);
  cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
  cairo_pattern_set_filter(p, style->smoothing ? CAIRO_FILTER_GOOD : CAIRO_FILTER_NEAREST);
  cairo_save(cr);
  cairo_new_path(cr);
  cairo_rectangle(cr, dst[0], dst[1], dst[2], dst[3]);
  cairo_clip(cr);
  cairo_set_source(cr, p);
  cairo_set_operator(cr, bdf_cairo_operator(style->blend));
  cairo_paint_with_alpha(cr, style->alpha);
  cairo_restore(cr);
  cairo_pattern_destroy(p);
  cairo_surface_destroy(part);
  cairo_surface_destroy(whole);
}

static void bdf_cairo_begin_layer(void *ctx, const double bounds[4], float alpha, uint8_t blend) {
  bdf_cairo *bc = ctx;
  (void)bounds;
  if (bc->nlayers == bc->cap_layers) {
    size_t cap = bc->cap_layers ? 2 * bc->cap_layers : 8;
    bdf_cairo_layer *layers = realloc(bc->layers, cap * sizeof *layers);
    if (!layers) {
      /* no layer: what is in it is drawn straight, and end_layer knows */
      cairo_save(bc->cr);
      return;
    }
    bc->layers = layers;
    bc->cap_layers = cap;
  }
  bc->layers[bc->nlayers].alpha = alpha;
  bc->layers[bc->nlayers].op = bdf_cairo_operator(blend);
  bc->nlayers++;
  cairo_save(bc->cr);
  cairo_push_group(bc->cr);
}

static void bdf_cairo_end_layer(void *ctx) {
  bdf_cairo *bc = ctx;
  if (bc->nlayers == 0) {
    cairo_restore(bc->cr);
    return;
  }
  bdf_cairo_layer l = bc->layers[--bc->nlayers];
  cairo_pop_group_to_source(bc->cr);
  cairo_set_operator(bc->cr, l.op);
  cairo_paint_with_alpha(bc->cr, l.alpha);
  cairo_restore(bc->cr);
}

void bdf_cairo_canvas(bdf_cairo *bc, cairo_t *cr, bdf_canvas *out) {
  memset(out, 0, sizeof *out);
  bc->cr = cr;
  out->ctx = bc;
  out->save = bdf_cairo_save;
  out->restore = bdf_cairo_restore;
  out->transform = bdf_cairo_transform;
  out->clip = bdf_cairo_clip;
  out->fill = bdf_cairo_fill;
  out->stroke = bdf_cairo_stroke;
  out->image = bdf_cairo_image_fn;
  out->begin_layer = bdf_cairo_begin_layer;
  out->end_layer = bdf_cairo_end_layer;
}

/* The device pixels a unit of the user space takes on cr. */
static double bdf_cairo_scale(cairo_t *cr) {
  cairo_matrix_t m;
  cairo_get_matrix(cr, &m);
  double s = sqrt(fabs(m.xx * m.yy - m.xy * m.yx));
  return s > 0 ? s : 1;
}

bdf_status bdf_cairo_draw_page(bdf_cairo *bc, size_t view, size_t page, cairo_t *cr,
                               bdf_error *err) {
  bdf_canvas canvas;
  bdf_cairo_canvas(bc, cr, &canvas);
  bc->nlayers = 0;
  return bdf_play_page(bc->renderer, view, page, bdf_cairo_scale(cr), &canvas, err);
}

bdf_status bdf_cairo_draw_part(bdf_cairo *bc, const uint8_t hash[16], cairo_t *cr, bdf_error *err) {
  bdf_canvas canvas;
  bdf_cairo_canvas(bc, cr, &canvas);
  bc->nlayers = 0;
  return bdf_play_part(bc->renderer, hash, bdf_cairo_scale(cr), &canvas, err);
}
