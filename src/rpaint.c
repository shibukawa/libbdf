/*
 * Gradients of the rasterizer: their colours at 1024 steps, and the linear,
 * radial (the two circles of Canvas) and conic ones sampled in user space.
 * A port of raster/imagebdf/paint.go of the reference implementation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

typedef struct bdfi_gstop {
  double off;
  double c[4]; /* not premultiplied */
} bdfi_gstop;

void bdfi_lut_build(bdfi_lut *lut, const bdf_stop *stops, size_t n) {
  memset(lut, 0, sizeof *lut);
  bdfi_gstop *ss = n ? bdfi_malloc_array(n, sizeof *ss) : NULL;
  if (!ss) {
    return; /* no stops, or no memory: transparent black */
  }
  for (size_t i = 0; i < n; i++) {
    uint32_t c = stops[i].color;
    bdfi_gstop s;
    s.off = bdfi_fmin(1, bdfi_fmax(0, (double)stops[i].offset));
    s.c[0] = (double)(c >> 24) / 255;
    s.c[1] = (double)(c >> 16 & 0xff) / 255;
    s.c[2] = (double)(c >> 8 & 0xff) / 255;
    s.c[3] = (double)(c & 0xff) / 255;
    /* addColorStop keeps stops with the same offset in the order they came:
     * sorted by insertion, which keeps that order */
    size_t k = i;
    while (k > 0 && s.off < ss[k - 1].off) {
      ss[k] = ss[k - 1];
      k--;
    }
    ss[k] = s;
  }
  /* k is the first stop from the second that t does not pass: it moves on
   * as t grows */
  size_t k = 1;
  for (int i = 0; i <= 1024; i++) {
    double t = (double)i / 1024;
    double c[4] = {0, 0, 0, 0};
    if (t <= ss[0].off) {
      memcpy(c, ss[0].c, sizeof c);
    } else if (t >= ss[n - 1].off) {
      memcpy(c, ss[n - 1].c, sizeof c);
    } else {
      while (k < n && !(t <= ss[k].off)) {
        k++;
      }
      if (k < n) {
        const bdfi_gstop *a = &ss[k - 1], *b = &ss[k];
        double u = 0.0;
        if (b->off > a->off) {
          u = (t - a->off) / (b->off - a->off);
        }
        for (int j = 0; j < 4; j++) {
          c[j] = a->c[j] + (b->c[j] - a->c[j]) * u;
        }
      }
    }
    lut->c[i][0] = (float)(c[0] * c[3]);
    lut->c[i][1] = (float)(c[1] * c[3]);
    lut->c[i][2] = (float)(c[2] * c[3]);
    lut->c[i][3] = (float)c[3];
  }
  free(ss);
}

static const float *bdfi_lut_at(const bdfi_lut *l, double t) {
  if (t <= 0 || bdfi_isnan(t)) {
    return l->c[0];
  }
  if (t >= 1) {
    return l->c[1024];
  }
  return l->c[(int)(t * 1024 + 0.5)];
}

/* Solves the two-circle gradient of Canvas for the largest ω whose circle
 * passes through (x, y) with a radius that is not negative. */
static int bdfi_radial_t(double x, double y, const float *c, double *t) {
  double x0 = (double)c[0], y0 = (double)c[1], r0 = (double)c[2];
  double x1 = (double)c[3], y1 = (double)c[4], r1 = (double)c[5];
  double cdx = x1 - x0, cdy = y1 - y0, dr = r1 - r0;
  double pdx = x - x0, pdy = y - y0;
  double a = cdx * cdx + cdy * cdy - dr * dr;
  double b = pdx * cdx + pdy * cdy + r0 * dr;
  double cc = pdx * pdx + pdy * pdy - r0 * r0;
  *t = 0;
  if (fabs(a) < 1e-12) {
    if (b == 0) {
      return 0;
    }
    double w = cc / (2 * b);
    *t = w;
    return r0 + w * dr >= 0;
  }
  double disc = b * b - a * cc;
  if (disc < 0) {
    return 0;
  }
  double sq = sqrt(disc);
  double w1 = (b + sq) / a, w2 = (b - sq) / a;
  if (w1 < w2) {
    double tmp = w1;
    w1 = w2;
    w2 = tmp;
  }
  if (r0 + w1 * dr >= 0) {
    *t = w1;
    return 1;
  }
  if (r0 + w2 * dr >= 0) {
    *t = w2;
    return 1;
  }
  return 0;
}

static void bdfi_gradient_shade(const bdfi_shader *sh, int x, int y, int n, float *dst) {
  const bdfi_gradient_shader *g = (const bdfi_gradient_shader *)sh;
  const float *c = g->coords;
  static const float none[4] = {0, 0, 0, 0};
  for (int i = 0; i < n; i++) {
    bdfi_pt u = bdfi_mat_pt(&g->inv, (double)(x + i) + 0.5, (double)y + 0.5);
    const float *col = none;
    switch (g->kind) {
    case BDF_PAINT_LINEAR: {
      double x0 = (double)c[0], y0 = (double)c[1], x1 = (double)c[2], y1 = (double)c[3];
      double dx = x1 - x0, dy = y1 - y0;
      double l = dx * dx + dy * dy;
      if (l > 0) {
        col = bdfi_lut_at(g->lut, ((u.x - x0) * dx + (u.y - y0) * dy) / l);
      }
      break;
    }
    case BDF_PAINT_RADIAL: {
      double t;
      if (bdfi_radial_t(u.x, u.y, c, &t)) {
        col = bdfi_lut_at(g->lut, t);
      }
      break;
    }
    case BDF_PAINT_CONIC: {
      double a = bdfi_atan2(u.y - (double)c[2], u.x - (double)c[1]) - (double)c[0];
      double t = bdfi_mod(a / BDFI_2PI, 1);
      if (t < 0) {
        t++;
      }
      col = bdfi_lut_at(g->lut, t);
      break;
    }
    default:
      break;
    }
    memcpy(dst + 4 * i, col, 4 * sizeof *dst);
  }
}

int bdfi_gradient_shader_init(bdfi_gradient_shader *g, const bdf_paint *p, const bdfi_mat *m,
                              const bdfi_lut *lut) {
  memset(g, 0, sizeof *g);
  if (!bdfi_mat_invert(m, &g->inv) || p->nstops == 0 || !lut) {
    return 0;
  }
  g->base.shade = bdfi_gradient_shade;
  g->base.flat = 0;
  g->kind = p->kind;
  /* the coordinates a kind lacks are 0, and are not read */
  for (size_t i = 0; i < p->ncoords && i < 6; i++) {
    g->coords[i] = p->coords[i];
  }
  g->lut = lut;
  return 1;
}
