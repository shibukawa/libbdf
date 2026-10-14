/* libbdf through Core Graphics: see bdf_coregraphics.h. */
#include "bdf/bdf_coregraphics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The CGImage of a bitmap that has an id, kept while the renderer may give
 * the id again. wrap: the image as the cell of a pattern (see
 * bdf_cg_image_of), 1 + BDF_REPEAT*; 0: as it is. */
typedef struct bdf_cg_image {
  uint64_t id;
  int wrap;
  CGImageRef image;
} bdf_cg_image;

struct bdf_cg {
  bdf_renderer *renderer;
  CGContextRef ctx;
  CGColorSpaceRef space;
  CGAffineTransform base; /* the user space of the page on the device */
  bdf_cg_image *images;
  size_t nimages, cap_images;
};

bdf_cg *bdf_cg_new(bdf_renderer *renderer) {
  bdf_cg *cg = calloc(1, sizeof *cg);
  if (cg) {
    cg->renderer = renderer;
    cg->space = CGColorSpaceCreateDeviceRGB();
  }
  return cg;
}

void bdf_cg_free(bdf_cg *cg) {
  if (!cg) {
    return;
  }
  for (size_t i = 0; i < cg->nimages; i++) {
    CGImageRelease(cg->images[i].image);
  }
  free(cg->images);
  CGColorSpaceRelease(cg->space);
  free(cg);
}

/*
 * A bitmap as a CGImage (retained for the caller): its pixels copied.
 *
 * With wrap (1 + BDF_REPEAT*) the image is the cell of a pattern, a pixel
 * larger on every side: along an axis the pattern repeats on, the pixels
 * of the far side, and along the other its own edge once more. Drawn a
 * pixel beyond its cell and clipped to it, the image is then interpolated
 * across the edges of the cells as the rasterizer does it.
 */
static CGImageRef bdf_cg_image_of(bdf_cg *cg, const bdf_canvas_bitmap *b, int wrap) {
  if (b->id != 0) {
    for (size_t i = 0; i < cg->nimages; i++) {
      if (cg->images[i].id == b->id && cg->images[i].wrap == wrap) {
        return CGImageRetain(cg->images[i].image);
      }
    }
  }
  size_t w = (size_t)b->width, h = (size_t)b->height;
  CFDataRef data;
  if (wrap) {
    int rx = wrap - 1 == BDF_REPEAT || wrap - 1 == BDF_REPEAT_X;
    int ry = wrap - 1 == BDF_REPEAT || wrap - 1 == BDF_REPEAT_Y;
    CFMutableDataRef padded = CFDataCreateMutable(NULL, 0);
    if (!padded) {
      return NULL;
    }
    CFDataSetLength(padded, (CFIndex)(4 * (w + 2) * (h + 2)));
    uint8_t *out = CFDataGetMutableBytePtr(padded);
    for (size_t y = 0; y < h + 2; y++) {
      size_t sy = y == 0 ? (ry ? h - 1 : 0) : y == h + 1 ? (ry ? 0 : h - 1) : y - 1;
      for (size_t x = 0; x < w + 2; x++) {
        size_t sx = x == 0 ? (rx ? w - 1 : 0) : x == w + 1 ? (rx ? 0 : w - 1) : x - 1;
        memcpy(out + 4 * (y * (w + 2) + x), b->pixels + 4 * (sy * w + sx), 4);
      }
    }
    data = padded;
    w += 2;
    h += 2;
  } else {
    data = CFDataCreate(NULL, b->pixels, (CFIndex)(4 * w * h));
  }
  if (!data) {
    return NULL;
  }
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CFRelease(data);
  if (!provider) {
    return NULL;
  }
  CGImageRef image =
      CGImageCreate(w, h, 8, 32, 4 * w, cg->space,
                    (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault,
                    provider, NULL, true, kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);
  if (image && b->id != 0) {
    if (cg->nimages == cg->cap_images) {
      size_t cap = cg->cap_images ? 2 * cg->cap_images : 16;
      bdf_cg_image *images = realloc(cg->images, cap * sizeof *images);
      if (!images) {
        return image; /* drawn, and made again the next time */
      }
      cg->images = images;
      cg->cap_images = cap;
    }
    cg->images[cg->nimages].id = b->id;
    cg->images[cg->nimages].wrap = wrap;
    cg->images[cg->nimages].image = CGImageRetain(image);
    cg->nimages++;
  }
  return image;
}

static CGBlendMode bdf_cg_blend(uint8_t blend) {
  static const CGBlendMode modes[] = {
      kCGBlendModeNormal,          kCGBlendModeMultiply,        kCGBlendModeScreen,
      kCGBlendModeOverlay,         kCGBlendModeDarken,          kCGBlendModeLighten,
      kCGBlendModeColorDodge,      kCGBlendModeColorBurn,       kCGBlendModeHardLight,
      kCGBlendModeSoftLight,       kCGBlendModeDifference,      kCGBlendModeExclusion,
      kCGBlendModeHue,             kCGBlendModeSaturation,      kCGBlendModeColor,
      kCGBlendModeLuminosity,      kCGBlendModeDestinationOver, kCGBlendModeDestinationIn,
      kCGBlendModeDestinationOut,  kCGBlendModeSourceIn,        kCGBlendModeSourceOut,
      kCGBlendModeSourceAtop,      kCGBlendModeDestinationAtop, kCGBlendModeXOR,
      kCGBlendModeCopy,            kCGBlendModePlusLighter,
  };
  return blend < sizeof modes / sizeof modes[0] ? modes[blend] : kCGBlendModeNormal;
}

static void bdf_cg_path(CGContextRef ctx, const bdf_canvas_path *p) {
  const double *c = p->coords;
  CGContextBeginPath(ctx);
  for (size_t i = 0; i < p->nverbs; i++) {
    switch (p->verbs[i]) {
    case BDF_VERB_MOVE:
      CGContextMoveToPoint(ctx, c[0], c[1]);
      c += 2;
      break;
    case BDF_VERB_LINE:
      CGContextAddLineToPoint(ctx, c[0], c[1]);
      c += 2;
      break;
    case BDF_VERB_QUAD:
      CGContextAddQuadCurveToPoint(ctx, c[0], c[1], c[2], c[3]);
      c += 4;
      break;
    case BDF_VERB_CUBIC:
      CGContextAddCurveToPoint(ctx, c[0], c[1], c[2], c[3], c[4], c[5]);
      c += 6;
      break;
    default:
      CGContextClosePath(ctx);
      break;
    }
  }
}

/* Clips to the current path, or to nothing when there is none (Core
 * Graphics does not take a clip to an empty path). */
static void bdf_cg_clip_path(CGContextRef ctx, uint8_t rule) {
  if (CGContextIsPathEmpty(ctx)) {
    CGContextClipToRect(ctx, CGRectZero);
  } else if (rule == BDF_FILL_EVENODD) {
    CGContextEOClip(ctx);
  } else {
    CGContextClip(ctx);
  }
}

/* The alpha, the blend mode and the shadow of a style. */
static void bdf_cg_state(bdf_cg *cg, const bdf_canvas_style *style) {
  CGContextRef ctx = cg->ctx;
  CGContextSetAlpha(ctx, style->alpha);
  CGContextSetBlendMode(ctx, bdf_cg_blend(style->blend));
  if ((style->shadow_color & 0xff) != 0 &&
      (style->shadow_blur > 0 || style->shadow_dx != 0 || style->shadow_dy != 0)) {
    /* a shadow is in units of the page whatever the transform is, and
     * Core Graphics takes it in the space of the device */
    CGAffineTransform b = cg->base;
    CGSize off = CGSizeMake(b.a * style->shadow_dx + b.c * style->shadow_dy,
                            b.b * style->shadow_dx + b.d * style->shadow_dy);
    double scale = sqrt(fabs(b.a * b.d - b.b * b.c));
    uint32_t c = style->shadow_color;
    const CGFloat comps[4] = {(c >> 24) / 255.0, (c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0,
                              (c & 0xff) / 255.0};
    CGColorRef color = CGColorCreate(cg->space, comps);
    /* Canvas blurs with a deviation of half the blur; so does Quartz */
    CGContextSetShadowWithColor(ctx, off, style->shadow_blur * scale, color);
    CGColorRelease(color);
  }
}

/* The colour of a gradient at t, between its stops (not premultiplied). */
static void bdf_cg_color_at(const bdf_canvas_paint *paint, double t, CGFloat out[4]) {
  const bdf_stop *lo = NULL, *hi = NULL;
  double lo_off = -1, hi_off = 2;
  for (size_t i = 0; i < paint->nstops; i++) {
    const bdf_stop *s = &paint->stops[i];
    double off = s->offset < 0 ? 0 : s->offset > 1 ? 1 : s->offset;
    if (off <= t && off >= lo_off) {
      lo = s;
      lo_off = off;
    }
    if (off >= t && off < hi_off) {
      hi = s;
      hi_off = off;
    }
  }
  if (!lo) {
    lo = hi;
    lo_off = hi_off;
  }
  if (!hi) {
    hi = lo;
    hi_off = lo_off;
  }
  double u = hi_off > lo_off ? (t - lo_off) / (hi_off - lo_off) : 0;
  for (int k = 0; k < 4; k++) {
    double c0 = (lo->color >> (24 - 8 * k) & 0xff) / 255.0;
    double c1 = (hi->color >> (24 - 8 * k) & 0xff) / 255.0;
    out[k] = c0 + (c1 - c0) * u;
  }
}

static int bdf_cg_stop_order(const void *a, const void *b) {
  const bdf_stop *x = *(const bdf_stop *const *)a, *y = *(const bdf_stop *const *)b;
  if (x->offset != y->offset) {
    return x->offset < y->offset ? -1 : 1;
  }
  return x < y ? -1 : x > y; /* stops at one place keep their order */
}

/* Paints the clip with a gradient or a pattern. */
static void bdf_cg_paint(bdf_cg *cg, const bdf_canvas_paint *paint) {
  CGContextRef ctx = cg->ctx;
  if (paint->kind == BDF_PAINT_PATTERN) {
    if (!paint->image) {
      return;
    }
    double w = paint->image->width, h = paint->image->height;
    const double far = 1e6;
    CGContextConcatCTM(ctx, CGAffineTransformMake(paint->matrix[0], paint->matrix[1],
                                                  paint->matrix[2], paint->matrix[3],
                                                  paint->matrix[4], paint->matrix[5]));
    /* a pattern shows in its cell where it does not repeat */
    if (paint->repeat == BDF_REPEAT_X) {
      CGContextClipToRect(ctx, CGRectMake(-far, 0, 2 * far, h));
    } else if (paint->repeat == BDF_REPEAT_Y) {
      CGContextClipToRect(ctx, CGRectMake(0, -far, w, 2 * far));
    } else if (paint->repeat == BDF_NO_REPEAT) {
      CGContextClipToRect(ctx, CGRectMake(0, 0, w, h));
    }
    /* the cells that show: a few are drawn one by one, each interpolated
     * between its pixels as the rasterizer draws a pattern (a tiled image
     * is not when it is enlarged), and not antialiased against each other,
     * so that no line shows between them */
    CGRect box = CGContextGetClipBoundingBox(ctx);
    double i0 = floor(box.origin.x / w), i1 = ceil((box.origin.x + box.size.width) / w);
    double j0 = floor(box.origin.y / h), j1 = ceil((box.origin.y + box.size.height) / h);
    if (paint->repeat == BDF_NO_REPEAT || paint->repeat == BDF_REPEAT_Y) {
      i0 = 0;
      i1 = 1;
    }
    if (paint->repeat == BDF_NO_REPEAT || paint->repeat == BDF_REPEAT_X) {
      j0 = 0;
      j1 = 1;
    }
    if (CGRectIsEmpty(box)) {
      return;
    }
    int cells = (i1 - i0) * (j1 - j0) <= 1024;
    CGImageRef image =
        bdf_cg_image_of(cg, paint->image, cells && paint->repeat <= BDF_NO_REPEAT ? 1 + paint->repeat : 0);
    if (!image) {
      return;
    }
    CGContextSetInterpolationQuality(ctx, kCGInterpolationLow);
    if (cells && paint->repeat <= BDF_NO_REPEAT) {
      CGContextSetShouldAntialias(ctx, false);
      for (double j = j0; j < j1; j++) {
        for (double i = i0; i < i1; i++) {
          CGContextSaveGState(ctx);
          CGContextTranslateCTM(ctx, i * w, j * h);
          CGContextClipToRect(ctx, CGRectMake(0, 0, w, h));
          /* images are drawn bottom up: turned over */
          CGContextScaleCTM(ctx, 1, -1);
          CGContextDrawImage(ctx, CGRectMake(-1, -h - 1, w + 2, h + 2), image);
          CGContextRestoreGState(ctx);
        }
      }
      CGContextSetShouldAntialias(ctx, true);
    } else {
      CGContextScaleCTM(ctx, 1, -1);
      CGContextDrawTiledImage(ctx, CGRectMake(0, -h, w, h), image);
    }
    CGImageRelease(image);
    return;
  }
  if (paint->nstops == 0) {
    return;
  }
  if (paint->kind == BDF_PAINT_CONIC) {
    /* wedges around the centre, each of the colour of its middle */
    enum { wedges = 360 };
    if (paint->ncoords < 3) {
      return;
    }
    double start = paint->coords[0], cx = paint->coords[1], cy = paint->coords[2];
    const double reach = 1e5;
    CGContextSetShouldAntialias(ctx, false);
    for (int i = 0; i < wedges; i++) {
      double a0 = start + 2 * M_PI * i / wedges, a1 = start + 2 * M_PI * (i + 1.02) / wedges;
      CGFloat c[4];
      bdf_cg_color_at(paint, (i + 0.5) / wedges, c);
      CGContextSetRGBFillColor(ctx, c[0], c[1], c[2], c[3]);
      CGContextBeginPath(ctx);
      CGContextMoveToPoint(ctx, cx, cy);
      CGContextAddLineToPoint(ctx, cx + reach * cos(a0), cy + reach * sin(a0));
      CGContextAddLineToPoint(ctx, cx + reach * cos(a1), cy + reach * sin(a1));
      CGContextClosePath(ctx);
      CGContextFillPath(ctx);
    }
    CGContextSetShouldAntialias(ctx, true);
    return;
  }
  /* the stops in the order of their places, as a canvas keeps them */
  size_t n = paint->nstops;
  const bdf_stop **order = malloc(n * sizeof *order);
  CGFloat *comps = malloc(4 * n * sizeof *comps), *locs = malloc(n * sizeof *locs);
  if (order && comps && locs) {
    for (size_t i = 0; i < n; i++) {
      order[i] = &paint->stops[i];
    }
    qsort(order, n, sizeof *order, bdf_cg_stop_order);
    for (size_t i = 0; i < n; i++) {
      uint32_t c = order[i]->color;
      double off = order[i]->offset;
      locs[i] = off < 0 ? 0 : off > 1 ? 1 : off;
      comps[4 * i] = (c >> 24) / 255.0;
      comps[4 * i + 1] = (c >> 16 & 0xff) / 255.0;
      comps[4 * i + 2] = (c >> 8 & 0xff) / 255.0;
      comps[4 * i + 3] = (c & 0xff) / 255.0;
    }
    CGGradientRef g = CGGradientCreateWithColorComponents(cg->space, comps, locs, n);
    if (g) {
      const CGGradientDrawingOptions both =
          kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation;
      if (paint->kind == BDF_PAINT_LINEAR && paint->ncoords >= 4) {
        CGContextDrawLinearGradient(ctx, g, CGPointMake(paint->coords[0], paint->coords[1]),
                                    CGPointMake(paint->coords[2], paint->coords[3]), both);
      } else if (paint->kind == BDF_PAINT_RADIAL && paint->ncoords >= 6) {
        CGContextDrawRadialGradient(ctx, g, CGPointMake(paint->coords[0], paint->coords[1]),
                                    paint->coords[2],
                                    CGPointMake(paint->coords[3], paint->coords[4]),
                                    paint->coords[5], both);
      }
      CGGradientRelease(g);
    }
  }
  free(order);
  free(comps);
  free(locs);
}

static void bdf_cg_save(void *c) { CGContextSaveGState(((bdf_cg *)c)->ctx); }

static void bdf_cg_restore(void *c) { CGContextRestoreGState(((bdf_cg *)c)->ctx); }

static void bdf_cg_transform(void *c, const double m[6]) {
  CGContextConcatCTM(((bdf_cg *)c)->ctx, CGAffineTransformMake(m[0], m[1], m[2], m[3], m[4], m[5]));
}

static void bdf_cg_clip(void *c, const bdf_canvas_path *path, uint8_t rule) {
  CGContextRef ctx = ((bdf_cg *)c)->ctx;
  bdf_cg_path(ctx, path);
  bdf_cg_clip_path(ctx, rule);
}

/* Fills the current path (already the outline of a stroke when stroked is
 * set) with a style. A gradient or a pattern is painted through the path
 * as a clip, on a layer of its own when the drawing has an alpha, a blend
 * mode or a shadow, which are those of the shape as a whole. */
static void bdf_cg_fill_path(bdf_cg *cg, uint8_t rule, const bdf_canvas_style *style) {
  CGContextRef ctx = cg->ctx;
  if (CGContextIsPathEmpty(ctx)) {
    return;
  }
  if (!style->paint) {
    uint32_t c = style->color;
    CGContextSetRGBFillColor(ctx, (c >> 24) / 255.0, (c >> 16 & 0xff) / 255.0,
                             (c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
    bdf_cg_state(cg, style);
    if (rule == BDF_FILL_EVENODD) {
      CGContextEOFillPath(ctx);
    } else {
      CGContextFillPath(ctx);
    }
    return;
  }
  int plain = style->alpha >= 1 && style->blend == BDF_BLEND_SOURCE_OVER &&
              ((style->shadow_color & 0xff) == 0 ||
               (style->shadow_blur <= 0 && style->shadow_dx == 0 && style->shadow_dy == 0));
  if (!plain) {
    CGPathRef outline = CGContextCopyPath(ctx);
    bdf_cg_state(cg, style);
    CGContextBeginTransparencyLayer(ctx, NULL);
    CGContextBeginPath(ctx);
    CGContextAddPath(ctx, outline);
    CGPathRelease(outline);
  }
  bdf_cg_clip_path(ctx, rule);
  bdf_cg_paint(cg, style->paint);
  if (!plain) {
    CGContextEndTransparencyLayer(ctx);
  }
}

static void bdf_cg_fill(void *c, const bdf_canvas_path *path, uint8_t rule,
                        const bdf_canvas_style *style) {
  bdf_cg *cg = c;
  CGContextSaveGState(cg->ctx);
  bdf_cg_path(cg->ctx, path);
  bdf_cg_fill_path(cg, rule, style);
  CGContextRestoreGState(cg->ctx);
}

static void bdf_cg_stroke(void *c, const bdf_canvas_path *path, const bdf_canvas_line *line,
                          const bdf_canvas_style *style) {
  bdf_cg *cg = c;
  CGContextRef ctx = cg->ctx;
  CGContextSaveGState(ctx);
  CGContextSetLineWidth(ctx, line->width);
  CGContextSetLineCap(ctx, line->cap == BDF_CAP_ROUND    ? kCGLineCapRound
                           : line->cap == BDF_CAP_SQUARE ? kCGLineCapSquare
                                                         : kCGLineCapButt);
  CGContextSetLineJoin(ctx, line->join == BDF_JOIN_ROUND   ? kCGLineJoinRound
                            : line->join == BDF_JOIN_BEVEL ? kCGLineJoinBevel
                                                           : kCGLineJoinMiter);
  CGContextSetMiterLimit(ctx, line->miter);
  if (line->dash && line->ndash > 0 && line->ndash <= 64) {
    CGFloat lengths[64];
    for (size_t i = 0; i < line->ndash; i++) {
      lengths[i] = line->dash[i];
    }
    CGContextSetLineDash(ctx, line->dash_offset, lengths, line->ndash);
  }
  bdf_cg_path(ctx, path);
  if (!style->paint) {
    uint32_t col = style->color;
    CGContextSetRGBStrokeColor(ctx, (col >> 24) / 255.0, (col >> 16 & 0xff) / 255.0,
                               (col >> 8 & 0xff) / 255.0, (col & 0xff) / 255.0);
    bdf_cg_state(cg, style);
    if (!CGContextIsPathEmpty(ctx)) {
      CGContextStrokePath(ctx);
    }
  } else if (!CGContextIsPathEmpty(ctx)) {
    /* the outline of the line, filled with the paint */
    CGContextReplacePathWithStrokedPath(ctx);
    bdf_cg_fill_path(cg, BDF_FILL_NONZERO, style);
  }
  CGContextRestoreGState(ctx);
}

static void bdf_cg_image_fn(void *c, const bdf_canvas_bitmap *bitmap, const double src[4],
                            const double dst[4], const bdf_canvas_style *style) {
  bdf_cg *cg = c;
  CGContextRef ctx = cg->ctx;
  if (!(src[2] > 0 && src[3] > 0 && dst[2] > 0 && dst[3] > 0)) {
    return;
  }
  CGImageRef whole = bdf_cg_image_of(cg, bitmap, 0);
  if (!whole) {
    return;
  }
  /* the whole pixels around the part, placed so that the part is on dst */
  double x0 = floor(src[0]), y0 = floor(src[1]);
  double x1 = ceil(src[0] + src[2]), y1 = ceil(src[1] + src[3]);
  CGImageRef part = whole;
  if (x0 > 0 || y0 > 0 || x1 < bitmap->width || y1 < bitmap->height) {
    part = CGImageCreateWithImageInRect(whole, CGRectMake(x0, y0, x1 - x0, y1 - y0));
    CGImageRelease(whole);
    if (!part) {
      return;
    }
  }
  double kx = dst[2] / src[2], ky = dst[3] / src[3];
  CGRect where = CGRectMake(dst[0] - (src[0] - x0) * kx, dst[1] - (src[1] - y0) * ky,
                            (x1 - x0) * kx, (y1 - y0) * ky);
  CGContextSaveGState(ctx);
  CGContextClipToRect(ctx, CGRectMake(dst[0], dst[1], dst[2], dst[3]));
  bdf_cg_state(cg, style);
  CGContextSetInterpolationQuality(ctx,
                                   style->smoothing ? kCGInterpolationMedium : kCGInterpolationNone);
  /* images are drawn bottom up: turned over within their rectangle */
  CGContextTranslateCTM(ctx, where.origin.x, where.origin.y + where.size.height);
  CGContextScaleCTM(ctx, 1, -1);
  CGContextDrawImage(ctx, CGRectMake(0, 0, where.size.width, where.size.height), part);
  CGContextRestoreGState(ctx);
  CGImageRelease(part);
}

static void bdf_cg_begin_layer(void *c, const double bounds[4], const bdf_canvas_style *style) {
  bdf_cg *cg = c;
  CGContextRef ctx = cg->ctx;
  CGContextSaveGState(ctx);
  /* the alpha, the blend mode and the shadow the layer is composited with:
   * inside it Quartz draws without them */
  bdf_cg_state(cg, style);
  CGContextBeginTransparencyLayerWithRect(ctx,
                                          CGRectMake(bounds[0], bounds[1], bounds[2], bounds[3]),
                                          NULL);
}

static void bdf_cg_end_layer(void *c) {
  CGContextRef ctx = ((bdf_cg *)c)->ctx;
  CGContextEndTransparencyLayer(ctx);
  CGContextRestoreGState(ctx);
}

void bdf_cg_canvas(bdf_cg *cg, CGContextRef ctx, bdf_canvas *out) {
  memset(out, 0, sizeof *out);
  cg->ctx = ctx;
  cg->base = CGContextGetCTM(ctx);
  out->ctx = cg;
  out->save = bdf_cg_save;
  out->restore = bdf_cg_restore;
  out->transform = bdf_cg_transform;
  out->clip = bdf_cg_clip;
  out->fill = bdf_cg_fill;
  out->stroke = bdf_cg_stroke;
  out->image = bdf_cg_image_fn;
  out->begin_layer = bdf_cg_begin_layer;
  out->end_layer = bdf_cg_end_layer;
}

/* The device pixels a unit of the user space takes. */
static double bdf_cg_scale(const bdf_cg *cg) {
  double s = sqrt(fabs(cg->base.a * cg->base.d - cg->base.b * cg->base.c));
  return s > 0 ? s : 1;
}

bdf_status bdf_cg_draw_page(bdf_cg *cg, size_t view, size_t page, CGContextRef ctx,
                            bdf_error *err) {
  bdf_canvas canvas;
  bdf_cg_canvas(cg, ctx, &canvas);
  return bdf_play_page(cg->renderer, view, page, bdf_cg_scale(cg), &canvas, err);
}

bdf_status bdf_cg_draw_region(bdf_cg *cg, size_t view, size_t page, const float region[4],
                              CGContextRef ctx, bdf_error *err) {
  bdf_canvas canvas;
  bdf_cg_canvas(cg, ctx, &canvas);
  return bdf_play_region(cg->renderer, view, page, region, bdf_cg_scale(cg), &canvas, err);
}

bdf_status bdf_cg_draw_part(bdf_cg *cg, const uint8_t hash[16], CGContextRef ctx, bdf_error *err) {
  bdf_canvas canvas;
  bdf_cg_canvas(cg, ctx, &canvas);
  return bdf_play_part(cg->renderer, hash, bdf_cg_scale(cg), &canvas, err);
}
