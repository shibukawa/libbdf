/*
 * The canvas of the rasterizer: premultiplied float pixels, and the
 * composite operations and blend modes of Canvas 2D (W3C Compositing and
 * Blending). A port of raster/imagebdf/surface.go of the reference
 * implementation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* Go's min and max of floats: a NaN when either is one. */
static float bdfi_minf(float a, float b) {
  if (a != a) {
    return a;
  }
  if (b != b) {
    return b;
  }
  return a < b ? a : b;
}

static float bdfi_maxf(float a, float b) {
  if (a != a) {
    return a;
  }
  if (b != b) {
    return b;
  }
  return a > b ? a : b;
}

static void bdfi_solid_shade(const bdfi_shader *sh, int x, int y, int n, float *dst) {
  const bdfi_solid *c = (const bdfi_solid *)sh;
  (void)x;
  (void)y;
  for (int i = 0; i < n; i++) {
    memcpy(dst + 4 * i, c->c, sizeof c->c);
  }
}

bdfi_solid bdfi_solid_of(float r, float g, float b, float a) {
  bdfi_solid s;
  s.base.shade = bdfi_solid_shade;
  s.base.flat = 1;
  s.c[0] = r;
  s.c[1] = g;
  s.c[2] = b;
  s.c[3] = a;
  return s;
}

bdfi_solid bdfi_premul(uint32_t c) {
  float r = (float)(c >> 24) / 255, g = (float)(c >> 16 & 0xff) / 255,
        b = (float)(c >> 8 & 0xff) / 255, a = (float)(c & 0xff) / 255;
  return bdfi_solid_of(r * a, g * a, b * a, a);
}

static void bdfi_surface_shade(const bdfi_shader *sh, int x, int y, int n, float *dst) {
  const bdfi_surface_shader *ss = (const bdfi_surface_shader *)sh;
  const bdfi_surface *s = ss->s;
  int sy = y - ss->dy, sx0 = x - ss->dx;
  if (sx0 >= 0 && sy >= 0 && sx0 + n <= s->w && sy < s->h) {
    /* the whole row is of the surface */
    memcpy(dst, s->pix + 4 * ((size_t)sy * (size_t)s->w + (size_t)sx0), 4 * (size_t)n * sizeof *dst);
    return;
  }
  for (int i = 0; i < n; i++) {
    int sx = x + i - ss->dx;
    if (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) {
      dst[4 * i] = dst[4 * i + 1] = dst[4 * i + 2] = dst[4 * i + 3] = 0;
      continue;
    }
    memcpy(dst + 4 * i, s->pix + 4 * ((size_t)sy * (size_t)s->w + (size_t)sx), 4 * sizeof *dst);
  }
}

bdfi_surface_shader bdfi_surface_shader_of(const bdfi_surface *s, int dx, int dy) {
  bdfi_surface_shader ss;
  ss.base.shade = bdfi_surface_shade;
  ss.base.flat = 0;
  ss.s = s;
  ss.dx = dx;
  ss.dy = dy;
  return ss;
}

/* The composite operations that change pixels the source does not cover
 * (Canvas applies them to the whole clip region). */
int bdfi_unbounded(uint8_t mode) {
  switch (mode) {
  case BDF_BLEND_SOURCE_IN:
  case BDF_BLEND_SOURCE_OUT:
  case BDF_BLEND_DESTINATION_IN:
  case BDF_BLEND_DESTINATION_ATOP:
  case BDF_BLEND_COPY:
    return 1;
  default:
    return 0;
  }
}

bdfi_irect bdfi_surface_bounds(const bdfi_surface *s) { return bdfi_irect_of(0, 0, s->w, s->h); }

static float bdfi_hard_light(float cb, float cs) {
  if (cs <= 0.5f) {
    return cb * 2 * cs;
  }
  float s = 2 * cs - 1;
  return cb + s - cb * s;
}

static float bdfi_separable(uint8_t mode, float cb, float cs) {
  switch (mode) {
  case BDF_BLEND_MULTIPLY:
    return cb * cs;
  case BDF_BLEND_SCREEN:
    return cb + cs - cb * cs;
  case BDF_BLEND_OVERLAY:
    return bdfi_hard_light(cs, cb);
  case BDF_BLEND_DARKEN:
    return bdfi_minf(cb, cs);
  case BDF_BLEND_LIGHTEN:
    return bdfi_maxf(cb, cs);
  case BDF_BLEND_COLOR_DODGE:
    if (cb == 0) {
      return 0;
    }
    if (cs >= 1) {
      return 1;
    }
    return bdfi_minf(1, cb / (1 - cs));
  case BDF_BLEND_COLOR_BURN:
    if (cb >= 1) {
      return 1;
    }
    if (cs <= 0) {
      return 0;
    }
    return 1 - bdfi_minf(1, (1 - cb) / cs);
  case BDF_BLEND_HARD_LIGHT:
    return bdfi_hard_light(cb, cs);
  case BDF_BLEND_SOFT_LIGHT: {
    if (cs <= 0.5f) {
      return cb - (1 - 2 * cs) * cb * (1 - cb);
    }
    float dd;
    if (cb <= 0.25f) {
      dd = ((16 * cb - 12) * cb + 4) * cb;
    } else {
      dd = (float)sqrt((double)cb);
    }
    return cb + (2 * cs - 1) * (dd - cb);
  }
  case BDF_BLEND_DIFFERENCE:
    if (cb > cs) {
      return cb - cs;
    }
    return cs - cb;
  case BDF_BLEND_EXCLUSION:
    return cb + cs - 2 * cb * cs;
  default:
    return cs;
  }
}

static float bdfi_lum(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

static void bdfi_clip_color(float c[3]) {
  float l = bdfi_lum(c);
  float n = bdfi_minf(bdfi_minf(c[0], c[1]), c[2]);
  float x = bdfi_maxf(bdfi_maxf(c[0], c[1]), c[2]);
  for (int i = 0; i < 3; i++) {
    if (n < 0 && l - n != 0) {
      c[i] = l + (c[i] - l) * l / (l - n);
    }
    if (x > 1 && x - l != 0) {
      c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
  }
}

static void bdfi_set_lum(const float c[3], float l, float out[3]) {
  float d = l - bdfi_lum(c);
  out[0] = c[0] + d;
  out[1] = c[1] + d;
  out[2] = c[2] + d;
  bdfi_clip_color(out);
}

static float bdfi_sat(const float c[3]) {
  return bdfi_maxf(bdfi_maxf(c[0], c[1]), c[2]) - bdfi_minf(bdfi_minf(c[0], c[1]), c[2]);
}

static void bdfi_set_sat(const float c[3], float s, float out[3]) {
  int idx[3] = {0, 1, 2};
  /* order the channels: min, mid, max */
  if (c[idx[0]] > c[idx[1]]) {
    int t = idx[0];
    idx[0] = idx[1];
    idx[1] = t;
  }
  if (c[idx[1]] > c[idx[2]]) {
    int t = idx[1];
    idx[1] = idx[2];
    idx[2] = t;
  }
  if (c[idx[0]] > c[idx[1]]) {
    int t = idx[0];
    idx[0] = idx[1];
    idx[1] = t;
  }
  out[0] = out[1] = out[2] = 0;
  float mn = c[idx[0]], md = c[idx[1]], mx = c[idx[2]];
  if (mx > mn) {
    out[idx[1]] = (md - mn) * s / (mx - mn);
    out[idx[2]] = s;
  }
}

static void bdfi_non_separable(uint8_t mode, const float cb[3], const float cs[3], float out[3]) {
  float t[3];
  switch (mode) {
  case BDF_BLEND_HUE:
    bdfi_set_sat(cs, bdfi_sat(cb), t);
    bdfi_set_lum(t, bdfi_lum(cb), out);
    break;
  case BDF_BLEND_SATURATION:
    bdfi_set_sat(cb, bdfi_sat(cs), t);
    bdfi_set_lum(t, bdfi_lum(cb), out);
    break;
  case BDF_BLEND_COLOR:
    bdfi_set_lum(cs, bdfi_lum(cb), out);
    break;
  default: /* luminosity */
    bdfi_set_lum(cb, bdfi_lum(cs), out);
    break;
  }
}

/* Porter and Duff: the source times fs and the destination times fd. */
static void bdfi_pd(const float s[4], const float d[4], float fs, float fd, float out[4]) {
  for (int i = 0; i < 4; i++) {
    out[i] = s[i] * fs + d[i] * fd;
  }
}

void bdfi_composite(uint8_t mode, const float s[4], const float d[4], float out[4]) {
  float as = s[3], ab = d[3];
  switch (mode) {
  case BDF_BLEND_SOURCE_OVER:
    bdfi_pd(s, d, 1, 1 - as, out);
    return;
  case BDF_BLEND_DESTINATION_OVER:
    bdfi_pd(s, d, 1 - ab, 1, out);
    return;
  case BDF_BLEND_SOURCE_IN:
    bdfi_pd(s, d, ab, 0, out);
    return;
  case BDF_BLEND_DESTINATION_IN:
    bdfi_pd(s, d, 0, as, out);
    return;
  case BDF_BLEND_SOURCE_OUT:
    bdfi_pd(s, d, 1 - ab, 0, out);
    return;
  case BDF_BLEND_DESTINATION_OUT:
    bdfi_pd(s, d, 0, 1 - as, out);
    return;
  case BDF_BLEND_SOURCE_ATOP:
    bdfi_pd(s, d, ab, 1 - as, out);
    return;
  case BDF_BLEND_DESTINATION_ATOP:
    bdfi_pd(s, d, 1 - ab, as, out);
    return;
  case BDF_BLEND_XOR:
    bdfi_pd(s, d, 1 - ab, 1 - as, out);
    return;
  case BDF_BLEND_COPY:
    memcpy(out, s, 4 * sizeof *out);
    return;
  case BDF_BLEND_LIGHTER:
    for (int i = 0; i < 4; i++) {
      out[i] = bdfi_minf(1, s[i] + d[i]);
    }
    return;
  default:
    break;
  }
  /* blend modes, composited source-over */
  float cs[3] = {0, 0, 0}, cb[3] = {0, 0, 0}, b[3];
  for (int i = 0; i < 3; i++) {
    if (as > 0) {
      cs[i] = s[i] / as;
    }
    if (ab > 0) {
      cb[i] = d[i] / ab;
    }
  }
  if (mode >= BDF_BLEND_HUE && mode <= BDF_BLEND_LUMINOSITY) {
    bdfi_non_separable(mode, cb, cs, b);
  } else {
    for (int i = 0; i < 3; i++) {
      b[i] = bdfi_separable(mode, cb[i], cs[i]);
    }
  }
  for (int i = 0; i < 3; i++) {
    out[i] = s[i] * (1 - ab) + d[i] * (1 - as) + as * ab * b[i];
  }
  out[3] = as + ab * (1 - as);
}

/* One pixel: the source through the coverage c and the clip k. */
static void bdfi_blend_pixel(float *d, const float *buf, float c, float k, uint8_t mode) {
  float src[4] = {buf[0] * c, buf[1] * c, buf[2] * c, buf[3] * c};
  float dst[4] = {d[0], d[1], d[2], d[3]};
  float out[4];
  if (mode == BDF_BLEND_SOURCE_OVER) {
    float ia = 1 - src[3];
    out[0] = src[0] + dst[0] * ia;
    out[1] = src[1] + dst[1] * ia;
    out[2] = src[2] + dst[2] * ia;
    out[3] = src[3] + dst[3] * ia;
  } else {
    bdfi_composite(mode, src, dst, out);
  }
  if (k == 1) {
    memcpy(d, out, sizeof out);
    return;
  }
  for (int j = 0; j < 4; j++) {
    d[j] = dst[j] + k * (out[j] - dst[j]);
  }
}

static float *bdfi_surface_row(bdfi_surface *s, size_t n) {
  if (s->cap_row < 4 * n) {
    float *row = bdfi_realloc_array(s->row, 4 * n, sizeof *row);
    if (!row) {
      return NULL;
    }
    s->row = row;
    s->cap_row = 4 * n;
  }
  return s->row;
}

/* The fill for the pixels r of the clip, inside the coverage or not: the
 * composite operations that change pixels the source does not cover. */
static int bdfi_surface_fill_all(bdfi_surface *s, const bdfi_mask *cov, const bdfi_shader *sh,
                                 float alpha, uint8_t mode, const bdfi_mask *clip, bdfi_irect r) {
  int n = BDFI_DX(r);
  float *buf = bdfi_malloc_array(4 * (size_t)n, sizeof *buf);
  if (!buf) {
    return -1;
  }
  int unbounded = bdfi_unbounded(mode);
  for (int y = r.y0; y < r.y1; y++) {
    sh->shade(sh, r.x0, y, n, buf);
    const float *krow = bdfi_mask_row(clip, y, r.x0);
    /* the coverage of this row: none of it, all of [lo, hi), or crow there */
    const float *crow = NULL;
    int lo = r.x0, hi = r.x1, any = 1;
    if (cov) {
      any = 0;
      if (y >= cov->r.y0 && y < cov->r.y1) {
        lo = r.x0 > cov->r.x0 ? r.x0 : cov->r.x0;
        hi = r.x1 < cov->r.x1 ? r.x1 : cov->r.x1;
        if (lo < hi) {
          any = 1;
          crow = bdfi_mask_row(cov, y, lo);
        }
      }
    }
    float *d = s->pix + 4 * ((size_t)y * (size_t)s->w + (size_t)r.x0);
    for (int i = 0; i < n; i++) {
      int x = r.x0 + i;
      float covered = 0;
      if (any && x >= lo && x < hi) {
        covered = crow ? crow[x - lo] : 1;
      }
      float c = covered * alpha;
      float k = krow ? krow[i] : 1;
      if (k == 0 || (c == 0 && !unbounded)) {
        continue;
      }
      bdfi_blend_pixel(d + 4 * i, buf + 4 * i, c, k, mode);
    }
  }
  free(buf);
  return 0;
}

int bdfi_surface_fill(bdfi_surface *s, const bdfi_mask *cov, const bdfi_shader *sh, float alpha,
                      uint8_t mode, const bdfi_mask *clip) {
  int unbounded = bdfi_unbounded(mode);
  if (bdfi_mask_is_empty(clip) || (alpha <= 0 && !unbounded)) {
    return 0;
  }
  bdfi_irect r = bdfi_irect_intersect(clip->r, bdfi_surface_bounds(s));
  if (cov && !unbounded) {
    r = bdfi_irect_intersect(r, cov->r);
  }
  if (bdfi_irect_empty(r)) {
    return 0;
  }
  if (unbounded) {
    return bdfi_surface_fill_all(s, cov, sh, alpha, mode, clip, r);
  }
  /* the pixels are all within the coverage: those of a row are told without
   * asking where each one is */
  int n = BDFI_DX(r);
  float *buf = bdfi_surface_row(s, (size_t)n);
  if (!buf) {
    return -1;
  }
  if (sh->flat) {
    sh->shade(sh, r.x0, r.y0, n, buf); /* the same in every row */
  }
  for (int y = r.y0; y < r.y1; y++) {
    if (!sh->flat) {
      sh->shade(sh, r.x0, y, n, buf);
    }
    const float *krow = bdfi_mask_row(clip, y, r.x0);
    const float *crow = cov ? bdfi_mask_row(cov, y, r.x0) : NULL;
    float *d = s->pix + 4 * ((size_t)y * (size_t)s->w + (size_t)r.x0);
    if (mode == BDF_BLEND_SOURCE_OVER && !krow) {
      /* the common case, without asking each pixel what the clip and the
       * composite operation are */
      if (!crow) {
        for (int i = 0; i < n; i++) {
          const float *b = buf + 4 * i;
          float *p = d + 4 * i;
          float s0 = b[0] * alpha, s1 = b[1] * alpha, s2 = b[2] * alpha, s3 = b[3] * alpha;
          float ia = 1 - s3;
          p[0] = s0 + p[0] * ia;
          p[1] = s1 + p[1] * ia;
          p[2] = s2 + p[2] * ia;
          p[3] = s3 + p[3] * ia;
        }
        continue;
      }
      for (int i = 0; i < n; i++) {
        float c = crow[i] * alpha;
        if (c == 0) {
          continue;
        }
        const float *b = buf + 4 * i;
        float *p = d + 4 * i;
        float s0 = b[0] * c, s1 = b[1] * c, s2 = b[2] * c, s3 = b[3] * c;
        float ia = 1 - s3;
        p[0] = s0 + p[0] * ia;
        p[1] = s1 + p[1] * ia;
        p[2] = s2 + p[2] * ia;
        p[3] = s3 + p[3] * ia;
      }
      continue;
    }
    if (mode == BDF_BLEND_SOURCE_OVER) {
      /* source-over through a clip with coverage: what bdfi_blend_pixel
       * does for it, operation for operation */
      for (int i = 0; i < n; i++) {
        float c = crow ? crow[i] * alpha : alpha;
        float k = krow[i];
        if (k == 0 || c == 0) {
          continue;
        }
        const float *b = buf + 4 * i;
        float *p = d + 4 * i;
        float s0 = b[0] * c, s1 = b[1] * c, s2 = b[2] * c, s3 = b[3] * c;
        float ia = 1 - s3;
        float o0 = s0 + p[0] * ia, o1 = s1 + p[1] * ia, o2 = s2 + p[2] * ia, o3 = s3 + p[3] * ia;
        if (k == 1) {
          p[0] = o0;
          p[1] = o1;
          p[2] = o2;
          p[3] = o3;
        } else {
          p[0] = p[0] + k * (o0 - p[0]);
          p[1] = p[1] + k * (o1 - p[1]);
          p[2] = p[2] + k * (o2 - p[2]);
          p[3] = p[3] + k * (o3 - p[3]);
        }
      }
      continue;
    }
    for (int i = 0; i < n; i++) {
      float c = crow ? crow[i] * alpha : alpha;
      float k = krow ? krow[i] : 1;
      if (k == 0 || c == 0) {
        continue;
      }
      bdfi_blend_pixel(d + 4 * i, buf + 4 * i, c, k, mode);
    }
  }
  return 0;
}

/* The background of a page or a sheet: a rectangle on whole pixels needs no
 * row of colours and no question of coverage or clip. */
int bdfi_surface_fill_background(bdfi_surface *s, const bdfi_mask *cov, const bdfi_solid *c) {
  if (!cov || cov->a) {
    bdfi_mask all;
    all.r = bdfi_surface_bounds(s);
    all.a = NULL;
    all.refs = 0;
    return bdfi_surface_fill(s, cov, &c->base, 1, BDF_BLEND_SOURCE_OVER, &all);
  }
  bdfi_irect r = bdfi_irect_intersect(cov->r, bdfi_surface_bounds(s));
  if (bdfi_irect_empty(r)) {
    return 0;
  }
  float ia = 1 - c->c[3];
  size_t n = (size_t)BDFI_DX(r);
  for (int y = r.y0; y < r.y1; y++) {
    float *d = s->pix + 4 * ((size_t)y * (size_t)s->w + (size_t)r.x0);
    for (size_t i = 0; i < 4 * n; i += 4) {
      float *p = d + i;
      p[0] = c->c[0] + p[0] * ia;
      p[1] = c->c[1] + p[1] * ia;
      p[2] = c->c[2] + p[2] * ia;
      p[3] = c->c[3] + p[3] * ia;
    }
  }
  return 0;
}

void bdfi_surface_to_rgba(const bdfi_surface *s, uint8_t *out) {
  size_t n = 4 * (size_t)s->w * (size_t)s->h;
  const float *pix = s->pix; /* in a register: out may be taken to alias s */
  for (size_t i = 0; i < n; i++) {
    float v = pix[i];
    /* rounded to the nearest: a float times 255 is exact as a double, and
     * so is the half added to what is not far below it. What is no number
     * is nothing, as is what is below zero, and what is above one is all:
     * held to [0, 1] (a NaN is not above 0), the arithmetic says the same
     * of them, and the loop has no branch for a compiler to wait on. */
    v = v > 0 ? v : 0;
    v = v < 1 ? v : 1;
    out[i] = (uint8_t)((double)v * 255 + 0.5);
  }
}
