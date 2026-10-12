/*
 * The drawer of the rasterizer: it runs the instructions of objects on a
 * canvas, with the state of Canvas 2D, groups, soft masks and shadows. A
 * port of raster/imagebdf/draw.go of the reference implementation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

static const char bdfi_too_much_memory[] =
    "the groups and clips of a page take too much memory: what is in the rest of them is not drawn";
static const char bdfi_stroke_cut[] =
    "a line has too many dashes, joins and caps: the rest of it is not drawn";

/* ---- States -------------------------------------------------------------- */

/* The initial state of Canvas 2D with a transform and a clip (which gets a
 * reference). */
static void bdfi_state_init(bdfi_state *st, const bdfi_mat *m, bdfi_mask *clip) {
  memset(st, 0, sizeof *st);
  st->m = *m;
  st->clip = bdfi_mask_ref(clip ? clip : bdfi_mask_empty());
  st->fill.color = 0x000000ff;
  st->stroke.color = 0x000000ff;
  st->line.width = 1;
  st->line.miter = 10;
  st->alpha = 1;
  st->size = 10;
  st->smooth = 1;
}

/* A copy of a state holds its clip and its dashes as well. */
static void bdfi_state_retain(bdfi_state *st) {
  bdfi_mask_ref(st->clip);
  if (st->line.dash) {
    st->line.dash->refs++;
  }
}

static void bdfi_state_release(bdfi_state *st) {
  bdfi_mask_unref(st->clip);
  bdfi_dash_unref(st->line.dash);
  st->clip = NULL;
  st->line.dash = NULL;
}

static void bdfi_state_set_clip(bdfi_state *st, bdfi_mask *clip) {
  bdfi_mask_unref(st->clip);
  st->clip = clip;
}

static int bdfi_shadow_active(const bdfi_state *s) {
  return (s->shadow & 0xff) != 0 && (s->shadow_blur > 0 || s->shadow_dx != 0 || s->shadow_dy != 0);
}

/* ---- Contexts ------------------------------------------------------------ */

static bdfi_ctx *bdfi_ctx_new(void) { return calloc(1, sizeof(bdfi_ctx)); }

/*
 * A canvas is looked at by what draws on it (the drawer's current one), by
 * the groups and soft masks begun on it, which come back to it when they
 * end, and by the USEs being drawn on it: each pins it. A well-formed
 * stream ends a canvas when nothing else looks at it. A malformed one can
 * end a soft mask with a group still open inside it, which comes back to the
 * canvas of the mask later, and draws on it: the canvas stays, with its
 * state, until nothing looks at it, and what is drawn on it by then shows
 * nowhere.
 */
static void bdfi_ctx_destroy(bdfi_drawer *d, bdfi_ctx *ctx) {
  bdfi_state_release(&ctx->st);
  for (size_t i = 0; i < ctx->nstack; i++) {
    bdfi_state_release(&ctx->stack[i]);
  }
  free(ctx->stack);
  if (ctx->target) {
    bdfi_r_release(d->r, ctx->target);
  }
  free(ctx);
}

static void bdfi_ctx_unpin(bdfi_drawer *d, bdfi_ctx *ctx) {
  if (--ctx->pins == 0 && ctx->done) {
    bdfi_ctx_destroy(d, ctx);
  }
}

/* Ends a canvas: it goes now, or when nothing looks at it any more. */
static void bdfi_ctx_close(bdfi_drawer *d, bdfi_ctx *ctx) {
  ctx->done = 1;
  if (ctx->pins == 0) {
    bdfi_ctx_destroy(d, ctx);
  }
}

/* Makes a canvas the one that is drawn on. */
static void bdfi_set_ctx(bdfi_drawer *d, bdfi_ctx *ctx) {
  bdfi_ctx *old = d->ctx;
  ctx->pins++;
  d->ctx = ctx;
  if (old) {
    bdfi_ctx_unpin(d, old);
  }
}

/* Counts n bytes of a canvas or a clip of ctx in the memory in use, and
 * reports whether there is room for them. */
static int bdfi_hold(bdfi_drawer *d, bdfi_ctx *ctx, int64_t n) {
  if (n > d->limit - d->live) {
    return 0;
  }
  ctx->held += n;
  d->live += n;
  return 1;
}

/* Lets go of the clip a state made. */
static void bdfi_drop(bdfi_drawer *d, bdfi_ctx *ctx, bdfi_state *st) {
  if (st->made) {
    int64_t n = (int64_t)bdfi_mask_bytes(st->made);
    ctx->held -= n;
    d->live -= n;
    st->made = NULL;
  }
}

/* What a canvas that is done with held is let go of. */
static void bdfi_closed(bdfi_drawer *d, bdfi_ctx *ctx) {
  d->live -= ctx->held;
  ctx->held = 0;
}

/* The area drawing can change: the target within the clip. */
static bdfi_irect bdfi_draw_bounds(const bdfi_drawer *d) {
  return bdfi_irect_intersect(d->ctx->st.clip->r, bdfi_surface_bounds(d->ctx->target));
}

/* ---- Shaders ------------------------------------------------------------- */

typedef union bdfi_any_shader {
  bdfi_shader base;
  bdfi_solid solid;
  bdfi_gradient_shader gradient;
} bdfi_any_shader;

/* The shader of a fill or stroke style drawn under m. */
static void bdfi_draw_shader(bdfi_drawer *d, const bdfi_style *s, const bdfi_mat *m,
                             bdfi_any_shader *out) {
  if (!s->has_paint) {
    out->solid = bdfi_premul(s->color);
    return;
  }
  out->solid = bdfi_solid_of(0, 0, 0, 0);
  if (s->paint.kind == BDF_PAINT_PATTERN) {
    return; /* images are not drawn yet: nothing */
  }
  const bdfi_lut *lut = bdfi_r_lut(d->r, s->owner, s->index, &s->paint);
  bdfi_gradient_shader g;
  if (bdfi_gradient_shader_init(&g, &s->paint, m, lut)) {
    out->gradient = g;
  }
}

static bdfi_style bdfi_paint_style(bdfi_drawer *d, uint64_t i) {
  bdfi_style s;
  memset(&s, 0, sizeof s);
  if (i >= bdf_object_paint_count(d->obj->obj) ||
      bdf_object_paint(d->obj->obj, (size_t)i, &s.paint) != BDF_OK) {
    bdfi_r_warn(d->r, "bad paint reference");
    return s;
  }
  s.has_paint = 1;
  s.owner = d->obj;
  s.index = (size_t)i;
  return s;
}

/* ---- Coverage and painting ----------------------------------------------- */

static bdfi_rpath *bdfi_draw_path(bdfi_drawer *d, uint64_t i) {
  if (i >= bdf_object_path_count(d->obj->obj)) {
    bdfi_r_warn(d->r, "bad path reference");
    return NULL;
  }
  return bdfi_r_path(d->r, d->obj, (size_t)i);
}

/* What a mask the rasterizer could not make leaves: nothing, and the
 * drawing is over (out of memory). */
static bdfi_mask *bdfi_draw_made(bdfi_drawer *d, bdfi_mask *m) {
  if (!m) {
    d->r->oom = 1;
    return bdfi_mask_empty();
  }
  return m;
}

/* The coverage of a path in user space under a transform. */
static bdfi_mask *bdfi_draw_coverage(bdfi_drawer *d, bdfi_rpath *p, uint8_t rule,
                                     const bdfi_mat *m) {
  if (!bdfi_mat_finite(m)) {
    return bdfi_mask_empty();
  }
  bdfi_irect bounds = bdfi_draw_bounds(d);
  if (bdfi_hidden(p, 0, m, bounds)) {
    return bdfi_mask_empty();
  }
  /* an axis-aligned rectangle: exact coverage without scanning */
  if (p->nsubs >= 1 && m->v[1] == 0 && m->v[2] == 0) {
    double x0, y0, x1, y1;
    if (bdfi_rpath_is_rect(p, &x0, &y0, &x1, &y1)) {
      bdfi_pt a = bdfi_mat_pt(m, x0, y0), b = bdfi_mat_pt(m, x1, y1);
      return bdfi_draw_made(d, bdfi_rect_mask(a.x, a.y, b.x, b.y, bounds));
    }
  }
  bdfi_scratch *sc = bdfi_r_scratch(d->r);
  bdfi_rpath_flatten(p, sc, m, BDFI_FLAT_TOL);
  bdfi_mask *cov = bdfi_rasterize(sc, sc->pts, sc->polys, sc->npolys, rule, bounds);
  if (sc->oom) {
    d->r->oom = 1;
  }
  return bdfi_draw_made(d, cov);
}

/* An approximate Gaussian blur of a plane: three box blurs (Kovesi). */
static void bdfi_box_h(const float *src, float *dst, int w, int h, int r) {
  float k = 1 / (float)(2 * r + 1);
  for (int y = 0; y < h; y++) {
    const float *row = src + (size_t)y * (size_t)w;
    float *out = dst + (size_t)y * (size_t)w;
    float sum = 0;
    for (int x = -r; x <= r; x++) {
      if (x >= 0 && x < w) {
        sum += row[x];
      }
    }
    for (int x = 0; x < w; x++) {
      out[x] = sum * k;
      if (x + r + 1 < w) {
        sum += row[x + r + 1];
      }
      if (x - r >= 0) {
        sum -= row[x - r];
      }
    }
  }
}

static void bdfi_box_v(const float *src, float *dst, int w, int h, int r) {
  float k = 1 / (float)(2 * r + 1);
  for (int x = 0; x < w; x++) {
    float sum = 0;
    for (int y = -r; y <= r; y++) {
      if (y >= 0 && y < h) {
        sum += src[(size_t)y * (size_t)w + (size_t)x];
      }
    }
    for (int y = 0; y < h; y++) {
      dst[(size_t)y * (size_t)w + (size_t)x] = sum * k;
      if (y + r + 1 < h) {
        sum += src[(size_t)(y + r + 1) * (size_t)w + (size_t)x];
      }
      if (y - r >= 0) {
        sum -= src[(size_t)(y - r) * (size_t)w + (size_t)x];
      }
    }
  }
}

static int bdfi_blur(float *a, int w, int h, double sigma) {
  double w_ideal = sqrt(12 * sigma * sigma / 3 + 1);
  int wl = (int)floor(w_ideal);
  if (wl % 2 == 0) {
    wl--;
  }
  int wu = wl + 2;
  double m_ideal = (12 * sigma * sigma - 3 * (double)(wl * wl) - 12 * (double)wl - 9) /
                   (-4 * (double)wl - 4);
  int mm = (int)round(m_ideal);
  float *tmp = bdfi_malloc_array((size_t)w * (size_t)h, sizeof *tmp);
  if (!tmp) {
    return -1;
  }
  for (int i = 0; i < 3; i++) {
    int size = i >= mm ? wu : wl;
    int r = (size - 1) / 2;
    if (r <= 0) {
      continue;
    }
    bdfi_box_h(a, tmp, w, h, r);
    bdfi_box_v(tmp, a, w, h, r);
  }
  free(tmp);
  return 0;
}

static void bdfi_draw_fill(bdfi_drawer *d, const bdfi_mask *cov, const bdfi_shader *sh, float alpha,
                           uint8_t mode, const bdfi_mask *clip) {
  if (bdfi_surface_fill(d->ctx->target, cov, sh, alpha, mode, clip) < 0) {
    d->r->oom = 1;
  }
}

/* The shadow of what a draw covers: its alpha, blurred and offset, in the
 * colour of the shadow. */
static void bdfi_draw_shadow(bdfi_drawer *d, const bdfi_mask *cov, const bdfi_shader *sh,
                             float alpha) {
  bdfi_state *st = &d->ctx->st;
  double sigma = st->shadow_blur / 2;
  int pad = (int)ceil(3 * sigma);
  bdfi_irect r = cov->r;
  int w = BDFI_DX(r), h = BDFI_DY(r);
  int dx = (int)round(st->shadow_dx), dy = (int)round(st->shadow_dy);
  bdfi_irect where = bdfi_irect_of(r.x0 - pad + dx, r.y0 - pad + dy, r.x1 + pad + dx, r.y1 + pad + dy);
  bdfi_solid colour = bdfi_premul(st->shadow);
  if (!bdfi_irect_overlaps(where, bdfi_draw_bounds(d))) {
    /* the shadow falls outside the canvas: nothing of it to blur */
    if (bdfi_unbounded(st->blend)) {
      bdfi_draw_fill(d, bdfi_mask_empty(), &colour.base, 1, st->blend, st->clip);
    }
    return;
  }
  int stride = w + 2 * pad;
  float *buf = bdfi_malloc_array(4 * (size_t)w, sizeof *buf);
  bdfi_mask sm;
  sm.r = where;
  sm.refs = 0;
  sm.a = bdfi_calloc_array((size_t)stride * (size_t)(h + 2 * pad), sizeof *sm.a);
  if (!buf || !sm.a) {
    free(buf);
    free(sm.a);
    d->r->oom = 1;
    return;
  }
  for (int y = r.y0; y < r.y1; y++) {
    sh->shade(sh, r.x0, y, w, buf);
    const float *row = bdfi_mask_row(cov, y, r.x0);
    float *out = sm.a + (size_t)(y - r.y0 + pad) * (size_t)stride + (size_t)pad;
    for (int x = 0; x < w; x++) {
      float c = row ? row[x] : 1;
      out[x] = buf[4 * x + 3] * c * alpha;
    }
  }
  free(buf);
  if (sigma > 0 && bdfi_blur(sm.a, stride, h + 2 * pad, sigma) < 0) {
    free(sm.a);
    d->r->oom = 1;
    return;
  }
  bdfi_draw_fill(d, &sm, &colour.base, 1, st->blend, st->clip);
  free(sm.a);
}

/* Composites a shader through a coverage with the alpha, the composite
 * operation, the clip and the shadow of the state. The coverage is the
 * caller's still. */
static void bdfi_draw_paint(bdfi_drawer *d, const bdfi_mask *cov, const bdfi_shader *sh,
                            float alpha) {
  bdfi_state *st = &d->ctx->st;
  if (bdfi_mask_is_empty(cov) && !bdfi_unbounded(st->blend)) {
    return;
  }
  if (bdfi_shadow_active(st) && !bdfi_mask_is_empty(cov)) {
    bdfi_draw_shadow(d, cov, sh, alpha * st->alpha);
  }
  bdfi_draw_fill(d, cov, sh, alpha * st->alpha, st->blend, st->clip);
}

static void bdfi_draw_fill_path(bdfi_drawer *d, bdfi_rpath *p, uint8_t rule, const bdfi_mat *m) {
  bdfi_mask *cov = bdfi_draw_coverage(d, p, rule, m);
  bdfi_any_shader sh;
  bdfi_draw_shader(d, &d->ctx->st.fill, m, &sh);
  bdfi_draw_paint(d, cov, &sh.base, 1);
  bdfi_mask_unref(cov);
}

/* Strokes a path in user space under m. Strokes thinner than a device pixel
 * are drawn a pixel wide with their width as alpha, as Skia draws hairlines. */
static void bdfi_draw_stroke_path(bdfi_drawer *d, bdfi_rpath *p, const bdfi_mat *m) {
  float alpha;
  int cut;
  bdfi_scratch *sc = bdfi_r_scratch(d->r);
  bdfi_mask *cov =
      bdfi_stroke_mask(sc, p, m, d->ctx->st.line, bdfi_draw_bounds(d), &alpha, &cut);
  if (sc->oom) {
    d->r->oom = 1;
  }
  cov = bdfi_draw_made(d, cov);
  if (cut) {
    bdfi_r_warn(d->r, bdfi_stroke_cut);
  }
  bdfi_any_shader sh;
  bdfi_draw_shader(d, &d->ctx->st.stroke, m, &sh);
  bdfi_draw_paint(d, cov, &sh.base, alpha);
  bdfi_mask_unref(cov);
}

/* Cuts the clip of the state to a coverage, which it takes over. */
static void bdfi_draw_clip(bdfi_drawer *d, bdfi_mask *cov) {
  bdfi_ctx *ctx = d->ctx;
  bdfi_state *st = &ctx->st;
  bdfi_mask *old = st->clip;
  if (!bdfi_mask_is_empty(old) && !bdfi_mask_is_empty(cov) && (old->a || cov->a)) {
    bdfi_irect r = bdfi_irect_intersect(old->r, cov->r);
    if (bdfi_irect_empty(r)) {
      bdfi_drop(d, ctx, st);
      bdfi_state_set_clip(st, bdfi_mask_empty());
      bdfi_mask_unref(cov);
      return;
    }
    if (!cov->a && bdfi_irect_eq(r, old->r)) {
      /* the clip lies within a rectangle: it stays as it is, with the
       * memory the state holds for it */
      bdfi_mask_unref(cov);
      return;
    }
    if (!old->a && bdfi_irect_eq(r, cov->r)) {
      /* the coverage lies within a rectangle: it is the product, and its
       * memory is the state's (it was made for this) */
      bdfi_drop(d, ctx, st);
      if (!bdfi_hold(d, ctx, (int64_t)bdfi_mask_bytes(cov))) {
        bdfi_r_warn(d->r, bdfi_too_much_memory);
        bdfi_state_set_clip(st, bdfi_mask_empty());
        bdfi_mask_unref(cov);
        return;
      }
      bdfi_state_set_clip(st, cov);
      st->made = cov;
      return;
    }
    /* their product takes memory */
    bdfi_drop(d, ctx, st);
    if (!bdfi_hold(d, ctx, 4 * (int64_t)BDFI_DX(r) * (int64_t)BDFI_DY(r))) {
      bdfi_r_warn(d->r, bdfi_too_much_memory);
      bdfi_state_set_clip(st, bdfi_mask_empty());
      bdfi_mask_unref(cov);
      return;
    }
    bdfi_mask *both = bdfi_draw_made(d, bdfi_mask_intersect(old, cov));
    bdfi_state_set_clip(st, both);
    st->made = both->a ? both : NULL;
    bdfi_mask_unref(cov);
    return;
  }
  bdfi_drop(d, ctx, st);
  bdfi_mask *both = bdfi_draw_made(d, bdfi_mask_intersect(old, cov));
  bdfi_state_set_clip(st, both);
  bdfi_mask_unref(cov);
}

/* ---- Groups and soft masks ----------------------------------------------- */

/* Whether there is room for the canvas of one more group or soft mask, of
 * w × h pixels, which it counts. One without room is left out with what is
 * in it. */
static int bdfi_draw_layer(bdfi_drawer *d, bdfi_ctx *ctx, int w, int h) {
  if (d->ngroups + d->nmasks >= BDFI_MAX_LAYERS) {
    bdfi_r_warnf(d->r,
                 "more than %d groups and soft masks are open: what is in the rest of them is not drawn",
                 BDFI_MAX_LAYERS);
  } else if (!bdfi_hold(d, ctx, 16 * (int64_t)w * (int64_t)h)) {
    bdfi_r_warn(d->r, bdfi_too_much_memory);
  } else {
    return 1;
  }
  d->skipped++;
  return 0;
}

static void bdfi_group_begin(bdfi_drawer *d, float alpha, uint8_t blend, double x, double y,
                             double w, double h) {
  bdfi_state *st = &d->ctx->st;
  const bdfi_mat *m = &st->m;
  const double cx[4] = {x, x + w, x, x + w}, cy[4] = {y, y, y + h, y + h};
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < 4; i++) {
    bdfi_pt p = bdfi_mat_pt(m, cx[i], cy[i]);
    x0 = bdfi_fmin(x0, p.x);
    y0 = bdfi_fmin(y0, p.y);
    x1 = bdfi_fmax(x1, p.x);
    y1 = bdfi_fmax(y1, p.y);
  }
  bdfi_irect r = bdfi_irect_of((int)floor(bdfi_clampf(x0)), (int)floor(bdfi_clampf(y0)),
                               (int)ceil(bdfi_clampf(x1)), (int)ceil(bdfi_clampf(y1)));
  /* nothing outside the parent canvas or its clip can show */
  r = bdfi_irect_intersect(r, bdfi_draw_bounds(d));
  if (bdfi_irect_empty(r)) {
    r = bdfi_irect_of(0, 0, 1, 1);
  }
  bdfi_ctx *gc = bdfi_ctx_new();
  if (!gc) {
    d->r->oom = 1;
    d->skipped++;
    return;
  }
  if (!bdfi_draw_layer(d, gc, BDFI_DX(r), BDFI_DY(r))) {
    free(gc);
    return;
  }
  gc->target = bdfi_r_new_surface(d->r, BDFI_DX(r), BDFI_DY(r));
  bdfi_mask *all = gc->target ? bdfi_mask_new(bdfi_surface_bounds(gc->target), 0) : NULL;
  if (d->ngroups == d->cap_groups) {
    size_t cap = d->cap_groups ? d->cap_groups * 2 : 8;
    bdfi_group *groups = bdfi_realloc_array(d->groups, cap, sizeof *groups);
    if (groups) {
      d->groups = groups;
      d->cap_groups = cap;
    }
  }
  if (!gc->target || !all || d->ngroups == d->cap_groups) {
    /* no memory for it: left out as one past the limits is */
    bdfi_mask_unref(all);
    if (gc->target) {
      bdfi_r_release(d->r, gc->target);
    }
    d->live -= gc->held;
    free(gc);
    d->r->oom = 1;
    d->skipped++;
    return;
  }
  bdfi_mat shift = {{1, 0, 0, 1, -(double)r.x0, -(double)r.y0}};
  bdfi_mat gm = bdfi_mat_mul2(shift, *m);
  bdfi_state_init(&gc->st, &gm, all);
  bdfi_mask_unref(all);
  gc->st.fill = st->fill;
  gc->st.stroke = st->stroke;
  gc->st.line.width = st->line.width;
  gc->st.font_owner = st->font_owner;
  gc->st.font_index = st->font_index;
  gc->st.size = st->size;
  bdfi_group *g = &d->groups[d->ngroups++];
  g->id = ++d->next_group;
  g->parent = d->ctx;
  g->parent->pins++;
  g->ctx = gc;
  g->x = r.x0;
  g->y = r.y0;
  g->alpha = alpha;
  g->blend = blend;
  bdfi_set_ctx(d, gc);
}

static void bdfi_group_end(bdfi_drawer *d) {
  if (d->ngroups == 0) {
    return;
  }
  bdfi_group g = d->groups[--d->ngroups];
  /* masks begun inside the group end with it */
  while (d->nmasks > 0 && d->masks[d->nmasks - 1].group == g.id) {
    bdfi_soft_mask m = d->masks[--d->nmasks];
    bdfi_closed(d, m.ctx);
    bdfi_ctx_close(d, m.ctx);
    bdfi_ctx_unpin(d, m.parent);
  }
  bdfi_closed(d, g.ctx);
  g.ctx->pins++; /* until it is composited */
  bdfi_set_ctx(d, g.parent);
  bdfi_ctx_unpin(d, g.parent); /* the group's own look at it; the drawer has one now */
  bdfi_surface *s = g.ctx->target;
  bdfi_mask whole;
  whole.r = bdfi_irect_of(g.x, g.y, g.x + s->w, g.y + s->h);
  whole.a = NULL;
  whole.refs = 0;
  uint8_t blend = g.blend;
  if (blend > BDF_BLEND_LIGHTER) {
    blend = BDF_BLEND_SOURCE_OVER;
  }
  bdfi_surface_shader ss = bdfi_surface_shader_of(s, g.x, g.y);
  bdfi_draw_fill(d, &whole, &ss.base, g.alpha, blend, d->ctx->st.clip);
  g.ctx->done = 1;
  bdfi_ctx_unpin(d, g.ctx);
}

static void bdfi_mask_begin(bdfi_drawer *d, uint8_t kind, uint32_t backdrop,
                            const uint8_t *transfer, size_t ntransfer) {
  uint64_t group = 0;
  int w = 1, h = 1;
  if (d->ngroups > 0) {
    const bdfi_group *g = &d->groups[d->ngroups - 1];
    group = g->id;
    w = g->ctx->target->w;
    h = g->ctx->target->h;
  }
  bdfi_ctx *mc = bdfi_ctx_new();
  if (!mc) {
    d->r->oom = 1;
    d->skipped++;
    return;
  }
  if (!bdfi_draw_layer(d, mc, w, h)) {
    free(mc);
    return;
  }
  mc->target = bdfi_r_new_surface(d->r, w, h);
  bdfi_mask *all = mc->target ? bdfi_mask_new(bdfi_surface_bounds(mc->target), 0) : NULL;
  if (d->nmasks == d->cap_masks) {
    size_t cap = d->cap_masks ? d->cap_masks * 2 : 8;
    bdfi_soft_mask *masks = bdfi_realloc_array(d->masks, cap, sizeof *masks);
    if (masks) {
      d->masks = masks;
      d->cap_masks = cap;
    }
  }
  if (!mc->target || !all || d->nmasks == d->cap_masks) {
    bdfi_mask_unref(all);
    if (mc->target) {
      bdfi_r_release(d->r, mc->target);
    }
    d->live -= mc->held;
    free(mc);
    d->r->oom = 1;
    d->skipped++;
    return;
  }
  if (kind == BDF_MASK_LUMINOSITY) {
    bdfi_solid c = bdfi_premul(backdrop | 0xff);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
      memcpy(mc->target->pix + 4 * i, c.c, sizeof c.c);
    }
  }
  bdfi_state_init(&mc->st, &d->ctx->st.m, all);
  bdfi_mask_unref(all);
  bdfi_soft_mask *m = &d->masks[d->nmasks++];
  m->parent = d->ctx;
  m->parent->pins++;
  m->ctx = mc;
  m->kind = kind;
  m->transfer = ntransfer == 256 ? transfer : NULL;
  m->group = group;
  bdfi_set_ctx(d, mc);
}

static void bdfi_mask_end(bdfi_drawer *d) {
  if (d->nmasks == 0) {
    return;
  }
  bdfi_soft_mask m = d->masks[--d->nmasks];
  bdfi_closed(d, m.ctx);
  m.ctx->pins++; /* until it is applied */
  bdfi_set_ctx(d, m.parent);
  bdfi_ctx_unpin(d, m.parent);
  const bdfi_surface *ms = m.ctx->target;
  if (m.group != 0 && d->ngroups > 0 && d->groups[d->ngroups - 1].id == m.group) {
    bdfi_surface *gs = d->groups[d->ngroups - 1].ctx->target;
    size_t n = (size_t)ms->w * (size_t)ms->h;
    for (size_t i = 0; i < n; i++) {
      const float *p = ms->pix + 4 * i;
      double level;
      if (m.kind == BDF_MASK_LUMINOSITY) {
        level = round((double)(0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]) * 255);
      } else {
        level = round((double)p[3] * 255);
      }
      /* what is no number, or none an int holds, is 0 in the reference */
      int v = level >= 0 && level <= 255 ? (int)level : level > 255 && level < 9.2e18 ? 255 : 0;
      if (m.transfer) {
        v = m.transfer[v];
      }
      float k = (float)v / 255;
      float *q = gs->pix + 4 * i;
      q[0] = q[0] * k;
      q[1] = q[1] * k;
      q[2] = q[2] * k;
      q[3] = q[3] * k;
    }
  }
  m.ctx->done = 1;
  bdfi_ctx_unpin(d, m.ctx);
}

/* ---- Instructions -------------------------------------------------------- */

static void bdfi_draw_run(bdfi_drawer *d, bdfi_robj *o);

/* Takes a RESTORE. */
static void bdfi_draw_restore(bdfi_drawer *d) {
  bdfi_ctx *ctx = d->ctx;
  if (ctx->unsaved > 0) {
    ctx->unsaved--;
    return;
  }
  if (ctx->nstack > 0) {
    bdfi_drop(d, ctx, &ctx->st);
    bdfi_state_release(&ctx->st);
    ctx->st = ctx->stack[--ctx->nstack];
  }
}

/* Takes an instruction of what is not drawn: a group or soft mask past the
 * limits, or an object drawn again past the limit of that. It counts what
 * begins and ends, so that what ends later ends what it began. */
static void bdfi_draw_left_out(bdfi_drawer *d, uint8_t op) {
  switch (op) {
  case BDF_OP_SAVE:
    d->ctx->unsaved++;
    break;
  case BDF_OP_RESTORE:
    bdfi_draw_restore(d);
    break;
  case BDF_OP_GROUP_BEGIN:
  case BDF_OP_MASK_BEGIN:
    d->skipped++;
    break;
  case BDF_OP_GROUP_END:
    if (d->skipped > 0) {
      d->skipped--;
    } else {
      bdfi_group_end(d);
    }
    break;
  case BDF_OP_MASK_END:
    if (d->skipped > 0) {
      d->skipped--;
    } else {
      bdfi_mask_end(d);
    }
    break;
  default:
    break;
  }
}

static void bdfi_draw_use(bdfi_drawer *d, const bdf_instr *in, uint8_t op) {
  bdfi_ctx *ctx = d->ctx;
  bdfi_state *st = &ctx->st;
  uint64_t i = bdf_instr_u64(in, 0);
  if (i >= bdf_object_object_count(d->obj->obj)) {
    return;
  }
  const uint8_t *hash = bdf_object_object(d->obj->obj, (size_t)i);
  bdfi_robj *child = bdfi_r_object(d->r, hash);
  if (!child) {
    return;
  }
  bdfi_state saved = *st;
  bdfi_state_retain(&saved);
  size_t depth = ctx->nstack;
  int64_t unsaved = ctx->unsaved;
  int again = d->again;
  int seen = bdfi_hash_set_add(&d->seen, hash);
  if (seen < 0) {
    d->r->oom = 1;
    seen = 0;
  }
  d->again = seen || again;
  st->made = NULL;
  if (op == BDF_OP_USE_AT) {
    st->m = bdfi_mat_translate(st->m, (double)bdf_instr_f32(in, 1), (double)bdf_instr_f32(in, 2));
  }
  ctx->pins++;
  bdfi_draw_run(d, child);
  d->again = again;
  /* USE is wrapped in an implicit save and restore, of the canvas it draws
   * on (which is gone if the object ended its group) */
  if (ctx->done) {
    bdfi_state_release(&saved);
    bdfi_ctx_unpin(d, ctx);
    return;
  }
  ctx->pins--;
  bdfi_drop(d, ctx, st);
  while (ctx->nstack > depth) {
    bdfi_state *top = &ctx->stack[--ctx->nstack];
    bdfi_drop(d, ctx, top);
    bdfi_state_release(top);
  }
  ctx->unsaved = unsaved;
  bdfi_state_release(st);
  *st = saved;
}

static void bdfi_draw_instr(bdfi_drawer *d, const bdf_instr *in) {
  uint8_t op = bdf_instr_op(in);
  if (d->again) {
    if (d->reused >= d->most) {
      bdfi_r_warn(d->r, "objects are drawn again too many times: the rest of them is not drawn");
      bdfi_draw_left_out(d, op);
      return;
    }
    d->reused++;
  }
  if (d->skipped > 0) {
    bdfi_draw_left_out(d, op);
    return;
  }
  bdfi_ctx *ctx = d->ctx;
  bdfi_state *st = &ctx->st;
#define F(i) ((double)bdf_instr_f32(in, (i)))
#define U(i) bdf_instr_u64(in, (i))
  switch (op) {
  case BDF_OP_SAVE:
    if (ctx->nstack >= BDFI_MAX_STATES) {
      bdfi_r_warnf(d->r, "more than %d states are saved at a time: the rest are not kept",
                   BDFI_MAX_STATES);
      ctx->unsaved++;
      return;
    }
    if (ctx->nstack == ctx->cap_stack) {
      size_t cap = ctx->cap_stack ? ctx->cap_stack * 2 : 8;
      bdfi_state *stack = bdfi_realloc_array(ctx->stack, cap, sizeof *stack);
      if (!stack) {
        d->r->oom = 1;
        ctx->unsaved++;
        return;
      }
      ctx->stack = stack;
      ctx->cap_stack = cap;
    }
    ctx->stack[ctx->nstack++] = *st;
    bdfi_state_retain(st);
    st->made = NULL;
    break;
  case BDF_OP_RESTORE:
    bdfi_draw_restore(d);
    break;
  case BDF_OP_TRANSFORM: {
    bdfi_mat n = {{F(0), F(1), F(2), F(3), F(4), F(5)}};
    st->m = bdfi_mat_mul2(st->m, n);
    break;
  }
  case BDF_OP_TRANSLATE:
    st->m = bdfi_mat_translate(st->m, F(0), F(1));
    break;
  case BDF_OP_SCALE:
    st->m = bdfi_mat_scale(st->m, F(0), F(1));
    break;
  case BDF_OP_CLIP_PATH: {
    bdfi_rpath *p = bdfi_draw_path(d, U(0));
    if (p) {
      bdfi_draw_clip(d, bdfi_draw_coverage(d, p, (uint8_t)U(1), &st->m));
    }
    break;
  }
  case BDF_OP_CLIP_RECT: {
    bdfi_rpath p;
    memset(&p, 0, sizeof p);
    bdfi_rpath_rect(&p, F(0), F(1), F(2), F(3));
    if (p.oom) {
      d->r->oom = 1;
    } else {
      bdfi_draw_clip(d, bdfi_draw_coverage(d, &p, BDF_FILL_NONZERO, &st->m));
    }
    bdfi_rpath_clear(&p);
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
    st->fill = bdfi_paint_style(d, U(0));
    break;
  case BDF_OP_STROKE_PAINT:
    st->stroke = bdfi_paint_style(d, U(0));
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
      d->r->oom = 1;
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
    double s = bdfi_mat_area_scale(&st->m);
    if (s == 0 || bdfi_isnan(s)) {
      s = 1;
    }
    st->shadow = (uint32_t)U(0);
    /* a value that is no number is ignored, as Canvas ignores it */
    double v = F(1) * s;
    if (!bdfi_isnan(v) && !bdfi_isinf(v)) {
      st->shadow_blur = bdfi_fmin(bdfi_fmax(0, v), BDFI_MAX_SHADOW_BLUR);
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
      bdfi_r_warnf(d->r, "FILTER \"%.*s\" is not applied", (int)(n > 200 ? 200 : n), s);
    }
    break;
  }
  case BDF_OP_FONT: {
    uint64_t i = U(0);
    if (i < bdf_object_font_count(d->obj->obj)) {
      st->font_owner = d->obj;
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
  case BDF_OP_FILL_RECT: {
    bdfi_rpath p;
    memset(&p, 0, sizeof p);
    bdfi_rpath_rect(&p, F(0), F(1), F(2), F(3));
    if (p.oom) {
      d->r->oom = 1;
    } else {
      bdfi_draw_fill_path(d, &p, BDF_FILL_NONZERO, &st->m);
    }
    bdfi_rpath_clear(&p);
    break;
  }
  case BDF_OP_STROKE_RECT: {
    bdfi_rpath p;
    memset(&p, 0, sizeof p);
    bdfi_rpath_rect(&p, F(0), F(1), F(2), F(3));
    if (p.oom) {
      d->r->oom = 1;
    } else {
      bdfi_draw_stroke_path(d, &p, &st->m);
    }
    bdfi_rpath_clear(&p);
    break;
  }
  case BDF_OP_CLEAR_RECT: {
    bdfi_rpath p;
    memset(&p, 0, sizeof p);
    bdfi_rpath_rect(&p, F(0), F(1), F(2), F(3));
    if (p.oom) {
      d->r->oom = 1;
    } else {
      bdfi_mask *cov = bdfi_draw_coverage(d, &p, BDF_FILL_NONZERO, &st->m);
      bdfi_solid black = bdfi_solid_of(0, 0, 0, 1);
      bdfi_draw_fill(d, cov, &black.base, 1, BDF_BLEND_DESTINATION_OUT, st->clip);
      bdfi_mask_unref(cov);
    }
    bdfi_rpath_clear(&p);
    break;
  }
  case BDF_OP_FILL_PATH: {
    bdfi_rpath *p = bdfi_draw_path(d, U(0));
    if (p) {
      bdfi_draw_fill_path(d, p, (uint8_t)U(1), &st->m);
    }
    break;
  }
  case BDF_OP_STROKE_PATH: {
    bdfi_rpath *p = bdfi_draw_path(d, U(0));
    if (p) {
      bdfi_draw_stroke_path(d, p, &st->m);
    }
    break;
  }
  case BDF_OP_FILL_PATH_AT: {
    bdfi_rpath *p = bdfi_draw_path(d, U(0));
    if (p) {
      bdfi_mat m = bdfi_mat_translate(st->m, F(2), F(3));
      bdfi_draw_fill_path(d, p, (uint8_t)U(1), &m);
    }
    break;
  }
  case BDF_OP_FILL_PATH_RUN: {
    uint8_t rule = (uint8_t)U(0);
    size_t n;
    const bdf_glyph *glyphs = bdf_instr_glyphs(in, 1, &n);
    bdfi_scratch *sc = bdfi_r_scratch(d->r);
    bdfi_irect bounds = bdfi_draw_bounds(d);
    for (size_t g = 0; g < n; g++) {
      bdfi_rpath *p = bdfi_draw_path(d, glyphs[g].path);
      if (!p) {
        continue;
      }
      bdfi_mat m = bdfi_mat_translate(st->m, (double)glyphs[g].x, (double)glyphs[g].y);
      if (bdfi_hidden(p, 0, &m, bounds)) {
        continue;
      }
      bdfi_rpath_flatten(p, sc, &m, BDFI_FLAT_TOL);
      if (sc->oom) {
        d->r->oom = 1;
        return;
      }
      if (sc->limited) {
        return;
      }
    }
    bdfi_mask *cov =
        bdfi_draw_made(d, bdfi_rasterize(sc, sc->pts, sc->polys, sc->npolys, rule, bounds));
    bdfi_any_shader sh;
    bdfi_draw_shader(d, &st->fill, &st->m, &sh);
    bdfi_draw_paint(d, cov, &sh.base, 1);
    bdfi_mask_unref(cov);
    break;
  }
  case BDF_OP_FILL_TEXT:
  case BDF_OP_STROKE_TEXT:
    bdfi_r_warn(d->r, "text is not drawn");
    break;
  case BDF_OP_IMAGE:
  case BDF_OP_IMAGE_SUB:
    bdfi_r_warn(d->r, "images are not drawn");
    break;
  case BDF_OP_SMOOTHING:
    st->smooth = U(0) != 0;
    break;
  case BDF_OP_USE:
  case BDF_OP_USE_AT:
    bdfi_draw_use(d, in, op);
    break;
  case BDF_OP_GROUP_BEGIN:
    bdfi_group_begin(d, (float)F(0), (uint8_t)U(1), F(2), F(3), F(4), F(5));
    break;
  case BDF_OP_GROUP_END:
    bdfi_group_end(d);
    break;
  case BDF_OP_MASK_BEGIN: {
    size_t n;
    const uint8_t *transfer = bdf_instr_bytes(in, 2, &n);
    bdfi_mask_begin(d, (uint8_t)U(0), (uint32_t)U(1), transfer, n);
    break;
  }
  case BDF_OP_MASK_END:
    bdfi_mask_end(d);
    break;
  default:
    break;
  }
#undef F
#undef U
}

static void bdfi_draw_run(bdfi_drawer *d, bdfi_robj *o) {
  if (d->depth > BDF_MAX_USE_DEPTH) {
    bdfi_r_warn(d->r, "objects nested too deep");
    return;
  }
  d->depth++;
  bdfi_robj *prev = d->obj;
  d->obj = o;
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
        d->r->oom = 1;
      } else {
        bdfi_r_warnf(d->r, "object: %s", e.message);
      }
      break;
    }
    bdfi_draw_instr(d, &it);
    if (d->r->oom) {
      break; /* nothing more can be drawn; what is open is closed by the caller */
    }
  }
  bdfi_instr_clear(&it);
  d->obj = prev;
  d->depth--;
}

void bdfi_draw_top(bdfi_drawer *d, bdfi_robj *o, const uint8_t *hash, const bdfi_mat *m,
                   bdfi_mask *clip) {
  if (!o) {
    return;
  }
  bdfi_ctx *top = d->top;
  bdfi_state saved = top->st;
  bdfi_state *stack = top->stack;
  size_t nstack = top->nstack, cap_stack = top->cap_stack;
  int64_t unsaved = top->unsaved;
  bdfi_state_init(&top->st, m, clip);
  top->stack = NULL;
  top->nstack = top->cap_stack = 0;
  top->unsaved = 0;
  int seen = bdfi_hash_set_add(&d->seen, hash);
  if (seen < 0) {
    d->r->oom = 1;
    seen = 0;
  }
  d->again = seen;
  bdfi_draw_run(d, o);
  /* unwind the groups and masks a malformed stream left open */
  while (d->nmasks > 0) {
    bdfi_soft_mask sm = d->masks[--d->nmasks];
    bdfi_closed(d, sm.ctx);
    bdfi_ctx_close(d, sm.ctx);
    bdfi_ctx_unpin(d, sm.parent);
  }
  while (d->ngroups > 0) {
    bdfi_group_end(d);
  }
  bdfi_set_ctx(d, top);
  d->skipped = 0;
  d->live = 0;
  top->held = 0;
  bdfi_state_release(&top->st);
  for (size_t i = 0; i < top->nstack; i++) {
    bdfi_state_release(&top->stack[i]);
  }
  free(top->stack);
  top->st = saved;
  top->stack = stack;
  top->nstack = nstack;
  top->cap_stack = cap_stack;
  top->unsaved = unsaved;
}

bdfi_drawer *bdfi_drawer_new(struct bdf_renderer *r, const bdfi_mat *m, int w, int h) {
  bdfi_drawer *d = calloc(1, sizeof *d);
  bdfi_ctx *ctx = d ? bdfi_ctx_new() : NULL;
  bdfi_surface *s = ctx ? bdfi_r_new_surface(r, w, h) : NULL;
  bdfi_mask *all = s ? bdfi_mask_new(bdfi_surface_bounds(s), 0) : NULL;
  if (!all) {
    if (s) {
      bdfi_r_release(r, s);
    }
    free(ctx);
    free(d);
    return NULL;
  }
  d->r = r;
  d->top = ctx;
  bdfi_set_ctx(d, ctx);
  ctx->target = s;
  int64_t limit = BDFI_MAX_LIVE * 16 * (int64_t)w * (int64_t)h;
  d->limit = limit > BDFI_MIN_LIVE ? limit : BDFI_MIN_LIVE;
  d->most = BDFI_MAX_REUSED_DRAWN;
  bdfi_state_init(&ctx->st, m, all);
  bdfi_mask_unref(all);
  return d;
}

void bdfi_drawer_free(bdfi_drawer *d) {
  if (!d) {
    return;
  }
  d->top->pins = 0;
  bdfi_ctx_destroy(d, d->top);
  free(d->groups);
  free(d->masks);
  bdfi_hash_set_free(&d->seen);
  free(d);
}

bdfi_mask *bdfi_draw_clip_to(bdfi_drawer *d, const bdfi_mat *m, double x, double y, double w,
                             double h) {
  bdfi_pt a = bdfi_mat_pt(m, x, y), b = bdfi_mat_pt(m, x + w, y + h);
  return bdfi_draw_made(d, bdfi_rect_mask(a.x, a.y, b.x, b.y, bdfi_surface_bounds(d->top->target)));
}
