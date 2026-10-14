/* libbdf through Skia: see bdf_skia.h. */
#include "bdf/bdf_skia.h"

#include "include/core/SkBlendMode.h"
#include "include/core/SkColor.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

struct bdf_skia {
  bdf_renderer *renderer = nullptr;
  SkCanvas *canvas = nullptr;
  SkMatrix base; /* the coordinates of the page on the device: shadows are in its units */
  std::unordered_map<uint64_t, sk_sp<SkImage>> images;
  /* An exception (Skia throws none of its own, the standard library may)
   * must not pass through libbdf, which is C: a canvas function that meets
   * one notes it here, and those that follow do nothing. */
  bool failed = false;
  int saves = 0; /* the saves and layers of the canvas that are not restored yet */
};

namespace {

SkBlendMode mode_of(uint8_t blend) {
  static const SkBlendMode modes[] = {
      SkBlendMode::kSrcOver,    SkBlendMode::kMultiply,   SkBlendMode::kScreen,
      SkBlendMode::kOverlay,    SkBlendMode::kDarken,     SkBlendMode::kLighten,
      SkBlendMode::kColorDodge, SkBlendMode::kColorBurn,  SkBlendMode::kHardLight,
      SkBlendMode::kSoftLight,  SkBlendMode::kDifference, SkBlendMode::kExclusion,
      SkBlendMode::kHue,        SkBlendMode::kSaturation, SkBlendMode::kColor,
      SkBlendMode::kLuminosity, SkBlendMode::kDstOver,    SkBlendMode::kDstIn,
      SkBlendMode::kDstOut,     SkBlendMode::kSrcIn,      SkBlendMode::kSrcOut,
      SkBlendMode::kSrcATop,    SkBlendMode::kDstATop,    SkBlendMode::kXor,
      SkBlendMode::kSrc,        SkBlendMode::kPlus,
  };
  return blend < sizeof modes / sizeof modes[0] ? modes[blend] : SkBlendMode::kSrcOver;
}

/* The composite operations that change what is outside the shape too: Skia
 * composites where the shape is only, so these are drawn on a layer of
 * their own, which is composited whole (as Chrome draws them). */
bool unbounded(uint8_t blend) {
  return blend == BDF_BLEND_DESTINATION_IN || blend == BDF_BLEND_SOURCE_IN ||
         blend == BDF_BLEND_SOURCE_OUT || blend == BDF_BLEND_DESTINATION_ATOP ||
         blend == BDF_BLEND_COPY;
}

SkColor color_of(uint32_t c) {
  return SkColorSetARGB(c & 0xff, c >> 24, c >> 16 & 0xff, c >> 8 & 0xff);
}

SkMatrix matrix_of(const double m[6]) {
  return SkMatrix::MakeAll(SkScalar(m[0]), SkScalar(m[2]), SkScalar(m[4]), SkScalar(m[1]),
                           SkScalar(m[3]), SkScalar(m[5]), 0, 0, 1);
}

SkPath path_of(const bdf_canvas_path *p, uint8_t rule) {
  SkPath out;
  const double *c = p->coords;
  for (size_t i = 0; i < p->nverbs; i++) {
    switch (p->verbs[i]) {
    case BDF_VERB_MOVE:
      out.moveTo(SkScalar(c[0]), SkScalar(c[1]));
      c += 2;
      break;
    case BDF_VERB_LINE:
      out.lineTo(SkScalar(c[0]), SkScalar(c[1]));
      c += 2;
      break;
    case BDF_VERB_QUAD:
      out.quadTo(SkScalar(c[0]), SkScalar(c[1]), SkScalar(c[2]), SkScalar(c[3]));
      c += 4;
      break;
    case BDF_VERB_CUBIC:
      out.cubicTo(SkScalar(c[0]), SkScalar(c[1]), SkScalar(c[2]), SkScalar(c[3]), SkScalar(c[4]),
                  SkScalar(c[5]));
      c += 6;
      break;
    default:
      out.close();
      break;
    }
  }
  out.setFillType(rule == BDF_FILL_EVENODD ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
  return out;
}

/* A bitmap as an SkImage: its pixels copied, and kept by its id. */
sk_sp<SkImage> image_of(bdf_skia *bs, const bdf_canvas_bitmap *b) {
  if (b->id != 0) {
    auto it = bs->images.find(b->id);
    if (it != bs->images.end()) {
      return it->second;
    }
  }
  const SkImageInfo info =
      SkImageInfo::Make(b->width, b->height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkImage> img =
      SkImages::RasterFromPixmapCopy(SkPixmap(info, b->pixels, size_t(4) * size_t(b->width)));
  if (img && b->id != 0) {
    bs->images.emplace(b->id, img);
  }
  return img;
}

/* The stops of a gradient in the order of their places, as a canvas keeps
 * them. */
void stops_of(const bdf_canvas_paint *paint, std::vector<SkColor> *colors,
              std::vector<SkScalar> *places) {
  std::vector<size_t> order(paint->nstops);
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  auto place = [&](size_t i) {
    const float at = paint->stops[i].offset;
    return at > 0 ? (at < 1 ? at : 1.0f) : 0.0f; /* (and what is no number is 0) */
  };
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return place(a) < place(b); });
  for (size_t i : order) {
    colors->push_back(color_of(paint->stops[i].color));
    places->push_back(place(i));
  }
}

/* The shader of a paint, or nullptr for one that paints nothing. */
sk_sp<SkShader> shader_of(bdf_skia *bs, const bdf_canvas_paint *paint) {
  if (paint->kind == BDF_PAINT_PATTERN) {
    if (!paint->image || paint->image->width <= 0 || paint->image->height <= 0) {
      return nullptr;
    }
    sk_sp<SkImage> img = image_of(bs, paint->image);
    if (!img) {
      return nullptr;
    }
    /* along an axis it does not repeat on, a pattern shows in its cell (the
     * caller clips to it), with the pixels of its edge up to the edge; and
     * it is interpolated between its pixels whatever the smoothing of
     * images is, as the rasterizer draws it */
    const bool rx = paint->repeat == BDF_REPEAT || paint->repeat == BDF_REPEAT_X;
    const bool ry = paint->repeat == BDF_REPEAT || paint->repeat == BDF_REPEAT_Y;
    const SkMatrix local = matrix_of(paint->matrix);
    return img->makeShader(rx ? SkTileMode::kRepeat : SkTileMode::kClamp,
                           ry ? SkTileMode::kRepeat : SkTileMode::kClamp,
                           SkSamplingOptions(SkFilterMode::kLinear), &local);
  }
  if (paint->nstops == 0) {
    return nullptr;
  }
  std::vector<SkColor> colors;
  std::vector<SkScalar> places;
  stops_of(paint, &colors, &places);
  const int n = int(colors.size());
  switch (paint->kind) {
  case BDF_PAINT_LINEAR:
    if (paint->ncoords >= 4) {
      const SkPoint pts[2] = {{paint->coords[0], paint->coords[1]},
                              {paint->coords[2], paint->coords[3]}};
      return SkGradientShader::MakeLinear(pts, colors.data(), places.data(), n,
                                          SkTileMode::kClamp);
    }
    break;
  case BDF_PAINT_RADIAL:
    if (paint->ncoords >= 6) {
      return SkGradientShader::MakeTwoPointConical(
          {paint->coords[0], paint->coords[1]}, paint->coords[2],
          {paint->coords[3], paint->coords[4]}, paint->coords[5], colors.data(), places.data(), n,
          SkTileMode::kClamp);
    }
    break;
  case BDF_PAINT_CONIC:
    if (paint->ncoords >= 3) {
      /* a sweep starts on the x axis: turned to the start angle */
      SkMatrix turn;
      turn.setRotate(SkScalar(double(paint->coords[0]) * 180 / 3.14159265358979323846),
                     paint->coords[1], paint->coords[2]);
      return SkGradientShader::MakeSweep(paint->coords[1], paint->coords[2], colors.data(),
                                         places.data(), n, 0, &turn);
    }
    break;
  default:
    break;
  }
  return nullptr;
}

/* The shadow of a style, as the image filter of a paint. */
void shadow_of(bdf_skia *bs, const bdf_canvas_style *style, SkPaint *out) {
  if ((style->shadow_color & 0xff) != 0 &&
      (style->shadow_blur > 0 || style->shadow_dx != 0 || style->shadow_dy != 0)) {
    /* A shadow is in the units of the page whatever the transform is, and
     * an image filter in those of what is drawn: the offset taken back
     * through the transform since the page began, the blur by its scale. */
    SkMatrix local, inverse;
    if (bs->base.invert(&inverse)) {
      local = SkMatrix::Concat(inverse, bs->canvas->getTotalMatrix());
      SkMatrix back;
      if (local.invert(&back)) {
        SkVector offset = {SkScalar(style->shadow_dx), SkScalar(style->shadow_dy)};
        back.mapVectors(&offset, 1);
        const double det = std::fabs(double(local.getScaleX()) * local.getScaleY() -
                                     double(local.getSkewX()) * local.getSkewY());
        const SkScalar sigma = SkScalar(style->shadow_blur / 2 / (det > 0 ? std::sqrt(det) : 1));
        out->setImageFilter(SkImageFilters::DropShadow(offset.x(), offset.y(), sigma, sigma,
                                                       color_of(style->shadow_color), nullptr));
      }
    }
  }
}

/* The paint of a style: its colour or shader, its alpha, and its shadow.
 * false for a style that paints nothing. */
bool paint_of(bdf_skia *bs, const bdf_canvas_style *style, SkPaint *out) {
  out->setAntiAlias(true);
  if (style->paint) {
    sk_sp<SkShader> shader = shader_of(bs, style->paint);
    if (!shader) {
      return false;
    }
    out->setShader(std::move(shader));
    out->setAlphaf(style->alpha);
  } else {
    out->setColor(color_of(style->color));
    out->setAlphaf(out->getAlphaf() * style->alpha);
  }
  shadow_of(bs, style, out);
  return true;
}

/* Clips to the band, or the cell, a pattern that does not repeat along both
 * axes shows in. */
void clip_band(SkCanvas *canvas, const bdf_canvas_paint *paint) {
  if (!paint || paint->kind != BDF_PAINT_PATTERN || paint->repeat == BDF_REPEAT || !paint->image) {
    return;
  }
  const SkScalar w = SkScalar(paint->image->width), h = SkScalar(paint->image->height);
  const SkScalar far = 1e7f;
  SkRect band = SkRect::MakeWH(w, h);
  if (paint->repeat == BDF_REPEAT_X) {
    band = SkRect::MakeLTRB(-far, 0, far, h);
  } else if (paint->repeat == BDF_REPEAT_Y) {
    band = SkRect::MakeLTRB(0, -far, w, far);
  }
  SkPath path;
  path.addRect(band);
  path.transform(matrix_of(paint->matrix));
  canvas->clipPath(path, SkClipOp::kIntersect, true);
}

/* Draws a path with a paint that has its style: within the band of a
 * pattern, and on a layer of its own for a composite operation that reaches
 * past the shape. */
void draw(bdf_skia *bs, const SkPath &path, SkPaint *paint, const bdf_canvas_style *style) {
  SkCanvas *canvas = bs->canvas;
  const bool layered = unbounded(style->blend);
  canvas->save();
  if (layered) {
    SkPaint whole;
    whole.setBlendMode(mode_of(style->blend));
    canvas->saveLayer(nullptr, &whole);
  } else {
    paint->setBlendMode(mode_of(style->blend));
  }
  clip_band(canvas, style->paint);
  canvas->drawPath(path, *paint);
  if (layered) {
    canvas->restore();
  }
  canvas->restore();
}

/* ---- The canvas ---------------------------------------------------------- */

/* Runs a canvas function, unless one before it failed. */
template <typename F> void guarded(void *ctx, F f) {
  bdf_skia *bs = static_cast<bdf_skia *>(ctx);
  if (bs->failed) {
    return;
  }
  try {
    f(bs);
  } catch (...) {
    bs->failed = true;
  }
}

void save(void *ctx) {
  guarded(ctx, [](bdf_skia *bs) {
    bs->canvas->save();
    bs->saves++;
  });
}

void restore(void *ctx) {
  guarded(ctx, [](bdf_skia *bs) {
    bs->canvas->restore();
    bs->saves--;
  });
}

void transform(void *ctx, const double m[6]) {
  guarded(ctx, [&](bdf_skia *bs) { bs->canvas->concat(matrix_of(m)); });
}

void clip(void *ctx, const bdf_canvas_path *path, uint8_t rule) {
  guarded(ctx, [&](bdf_skia *bs) {
    if (path->nverbs == 0) {
      /* nothing is left of the clip */
      bs->canvas->clipRect(SkRect::MakeEmpty());
      return;
    }
    bs->canvas->clipPath(path_of(path, rule), SkClipOp::kIntersect, true);
  });
}

void fill(void *ctx, const bdf_canvas_path *path, uint8_t rule, const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_skia *bs) {
    SkPaint paint;
    if (paint_of(bs, style, &paint)) {
      draw(bs, path_of(path, rule), &paint, style);
    }
  });
}

void stroke(void *ctx, const bdf_canvas_path *path, const bdf_canvas_line *line,
            const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_skia *bs) {
    SkPaint paint;
    if (!(line->width > 0) || !paint_of(bs, style, &paint)) {
      return; /* (a line of no width is a hairline to Skia) */
    }
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(SkScalar(line->width));
    paint.setStrokeCap(line->cap == BDF_CAP_ROUND    ? SkPaint::kRound_Cap
                       : line->cap == BDF_CAP_SQUARE ? SkPaint::kSquare_Cap
                                                     : SkPaint::kButt_Cap);
    paint.setStrokeJoin(line->join == BDF_JOIN_ROUND   ? SkPaint::kRound_Join
                        : line->join == BDF_JOIN_BEVEL ? SkPaint::kBevel_Join
                                                       : SkPaint::kMiter_Join);
    paint.setStrokeMiter(SkScalar(line->miter));
    if (line->dash && line->ndash >= 2) {
      std::vector<SkScalar> dashes(line->dash, line->dash + (line->ndash & ~size_t(1)));
      paint.setPathEffect(
          SkDashPathEffect::Make(dashes.data(), int(dashes.size()), SkScalar(line->dash_offset)));
    }
    draw(bs, path_of(path, BDF_FILL_NONZERO), &paint, style);
  });
}

void image(void *ctx, const bdf_canvas_bitmap *bitmap, const double src[4], const double dst[4],
           const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_skia *bs) {
    if (bitmap->width <= 0 || bitmap->height <= 0) {
      return;
    }
    sk_sp<SkImage> img = image_of(bs, bitmap);
    SkPaint paint;
    bdf_canvas_style plain = *style;
    plain.paint = nullptr;
    plain.color = 0x000000ff; /* an image is drawn with the alpha of the style alone */
    if (!img || !paint_of(bs, &plain, &paint)) {
      return;
    }
    paint.setBlendMode(mode_of(style->blend));
    bs->canvas->drawImageRect(
        img, SkRect::MakeXYWH(SkScalar(src[0]), SkScalar(src[1]), SkScalar(src[2]), SkScalar(src[3])),
        SkRect::MakeXYWH(SkScalar(dst[0]), SkScalar(dst[1]), SkScalar(dst[2]), SkScalar(dst[3])),
        SkSamplingOptions(style->smoothing ? SkFilterMode::kLinear : SkFilterMode::kNearest),
        &paint, SkCanvas::kStrict_SrcRectConstraint);
  });
}

void begin_layer(void *ctx, const double bounds[4], const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_skia *bs) {
    SkPaint whole;
    whole.setAlphaf(style->alpha);
    whole.setBlendMode(mode_of(style->blend));
    shadow_of(bs, style, &whole); // the layer casts the shadow as a whole
    const SkRect r = SkRect::MakeXYWH(SkScalar(bounds[0]), SkScalar(bounds[1]),
                                      SkScalar(bounds[2]), SkScalar(bounds[3]));
    bs->canvas->saveLayer(&r, &whole);
    bs->saves++;
  });
}

void end_layer(void *ctx) {
  guarded(ctx, [](bdf_skia *bs) {
    bs->canvas->restore();
    bs->saves--;
  });
}

template <typename Play> bdf_status play(bdf_skia *bs, SkCanvas *canvas, bdf_error *err, Play f) {
  bdf_canvas functions;
  bdf_skia_canvas(bs, canvas, &functions);
  bs->failed = false;
  bs->saves = 0;
  const int depth = canvas->save();
  const SkMatrix m = canvas->getTotalMatrix();
  /* the pixels a unit of the canvas is, for what libbdf draws itself */
  const double det =
      std::fabs(double(m.getScaleX()) * m.getScaleY() - double(m.getSkewX()) * m.getSkewY());
  bdf_status st = f(&functions, det > 0 ? std::sqrt(det) : 1);
  if (bs->failed && st == BDF_OK) {
    st = BDF_ERR_NOMEM;
    if (err) {
      err->status = st;
      err->offset = 0;
      err->message = "out of memory";
    }
  }
  /* (and what a function that failed left open) */
  canvas->restoreToCount(depth);
  return st;
}

} // namespace

bdf_skia *bdf_skia_new(bdf_renderer *renderer) {
  bdf_skia *bs = new (std::nothrow) bdf_skia;
  if (bs) {
    bs->renderer = renderer;
  }
  return bs;
}

void bdf_skia_free(bdf_skia *bs) { delete bs; }

void bdf_skia_canvas(bdf_skia *bs, SkCanvas *canvas, bdf_canvas *out) {
  std::memset(out, 0, sizeof *out);
  bs->canvas = canvas;
  bs->base = canvas->getTotalMatrix();
  out->ctx = bs;
  out->save = save;
  out->restore = restore;
  out->transform = transform;
  out->clip = clip;
  out->fill = fill;
  out->stroke = stroke;
  out->image = image;
  out->begin_layer = begin_layer;
  out->end_layer = end_layer;
}

bdf_status bdf_skia_draw_page(bdf_skia *bs, size_t view, size_t page, SkCanvas *canvas,
                              bdf_error *err) {
  return play(bs, canvas, err, [&](const bdf_canvas *functions, double scale) {
    return bdf_play_page(bs->renderer, view, page, scale, functions, err);
  });
}

bdf_status bdf_skia_draw_part(bdf_skia *bs, const uint8_t hash[16], SkCanvas *canvas,
                              bdf_error *err) {
  return play(bs, canvas, err, [&](const bdf_canvas *functions, double scale) {
    return bdf_play_part(bs->renderer, hash, scale, functions, err);
  });
}
