/*
 * Drawing through another canvas: the instructions of a page run as the
 * rasterizer runs them (rdraw.c), with what they draw handed to the
 * functions of a bdf_canvas instead of being rasterized here. The drawing
 * state is kept here and every drawing carries its whole style; the
 * transform and the clip are the canvas's. What a 2D library has no one
 * name for is done here and given as something it has: text is the
 * outlines of its glyphs unless the canvas draws text, and a soft mask is
 * drawn by the rasterizer and given as an image that what was drawn is
 * multiplied by.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* What SAVE and RESTORE keep, beside the transform and clip of the canvas. */
typedef struct bdfi_pstate {
  bdfi_mat m; /* the user space in the space of the page */
  bdfi_style fill, stroke;
  bdfi_line_style line; /* its dashes counted */
  float alpha;
  uint8_t blend;
  uint32_t shadow;
  double shadow_blur, shadow_dx, shadow_dy; /* in units of the page */
  bdfi_robj *font_owner;
  size_t font_index;
  double size;
  uint8_t align, baseline, dir;
  double spacing;
  int smooth;
} bdfi_pstate;

/* A group that is open: a layer of the canvas. */
typedef struct bdfi_player_layer {
  size_t base;       /* the states saved outside it, which RESTORE inside does not reach */
  int64_t unsaved;   /* and the SAVEs that kept none */
  bdfi_pstate saved; /* the state the group ends in */
  bdfi_irect dev;    /* its pixels at the scale of the playing: what a soft mask covers */
} bdfi_player_layer;

typedef struct bdfi_player {
  struct bdf_renderer *r;
  const bdf_canvas *c;
  double scale;
  bdfi_pstate st;
  bdfi_pstate *stack;
  size_t nstack, cap_stack;
  int64_t unsaved;
  bdfi_player_layer *layers;
  size_t nlayers, cap_layers;
  size_t layer_floor; /* the groups open where the object being run began: it ends none of them */
  bdfi_irect page; /* the pixels of what is played */
  bdfi_robj *obj;
  int depth;
  bdfi_hash_set seen;
  int again;
  int64_t reused;
  int64_t skipped; /* the groups and soft masks that are open but not drawn */
  /* a soft mask the rasterizer is drawing: the instructions go to it */
  bdfi_drawer *mask;
  int64_t mask_depth; /* the MASK_BEGINs inside it that have not ended */
  uint8_t mask_kind;
  uint8_t transfer[256];
  int has_transfer, mask_applies;
  /* the path being handed over */
  uint8_t *verbs;
  size_t nverbs, cap_verbs;
  double *coords;
  size_t ncoords, cap_coords;
} bdfi_player;

static void bdfi_pstate_init(bdfi_pstate *st, const bdfi_mat *m) {
  memset(st, 0, sizeof *st);
  st->m = *m;
  st->fill.color = 0x000000ff;
  st->stroke.color = 0x000000ff;
  st->line.width = 1;
  st->line.miter = 10;
  st->alpha = 1;
  st->size = 10;
  st->smooth = 1;
}

static void bdfi_pstate_retain(bdfi_pstate *st) {
  if (st->line.dash) {
    st->line.dash->refs++;
  }
}

static void bdfi_pstate_release(bdfi_pstate *st) {
  bdfi_dash_unref(st->line.dash);
  st->line.dash = NULL;
}

/* ---- Paths --------------------------------------------------------------- */

static int bdfi_player_room(bdfi_player *p, size_t verbs, size_t coords) {
  if (p->nverbs + verbs > p->cap_verbs) {
    size_t cap = 2 * (p->nverbs + verbs) + 16;
    uint8_t *v = bdfi_realloc_array(p->verbs, cap, 1);
    if (!v) {
      p->r->oom = 1;
      return 0;
    }
    p->verbs = v;
    p->cap_verbs = cap;
  }
  if (p->ncoords + coords > p->cap_coords) {
    size_t cap = 2 * (p->ncoords + coords) + 64;
    double *c = bdfi_realloc_array(p->coords, cap, sizeof *c);
    if (!c) {
      p->r->oom = 1;
      return 0;
    }
    p->coords = c;
    p->cap_coords = cap;
  }
  return 1;
}

static void bdfi_player_path_reset(bdfi_player *p) {
  p->nverbs = 0;
  p->ncoords = 0;
}

/* Adds a path to the one being handed over, under a matrix (NULL: as it is). */
static void bdfi_player_add_path(bdfi_player *p, const bdfi_rpath *path, const bdfi_mat *m) {
  for (size_t i = 0; i < path->nsubs; i++) {
    const bdfi_sub *s = &path->subs[i];
    if (s->nsegs == 0) {
      /* a point alone: nothing is filled or stroked of it (the rasterizer
       * draws nothing either), and 2D libraries do not agree on what a
       * lone move is */
      continue;
    }
    if (!bdfi_player_room(p, s->nsegs + 2, 6 * s->nsegs + 2)) {
      return;
    }
    bdfi_pt q = m ? bdfi_mat_pt(m, s->start.x, s->start.y) : s->start;
    p->verbs[p->nverbs++] = BDF_VERB_MOVE;
    p->coords[p->ncoords++] = q.x;
    p->coords[p->ncoords++] = q.y;
    for (size_t k = 0; k < s->nsegs; k++) {
      const bdfi_seg *g = &path->segs[s->seg0 + k];
      int n = g->kind == BDFI_SEG_LINE ? 1 : g->kind == BDFI_SEG_QUAD ? 2 : 3;
      p->verbs[p->nverbs++] =
          (uint8_t)(n == 1 ? BDF_VERB_LINE : n == 2 ? BDF_VERB_QUAD : BDF_VERB_CUBIC);
      for (int j = 0; j < n; j++) {
        q.x = g->p[2 * j];
        q.y = g->p[2 * j + 1];
        if (m) {
          q = bdfi_mat_pt(m, q.x, q.y);
        }
        p->coords[p->ncoords++] = q.x;
        p->coords[p->ncoords++] = q.y;
      }
    }
    if (s->closed) {
      p->verbs[p->nverbs++] = BDF_VERB_CLOSE;
    }
  }
}

static bdf_canvas_path bdfi_player_path(const bdfi_player *p) {
  bdf_canvas_path out = {p->verbs, p->nverbs, p->coords, p->ncoords};
  return out;
}

static void bdfi_player_rect(bdfi_player *p, double x, double y, double w, double h) {
  bdfi_rpath r;
  memset(&r, 0, sizeof r);
  bdfi_rpath_rect(&r, x, y, w, h);
  if (r.oom) {
    p->r->oom = 1;
  } else {
    bdfi_player_add_path(p, &r, NULL);
  }
  bdfi_rpath_clear(&r);
}

/* ---- Styles -------------------------------------------------------------- */

/* A picture as the bitmap a canvas is given. */
static bdf_canvas_bitmap bdfi_player_bitmap(const bdfi_picture *pic) {
  bdf_canvas_bitmap b = {pic->serial, pic->w, pic->h, pic->levels[0].pix};
  return b;
}

/* The style of a fill or a stroke as a canvas takes it. paint and image
 * are where what it points to is kept. */
static void bdfi_player_style(const bdfi_player *p, const bdfi_style *s, bdf_canvas_style *out,
                              bdf_canvas_paint *paint, bdf_canvas_bitmap *image) {
  const bdfi_pstate *st = &p->st;
  memset(out, 0, sizeof *out);
  out->color = s->color;
  out->alpha = st->alpha;
  out->blend = st->blend;
  out->shadow_color = st->shadow;
  out->shadow_blur = st->shadow_blur;
  out->shadow_dx = st->shadow_dx;
  out->shadow_dy = st->shadow_dy;
  out->smoothing = st->smooth;
  if (!s->has_paint) {
    return;
  }
  memset(paint, 0, sizeof *paint);
  paint->kind = s->paint.kind;
  out->paint = paint;
  if (s->paint.kind == BDF_PAINT_PATTERN) {
    /* a pattern of no image paints nothing, as the rasterizer has it */
    out->color = 0;
    if (!s->image) {
      out->paint = NULL;
      return;
    }
    *image = bdfi_player_bitmap(s->image);
    paint->image = image;
    paint->repeat = s->paint.repeat;
    for (int i = 0; i < 6; i++) {
      paint->matrix[i] = (double)s->paint.matrix[i];
    }
    return;
  }
  paint->coords = s->paint.coords;
  paint->ncoords = s->paint.ncoords;
  paint->stops = s->paint.stops;
  paint->nstops = s->paint.nstops;
  if (s->paint.nstops == 0) {
    /* a gradient without stops paints nothing */
    out->paint = NULL;
    out->color = 0;
  }
}

static void bdfi_player_line(const bdfi_pstate *st, bdf_canvas_line *out, float *dash,
                             size_t cap) {
  memset(out, 0, sizeof *out);
  out->width = st->line.width;
  out->cap = st->line.cap;
  out->join = st->line.join;
  out->miter = st->line.miter;
  if (st->line.dash && st->line.dash->n <= cap) {
    for (size_t i = 0; i < st->line.dash->n; i++) {
      dash[i] = (float)st->line.dash->v[i];
    }
    out->dash = dash;
    out->ndash = st->line.dash->n;
    out->dash_offset = st->line.dash_off;
  }
}

static bdfi_style bdfi_player_paint_style(bdfi_player *p, uint64_t i) {
  bdfi_style s;
  memset(&s, 0, sizeof s);
  if (i >= bdf_object_paint_count(p->obj->obj) ||
      bdf_object_paint(p->obj->obj, (size_t)i, &s.paint) != BDF_OK) {
    bdfi_r_warn(p->r, "bad paint reference");
    return s;
  }
  s.has_paint = 1;
  s.owner = p->obj;
  s.index = (size_t)i;
  if (s.paint.kind == BDF_PAINT_PATTERN && s.paint.image < bdf_object_image_count(p->obj->obj)) {
    s.image = bdfi_r_picture(p->r, bdf_object_image(p->obj->obj, (size_t)s.paint.image));
  }
  return s;
}

/* ---- Drawing ------------------------------------------------------------- */

/* The most dashes of a line handed over: a pattern of more is drawn solid. */
#define BDFI_PLAYER_DASHES 64

static void bdfi_player_fill(bdfi_player *p, uint8_t rule) {
  bdf_canvas_style style;
  bdf_canvas_paint paint;
  bdf_canvas_bitmap image;
  if (p->nverbs == 0 || p->r->oom) {
    return;
  }
  bdfi_player_style(p, &p->st.fill, &style, &paint, &image);
  bdf_canvas_path path = bdfi_player_path(p);
  p->c->fill(p->c->ctx, &path, rule == BDF_FILL_EVENODD ? BDF_FILL_EVENODD : BDF_FILL_NONZERO,
             &style);
}

static void bdfi_player_stroke(bdfi_player *p) {
  bdf_canvas_style style;
  bdf_canvas_paint paint;
  bdf_canvas_bitmap image;
  bdf_canvas_line line;
  float dash[BDFI_PLAYER_DASHES];
  if (p->nverbs == 0 || p->r->oom) {
    return;
  }
  bdfi_player_style(p, &p->st.stroke, &style, &paint, &image);
  bdfi_player_line(&p->st, &line, dash, BDFI_PLAYER_DASHES);
  bdf_canvas_path path = bdfi_player_path(p);
  p->c->stroke(p->c->ctx, &path, &line, &style);
}

static bdfi_rpath *bdfi_player_object_path(bdfi_player *p, uint64_t i) {
  if (i >= bdf_object_path_count(p->obj->obj)) {
    bdfi_r_warn(p->r, "bad path reference");
    return NULL;
  }
  return bdfi_r_path(p->r, p->obj, (size_t)i);
}

/* image: an image, or the part src of it, drawn into (x, y, w, h). The
 * canvas is given the part that lies within the image, under a transform
 * that takes the pixels of the image to where they are drawn (turned over
 * when a size is negative). */
static void bdfi_player_image(bdfi_player *p, uint64_t i, const double *src, double x, double y,
                              double w, double h) {
  if (i >= bdf_object_image_count(p->obj->obj)) {
    bdfi_r_warn(p->r, "bad image reference");
    return;
  }
  bdfi_picture *pic = bdfi_r_picture(p->r, bdf_object_image(p->obj->obj, (size_t)i));
  if (!pic || w == 0 || h == 0) {
    return;
  }
  double sx = 0, sy = 0, sw = (double)pic->w, sh = (double)pic->h;
  if (src) {
    sx = src[0];
    sy = src[1];
    sw = src[2];
    sh = src[3];
    if (sw == 0 || sh == 0) {
      return;
    }
  }
  bdfi_mat place = {{w / sw, 0, 0, h / sh, x - sx * w / sw, y - sy * h / sh}};
  if (!bdfi_mat_finite(&place)) {
    return;
  }
  /* the part of the image that is asked for */
  double x0 = bdfi_fmax(0, bdfi_fmin(sx, sx + sw)), y0 = bdfi_fmax(0, bdfi_fmin(sy, sy + sh));
  double x1 = bdfi_fmin((double)pic->w, bdfi_fmax(sx, sx + sw));
  double y1 = bdfi_fmin((double)pic->h, bdfi_fmax(sy, sy + sh));
  if (!(x1 > x0 && y1 > y0)) {
    return;
  }
  bdf_canvas_style style;
  bdf_canvas_paint paint;
  bdf_canvas_bitmap image, bitmap = bdfi_player_bitmap(pic);
  bdfi_style none;
  memset(&none, 0, sizeof none);
  bdfi_player_style(p, &none, &style, &paint, &image);
  const double part[4] = {x0, y0, x1 - x0, y1 - y0};
  p->c->save(p->c->ctx);
  p->c->transform(p->c->ctx, place.v);
  p->c->image(p->c->ctx, &bitmap, part, part, &style);
  p->c->restore(p->c->ctx);
}

/* ---- Text ---------------------------------------------------------------- */

#define BDFI_PLAYER_ITALIC_SKEW 0.25

/* text: a string at (x, y). A canvas that draws text is given the string;
 * otherwise the outlines of the glyphs are filled or stroked, laid out as
 * the rasterizer lays them out. */
static void bdfi_player_text(bdfi_player *p, const char *s, size_t len, double x, double y,
                             double advance, int stroke) {
  bdfi_pstate *st = &p->st;
  if (st->size <= 0 || len == 0) {
    return;
  }
  bdf_font font;
  int has_font = st->font_owner &&
                 bdf_object_font(st->font_owner->obj, st->font_index, &font) == BDF_OK;
  bdf_canvas_style style;
  bdf_canvas_paint paint;
  bdf_canvas_bitmap image;
  bdf_canvas_line line;
  float dash[BDFI_PLAYER_DASHES];
  bdfi_player_style(p, stroke ? &st->stroke : &st->fill, &style, &paint, &image);
  bdfi_player_line(st, &line, dash, BDFI_PLAYER_DASHES);
  if (p->c->text) {
    bdf_canvas_text t;
    memset(&t, 0, sizeof t);
    t.text = s;
    t.len = len;
    t.x = x;
    t.y = y;
    t.advance = advance;
    t.stroke = stroke;
    t.font = has_font ? &font : NULL;
    if (has_font && font.kind == BDF_FONT_EMBEDDED && font.hash) {
      t.font_data = bdfi_r_font_data(p->r, font.hash, &t.font_len, &t.font_id);
    }
    t.size = st->size;
    t.align = st->align;
    t.baseline = st->baseline;
    t.dir = st->dir;
    t.spacing = st->spacing;
    if (p->c->text(p->c->ctx, &t, &line, &style)) {
      return;
    }
  }
  bdfi_chain *ch = bdfi_r_chain(p->r, has_font ? &font : NULL);
  if (!ch) {
    return;
  }
  bdfi_placed *glyphs;
  size_t n;
  double width;
  if (bdfi_chain_layout(p->r, ch, s, len, st->size, st->spacing, st->dir == BDF_DIR_RTL, &glyphs,
                        &n, &width) < 0 ||
      n == 0) {
    return;
  }
  /* the text in the user space: where the string is, fitted to its advance,
   * aligned, on its baseline */
  bdfi_mat m = bdfi_mat_translate(bdfi_mat_identity(), x, y);
  if (advance > 0 && width > 0 && fabs(width - advance) / advance > 0.005) {
    m = bdfi_mat_scale(m, advance / width, 1);
  }
  if (st->align == BDF_ALIGN_RIGHT || (st->align == BDF_ALIGN_END && st->dir != BDF_DIR_RTL) ||
      (st->align == BDF_ALIGN_START && st->dir == BDF_DIR_RTL)) {
    m = bdfi_mat_translate(m, -width, 0);
  } else if (st->align == BDF_ALIGN_CENTER) {
    m = bdfi_mat_translate(m, -width / 2, 0);
  }
  m = bdfi_mat_translate(m, 0, bdfi_chain_baseline_shift(ch, st->baseline, st->size));
  /* the faces that are made bold are outlined too: their own path */
  bdfi_mat device = bdfi_mat_mul2(st->m, m);
  double bold_w = bdfi_fake_bold_width(st->size * bdfi_mat_area_scale(&device) * p->scale);
  double user_bold = bold_w / bdfi_fmax(bdfi_mat_area_scale(&st->m) * p->scale, 1e-9);
  for (int pass = 0; pass < 2 && !p->r->oom; pass++) {
    bdfi_player_path_reset(p);
    for (size_t i = 0; i < n; i++) {
      int thick = glyphs[i].use.bold && bold_w > 0 && !stroke;
      if (thick != pass) {
        continue;
      }
      bdfi_rpath *outline = bdfi_face_outline(glyphs[i].use.face, glyphs[i].gid, &p->r->oom);
      if (!outline) {
        continue;
      }
      double k = st->size / bdfi_face_upem(glyphs[i].use.face);
      bdfi_mat gm = bdfi_mat_scale(bdfi_mat_translate(m, glyphs[i].x, 0), k, -k);
      if (glyphs[i].use.italic) {
        bdfi_mat skew = {{1, 0, BDFI_PLAYER_ITALIC_SKEW, 1, 0, 0}};
        gm = bdfi_mat_mul2(gm, skew);
      }
      bdfi_player_add_path(p, outline, &gm);
    }
    if (p->nverbs == 0 || p->r->oom) {
      continue;
    }
    bdf_canvas_path path = bdfi_player_path(p);
    if (stroke) {
      p->c->stroke(p->c->ctx, &path, &line, &style);
    } else if (!pass) {
      p->c->fill(p->c->ctx, &path, BDF_FILL_NONZERO, &style);
    } else {
      /* filled and outlined as one: on a layer when it is not opaque, so
       * that the two do not show through each other */
      bdf_canvas_line around;
      memset(&around, 0, sizeof around);
      around.width = user_bold;
      around.join = BDF_JOIN_ROUND;
      around.miter = 10;
      int layer = style.alpha < 1 || style.paint || (style.color & 0xff) != 0xff ||
                  style.blend != BDF_BLEND_SOURCE_OVER;
      bdf_canvas_style solid = style;
      if (layer) {
        double box[4] = {0, 0, 0, 0};
        for (size_t c = 0; c + 1 < p->ncoords; c += 2) {
          if (c == 0 || p->coords[c] < box[0]) {
            box[0] = p->coords[c];
          }
          if (c == 0 || p->coords[c + 1] < box[1]) {
            box[1] = p->coords[c + 1];
          }
          if (c == 0 || p->coords[c] > box[2]) {
            box[2] = p->coords[c];
          }
          if (c == 0 || p->coords[c + 1] > box[3]) {
            box[3] = p->coords[c + 1];
          }
        }
        const double bounds[4] = {box[0] - user_bold, box[1] - user_bold,
                                  box[2] - box[0] + 2 * user_bold, box[3] - box[1] + 2 * user_bold};
        p->c->begin_layer(p->c->ctx, bounds, style.alpha, style.blend);
        solid.alpha = 1;
        solid.blend = BDF_BLEND_SOURCE_OVER;
        solid.shadow_color = 0;
      }
      p->c->fill(p->c->ctx, &path, BDF_FILL_NONZERO, &solid);
      p->c->stroke(p->c->ctx, &path, &around, &solid);
      if (layer) {
        p->c->end_layer(p->c->ctx);
      }
    }
  }
  free(glyphs);
}

/* ---- Groups and soft masks ----------------------------------------------- */

static int bdfi_player_layers_open(const bdfi_player *p) {
  return (int)p->nlayers + (p->mask ? 1 : 0);
}

/* The pixels of a rectangle of the user space at the scale of the playing,
 * within the layer that is open (the page without one). */
static bdfi_irect bdfi_player_device(const bdfi_player *p, double x, double y, double w, double h) {
  const double cx[4] = {x, x + w, x, x + w}, cy[4] = {y, y, y + h, y + h};
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < 4; i++) {
    bdfi_pt q = bdfi_mat_pt(&p->st.m, cx[i], cy[i]);
    x0 = bdfi_fmin(x0, q.x * p->scale);
    y0 = bdfi_fmin(y0, q.y * p->scale);
    x1 = bdfi_fmax(x1, q.x * p->scale);
    y1 = bdfi_fmax(y1, q.y * p->scale);
  }
  bdfi_irect r = bdfi_irect_of((int)floor(bdfi_clampf(x0)), (int)floor(bdfi_clampf(y0)),
                               (int)ceil(bdfi_clampf(x1)), (int)ceil(bdfi_clampf(y1)));
  r = bdfi_irect_intersect(r, p->nlayers ? p->layers[p->nlayers - 1].dev : p->page);
  if (bdfi_irect_empty(r)) {
    r = bdfi_irect_of(0, 0, 1, 1);
  }
  return r;
}

static void bdfi_player_group_begin(bdfi_player *p, float alpha, uint8_t blend, double x, double y,
                                    double w, double h) {
  if (bdfi_player_layers_open(p) >= BDFI_MAX_LAYERS) {
    bdfi_r_warnf(p->r,
                 "more than %d groups and soft masks are open: what is in the rest of them is "
                 "not drawn",
                 BDFI_MAX_LAYERS);
    p->skipped++;
    return;
  }
  if (p->nlayers == p->cap_layers) {
    size_t cap = p->cap_layers ? 2 * p->cap_layers : 8;
    bdfi_player_layer *layers = bdfi_realloc_array(p->layers, cap, sizeof *layers);
    if (!layers) {
      p->r->oom = 1;
      p->skipped++;
      return;
    }
    p->layers = layers;
    p->cap_layers = cap;
  }
  bdfi_irect dev = bdfi_player_device(p, x, y, w, h);
  bdfi_player_layer *l = &p->layers[p->nlayers++];
  l->base = p->nstack;
  l->unsaved = p->unsaved;
  l->saved = p->st;
  l->dev = dev;
  /* a group starts in the state of a new canvas, with the styles, the line
   * width and the font it was begun in */
  bdfi_pstate inner;
  bdfi_pstate_init(&inner, &p->st.m);
  inner.fill = p->st.fill;
  inner.stroke = p->st.stroke;
  inner.line.width = p->st.line.width;
  inner.font_owner = p->st.font_owner;
  inner.font_index = p->st.font_index;
  inner.size = p->st.size;
  if (blend > BDF_BLEND_LIGHTER) {
    blend = BDF_BLEND_SOURCE_OVER;
  }
  const double bounds[4] = {x, y, w, h};
  p->c->begin_layer(p->c->ctx, bounds, alpha, blend);
  /* nothing of a group shows outside its rectangle */
  bdfi_player_path_reset(p);
  bdfi_player_rect(p, x, y, w, h);
  if (p->nverbs > 0) {
    bdf_canvas_path path = bdfi_player_path(p);
    p->c->clip(p->c->ctx, &path, BDF_FILL_NONZERO);
  }
  p->st = inner; /* the state of the group's start is kept in the layer */
  p->unsaved = 0;
}

static void bdfi_player_group_end(bdfi_player *p) {
  if (p->nlayers <= p->layer_floor) {
    return; /* none, or one the object that drew this one began */
  }
  bdfi_player_layer l = p->layers[--p->nlayers];
  /* the states the group saved and did not restore */
  while (p->nstack > l.base) {
    bdfi_pstate_release(&p->st);
    p->st = p->stack[--p->nstack];
    p->c->restore(p->c->ctx);
  }
  bdfi_pstate_release(&p->st);
  p->st = l.saved;
  p->unsaved = l.unsaved;
  p->c->end_layer(p->c->ctx);
}

/* MASK_BEGIN: from here to its MASK_END the instructions draw a soft mask,
 * which the rasterizer draws, over the pixels of the group that is open. */
static void bdfi_player_mask_begin(bdfi_player *p, uint8_t kind, uint32_t backdrop,
                                   const uint8_t *transfer, size_t ntransfer) {
  if (bdfi_player_layers_open(p) >= BDFI_MAX_LAYERS) {
    p->skipped++;
    return;
  }
  bdfi_irect dev = p->nlayers ? p->layers[p->nlayers - 1].dev : bdfi_irect_of(0, 0, 1, 1);
  /* the user space of the mask on the pixels of the group */
  bdfi_mat to_dev = {{p->scale, 0, 0, p->scale, -(double)dev.x0, -(double)dev.y0}};
  bdfi_mat m = bdfi_mat_mul2(to_dev, p->st.m);
  uint64_t drawing = p->r->drawing;
  p->mask = bdfi_drawer_new(p->r, &m, BDFI_DX(dev), BDFI_DY(dev));
  /* it is part of this drawing: the pictures of the styles are still its */
  p->r->drawing = drawing;
  if (!p->mask) {
    p->r->oom = 1;
    p->skipped++;
    return;
  }
  if (kind == BDF_MASK_LUMINOSITY) {
    bdfi_solid c = bdfi_premul(backdrop | 0xff);
    bdfi_surface *s = p->mask->top->target;
    for (size_t i = 0; i < (size_t)s->w * (size_t)s->h; i++) {
      memcpy(s->pix + 4 * i, c.c, sizeof c.c);
    }
  }
  p->mask_depth = 0;
  p->mask_kind = kind;
  p->has_transfer = ntransfer == 256;
  if (p->has_transfer) {
    memcpy(p->transfer, transfer, 256);
  }
  p->mask_applies = p->nlayers > 0;
}

/* MASK_END: what the group has drawn so far is multiplied by the mask. */
static void bdfi_player_mask_end(bdfi_player *p) {
  bdfi_drawer *d = p->mask;
  p->mask = NULL;
  const bdfi_surface *s = d->top->target;
  size_t n = (size_t)s->w * (size_t)s->h;
  uint8_t *pix = p->mask_applies && !p->r->oom ? bdfi_malloc_array(n, 4) : NULL;
  bdfi_mat inv;
  if (pix && bdfi_mat_invert(&p->st.m, &inv)) {
    for (size_t i = 0; i < n; i++) {
      const float *q = s->pix + 4 * i;
      double level;
      if (p->mask_kind == BDF_MASK_LUMINOSITY) {
        level = round((double)(0.3f * q[0] + 0.59f * q[1] + 0.11f * q[2]) * 255);
      } else {
        level = round((double)q[3] * 255);
      }
      int v = level >= 0 && level <= 255 ? (int)level : level > 255 && level < 9.2e18 ? 255 : 0;
      if (p->has_transfer) {
        v = p->transfer[v];
      }
      pix[4 * i] = 0;
      pix[4 * i + 1] = 0;
      pix[4 * i + 2] = 0;
      pix[4 * i + 3] = (uint8_t)v;
    }
    const bdfi_irect dev = p->layers[p->nlayers - 1].dev;
    const bdf_canvas_bitmap bitmap = {0, s->w, s->h, pix};
    const double src[4] = {0, 0, (double)s->w, (double)s->h};
    const double dst[4] = {(double)dev.x0 / p->scale, (double)dev.y0 / p->scale,
                           (double)s->w / p->scale, (double)s->h / p->scale};
    bdf_canvas_style style;
    memset(&style, 0, sizeof style);
    style.alpha = 1;
    style.blend = BDF_BLEND_DESTINATION_IN;
    /* in the space of the page, whatever the transform is now */
    p->c->save(p->c->ctx);
    p->c->transform(p->c->ctx, inv.v);
    p->c->image(p->c->ctx, &bitmap, src, dst, &style);
    p->c->restore(p->c->ctx);
  } else if (p->mask_applies && !pix) {
    p->r->oom = 1;
  }
  free(pix);
  uint64_t drawing = p->r->drawing;
  bdfi_drawer_free(d);
  p->r->drawing = drawing;
}

/* ---- Instructions -------------------------------------------------------- */

static void bdfi_player_run(bdfi_player *p, bdfi_robj *o);

static void bdfi_player_restore(bdfi_player *p) {
  size_t base = p->nlayers ? p->layers[p->nlayers - 1].base : 0;
  if (p->unsaved > 0) {
    p->unsaved--;
    return;
  }
  if (p->nstack > base) {
    bdfi_pstate_release(&p->st);
    p->st = p->stack[--p->nstack];
    p->c->restore(p->c->ctx);
  }
}

/* What an instruction that is not drawn still does: the SAVEs, RESTOREs,
 * groups and masks are counted, so that what follows meets its own. */
static void bdfi_player_left_out(bdfi_player *p, uint8_t op) {
  switch (op) {
  case BDF_OP_SAVE:
    p->unsaved++;
    break;
  case BDF_OP_RESTORE:
    bdfi_player_restore(p);
    break;
  case BDF_OP_GROUP_BEGIN:
  case BDF_OP_MASK_BEGIN:
    p->skipped++;
    break;
  case BDF_OP_GROUP_END:
    if (p->skipped > 0) {
      p->skipped--;
    } else {
      bdfi_player_group_end(p);
    }
    break;
  case BDF_OP_MASK_END:
    if (p->skipped > 0) {
      p->skipped--;
    }
    break;
  default:
    break;
  }
}

static void bdfi_player_use(bdfi_player *p, const bdf_instr *in, uint8_t op) {
  uint64_t i = bdf_instr_u64(in, 0);
  if (i >= bdf_object_object_count(p->obj->obj)) {
    return;
  }
  const uint8_t *hash = bdf_object_object(p->obj->obj, (size_t)i);
  bdfi_robj *child = bdfi_r_object(p->r, hash);
  if (!child) {
    return;
  }
  /*
   * USE is wrapped in a save and a restore of its own. Here an object is
   * also closed in itself: the groups and the soft mask it leaves open end
   * with it, and it ends no group it did not begin. (The rasterizer lets
   * both reach across objects, which no writer makes use of; a canvas that
   * nests its layers with its saved states could not follow.)
   */
  bdfi_pstate saved = p->st;
  bdfi_pstate_retain(&saved);
  size_t depth = p->nstack, floor = p->layer_floor;
  int64_t unsaved = p->unsaved, skipped = p->skipped;
  int again = p->again;
  int seen = bdfi_hash_set_add(&p->seen, hash);
  if (seen < 0) {
    p->r->oom = 1;
    seen = 0;
  }
  p->again = seen || again;
  p->layer_floor = p->nlayers;
  p->c->save(p->c->ctx);
  if (op == BDF_OP_USE_AT) {
    double x = (double)bdf_instr_f32(in, 1), y = (double)bdf_instr_f32(in, 2);
    const double t[6] = {1, 0, 0, 1, x, y};
    p->st.m = bdfi_mat_translate(p->st.m, x, y);
    p->c->transform(p->c->ctx, t);
  }
  bdfi_player_run(p, child);
  p->again = again;
  if (p->mask) {
    bdfi_player_mask_end(p);
  }
  while (p->nlayers > p->layer_floor) {
    bdfi_player_group_end(p);
  }
  p->layer_floor = floor;
  p->skipped = skipped;
  while (p->nstack > depth) {
    bdfi_pstate_release(&p->st);
    p->st = p->stack[--p->nstack];
    p->c->restore(p->c->ctx);
  }
  p->unsaved = unsaved;
  bdfi_pstate_release(&p->st);
  p->st = saved;
  p->c->restore(p->c->ctx);
}

static void bdfi_player_instr(bdfi_player *p, const bdf_instr *in) {
  uint8_t op = bdf_instr_op(in);
  /* inside a soft mask the rasterizer draws: the mask is its drawing */
  if (p->mask) {
    if (op == BDF_OP_MASK_BEGIN) {
      p->mask_depth++;
    } else if (op == BDF_OP_MASK_END) {
      if (p->mask_depth == 0) {
        bdfi_player_mask_end(p);
        return;
      }
      p->mask_depth--;
    }
    bdfi_drawer_instr(p->mask, p->obj, in);
    return;
  }
  if (p->again) {
    if (p->reused >= BDFI_MAX_REUSED_DRAWN) {
      bdfi_r_warn(p->r, "objects are drawn again too many times: the rest of them is not drawn");
      bdfi_player_left_out(p, op);
      return;
    }
    p->reused++;
  }
  if (p->skipped > 0) {
    bdfi_player_left_out(p, op);
    return;
  }
  bdfi_pstate *st = &p->st;
  const bdf_canvas *c = p->c;
#define F(i) ((double)bdf_instr_f32(in, (i)))
#define U(i) bdf_instr_u64(in, (i))
  switch (op) {
  case BDF_OP_SAVE:
    if (p->nstack - (p->nlayers ? p->layers[p->nlayers - 1].base : 0) >= BDFI_MAX_STATES) {
      bdfi_r_warnf(p->r, "more than %d states are saved at a time: the rest are not kept",
                   BDFI_MAX_STATES);
      p->unsaved++;
      return;
    }
    if (p->nstack == p->cap_stack) {
      size_t cap = p->cap_stack ? 2 * p->cap_stack : 8;
      bdfi_pstate *stack = bdfi_realloc_array(p->stack, cap, sizeof *stack);
      if (!stack) {
        p->r->oom = 1;
        p->unsaved++;
        return;
      }
      p->stack = stack;
      p->cap_stack = cap;
    }
    p->stack[p->nstack++] = *st;
    bdfi_pstate_retain(st);
    c->save(c->ctx);
    break;
  case BDF_OP_RESTORE:
    bdfi_player_restore(p);
    break;
  case BDF_OP_TRANSFORM: {
    bdfi_mat n = {{F(0), F(1), F(2), F(3), F(4), F(5)}};
    st->m = bdfi_mat_mul2(st->m, n);
    c->transform(c->ctx, n.v);
    break;
  }
  case BDF_OP_TRANSLATE: {
    const double n[6] = {1, 0, 0, 1, F(0), F(1)};
    st->m = bdfi_mat_translate(st->m, n[4], n[5]);
    c->transform(c->ctx, n);
    break;
  }
  case BDF_OP_SCALE: {
    const double n[6] = {F(0), 0, 0, F(1), 0, 0};
    st->m = bdfi_mat_scale(st->m, n[0], n[3]);
    c->transform(c->ctx, n);
    break;
  }
  case BDF_OP_CLIP_PATH: {
    bdfi_rpath *path = bdfi_player_object_path(p, U(0));
    if (path) {
      bdfi_player_path_reset(p);
      bdfi_player_add_path(p, path, NULL);
      /* a path of nothing clips everything away, as an empty one does */
      bdf_canvas_path cp = bdfi_player_path(p);
      c->clip(c->ctx, &cp, U(1) == BDF_FILL_EVENODD ? BDF_FILL_EVENODD : BDF_FILL_NONZERO);
    }
    break;
  }
  case BDF_OP_CLIP_RECT: {
    bdfi_player_path_reset(p);
    bdfi_player_rect(p, F(0), F(1), F(2), F(3));
    bdf_canvas_path cp = bdfi_player_path(p);
    c->clip(c->ctx, &cp, BDF_FILL_NONZERO);
    break;
  }
  case BDF_OP_FILL_COLOR:
    memset(&st->fill, 0, sizeof st->fill);
    st->fill.color = (uint32_t)U(0);
    break;
  case BDF_OP_STROKE_COLOR:
    memset(&st->stroke, 0, sizeof st->stroke);
    st->stroke.color = (uint32_t)U(0);
    break;
  case BDF_OP_FILL_PAINT:
    st->fill = bdfi_player_paint_style(p, U(0));
    break;
  case BDF_OP_STROKE_PAINT:
    st->stroke = bdfi_player_paint_style(p, U(0));
    break;
  case BDF_OP_LINE: {
    double w = F(0), miter = F(3);
    if (w > 0 && !bdfi_isinf(w)) {
      st->line.width = w;
    }
    st->line.cap = (uint8_t)U(1);
    st->line.join = (uint8_t)U(2);
    if (miter > 0 && !bdfi_isinf(miter)) {
      st->line.miter = miter;
    }
    break;
  }
  case BDF_OP_DASH: {
    size_t n;
    const float *segs = bdf_instr_f32s(in, 0, &n);
    if (bdfi_line_set_dash(&st->line, segs, n, bdf_instr_f32(in, 1)) < 0) {
      p->r->oom = 1;
    }
    break;
  }
  case BDF_OP_ALPHA: {
    double a = F(0);
    if (a >= 0 && a <= 1) {
      st->alpha = (float)a;
    }
    break;
  }
  case BDF_OP_BLEND: {
    uint64_t b = U(0);
    if (b <= BDF_BLEND_LIGHTER) {
      st->blend = (uint8_t)b;
    }
    break;
  }
  case BDF_OP_SHADOW: {
    /* in units of the page: the lengths of the instruction are in the user
     * space it is given in */
    double s = bdfi_mat_area_scale(&st->m);
    if (s == 0 || bdfi_isnan(s)) {
      s = 1;
    }
    st->shadow = (uint32_t)U(0);
    double v = F(1) * s;
    if (!bdfi_isnan(v) && !bdfi_isinf(v)) {
      st->shadow_blur = bdfi_fmax(0, v);
    }
    v = F(2) * s;
    if (!bdfi_isnan(v) && !bdfi_isinf(v)) {
      st->shadow_dx = bdfi_clampf(v);
    }
    v = F(3) * s;
    if (!bdfi_isnan(v) && !bdfi_isinf(v)) {
      st->shadow_dy = bdfi_clampf(v);
    }
    break;
  }
  case BDF_OP_FILTER: {
    size_t n;
    const char *s = bdf_instr_string(in, 0, &n);
    if (n != 0 && !(n == 4 && memcmp(s, "none", 4) == 0)) {
      bdfi_r_warnf(p->r, "FILTER \"%.*s\" is not applied", (int)(n > 200 ? 200 : n), s);
    }
    break;
  }
  case BDF_OP_FONT: {
    uint64_t i = U(0);
    if (i < bdf_object_font_count(p->obj->obj)) {
      st->font_owner = p->obj;
      st->font_index = (size_t)i;
    }
    st->size = F(1);
    break;
  }
  case BDF_OP_TEXT_STYLE:
    st->align = (uint8_t)U(0);
    st->baseline = (uint8_t)U(1);
    st->dir = (uint8_t)U(2);
    st->spacing = F(3);
    break;
  case BDF_OP_FILL_RECT:
    bdfi_player_path_reset(p);
    bdfi_player_rect(p, F(0), F(1), F(2), F(3));
    bdfi_player_fill(p, BDF_FILL_NONZERO);
    break;
  case BDF_OP_STROKE_RECT:
    bdfi_player_path_reset(p);
    bdfi_player_rect(p, F(0), F(1), F(2), F(3));
    bdfi_player_stroke(p);
    break;
  case BDF_OP_CLEAR_RECT: {
    /* what is there is taken away: black, out of the destination */
    bdf_canvas_style style;
    memset(&style, 0, sizeof style);
    style.color = 0x000000ff;
    style.alpha = 1;
    style.blend = BDF_BLEND_DESTINATION_OUT;
    bdfi_player_path_reset(p);
    bdfi_player_rect(p, F(0), F(1), F(2), F(3));
    if (p->nverbs > 0) {
      bdf_canvas_path path = bdfi_player_path(p);
      c->fill(c->ctx, &path, BDF_FILL_NONZERO, &style);
    }
    break;
  }
  case BDF_OP_FILL_PATH:
  case BDF_OP_STROKE_PATH: {
    bdfi_rpath *path = bdfi_player_object_path(p, U(0));
    if (path) {
      bdfi_player_path_reset(p);
      bdfi_player_add_path(p, path, NULL);
      if (op == BDF_OP_FILL_PATH) {
        bdfi_player_fill(p, (uint8_t)U(1));
      } else {
        bdfi_player_stroke(p);
      }
    }
    break;
  }
  case BDF_OP_FILL_PATH_AT: {
    bdfi_rpath *path = bdfi_player_object_path(p, U(0));
    if (path) {
      /* the paint moves with the path: its user space is moved */
      const double t[6] = {1, 0, 0, 1, F(2), F(3)};
      bdfi_player_path_reset(p);
      bdfi_player_add_path(p, path, NULL);
      c->save(c->ctx);
      c->transform(c->ctx, t);
      bdfi_player_fill(p, (uint8_t)U(1));
      c->restore(c->ctx);
    }
    break;
  }
  case BDF_OP_FILL_PATH_RUN: {
    /* the paths at their places, as one fill in the user space */
    size_t n;
    const bdf_glyph *glyphs = bdf_instr_glyphs(in, 1, &n);
    bdfi_player_path_reset(p);
    for (size_t g = 0; g < n && !p->r->oom; g++) {
      bdfi_rpath *path = bdfi_player_object_path(p, glyphs[g].path);
      if (path) {
        bdfi_mat at = {{1, 0, 0, 1, (double)glyphs[g].x, (double)glyphs[g].y}};
        bdfi_player_add_path(p, path, &at);
      }
    }
    bdfi_player_fill(p, (uint8_t)U(0));
    break;
  }
  case BDF_OP_FILL_TEXT:
  case BDF_OP_STROKE_TEXT: {
    size_t n = 0;
    const char *s = bdf_instr_string(in, 0, &n);
    bdfi_player_text(p, s, n, F(1), F(2), F(3), op == BDF_OP_STROKE_TEXT);
    break;
  }
  case BDF_OP_IMAGE:
    bdfi_player_image(p, U(0), NULL, F(1), F(2), F(3), F(4));
    break;
  case BDF_OP_IMAGE_SUB: {
    const double src[4] = {F(1), F(2), F(3), F(4)};
    bdfi_player_image(p, U(0), src, F(5), F(6), F(7), F(8));
    break;
  }
  case BDF_OP_SMOOTHING:
    st->smooth = U(0) != 0;
    break;
  case BDF_OP_USE:
  case BDF_OP_USE_AT:
    bdfi_player_use(p, in, op);
    break;
  case BDF_OP_GROUP_BEGIN:
    bdfi_player_group_begin(p, bdf_instr_f32(in, 0), (uint8_t)U(1), F(2), F(3), F(4), F(5));
    break;
  case BDF_OP_GROUP_END:
    bdfi_player_group_end(p);
    break;
  case BDF_OP_MASK_BEGIN: {
    size_t n;
    const uint8_t *transfer = bdf_instr_bytes(in, 2, &n);
    bdfi_player_mask_begin(p, (uint8_t)U(0), (uint32_t)U(1), transfer, n);
    break;
  }
  case BDF_OP_MASK_END:
    break; /* one that ends no mask */
  default:
    break; /* LINK, MARK and EXT draw nothing */
  }
#undef F
#undef U
}

static void bdfi_player_run(bdfi_player *p, bdfi_robj *o) {
  if (p->depth > BDF_MAX_USE_DEPTH) {
    bdfi_r_warn(p->r, "objects nested too deep");
    return;
  }
  p->depth++;
  bdfi_robj *prev = p->obj;
  p->obj = o;
  bdf_instr it;
  bdfi_instr_init(&it, o->obj);
  for (;;) {
    bdf_error e;
    int rc = bdf_instr_next(&it, &e);
    if (rc == 0) {
      break;
    }
    if (rc < 0) {
      if (e.status == BDF_ERR_NOMEM) {
        p->r->oom = 1;
      } else {
        bdfi_r_warnf(p->r, "object: %s", e.message);
      }
      break;
    }
    bdfi_player_instr(p, &it);
    if (p->r->oom) {
      break;
    }
  }
  bdfi_instr_clear(&it);
  p->obj = prev;
  p->depth--;
}

/* An object drawn from the state of a new canvas; what it leaves open is
 * closed, so that the canvas is as it was. */
static void bdfi_player_top(bdfi_player *p, bdfi_robj *o, const uint8_t *hash) {
  if (!o) {
    return;
  }
  bdfi_mat identity = bdfi_mat_identity();
  bdfi_pstate_init(&p->st, &identity);
  p->unsaved = 0;
  int seen = bdfi_hash_set_add(&p->seen, hash);
  if (seen < 0) {
    p->r->oom = 1;
    seen = 0;
  }
  p->again = seen;
  p->c->save(p->c->ctx);
  p->layer_floor = 0;
  bdfi_player_run(p, o);
  if (p->mask) {
    bdfi_player_mask_end(p);
  }
  while (p->nlayers > 0) {
    bdfi_player_group_end(p);
  }
  while (p->nstack > 0) {
    bdfi_pstate_release(&p->st);
    p->st = p->stack[--p->nstack];
    p->c->restore(p->c->ctx);
  }
  bdfi_pstate_release(&p->st);
  p->skipped = 0;
  p->c->restore(p->c->ctx);
}

static int bdfi_player_begin(bdfi_player *p, bdf_renderer *r, double scale, const bdf_canvas *c,
                             bdf_error *err) {
  bdfi_error_clear(err);
  if (!r || !c || !c->save || !c->restore || !c->transform || !c->clip || !c->fill || !c->stroke ||
      !c->image || !c->begin_layer || !c->end_layer) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return 0;
  }
  memset(p, 0, sizeof *p);
  p->r = r;
  p->c = c;
  p->scale = scale > 0 && !bdfi_isinf(scale) ? scale : 1;
  r->oom = 0;
  r->drawing++;
  return 1;
}

static bdf_status bdfi_player_end(bdfi_player *p, bdf_error *err) {
  bdf_status st = p->r->oom ? BDF_ERR_NOMEM : BDF_OK;
  free(p->stack);
  free(p->layers);
  free(p->verbs);
  free(p->coords);
  bdfi_hash_set_free(&p->seen);
  p->r->oom = 0;
  if (st != BDF_OK) {
    bdfi_error_set(err, st, 0, "out of memory");
  }
  return st;
}

bdf_status bdf_play_page(bdf_renderer *r, size_t view, size_t page, double scale,
                         const bdf_canvas *canvas, bdf_error *err) {
  bdfi_player p;
  if (!bdfi_player_begin(&p, r, scale, canvas, err)) {
    return BDF_ERR_ARG;
  }
  bdf_page pg;
  if (!r->doc || bdf_doc_page(r->doc, view, page, &pg) != BDF_OK) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "no such page");
    return BDF_ERR_ARG;
  }
  p.page = bdfi_irect_of(0, 0, (int)ceil(bdfi_clampf((double)pg.w * p.scale)),
                         (int)ceil(bdfi_clampf((double)pg.h * p.scale)));
  if (bdfi_irect_empty(p.page)) {
    p.page = bdfi_irect_of(0, 0, 1, 1);
  }
  canvas->save(canvas->ctx);
  bdfi_player_path_reset(&p);
  bdfi_player_rect(&p, 0, 0, (double)pg.w, (double)pg.h);
  bdf_canvas_path clip = bdfi_player_path(&p);
  canvas->clip(canvas->ctx, &clip, BDF_FILL_NONZERO);
  for (size_t l = 0; l < pg.nlayers && !r->oom; l++) {
    bdf_layer layer;
    if (bdf_doc_layer(r->doc, view, page, l, &layer) != BDF_OK) {
      continue;
    }
    bdfi_player_top(&p, bdfi_r_object(r, layer.obj), layer.obj);
  }
  canvas->restore(canvas->ctx);
  return bdfi_player_end(&p, err);
}

bdf_status bdf_play_part(bdf_renderer *r, const uint8_t hash[16], double scale,
                         const bdf_canvas *canvas, bdf_error *err) {
  bdfi_player p;
  if (!bdfi_player_begin(&p, r, scale, canvas, err)) {
    return BDF_ERR_ARG;
  }
  if (!hash || !r->doc) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  bdfi_robj *o = bdfi_r_object(r, hash);
  if (!o) {
    bdf_status st = r->oom ? BDF_ERR_NOMEM : BDF_ERR_ARG;
    bdfi_player_end(&p, err);
    bdfi_error_set(err, st, 0, st == BDF_ERR_NOMEM ? "out of memory" : "no such object");
    return st;
  }
  /* the pixels a soft mask may take: those of the object's bounding box */
  float box[4] = {0, 0, 0, 0};
  bdf_object_bbox(o->obj, box);
  p.page = bdfi_irect_of((int)floor(bdfi_clampf((double)box[0] * p.scale)),
                         (int)floor(bdfi_clampf((double)box[1] * p.scale)),
                         (int)ceil(bdfi_clampf((double)(box[0] + box[2]) * p.scale)),
                         (int)ceil(bdfi_clampf((double)(box[1] + box[3]) * p.scale)));
  if (bdfi_irect_empty(p.page)) {
    p.page = bdfi_irect_of(0, 0, 1, 1);
  }
  bdfi_player_top(&p, o, hash);
  return bdfi_player_end(&p, err);
}
