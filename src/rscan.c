/*
 * Coverage: the masks of the rasterizer and its scanline filler, which
 * samples 16 scanlines a pixel row and covers spans exactly along x. A port
 * of raster/imagebdf/scan.go of the reference implementation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* ---- Masks --------------------------------------------------------------- */

bdfi_mask *bdfi_mask_empty(void) {
  /* shared and never changed: nobody counts it */
  static bdfi_mask empty = {{0, 0, 0, 0}, NULL, 0};
  return &empty;
}

bdfi_mask *bdfi_mask_new(bdfi_irect r, int with_coverage) {
  if (bdfi_irect_empty(r)) {
    return bdfi_mask_empty();
  }
  bdfi_mask *m = malloc(sizeof *m);
  if (!m) {
    return NULL;
  }
  m->r = r;
  m->a = NULL;
  m->refs = 1;
  if (with_coverage) {
    m->a = bdfi_calloc_array((size_t)BDFI_DX(r) * (size_t)BDFI_DY(r), sizeof *m->a);
    if (!m->a) {
      free(m);
      return NULL;
    }
  }
  return m;
}

bdfi_mask *bdfi_mask_ref(bdfi_mask *m) {
  if (m && m->refs > 0) {
    m->refs++;
  }
  return m;
}

void bdfi_mask_unref(bdfi_mask *m) {
  if (m && m->refs > 0 && --m->refs == 0) {
    free(m->a);
    free(m);
  }
}

int bdfi_mask_is_empty(const bdfi_mask *m) { return !m || bdfi_irect_empty(m->r); }

size_t bdfi_mask_bytes(const bdfi_mask *m) {
  return m->a ? 4 * (size_t)BDFI_DX(m->r) * (size_t)BDFI_DY(m->r) : 0;
}

const float *bdfi_mask_row(const bdfi_mask *m, int y, int x0) {
  if (!m->a) {
    return NULL;
  }
  return m->a + (size_t)(y - m->r.y0) * (size_t)BDFI_DX(m->r) + (size_t)(x0 - m->r.x0);
}

bdfi_mask *bdfi_mask_intersect(const bdfi_mask *a, const bdfi_mask *b) {
  if (bdfi_mask_is_empty(a) || bdfi_mask_is_empty(b)) {
    return bdfi_mask_empty();
  }
  bdfi_irect r = bdfi_irect_intersect(a->r, b->r);
  if (bdfi_irect_empty(r)) {
    return bdfi_mask_empty();
  }
  if (!a->a && !b->a) {
    return bdfi_mask_new(r, 0);
  }
  bdfi_mask *out = bdfi_mask_new(r, 1);
  if (!out) {
    return NULL;
  }
  size_t i = 0;
  int w = BDFI_DX(r);
  for (int y = r.y0; y < r.y1; y++) {
    const float *ra = bdfi_mask_row(a, y, r.x0), *rb = bdfi_mask_row(b, y, r.x0);
    for (int x = 0; x < w; x++) {
      float v = 1;
      if (ra) {
        v = ra[x];
      }
      if (rb) {
        v *= rb[x];
      }
      out->a[i++] = v;
    }
  }
  return out;
}

/* How much of the pixel [i, i+1) the stretch [p0, p1] covers. */
static float bdfi_cov1(double p0, double p1, int i) {
  double lo = bdfi_fmax(p0, (double)i), hi = bdfi_fmin(p1, (double)(i + 1));
  if (hi <= lo) {
    return 0;
  }
  return (float)(hi - lo);
}

bdfi_mask *bdfi_rect_mask(double x0, double y0, double x1, double y1, bdfi_irect bounds) {
  if (x1 < x0) {
    double t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y1 < y0) {
    double t = y0;
    y0 = y1;
    y1 = t;
  }
  if (!(x1 > x0 && y1 > y0) || (bdfi_isinf(x0) && bdfi_isinf(x1))) {
    return bdfi_mask_empty();
  }
  bdfi_irect r = bdfi_irect_intersect(
      bdfi_irect_of((int)floor(bdfi_clampf(x0)), (int)floor(bdfi_clampf(y0)),
                    (int)ceil(bdfi_clampf(x1)), (int)ceil(bdfi_clampf(y1))),
      bounds);
  if (bdfi_irect_empty(r)) {
    return bdfi_mask_empty();
  }
  if (x0 == floor(x0) && x1 == floor(x1) && y0 == floor(y0) && y1 == floor(y1)) {
    return bdfi_mask_new(r, 0);
  }
  bdfi_mask *m = bdfi_mask_new(r, 1);
  int w = BDFI_DX(r);
  float *xs = m ? bdfi_malloc_array((size_t)w, sizeof *xs) : NULL;
  if (!xs) {
    bdfi_mask_unref(m);
    return NULL;
  }
  for (int x = 0; x < w; x++) {
    xs[x] = bdfi_cov1(x0, x1, r.x0 + x);
  }
  size_t i = 0;
  for (int y = r.y0; y < r.y1; y++) {
    float cy = bdfi_cov1(y0, y1, y);
    for (int x = 0; x < w; x++) {
      m->a[i++] = xs[x] * cy;
    }
  }
  free(xs);
  return m;
}

/* ---- The scanline filler ------------------------------------------------- */

#define BDFI_SUB_SAMPLES 16
/* The most crossings of a scanline that are sorted by insertion whatever
 * their order; more are, as long as each takes few moves (they come nearly
 * sorted from the scanline before), and are sorted anew when not. */
#define BDFI_INSERTION_LIMIT 16
#define BDFI_INSERTION_MOVES 4

static int bdfi_finite_pt(bdfi_pt p) {
  return !bdfi_isnan(p.x) && !bdfi_isnan(p.y) && !bdfi_isinf(p.x) && !bdfi_isinf(p.y);
}

static int bdfi_edge_cmp(const void *pa, const void *pb) {
  const bdfi_edge *a = pa, *b = pb;
  return a->y0 < b->y0 ? -1 : a->y0 > b->y0 ? 1 : 0;
}

static int bdfi_crossing_cmp(const void *pa, const void *pb) {
  const bdfi_crossing *a = pa, *b = pb;
  return a->x < b->x ? -1 : a->x > b->x ? 1 : 0;
}

/*
 * A sort by a key that is a double, in time proportional to n log n with
 * the comparisons inline: runs of 16 sorted by insertion, then merged
 * through tmp, which holds n items. Items of one key may end up in any
 * order, as they may with qsort: nothing drawn depends on their order.
 */
#define BDFI_DEFINE_SORT(name, type, key)                                                          \
  static void name(type *a, type *tmp, size_t n) {                                                 \
    for (size_t lo = 0; lo < n; lo += 16) {                                                        \
      size_t end = n - lo < 16 ? n : lo + 16;                                                      \
      for (size_t i = lo + 1; i < end; i++) {                                                      \
        type v = a[i];                                                                             \
        size_t k = i;                                                                              \
        while (k > lo && v.key < a[k - 1].key) {                                                   \
          a[k] = a[k - 1];                                                                         \
          k--;                                                                                     \
        }                                                                                          \
        a[k] = v;                                                                                  \
      }                                                                                            \
    }                                                                                              \
    type *from = a, *to = tmp;                                                                     \
    for (size_t width = 16; width < n; width *= 2) {                                               \
      for (size_t lo = 0; lo < n; lo += 2 * width) {                                               \
        size_t mid = n - lo < width ? n : lo + width;                                              \
        size_t hi = n - lo < 2 * width ? n : lo + 2 * width;                                       \
        size_t i = lo, j = mid, k = lo;                                                            \
        while (i < mid && j < hi) {                                                                \
          to[k++] = from[j].key < from[i].key ? from[j++] : from[i++];                             \
        }                                                                                          \
        while (i < mid) {                                                                          \
          to[k++] = from[i++];                                                                     \
        }                                                                                          \
        while (j < hi) {                                                                           \
          to[k++] = from[j++];                                                                     \
        }                                                                                          \
      }                                                                                            \
      type *t = from;                                                                              \
      from = to;                                                                                   \
      to = t;                                                                                      \
    }                                                                                              \
    if (from != a) {                                                                               \
      memcpy(a, from, n * sizeof *a);                                                              \
    }                                                                                              \
  }

BDFI_DEFINE_SORT(bdfi_sort_edges, bdfi_edge, y0)
BDFI_DEFINE_SORT(bdfi_sort_crossings, bdfi_crossing, x)

typedef struct bdfi_fill {
  bdfi_scratch *sc;
  size_t nedges;
  double min_x, min_y, max_x, max_y;
} bdfi_fill;

static int bdfi_fill_edge(bdfi_fill *f, bdfi_pt a, bdfi_pt b) {
  if (a.y == b.y || !bdfi_finite_pt(a) || !bdfi_finite_pt(b)) {
    return 0;
  }
  bdfi_edge e;
  e.dir = 1;
  if (a.y > b.y) {
    bdfi_pt t = a;
    a = b;
    b = t;
    e.dir = -1;
  }
  e.x0 = a.x;
  e.y0 = a.y;
  e.y1 = b.y;
  e.slope = (b.x - a.x) / (b.y - a.y);
  if (bdfi_isnan(e.slope) || bdfi_isinf(e.slope)) {
    return 0; /* the ends are too far apart for a double to tell where it crosses */
  }
  bdfi_scratch *sc = f->sc;
  if (f->nedges == sc->cap_edges) {
    size_t cap = sc->cap_edges ? sc->cap_edges * 2 : 256;
    bdfi_edge *edges = bdfi_realloc_array(sc->edges, cap, sizeof *edges);
    if (!edges) {
      sc->oom = 1;
      return -1;
    }
    sc->edges = edges;
    sc->cap_edges = cap;
  }
  sc->edges[f->nedges++] = e;
  f->min_x = bdfi_fmin(f->min_x, bdfi_fmin(a.x, b.x));
  f->max_x = bdfi_fmax(f->max_x, bdfi_fmax(a.x, b.x));
  f->min_y = bdfi_fmin(f->min_y, a.y);
  f->max_y = bdfi_fmax(f->max_y, b.y);
  return 0;
}

bdfi_mask *bdfi_rasterize(bdfi_scratch *sc, const bdfi_pt *pts, const bdfi_poly *polys,
                          size_t npolys, uint8_t rule, bdfi_irect bounds) {
  if (sc->limited || sc->oom) {
    return bdfi_mask_empty();
  }
  bdfi_fill f = {sc, 0, INFINITY, INFINITY, -INFINITY, -INFINITY};
  for (size_t pi = 0; pi < npolys; pi++) {
    const bdfi_pt *p = pts + polys[pi].off;
    size_t n = polys[pi].n;
    if (n < 2) {
      continue;
    }
    for (size_t i = 1; i < n; i++) {
      if (bdfi_fill_edge(&f, p[i - 1], p[i]) < 0) {
        return NULL;
      }
      if (f.nedges > BDFI_MAX_PATH_POINTS) {
        bdfi_scratch_stop(sc);
        return bdfi_mask_empty();
      }
    }
    if (bdfi_fill_edge(&f, p[n - 1], p[0]) < 0) {
      return NULL;
    }
    if (f.nedges > BDFI_MAX_PATH_POINTS) {
      bdfi_scratch_stop(sc);
      return bdfi_mask_empty();
    }
  }
  if (f.nedges == 0) {
    return bdfi_mask_empty();
  }
  bdfi_irect r = bdfi_irect_intersect(
      bdfi_irect_of((int)floor(bdfi_clampf(f.min_x)), (int)floor(bdfi_clampf(f.min_y)),
                    (int)ceil(bdfi_clampf(f.max_x)), (int)ceil(bdfi_clampf(f.max_y))),
      bounds);
  if (bdfi_irect_empty(r)) {
    return bdfi_mask_empty();
  }
  bdfi_edge *edges = sc->edges;
  size_t nedges = f.nedges;
  /* room for the merges, or qsort when there is none for them */
  if (sc->cap_edge_tmp < nedges) {
    bdfi_edge *tmp = bdfi_realloc_array(sc->edge_tmp, nedges, sizeof *tmp);
    if (tmp) {
      sc->edge_tmp = tmp;
      sc->cap_edge_tmp = nedges;
    }
  }
  if (sc->cap_xs_tmp < nedges) {
    bdfi_crossing *tmp = bdfi_realloc_array(sc->xs_tmp, nedges, sizeof *tmp);
    if (tmp) {
      sc->xs_tmp = tmp;
      sc->cap_xs_tmp = nedges;
    }
  }
  if (sc->cap_edge_tmp >= nedges) {
    bdfi_sort_edges(edges, sc->edge_tmp, nedges);
  } else {
    qsort(edges, nedges, sizeof *edges, bdfi_edge_cmp);
  }
  int w = BDFI_DX(r);
  bdfi_mask *m = bdfi_mask_new(r, 1);
  if (!m) {
    sc->oom = 1;
    return NULL;
  }
  if (sc->cap_area < (size_t)w + 2) {
    free(sc->area);
    free(sc->cover);
    sc->area = bdfi_calloc_array((size_t)w + 2, sizeof *sc->area);
    sc->cover = bdfi_calloc_array((size_t)w + 2, sizeof *sc->cover);
    sc->cap_area = sc->area && sc->cover ? (size_t)w + 2 : 0;
    if (!sc->cap_area) {
      free(sc->area);
      free(sc->cover);
      sc->area = sc->cover = NULL;
      sc->oom = 1;
      bdfi_mask_unref(m);
      return NULL;
    }
  }
  float *area = sc->area, *cover = sc->cover;
  memset(area, 0, ((size_t)w + 1) * sizeof *area);
  memset(cover, 0, ((size_t)w + 2) * sizeof *cover);
  /* the edges that cross the scanlines being sampled, and their crossings:
   * never more than the edges */
  if (sc->cap_active < nedges) {
    uint32_t *active = bdfi_realloc_array(sc->active, nedges, sizeof *active);
    if (active) {
      sc->active = active;
      sc->cap_active = nedges;
    }
  }
  if (sc->cap_xs < nedges) {
    bdfi_crossing *xs = bdfi_realloc_array(sc->xs, nedges, sizeof *xs);
    if (xs) {
      sc->xs = xs;
      sc->cap_xs = nedges;
    }
  }
  if (sc->cap_active < nedges || sc->cap_xs < nedges) {
    sc->oom = 1;
    bdfi_mask_unref(m);
    return NULL;
  }
  uint32_t *active = sc->active;
  bdfi_crossing *xs = sc->xs;
  size_t nactive = 0, next = 0;
  const float wgt = 1.0f / BDFI_SUB_SAMPLES;
  const double dwgt = 1.0 / BDFI_SUB_SAMPLES;
  double left = (double)r.x0;
  for (int y = r.y0; y < r.y1; y++) {
    /* the cells the spans of the row touch: [lo, hi] of area and cover */
    int lo = w, hi = -1;
    for (int s = 0; s < BDFI_SUB_SAMPLES; s++) {
      double sy = (double)y + ((double)s + 0.5) / BDFI_SUB_SAMPLES;
      while (next < nedges && edges[next].y0 <= sy) {
        active[nactive++] = (uint32_t)next;
        next++;
      }
      if ((int64_t)nactive > BDFI_MAX_SCAN_WORK - sc->work) {
        bdfi_scratch_stop(sc);
        bdfi_mask_unref(m);
        return bdfi_mask_empty();
      }
      sc->work += (int64_t)nactive;
      size_t nxs = 0, k = 0;
      for (size_t ai = 0; ai < nactive; ai++) {
        uint32_t i = active[ai];
        const bdfi_edge *e = &edges[i];
        if (e->y1 <= sy) {
          continue; /* done */
        }
        active[k++] = i;
        if (e->y0 <= sy) {
          xs[nxs].x = e->x0 + (sy - e->y0) * e->slope;
          xs[nxs].dir = e->dir;
          xs[nxs].edge = i;
          nxs++;
        }
      }
      nactive = k;
      if (nxs < 2) {
        continue;
      }
      int sorted = nxs <= BDFI_INSERTION_LIMIT;
      if (sorted) {
        /* insertion sort: the crossings are few and nearly sorted */
        for (size_t i = 1; i < nxs; i++) {
          for (size_t j = i; j > 0 && xs[j].x < xs[j - 1].x; j--) {
            bdfi_crossing t = xs[j];
            xs[j] = xs[j - 1];
            xs[j - 1] = t;
          }
        }
      } else {
        /* the edges are in the order of their crossings on the scanline
         * before, so insertion sorts them in a few moves, unless this
         * scanline orders them anew */
        int64_t moves = BDFI_INSERTION_MOVES * (int64_t)nxs;
        sorted = 1;
        for (size_t i = 1; i < nxs && sorted; i++) {
          for (size_t j = i; j > 0 && xs[j].x < xs[j - 1].x; j--) {
            bdfi_crossing t = xs[j];
            xs[j] = xs[j - 1];
            xs[j - 1] = t;
            if (--moves < 0) {
              sorted = 0;
              break;
            }
          }
        }
      }
      if (!sorted) {
        if (sc->cap_xs_tmp >= nxs) {
          bdfi_sort_crossings(xs, sc->xs_tmp, nxs);
        } else {
          qsort(xs, nxs, sizeof *xs, bdfi_crossing_cmp);
        }
      }
      /* every edge that is active crosses: in this order next */
      if (nxs > BDFI_INSERTION_LIMIT && nxs == nactive) {
        for (size_t i = 0; i < nxs; i++) {
          active[i] = xs[i].edge;
        }
      }
      int64_t wind = 0;
      for (size_t i = 0; i + 1 < nxs; i++) {
        wind += xs[i].dir;
        int inside = rule == BDF_FILL_EVENODD ? (wind & 1) != 0 : wind != 0;
        if (!inside) {
          continue;
        }
        /* the span between two crossings: what it covers of each pixel */
        double xa = bdfi_fmax(xs[i].x - left, 0), xb = bdfi_fmin(xs[i + 1].x - left, (double)w);
        if (!(xb > xa)) {
          continue;
        }
        int ia = (int)xa, ib = (int)xb;
        if (ia < lo) {
          lo = ia;
        }
        if (ib > hi) {
          hi = ib;
        }
        if (ia == ib) {
          area[ia] += (float)((xb - xa) * dwgt);
          continue;
        }
        area[ia] += (float)(((double)(ia + 1) - xa) * dwgt);
        cover[ia + 1] += wgt;
        cover[ib] -= wgt;
        if (ib < w) {
          area[ib] += (float)((xb - (double)ib) * dwgt);
        }
      }
    }
    /* The row from the first cell touched to the last: before it, nothing
     * was added to area or cover, and the running sum is zero; after it
     * the sum is the whole of it, exactly zero (the entries are multiples
     * of a sixteenth, which a float adds without rounding, and each one
     * added at ia + 1 was taken away at ib). The pixels there are zero,
     * which the mask was made with. */
    if (hi >= 0) {
      float *row = m->a + (size_t)(y - r.y0) * (size_t)w;
      float run = 0;
      int end = hi < w ? hi : w - 1;
      for (int x = lo; x <= end; x++) {
        run += cover[x];
        float v = area[x] + run;
        if (v > 1) {
          v = 1;
        } else if (v < 0) {
          v = 0;
        }
        row[x] = v;
        area[x] = 0;
        cover[x] = 0;
      }
      for (int x = end + 1; x <= hi; x++) {
        area[x] = 0;
        cover[x] = 0;
      }
    }
    if (next >= nedges && nactive == 0) {
      break;
    }
  }
  return m;
}

bdfi_irect bdfi_irect_union(bdfi_irect r, bdfi_irect s) {
  if (bdfi_irect_empty(r)) {
    return s;
  }
  if (bdfi_irect_empty(s)) {
    return r;
  }
  if (r.x0 > s.x0) {
    r.x0 = s.x0;
  }
  if (r.y0 > s.y0) {
    r.y0 = s.y0;
  }
  if (r.x1 < s.x1) {
    r.x1 = s.x1;
  }
  if (r.y1 < s.y1) {
    r.y1 = s.y1;
  }
  return r;
}


/* The larger coverage of two masks at each pixel. The result is one of the
 * two (with a reference of its own) when the other is empty. */
bdfi_mask *bdfi_mask_union(bdfi_mask *a, bdfi_mask *b) {
  if (bdfi_mask_is_empty(a)) {
    return bdfi_mask_ref(b);
  }
  if (bdfi_mask_is_empty(b)) {
    return bdfi_mask_ref(a);
  }
  bdfi_irect r = bdfi_irect_union(a->r, b->r);
  bdfi_mask *out = bdfi_mask_new(r, 1);
  if (!out) {
    return NULL;
  }
  const bdfi_mask *both[2] = {a, b};
  for (int k = 0; k < 2; k++) {
    const bdfi_mask *m = both[k];
    int w = BDFI_DX(m->r);
    for (int y = m->r.y0; y < m->r.y1; y++) {
      const float *row = bdfi_mask_row(m, y, m->r.x0);
      float *o = out->a + (size_t)(y - r.y0) * (size_t)BDFI_DX(r) + (size_t)(m->r.x0 - r.x0);
      for (int x = 0; x < w; x++) {
        float v = row ? row[x] : 1;
        /* the larger; a coverage is never a NaN */
        if (v > o[x]) {
          o[x] = v;
        }
      }
    }
  }
  return out;
}

