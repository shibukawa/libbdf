/*
 * Paths of the rasterizer: transforms, the Path2D of Canvas with its arcs,
 * and its flattening into polylines. A port of raster/imagebdf/path.go of
 * the reference implementation, operation for operation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* ---- Rectangles of pixels ------------------------------------------------ */

bdfi_irect bdfi_irect_of(int x0, int y0, int x1, int y1) {
  bdfi_irect r;
  if (x0 > x1) {
    int t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  r.x0 = x0;
  r.y0 = y0;
  r.x1 = x1;
  r.y1 = y1;
  return r;
}

int bdfi_irect_empty(bdfi_irect r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }

bdfi_irect bdfi_irect_intersect(bdfi_irect r, bdfi_irect s) {
  if (r.x0 < s.x0) {
    r.x0 = s.x0;
  }
  if (r.y0 < s.y0) {
    r.y0 = s.y0;
  }
  if (r.x1 > s.x1) {
    r.x1 = s.x1;
  }
  if (r.y1 > s.y1) {
    r.y1 = s.y1;
  }
  if (bdfi_irect_empty(r)) {
    memset(&r, 0, sizeof r);
  }
  return r;
}

int bdfi_irect_overlaps(bdfi_irect r, bdfi_irect s) {
  return !bdfi_irect_empty(r) && !bdfi_irect_empty(s) && r.x0 < s.x1 && s.x0 < r.x1 &&
         r.y0 < s.y1 && s.y0 < r.y1;
}

int bdfi_irect_eq(bdfi_irect r, bdfi_irect s) {
  return r.x0 == s.x0 && r.y0 == s.y0 && r.x1 == s.x1 && r.y1 == s.y1;
}

/* ---- Transforms ---------------------------------------------------------- */

bdfi_mat bdfi_mat_identity(void) {
  bdfi_mat m = {{1, 0, 0, 1, 0, 0}};
  return m;
}

bdfi_mat bdfi_mat_mul2(bdfi_mat m, bdfi_mat n) {
  bdfi_mat r;
  r.v[0] = m.v[0] * n.v[0] + m.v[2] * n.v[1];
  r.v[1] = m.v[1] * n.v[0] + m.v[3] * n.v[1];
  r.v[2] = m.v[0] * n.v[2] + m.v[2] * n.v[3];
  r.v[3] = m.v[1] * n.v[2] + m.v[3] * n.v[3];
  r.v[4] = m.v[0] * n.v[4] + m.v[2] * n.v[5] + m.v[4];
  r.v[5] = m.v[1] * n.v[4] + m.v[3] * n.v[5] + m.v[5];
  return r;
}

bdfi_mat bdfi_mat_translate(bdfi_mat m, double x, double y) {
  bdfi_mat n = {{1, 0, 0, 1, x, y}};
  return bdfi_mat_mul2(m, n);
}

bdfi_mat bdfi_mat_scale(bdfi_mat m, double x, double y) {
  bdfi_mat n = {{x, 0, 0, y, 0, 0}};
  return bdfi_mat_mul2(m, n);
}

double bdfi_mat_det(const bdfi_mat *m) { return m->v[0] * m->v[3] - m->v[1] * m->v[2]; }

int bdfi_mat_invert(const bdfi_mat *m, bdfi_mat *out) {
  double d = bdfi_mat_det(m);
  if (d == 0 || bdfi_isnan(d) || bdfi_isinf(d)) {
    memset(out, 0, sizeof *out);
    return 0;
  }
  out->v[0] = m->v[3] / d;
  out->v[1] = -m->v[1] / d;
  out->v[2] = -m->v[2] / d;
  out->v[3] = m->v[0] / d;
  out->v[4] = (m->v[2] * m->v[5] - m->v[3] * m->v[4]) / d;
  out->v[5] = (m->v[1] * m->v[4] - m->v[0] * m->v[5]) / d;
  return 1;
}

double bdfi_mat_max_scale(const bdfi_mat *m) {
  return bdfi_fmax(bdfi_hypot(m->v[0], m->v[1]), bdfi_hypot(m->v[2], m->v[3]));
}

double bdfi_mat_area_scale(const bdfi_mat *m) { return sqrt(fabs(bdfi_mat_det(m))); }

int bdfi_mat_finite(const bdfi_mat *m) {
  for (int i = 0; i < 6; i++) {
    if (bdfi_isnan(m->v[i]) || bdfi_isinf(m->v[i])) {
      return 0;
    }
  }
  return 1;
}

/* ---- Building paths ------------------------------------------------------ */

void bdfi_rpath_clear(bdfi_rpath *p) {
  free(p->subs);
  free(p->segs);
  memset(p, 0, sizeof *p);
}

static bdfi_sub *bdfi_rpath_cur(bdfi_rpath *p) { return p->nsubs ? &p->subs[p->nsubs - 1] : NULL; }

/* The current point, and whether there is one. */
static int bdfi_rpath_current(bdfi_rpath *p, bdfi_pt *out) {
  bdfi_sub *s = bdfi_rpath_cur(p);
  if (!s) {
    out->x = out->y = 0;
    return 0;
  }
  if (s->nsegs > 0) {
    const bdfi_seg *g = &p->segs[s->seg0 + s->nsegs - 1];
    int at = g->kind == BDFI_SEG_LINE ? 0 : g->kind == BDFI_SEG_QUAD ? 2 : 4;
    out->x = g->p[at];
    out->y = g->p[at + 1];
    return 1;
  }
  *out = s->start;
  return 1;
}

static void bdfi_rpath_add_sub(bdfi_rpath *p, bdfi_pt start) {
  if (p->nsubs == p->cap_subs) {
    size_t cap = p->cap_subs ? p->cap_subs * 2 : 4;
    bdfi_sub *subs = bdfi_realloc_array(p->subs, cap, sizeof *subs);
    if (!subs) {
      p->oom = 1;
      return;
    }
    p->subs = subs;
    p->cap_subs = cap;
  }
  bdfi_sub *s = &p->subs[p->nsubs++];
  s->start = start;
  s->seg0 = p->nsegs;
  s->nsegs = 0;
  s->closed = 0;
}

static void bdfi_rpath_add_seg(bdfi_rpath *p, int kind, double a, double b, double c, double d,
                               double e, double f) {
  bdfi_sub *s = bdfi_rpath_cur(p);
  if (!s) {
    return; /* out of memory before */
  }
  if (p->nsegs == p->cap_segs) {
    size_t cap = p->cap_segs ? p->cap_segs * 2 : 8;
    bdfi_seg *segs = bdfi_realloc_array(p->segs, cap, sizeof *segs);
    if (!segs) {
      p->oom = 1;
      return;
    }
    p->segs = segs;
    p->cap_segs = cap;
  }
  bdfi_seg *g = &p->segs[p->nsegs++];
  g->kind = kind;
  g->p[0] = a;
  g->p[1] = b;
  g->p[2] = c;
  g->p[3] = d;
  g->p[4] = e;
  g->p[5] = f;
  s->nsegs++;
}

void bdfi_rpath_move_to(bdfi_rpath *p, double x, double y) {
  bdfi_pt start = {x, y};
  p->boxed = 0;
  bdfi_rpath_add_sub(p, start);
}

/* Canvas: "ensure there is a subpath". */
static void bdfi_rpath_ensure(bdfi_rpath *p, double x, double y) {
  if (p->nsubs == 0) {
    bdfi_rpath_move_to(p, x, y);
  }
}

void bdfi_rpath_line_to(bdfi_rpath *p, double x, double y) {
  if (p->nsubs == 0) {
    bdfi_rpath_move_to(p, x, y);
    return;
  }
  p->boxed = 0;
  bdfi_rpath_add_seg(p, BDFI_SEG_LINE, x, y, 0, 0, 0, 0);
}

void bdfi_rpath_quad_to(bdfi_rpath *p, double cx, double cy, double x, double y) {
  p->boxed = 0;
  bdfi_rpath_ensure(p, cx, cy);
  bdfi_rpath_add_seg(p, BDFI_SEG_QUAD, cx, cy, x, y, 0, 0);
}

void bdfi_rpath_cubic_to(bdfi_rpath *p, double c1x, double c1y, double c2x, double c2y, double x,
                         double y) {
  p->boxed = 0;
  bdfi_rpath_ensure(p, c1x, c1y);
  bdfi_rpath_add_seg(p, BDFI_SEG_CUBIC, c1x, c1y, c2x, c2y, x, y);
}

/* Closes the current sub-path; the next one starts where it started. */
void bdfi_rpath_close(bdfi_rpath *p) {
  bdfi_sub *s = bdfi_rpath_cur(p);
  if (s) {
    bdfi_pt start = s->start;
    s->closed = 1;
    bdfi_rpath_add_sub(p, start);
  }
}

void bdfi_rpath_rect(bdfi_rpath *p, double x, double y, double w, double h) {
  bdfi_rpath_move_to(p, x, y);
  bdfi_rpath_line_to(p, x + w, y);
  bdfi_rpath_line_to(p, x + w, y + h);
  bdfi_rpath_line_to(p, x, y + h);
  bdfi_rpath_close(p);
  bdfi_rpath_move_to(p, x, y);
}

typedef struct bdfi_arc {
  double cx, cy, rx, ry, sin, cos;
} bdfi_arc;

/* A point of the unit circle, scaled and rotated. */
static bdfi_pt bdfi_arc_tr(const bdfi_arc *a, double x, double y) {
  bdfi_pt p;
  x = a->rx * x;
  y = a->ry * y;
  p.x = a->cx + x * a->cos - y * a->sin;
  p.y = a->cy + x * a->sin + y * a->cos;
  return p;
}

/* An elliptical arc with Canvas semantics: a line from the current point to
 * the start of the arc, angles measured clockwise from the x axis (y down)
 * and rotated by rot. */
void bdfi_rpath_ellipse(bdfi_rpath *p, double cx, double cy, double rx, double ry, double rot,
                        double a0, double a1, int ccw) {
  if (rx < 0 || ry < 0) {
    return;
  }
  const double tau = BDFI_2PI;
  double sweep;
  if (!ccw) {
    if (a1 - a0 >= tau) {
      sweep = tau;
    } else {
      sweep = bdfi_mod(a1 - a0, tau);
      if (sweep < 0) {
        sweep += tau;
      }
    }
  } else {
    if (a0 - a1 >= tau) {
      sweep = -tau;
    } else {
      sweep = -bdfi_mod(a0 - a1, tau);
      if (sweep > 0) {
        sweep -= tau;
      }
    }
  }
  bdfi_arc arc = {cx, cy, rx, ry, 0, 0};
  bdfi_sincos(rot, &arc.sin, &arc.cos);
  double s, c;
  bdfi_sincos(a0, &s, &c);
  bdfi_pt start = bdfi_arc_tr(&arc, c, s), cur;
  if (bdfi_rpath_current(p, &cur)) {
    bdfi_rpath_line_to(p, start.x, start.y);
  } else {
    bdfi_rpath_move_to(p, start.x, start.y);
  }
  if (sweep == 0 || rx == 0 || ry == 0) {
    bdfi_sincos(a0 + sweep, &s, &c);
    bdfi_pt end = bdfi_arc_tr(&arc, c, s);
    bdfi_rpath_line_to(p, end.x, end.y);
    return;
  }
  int64_t n = bdfi_to_int(ceil(fabs(sweep) / BDFI_PI_2));
  double step = sweep / (double)n;
  double k = 4.0 / 3 * bdfi_tan(step / 4);
  double a = a0;
  for (int64_t i = 0; i < n; i++) {
    double b = a + step;
    double sa, ca, sb, cb;
    bdfi_sincos(a, &sa, &ca);
    bdfi_sincos(b, &sb, &cb);
    /* the control points of the arc of the unit circle */
    bdfi_pt c1 = bdfi_arc_tr(&arc, ca - k * sa, sa + k * ca);
    bdfi_pt c2 = bdfi_arc_tr(&arc, cb + k * sb, sb - k * cb);
    bdfi_pt e = bdfi_arc_tr(&arc, cb, sb);
    bdfi_rpath_cubic_to(p, c1.x, c1.y, c2.x, c2.y, e.x, e.y);
    if (p->oom) {
      return;
    }
    a = b;
  }
}

/* A line and an arc of radius r tangent to the lines from the current point
 * to (x1, y1) and from there to (x2, y2). */
void bdfi_rpath_arc_to(bdfi_rpath *p, double x1, double y1, double x2, double y2, double r) {
  if (r < 0) {
    return;
  }
  bdfi_pt p0;
  if (!bdfi_rpath_current(p, &p0)) {
    bdfi_rpath_move_to(p, x1, y1);
    p0.x = x1;
    p0.y = y1;
  }
  if ((p0.x == x1 && p0.y == y1) || (x1 == x2 && y1 == y2) || r == 0) {
    bdfi_rpath_line_to(p, x1, y1);
    return;
  }
  double ax = p0.x - x1, ay = p0.y - y1;
  double bx = x2 - x1, by = y2 - y1;
  double la = bdfi_hypot(ax, ay), lb = bdfi_hypot(bx, by);
  double cross = ax * by - ay * bx;
  if (fabs(cross) < 1e-12 * la * lb) {
    bdfi_rpath_line_to(p, x1, y1); /* collinear */
    return;
  }
  double cos_t = (ax * bx + ay * by) / (la * lb);
  double theta = bdfi_acos(bdfi_fmax(-1, bdfi_fmin(1, cos_t)));
  double d = r / bdfi_tan(theta / 2);
  double t0x = x1 + ax / la * d, t0y = y1 + ay / la * d;
  double t1x = x1 + bx / lb * d, t1y = y1 + by / lb * d;
  /* the centre: along the bisector */
  double bisx = ax / la + bx / lb, bisy = ay / la + by / lb;
  double bl = bdfi_hypot(bisx, bisy);
  double h = r / bdfi_sin(theta / 2);
  double cx = x1 + bisx / bl * h, cy = y1 + bisy / bl * h;
  double a0 = bdfi_atan2(t0y - cy, t0x - cx);
  double a1 = bdfi_atan2(t1y - cy, t1x - cx);
  bdfi_rpath_line_to(p, t0x, t0y);
  bdfi_rpath_ellipse(p, cx, cy, r, r, 0, a0, a1, cross > 0);
}

/* A rectangle with corners rounded by r (roundRect of Canvas with one
 * radius). */
void bdfi_rpath_round_rect(bdfi_rpath *p, double x, double y, double w, double h, double r) {
  if (w < 0) {
    x = x + w;
    w = -w;
  }
  if (h < 0) {
    y = y + h;
    h = -h;
  }
  r = bdfi_fmax(0, bdfi_fmin(r, bdfi_fmin(w, h) / 2));
  if (r == 0) {
    bdfi_rpath_rect(p, x, y, w, h);
    return;
  }
  bdfi_rpath_move_to(p, x + r, y);
  bdfi_rpath_line_to(p, x + w - r, y);
  bdfi_rpath_ellipse(p, x + w - r, y + r, r, r, 0, -BDFI_PI_2, 0, 0);
  bdfi_rpath_line_to(p, x + w, y + h - r);
  bdfi_rpath_ellipse(p, x + w - r, y + h - r, r, r, 0, 0, BDFI_PI_2, 0);
  bdfi_rpath_line_to(p, x + r, y + h);
  bdfi_rpath_ellipse(p, x + r, y + h - r, r, r, 0, BDFI_PI_2, BDFI_PI, 0);
  bdfi_rpath_line_to(p, x, y + r);
  bdfi_rpath_ellipse(p, x + r, y + r, r, r, 0, BDFI_PI, BDFI_3PI_2, 0);
  bdfi_rpath_close(p);
  bdfi_rpath_move_to(p, x, y);
}

void bdfi_rpath_build(bdfi_rpath *p, const bdf_path *bp) {
  const float *a = bp->args;
  size_t i = 0, n = bp->nargs;
/* operand k of the verb at i; one past the end is 0 */
#define F(k) (i + (k) >= n ? 0.0 : (double)a[i + (k)])
  for (size_t v = 0; v < bp->nverbs && !p->oom; v++) {
    switch (bp->verbs[v]) {
    case BDF_VERB_MOVE:
      bdfi_rpath_move_to(p, F(0), F(1));
      i += 2;
      break;
    case BDF_VERB_LINE:
      bdfi_rpath_line_to(p, F(0), F(1));
      i += 2;
      break;
    case BDF_VERB_QUAD:
      bdfi_rpath_quad_to(p, F(0), F(1), F(2), F(3));
      i += 4;
      break;
    case BDF_VERB_CUBIC:
      bdfi_rpath_cubic_to(p, F(0), F(1), F(2), F(3), F(4), F(5));
      i += 6;
      break;
    case BDF_VERB_CLOSE:
      bdfi_rpath_close(p);
      break;
    case BDF_VERB_RECT:
      bdfi_rpath_rect(p, F(0), F(1), F(2), F(3));
      i += 4;
      break;
    case BDF_VERB_ELLIPSE:
      bdfi_rpath_ellipse(p, F(0), F(1), F(2), F(3), F(4), F(5), F(6), F(7) != 0);
      i += 8;
      break;
    case BDF_VERB_ARC_TO:
      bdfi_rpath_arc_to(p, F(0), F(1), F(2), F(3), F(4));
      i += 5;
      break;
    case BDF_VERB_ROUND_RECT:
      bdfi_rpath_round_rect(p, F(0), F(1), F(2), F(3), F(4));
      i += 5;
      break;
    default:
      break;
    }
  }
#undef F
}

int bdfi_rpath_extent(bdfi_rpath *p, double box[4]) {
  if (!p->boxed) {
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
#define ADD(px, py)                                                                                \
  do {                                                                                             \
    x0 = bdfi_fmin(x0, (px));                                                                      \
    y0 = bdfi_fmin(y0, (py));                                                                      \
    x1 = bdfi_fmax(x1, (px));                                                                      \
    y1 = bdfi_fmax(y1, (py));                                                                      \
  } while (0)
    for (size_t i = 0; i < p->nsubs; i++) {
      const bdfi_sub *s = &p->subs[i];
      if (s->nsegs == 0) {
        continue;
      }
      ADD(s->start.x, s->start.y);
      for (size_t j = 0; j < s->nsegs; j++) {
        const bdfi_seg *g = &p->segs[s->seg0 + j];
        ADD(g->p[0], g->p[1]);
        if (g->kind != BDFI_SEG_LINE) {
          ADD(g->p[2], g->p[3]);
        }
        if (g->kind == BDFI_SEG_CUBIC) {
          ADD(g->p[4], g->p[5]);
        }
      }
    }
#undef ADD
    p->box[0] = x0;
    p->box[1] = y0;
    p->box[2] = x1;
    p->box[3] = y1;
    p->boxed = 1;
  }
  memcpy(box, p->box, sizeof p->box);
  return box[0] <= box[2] && box[1] <= box[3] && !bdfi_isinf(box[0]) && !bdfi_isinf(box[1]) &&
         !bdfi_isinf(box[2]) && !bdfi_isinf(box[3]);
}

int bdfi_rpath_is_rect(const bdfi_rpath *p, double *x0, double *y0, double *x1, double *y1) {
  const bdfi_sub *rs = NULL;
  for (size_t i = 0; i < p->nsubs; i++) {
    const bdfi_sub *s = &p->subs[i];
    if (s->nsegs == 0) {
      continue;
    }
    if (rs) {
      return 0;
    }
    rs = s;
  }
  if (!rs) {
    return 0;
  }
  const bdfi_seg *g = &p->segs[rs->seg0];
  if (rs->nsegs != 3 &&
      !(rs->nsegs == 4 && g[3].p[0] == rs->start.x && g[3].p[1] == rs->start.y)) {
    return 0;
  }
  bdfi_pt pts[4];
  pts[0] = rs->start;
  for (int i = 0; i < 3; i++) {
    if (g[i].kind != BDFI_SEG_LINE) {
      return 0;
    }
    pts[i + 1].x = g[i].p[0];
    pts[i + 1].y = g[i].p[1];
  }
  if (g[rs->nsegs - 1].kind != BDFI_SEG_LINE) {
    return 0;
  }
  bdfi_pt a = pts[0], b = pts[1], c = pts[2], d = pts[3];
  int horizontal_first = a.y == b.y && b.x == c.x && c.y == d.y && d.x == a.x;
  int vertical_first = a.x == b.x && b.y == c.y && c.x == d.x && d.y == a.y;
  if (!horizontal_first && !vertical_first) {
    return 0;
  }
  *x0 = bdfi_fmin(a.x, c.x);
  *y0 = bdfi_fmin(a.y, c.y);
  *x1 = bdfi_fmax(a.x, c.x);
  *y1 = bdfi_fmax(a.y, c.y);
  return 1;
}

/* ---- Flattening ---------------------------------------------------------- */

/* The most entries of a buffer kept for the next drawing: a path of
 * millions of edges does not hold its memory for ever. */
#define BDFI_SCRATCH_KEPT ((size_t)1 << 20)

void bdfi_scratch_reset(bdfi_scratch *sc) {
  if (sc->cap_pts > BDFI_SCRATCH_KEPT) {
    free(sc->pts);
    sc->pts = NULL;
    sc->cap_pts = 0;
  }
  if (sc->cap_polys > BDFI_SCRATCH_KEPT) {
    free(sc->polys);
    sc->polys = NULL;
    sc->cap_polys = 0;
  }
  if (sc->cap_edges > BDFI_SCRATCH_KEPT) {
    free(sc->edges);
    sc->edges = NULL;
    sc->cap_edges = 0;
  }
  if (sc->cap_active > BDFI_SCRATCH_KEPT) {
    free(sc->active);
    sc->active = NULL;
    sc->cap_active = 0;
  }
  if (sc->cap_xs > BDFI_SCRATCH_KEPT) {
    free(sc->xs);
    sc->xs = NULL;
    sc->cap_xs = 0;
  }
  if (sc->cap_edge_tmp > BDFI_SCRATCH_KEPT) {
    free(sc->edge_tmp);
    sc->edge_tmp = NULL;
    sc->cap_edge_tmp = 0;
  }
  if (sc->cap_xs_tmp > BDFI_SCRATCH_KEPT) {
    free(sc->xs_tmp);
    sc->xs_tmp = NULL;
    sc->cap_xs_tmp = 0;
  }
  sc->npts = 0;
  sc->npolys = 0;
  sc->limited = 0;
  sc->work = 0;
}

void bdfi_scratch_free(bdfi_scratch *sc) {
  free(sc->edges);
  free(sc->active);
  free(sc->xs);
  free(sc->edge_tmp);
  free(sc->xs_tmp);
  free(sc->area);
  free(sc->cover);
  free(sc->pts);
  free(sc->polys);
  memset(sc, 0, sizeof *sc);
}

static int bdfi_scratch_point(bdfi_scratch *sc, double x, double y) {
  if (sc->npts == sc->cap_pts) {
    size_t cap = sc->cap_pts ? sc->cap_pts * 2 : 256;
    bdfi_pt *pts = bdfi_realloc_array(sc->pts, cap, sizeof *pts);
    if (!pts) {
      sc->oom = 1;
      return -1;
    }
    sc->pts = pts;
    sc->cap_pts = cap;
  }
  sc->pts[sc->npts].x = x;
  sc->pts[sc->npts].y = y;
  sc->npts++;
  return 0;
}

/* How many straight pieces keep a curve whose control polygon bends by dd
 * within tol. */
static int bdfi_segments_for(double dd, double tol) {
  int64_t n = bdfi_to_int(ceil(sqrt(dd / (8 * tol))));
  if (n < 1 || bdfi_isnan(dd)) {
    return 1;
  }
  if (n > 256) {
    return 256;
  }
  return (int)n;
}

static void bdfi_flatten_quad(bdfi_scratch *sc, double x0, double y0, double x1, double y1,
                              double x2, double y2, double tol) {
  double dd = bdfi_hypot(x0 - 2 * x1 + x2, y0 - 2 * y1 + y2) * 2;
  int n = bdfi_segments_for(dd, tol);
  for (int i = 1; i <= n; i++) {
    double t = (double)i / (double)n;
    double u = 1 - t;
    if (bdfi_scratch_point(sc, u * u * x0 + 2 * u * t * x1 + t * t * x2,
                           u * u * y0 + 2 * u * t * y1 + t * t * y2) < 0) {
      return;
    }
  }
}

static void bdfi_flatten_cubic(bdfi_scratch *sc, double x0, double y0, double x1, double y1,
                               double x2, double y2, double x3, double y3, double tol) {
  double dd = 6 * bdfi_fmax(bdfi_hypot(x0 - 2 * x1 + x2, y0 - 2 * y1 + y2),
                            bdfi_hypot(x1 - 2 * x2 + x3, y1 - 2 * y2 + y3));
  int n = bdfi_segments_for(dd, tol);
  for (int i = 1; i <= n; i++) {
    double t = (double)i / (double)n;
    double u = 1 - t;
    double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    if (bdfi_scratch_point(sc, a * x0 + b * x1 + c * x2 + d * x3,
                           a * y0 + b * y1 + c * y2 + d * y3) < 0) {
      return;
    }
  }
}

void bdfi_rpath_flatten(const bdfi_rpath *p, bdfi_scratch *sc, const bdfi_mat *m, double tol) {
  if (sc->limited || sc->oom) {
    return;
  }
  for (size_t si = 0; si < p->nsubs; si++) {
    const bdfi_sub *s = &p->subs[si];
    if (sc->npts >= BDFI_MAX_PATH_POINTS) {
      bdfi_scratch_stop(sc);
      return;
    }
    size_t start = sc->npts;
    bdfi_pt c = bdfi_mat_pt(m, s->start.x, s->start.y);
    if (bdfi_scratch_point(sc, c.x, c.y) < 0) {
      return;
    }
    for (size_t j = 0; j < s->nsegs; j++) {
      const bdfi_seg *g = &p->segs[s->seg0 + j];
      switch (g->kind) {
      case BDFI_SEG_LINE: {
        bdfi_pt e = bdfi_mat_pt(m, g->p[0], g->p[1]);
        bdfi_scratch_point(sc, e.x, e.y);
        c = e;
        break;
      }
      case BDFI_SEG_QUAD: {
        bdfi_pt q = bdfi_mat_pt(m, g->p[0], g->p[1]);
        bdfi_pt e = bdfi_mat_pt(m, g->p[2], g->p[3]);
        bdfi_flatten_quad(sc, c.x, c.y, q.x, q.y, e.x, e.y, tol);
        c = e;
        break;
      }
      default: {
        bdfi_pt a = bdfi_mat_pt(m, g->p[0], g->p[1]);
        bdfi_pt b = bdfi_mat_pt(m, g->p[2], g->p[3]);
        bdfi_pt e = bdfi_mat_pt(m, g->p[4], g->p[5]);
        bdfi_flatten_cubic(sc, c.x, c.y, a.x, a.y, b.x, b.y, e.x, e.y, tol);
        c = e;
        break;
      }
      }
      if (sc->oom) {
        return;
      }
      if (sc->npts > BDFI_MAX_PATH_POINTS) {
        sc->npts = BDFI_MAX_PATH_POINTS;
        bdfi_scratch_stop(sc);
        return;
      }
    }
    if (sc->npolys == sc->cap_polys) {
      size_t cap = sc->cap_polys ? sc->cap_polys * 2 : 16;
      bdfi_poly *polys = bdfi_realloc_array(sc->polys, cap, sizeof *polys);
      if (!polys) {
        sc->oom = 1;
        return;
      }
      sc->polys = polys;
      sc->cap_polys = cap;
    }
    bdfi_poly *pl = &sc->polys[sc->npolys++];
    pl->off = start;
    pl->n = sc->npts - start;
    pl->closed = s->closed;
  }
}

int bdfi_outside(const double box[4], double reach, const bdfi_mat *m, bdfi_irect bounds) {
  if (bdfi_irect_empty(bounds)) {
    return 1;
  }
  const double cx[4] = {box[0] - reach, box[2] + reach, box[0] - reach, box[2] + reach};
  const double cy[4] = {box[1] - reach, box[1] - reach, box[3] + reach, box[3] + reach};
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < 4; i++) {
    bdfi_pt p = bdfi_mat_pt(m, cx[i], cy[i]);
    if (bdfi_isnan(p.x) || bdfi_isnan(p.y)) {
      return 0;
    }
    x0 = bdfi_fmin(x0, p.x);
    y0 = bdfi_fmin(y0, p.y);
    x1 = bdfi_fmax(x1, p.x);
    y1 = bdfi_fmax(y1, p.y);
  }
  return x1 < (double)bounds.x0 - 1 || x0 > (double)bounds.x1 + 1 || y1 < (double)bounds.y0 - 1 ||
         y0 > (double)bounds.y1 + 1;
}

int bdfi_hidden(bdfi_rpath *p, double reach, const bdfi_mat *m, bdfi_irect bounds) {
  double box[4];
  return bdfi_rpath_extent(p, box) && bdfi_outside(box, reach, m, bounds);
}

/* boxUnder: the bounds of a box under a transform (a NaN makes it one). */
void bdfi_box_under(const double box[4], const bdfi_mat *m, double out[4]) {
  const double cx[4] = {box[0], box[2], box[0], box[2]};
  const double cy[4] = {box[1], box[1], box[3], box[3]};
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < 4; i++) {
    bdfi_pt p = bdfi_mat_pt(m, cx[i], cy[i]);
    if (bdfi_isnan(p.x) || bdfi_isnan(p.y)) {
      out[0] = out[2] = p.x;
      out[1] = out[3] = p.y;
      return;
    }
    x0 = bdfi_fmin(x0, p.x);
    y0 = bdfi_fmin(y0, p.y);
    x1 = bdfi_fmax(x1, p.x);
    y1 = bdfi_fmax(y1, p.y);
  }
  out[0] = x0;
  out[1] = y0;
  out[2] = x1;
  out[3] = y1;
}

