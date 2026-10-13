/*
 * Strokes: the outline of lines with their joins, caps and dashes, as
 * polygons that all wind the same way. A port of raster/imagebdf/stroke.go
 * of the reference implementation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

double bdfi_miter_reach(const bdfi_line_style *ls) {
  if (ls->join != BDF_JOIN_MITER) {
    return 1;
  }
  if (!(ls->miter > 0)) {
    return 10;
  }
  return ls->miter;
}

void bdfi_dash_unref(bdfi_dash *d) {
  if (d && --d->refs == 0) {
    free(d);
  }
}

/* setLineDash: an odd list repeats, a list with a negative value or one
 * that is no number is ignored, and a list of zeros draws solid lines. */
int bdfi_line_set_dash(bdfi_line_style *ls, const float *segs, size_t n, float off) {
  double sum = 0;
  for (size_t i = 0; i < n; i++) {
    double v = (double)segs[i];
    if (v < 0 || bdfi_isnan(v) || bdfi_isinf(v)) {
      return 0;
    }
    sum += v;
  }
  bdfi_dash *d = NULL;
  if (sum != 0) {
    size_t count = n % 2 == 1 ? 2 * n : n;
    d = malloc(sizeof *d + (count ? count - 1 : 0) * sizeof d->v[0]);
    if (!d) {
      return -1;
    }
    d->refs = 1;
    d->n = count;
    for (size_t i = 0; i < count; i++) {
      d->v[i] = (double)segs[i % n];
    }
  }
  bdfi_dash_unref(ls->dash);
  ls->dash = d;
  double o = (double)off;
  if (!bdfi_isnan(o) && !bdfi_isinf(o)) {
    ls->dash_off = o;
  }
  return 0;
}

void bdfi_stroker_init(bdfi_stroker *s, const bdfi_line_style *ls, double dev_scale) {
  memset(s, 0, sizeof *s);
  s->ls = ls;
  s->hw = ls->width / 2;
  s->tol = 0.2 / bdfi_fmax(dev_scale, 1e-9);
  s->points = BDFI_STROKE_POINTS;
  s->dashes = BDFI_STROKE_DASHES;
}

void bdfi_stroker_within(bdfi_stroker *s, const bdfi_mat *m, bdfi_irect bounds) {
  s->m = *m;
  s->bounds = bounds;
  s->cull = 1;
}

void bdfi_stroker_free(bdfi_stroker *s) {
  free(s->pts);
  free(s->out);
  free(s->line);
  free(s->piece);
  free(s->cur);
}

/* Whether a box lies outside of what shows. */
static int bdfi_stroker_hidden(const bdfi_stroker *s, double x0, double y0, double x1, double y1) {
  const double box[4] = {x0, y0, x1, y1};
  return s->cull && bdfi_outside(box, 0, &s->m, s->bounds);
}

/* Counts n points of the outline, and reports whether it may have them. */
static int bdfi_stroker_take(bdfi_stroker *s, size_t n) {
  if (s->cut || (int64_t)n > s->points) {
    s->cut = 1;
    return 0;
  }
  s->points -= (int64_t)n;
  return 1;
}

static double bdfi_signed_area(const bdfi_pt *pts, size_t n) {
  double a = 0.0;
  for (size_t i = 0; i < n; i++) {
    bdfi_pt p = pts[i], q = pts[(i + 1) % n];
    a += p.x * q.y - q.x * p.y;
  }
  return a;
}

/* Adds a polygon to the outline. */
static void bdfi_stroker_put(bdfi_stroker *s, const bdfi_pt *pts, size_t n) {
  if (s->oom || !bdfi_stroker_take(s, n)) {
    return;
  }
  if (n > s->cap_pts - s->npts) {
    size_t cap = s->cap_pts ? s->cap_pts : 256;
    while (n > cap - s->npts) {
      cap *= 2;
    }
    bdfi_pt *grown = bdfi_realloc_array(s->pts, cap, sizeof *grown);
    if (!grown) {
      s->oom = 1;
      return;
    }
    s->pts = grown;
    s->cap_pts = cap;
  }
  if (s->nout == s->cap_out) {
    size_t cap = s->cap_out ? s->cap_out * 2 : 64;
    bdfi_poly *out = bdfi_realloc_array(s->out, cap, sizeof *out);
    if (!out) {
      s->oom = 1;
      return;
    }
    s->out = out;
    s->cap_out = cap;
  }
  bdfi_pt *dst = s->pts + s->npts;
  if (bdfi_signed_area(pts, n) < 0) {
    for (size_t i = 0; i < n; i++) {
      dst[i] = pts[n - 1 - i];
    }
  } else {
    memcpy(dst, pts, n * sizeof *dst);
  }
  bdfi_poly *pl = &s->out[s->nout++];
  pl->off = s->npts;
  pl->n = n;
  pl->closed = 1;
  s->npts += n;
}

/* Adds a polygon, unless it lies outside of what shows. */
static void bdfi_stroker_add(bdfi_stroker *s, const bdfi_pt *pts, size_t n) {
  if (s->cull) {
    double x0 = pts[0].x, y0 = pts[0].y, x1 = pts[0].x, y1 = pts[0].y;
    for (size_t i = 1; i < n; i++) {
      x0 = bdfi_fmin(x0, pts[i].x);
      y0 = bdfi_fmin(y0, pts[i].y);
      x1 = bdfi_fmax(x1, pts[i].x);
      y1 = bdfi_fmax(y1, pts[i].y);
    }
    if (bdfi_stroker_hidden(s, x0, y0, x1, y1)) {
      return;
    }
  }
  bdfi_stroker_put(s, pts, n);
}

/* The four corners of a segment or of a square cap. */
static void bdfi_stroker_quad(bdfi_stroker *s, bdfi_pt a, bdfi_pt b, bdfi_pt c, bdfi_pt d) {
  if (s->cull &&
      bdfi_stroker_hidden(s, bdfi_fmin(bdfi_fmin(a.x, b.x), bdfi_fmin(c.x, d.x)),
                          bdfi_fmin(bdfi_fmin(a.y, b.y), bdfi_fmin(c.y, d.y)),
                          bdfi_fmax(bdfi_fmax(a.x, b.x), bdfi_fmax(c.x, d.x)),
                          bdfi_fmax(bdfi_fmax(a.y, b.y), bdfi_fmax(c.y, d.y)))) {
    return;
  }
  const bdfi_pt pts[4] = {a, b, c, d};
  bdfi_stroker_put(s, pts, 4);
}

/* A circle of the width of the line: a round cap or join. */
static void bdfi_stroker_round(bdfi_stroker *s, bdfi_pt c) {
  if (s->cut || bdfi_stroker_hidden(s, c.x - s->hw, c.y - s->hw, c.x + s->hw, c.y + s->hw)) {
    return;
  }
  /* a polygon that stays within tol of the circle */
  double r = s->hw, tol = s->tol;
  int64_t n = 8;
  if (r > tol) {
    double step = 2 * bdfi_acos(1 - tol / r);
    double count = bdfi_fmin(ceil(BDFI_2PI / step), 256);
    /* what is no number is the smallest int in the reference */
    n = bdfi_isnan(count) ? INT64_MIN : (int64_t)count;
  }
  n = n < 8 ? 8 : n > 256 ? 256 : n;
  bdfi_pt pts[256];
  for (int64_t i = 0; i < n; i++) {
    double sn, co;
    bdfi_sincos(BDFI_2PI * (double)i / (double)n, &sn, &co);
    pts[i].x = c.x + r * co;
    pts[i].y = c.y + r * sn;
  }
  bdfi_stroker_put(s, pts, (size_t)n);
}

/* A square of the width of the line: the dot of a square cap. */
static void bdfi_stroker_square(bdfi_stroker *s, bdfi_pt c) {
  double hw = s->hw;
  bdfi_pt p0 = {c.x - hw, c.y - hw}, p1 = {c.x + hw, c.y - hw}, p2 = {c.x + hw, c.y + hw},
          p3 = {c.x - hw, c.y + hw};
  bdfi_stroker_quad(s, p0, p1, p2, p3);
}

/* The left normal of a→b scaled to hw. */
static void bdfi_normal(bdfi_pt a, bdfi_pt b, double hw, double *nx, double *ny) {
  double dx = b.x - a.x, dy = b.y - a.y;
  double l = bdfi_hypot(dx, dy);
  if (l == 0) {
    *nx = *ny = 0;
    return;
  }
  *nx = -dy / l * hw;
  *ny = dx / l * hw;
}

/* Fills the corner at v between the segments prev→v and v→next. */
static void bdfi_stroker_join(bdfi_stroker *s, bdfi_pt prev, bdfi_pt v, bdfi_pt next) {
  const bdfi_line_style *ls = s->ls;
  double hw = s->hw, tol = s->tol;
  double ax = v.x - prev.x, ay = v.y - prev.y;
  double bx = next.x - v.x, by = next.y - v.y;
  double la = bdfi_hypot(ax, ay), lb = bdfi_hypot(bx, by);
  if (la == 0 || lb == 0) {
    return;
  }
  double cross = ax * by - ay * bx;
  double dot = ax * bx + ay * by;
  double turn = bdfi_atan2(fabs(cross), dot); /* 0: straight on */
  if (turn < 1e-9) {
    return;
  }
  /* the outer side: the offsets of both segments away from the turn */
  double n1x, n1y, n2x, n2y;
  bdfi_normal(prev, v, hw, &n1x, &n1y);
  bdfi_normal(v, next, hw, &n2x, &n2y);
  if (cross > 0) {
    n1x = -n1x;
    n1y = -n1y;
    n2x = -n2x;
    n2y = -n2y;
  }
  bdfi_pt p1 = {v.x + n1x, v.y + n1y};
  bdfi_pt p2 = {v.x + n2x, v.y + n2y};
  bdfi_pt tri[3] = {v, p1, p2};
  switch (ls->join) {
  case BDF_JOIN_ROUND:
    if (turn * hw < 4 * tol) {
      bdfi_stroker_add(s, tri, 3);
      return;
    }
    bdfi_stroker_round(s, v);
    return;
  case BDF_JOIN_MITER: {
    /* the miter length over the line width is 1/sin(θ/2), θ the angle
     * between the segments */
    double theta = BDFI_PI - turn;
    double limit = ls->miter;
    if (!(limit > 0)) {
      limit = 10;
    }
    double sn = bdfi_sin(theta / 2);
    if (sn > 0 && 1 / sn <= limit) {
      double d = hw / bdfi_cos(turn / 2);
      double mx = (n1x + n2x) / 2, my = (n1y + n2y) / 2;
      double ml = bdfi_hypot(mx, my);
      if (ml > 0) {
        bdfi_pt quad[4] = {v, p1, {v.x + mx / ml * d, v.y + my / ml * d}, p2};
        bdfi_stroker_add(s, quad, 4);
        return;
      }
    }
    bdfi_stroker_add(s, tri, 3);
    return;
  }
  default:
    bdfi_stroker_add(s, tri, 3);
    return;
  }
}

static int bdfi_pt_eq(bdfi_pt a, bdfi_pt b) { return a.x == b.x && a.y == b.y; }

/* Copies points without the ones that repeat the one before them into a
 * buffer of the stroker: how many, or -1 when out of memory. */
static int64_t bdfi_dedupe(bdfi_stroker *s, bdfi_pt **buf, size_t *cap, const bdfi_pt *pts,
                           size_t n, size_t room) {
  if (n + room > *cap) {
    bdfi_pt *grown = bdfi_realloc_array(*buf, n + room, sizeof *grown);
    if (!grown) {
      s->oom = 1;
      return -1;
    }
    *buf = grown;
    *cap = n + room;
  }
  bdfi_pt *out = *buf;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (k == 0 || !bdfi_pt_eq(pts[i], out[k - 1])) {
      out[k++] = pts[i];
    }
  }
  return (int64_t)k;
}

static void bdfi_stroker_cap(bdfi_stroker *s, bdfi_pt end, bdfi_pt from) {
  double hw = s->hw;
  switch (s->ls->cap) {
  case BDF_CAP_ROUND:
    bdfi_stroker_round(s, end);
    break;
  case BDF_CAP_SQUARE: {
    double dx = end.x - from.x, dy = end.y - from.y;
    double l = bdfi_hypot(dx, dy);
    double ux = dx / l * hw, uy = dy / l * hw;
    double nx = -uy, ny = ux;
    bdfi_pt a = {end.x + nx, end.y + ny}, b = {end.x + nx + ux, end.y + ny + uy},
            c = {end.x - nx + ux, end.y - ny + uy}, d = {end.x - nx, end.y - ny};
    bdfi_stroker_quad(s, a, b, c, d);
    break;
  }
  default:
    break;
  }
}

/* Outlines one polyline: a quadrilateral for each segment, a join at each
 * corner and caps at the ends of an open one. */
static void bdfi_stroker_piece(bdfi_stroker *s, const bdfi_pt *in, size_t nin, int closed) {
  const bdfi_line_style *ls = s->ls;
  double hw = s->hw;
  int64_t count = bdfi_dedupe(s, &s->piece, &s->cap_piece, in, nin, 0);
  if (count < 0) {
    return;
  }
  const bdfi_pt *pts = s->piece;
  size_t n = (size_t)count;
  if (closed && n > 2 && bdfi_pt_eq(pts[0], pts[n - 1])) {
    n--;
  }
  if (n < 2) {
    if (n == 1 && ls->cap != BDF_CAP_BUTT) {
      if (ls->cap == BDF_CAP_ROUND) {
        bdfi_stroker_round(s, pts[0]);
      } else {
        bdfi_stroker_square(s, pts[0]);
      }
    }
    return;
  }
  size_t segs = closed ? n : n - 1;
  for (size_t i = 0; i < segs && !s->cut && !s->oom; i++) {
    bdfi_pt a = pts[i], b = pts[(i + 1) % n];
    double nx, ny;
    bdfi_normal(a, b, hw, &nx, &ny);
    bdfi_pt q0 = {a.x + nx, a.y + ny}, q1 = {b.x + nx, b.y + ny}, q2 = {b.x - nx, b.y - ny},
            q3 = {a.x - nx, a.y - ny};
    bdfi_stroker_quad(s, q0, q1, q2, q3);
  }
  for (size_t i = 1; i + 1 < n && !s->cut && !s->oom; i++) {
    bdfi_stroker_join(s, pts[i - 1], pts[i], pts[i + 1]);
  }
  if (closed) {
    bdfi_stroker_join(s, pts[n - 2], pts[n - 1], pts[0]);
    bdfi_stroker_join(s, pts[n - 1], pts[0], pts[1]);
    return;
  }
  bdfi_stroker_cap(s, pts[0], pts[1]);
  bdfi_stroker_cap(s, pts[n - 1], pts[n - 2]);
}

static int bdfi_cur_push(bdfi_stroker *s, bdfi_pt p) {
  if (s->ncur == s->cap_cur) {
    size_t cap = s->cap_cur ? s->cap_cur * 2 : 16;
    bdfi_pt *cur = bdfi_realloc_array(s->cur, cap, sizeof *cur);
    if (!cur) {
      s->oom = 1;
      return -1;
    }
    s->cur = cur;
    s->cap_cur = cap;
  }
  s->cur[s->ncur++] = p;
  return 0;
}

/* Strokes the dashes of a polyline: the pieces a pattern (on, off, …) cuts
 * it into, starting dash_off into the pattern. */
static void bdfi_stroker_dash(bdfi_stroker *s, const bdfi_pt *pts, size_t n) {
  const bdfi_dash *pattern = s->ls->dash;
  double offset = s->ls->dash_off;
  double total = 0.0;
  for (size_t i = 0; i < pattern->n; i++) {
    total += pattern->v[i];
  }
  if (total <= 0) {
    bdfi_stroker_piece(s, pts, n, 0);
    return;
  }
  offset = bdfi_mod(offset, total);
  if (offset < 0) {
    offset += total;
  }
  size_t idx = 0;
  double left = pattern->v[0];
  while (offset > 0) {
    if (offset < left) {
      left -= offset;
      break;
    }
    offset -= left;
    idx = (idx + 1) % pattern->n;
    left = pattern->v[idx];
  }
  int on = idx % 2 == 0;
  s->ncur = 0;
  if (on && bdfi_cur_push(s, pts[0]) < 0) {
    return;
  }
  /* whether the dashes of a stretch of a segment lie outside of what shows,
   * with their joins and caps */
  double reach = s->hw * bdfi_fmax(BDFI_SQRT2, bdfi_miter_reach(s->ls));
  for (size_t i = 1; i < n; i++) {
    bdfi_pt a = pts[i - 1], b = pts[i];
    double seg_len = bdfi_hypot(b.x - a.x, b.y - a.y);
    double pos = 0.0;
    int skip = bdfi_stroker_hidden(s, bdfi_fmin(a.x, b.x) - reach, bdfi_fmin(a.y, b.y) - reach,
                                   bdfi_fmax(a.x, b.x) + reach, bdfi_fmax(a.y, b.y) + reach);
    while (seg_len - pos > left) {
      if (--s->dashes < 0 || s->cut) {
        s->cut = 1; /* a pattern far too fine for the line */
        return;
      }
      pos += left;
      double t = pos / seg_len;
      bdfi_pt p = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
      if (on) {
        if (!skip || s->ncur > 1) {
          if (bdfi_cur_push(s, p) < 0) {
            return;
          }
          bdfi_stroker_piece(s, s->cur, s->ncur, 0);
        }
        s->ncur = 0;
      } else {
        s->ncur = 0;
        if (bdfi_cur_push(s, p) < 0) {
          return;
        }
      }
      if (s->oom) {
        return;
      }
      on = !on;
      idx = (idx + 1) % pattern->n;
      left = pattern->v[idx];
    }
    left -= seg_len - pos;
    if (on && bdfi_cur_push(s, b) < 0) {
      return;
    }
  }
  if (on && s->ncur > 1) {
    bdfi_stroker_piece(s, s->cur, s->ncur, 0);
  }
}

void bdfi_stroker_stroke(bdfi_stroker *s, const bdfi_pt *all, const bdfi_poly *lines,
                         size_t nlines) {
  const bdfi_line_style *ls = s->ls;
  if (!(s->hw > 0)) {
    return;
  }
  for (size_t i = 0; i < nlines; i++) {
    s->points += BDFI_STROKE_POINTS_EACH * (int64_t)lines[i].n;
  }
  for (size_t i = 0; i < nlines; i++) {
    if (s->cut || s->oom) {
      return;
    }
    const bdfi_pt *raw = all + lines[i].off;
    /* room for the first point again, which closes the line for its dashes */
    int64_t count = bdfi_dedupe(s, &s->line, &s->cap_line, raw, lines[i].n, 1);
    if (count < 0) {
      return;
    }
    size_t n = (size_t)count;
    bdfi_pt *pts = s->line;
    if (n == 1) {
      /* a sub-path of no length: a dot under round and square caps */
      if (lines[i].n >= 2) {
        if (ls->cap == BDF_CAP_ROUND) {
          bdfi_stroker_round(s, pts[0]);
        } else if (ls->cap == BDF_CAP_SQUARE) {
          bdfi_stroker_square(s, pts[0]);
        }
      }
      continue;
    }
    if (n < 2) {
      continue;
    }
    if (!ls->dash) {
      bdfi_stroker_piece(s, pts, n, lines[i].closed);
      continue;
    }
    if (lines[i].closed) {
      pts[n++] = pts[0];
    }
    bdfi_stroker_dash(s, pts, n);
  }
}

bdfi_mask *bdfi_stroke_mask(bdfi_scratch *sc, bdfi_rpath *p, const bdfi_mat *m, bdfi_line_style ls,
                            bdfi_irect bounds, float *alpha, int *cut) {
  *alpha = 0;
  *cut = 0;
  if (!bdfi_mat_finite(m)) {
    return bdfi_mask_empty();
  }
  double scale = bdfi_mat_area_scale(m);
  if (scale == 0) {
    return bdfi_mask_empty();
  }
  *alpha = 1;
  double dw = ls.width * scale;
  if (dw < 1) {
    *alpha = (float)dw;
    ls.width = 1 / scale;
  }
  if (bdfi_hidden(p, ls.width / 2 * bdfi_fmax(BDFI_SQRT2, bdfi_miter_reach(&ls)), m, bounds)) {
    return bdfi_mask_empty();
  }
  double ms = bdfi_mat_max_scale(m);
  bdfi_mat identity = bdfi_mat_identity();
  bdfi_rpath_flatten(p, sc, &identity, BDFI_FLAT_TOL / ms);
  if (sc->oom) {
    return NULL;
  }
  if (sc->limited) {
    return bdfi_mask_empty();
  }
  bdfi_stroker s;
  bdfi_stroker_init(&s, &ls, ms);
  bdfi_stroker_within(&s, m, bounds);
  bdfi_stroker_stroke(&s, sc->pts, sc->polys, sc->npolys);
  bdfi_mask *cov = NULL;
  if (!s.oom) {
    for (size_t i = 0; i < s.npts; i++) {
      s.pts[i] = bdfi_mat_pt(m, s.pts[i].x, s.pts[i].y);
    }
    cov = bdfi_rasterize(sc, s.pts, s.out, s.nout, BDF_FILL_NONZERO, bounds);
    *cut = s.cut;
  } else {
    sc->oom = 1;
  }
  bdfi_stroker_free(&s);
  return cov;
}
