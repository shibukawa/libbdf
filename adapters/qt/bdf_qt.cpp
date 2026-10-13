/* libbdf through QPainter: see bdf_qt.h. */
#include "bdf/bdf_qt.h"

#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPen>
#include <QTransform>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {

/* A layer that is open: an image of its own, the painter on it, and what it
 * is composited with when it ends. */
struct Layer {
  QImage image;
  std::unique_ptr<QPainter> painter;
  QPainter *outer;
  QPoint origin; /* of the image, in the pixels of the device of outer */
  double alpha;
  QPainter::CompositionMode mode;
};

} // namespace

struct bdf_qt {
  bdf_renderer *renderer = nullptr;
  QPainter *cur = nullptr; /* the painter drawn on: the caller's, or that of a layer */
  std::vector<std::unique_ptr<Layer>> layers;
  /* the images of the bitmaps that have an id; with 1 + BDF_REPEAT* added
   * to 8 times the id, the cell of a pattern with its edges once more */
  QHash<quint64, QImage> images;
  /* An exception of Qt (it throws when it has no memory) must not pass
   * through libbdf, which is C: a canvas function that meets one notes it
   * here, and those that follow do nothing. */
  bool failed = false;
  int saves = 0; /* the saves of the caller's painter that are not restored yet */
};

namespace {

QPainter::CompositionMode mode_of(uint8_t blend) {
  static const QPainter::CompositionMode modes[] = {
      QPainter::CompositionMode_SourceOver,      QPainter::CompositionMode_Multiply,
      QPainter::CompositionMode_Screen,          QPainter::CompositionMode_Overlay,
      QPainter::CompositionMode_Darken,          QPainter::CompositionMode_Lighten,
      QPainter::CompositionMode_ColorDodge,      QPainter::CompositionMode_ColorBurn,
      QPainter::CompositionMode_HardLight,       QPainter::CompositionMode_SoftLight,
      QPainter::CompositionMode_Difference,      QPainter::CompositionMode_Exclusion,
      QPainter::CompositionMode_SourceOver,      QPainter::CompositionMode_SourceOver,
      QPainter::CompositionMode_SourceOver,      QPainter::CompositionMode_SourceOver,
      QPainter::CompositionMode_DestinationOver, QPainter::CompositionMode_DestinationIn,
      QPainter::CompositionMode_DestinationOut,  QPainter::CompositionMode_SourceIn,
      QPainter::CompositionMode_SourceOut,       QPainter::CompositionMode_SourceAtop,
      QPainter::CompositionMode_DestinationAtop, QPainter::CompositionMode_Xor,
      QPainter::CompositionMode_Source,          QPainter::CompositionMode_Plus,
  };
  return blend < sizeof modes / sizeof modes[0] ? modes[blend]
                                                : QPainter::CompositionMode_SourceOver;
}

/* The composite operations that change what is outside the shape too:
 * QPainter composites where the shape is only, so these are drawn on a
 * layer of their own first, which is composited whole. */
bool unbounded(uint8_t blend) {
  return blend == BDF_BLEND_DESTINATION_IN || blend == BDF_BLEND_SOURCE_IN ||
         blend == BDF_BLEND_SOURCE_OUT || blend == BDF_BLEND_DESTINATION_ATOP ||
         blend == BDF_BLEND_COPY;
}

QColor color_of(uint32_t c) {
  return QColor(int(c >> 24), int(c >> 16 & 0xff), int(c >> 8 & 0xff), int(c & 0xff));
}

QTransform transform_of(const double m[6]) { return QTransform(m[0], m[1], m[2], m[3], m[4], m[5]); }

QPainterPath path_of(const bdf_canvas_path *p, uint8_t rule) {
  QPainterPath out;
  const double *c = p->coords;
  for (size_t i = 0; i < p->nverbs; i++) {
    switch (p->verbs[i]) {
    case BDF_VERB_MOVE:
      out.moveTo(c[0], c[1]);
      c += 2;
      break;
    case BDF_VERB_LINE:
      out.lineTo(c[0], c[1]);
      c += 2;
      break;
    case BDF_VERB_QUAD:
      out.quadTo(c[0], c[1], c[2], c[3]);
      c += 4;
      break;
    case BDF_VERB_CUBIC:
      out.cubicTo(c[0], c[1], c[2], c[3], c[4], c[5]);
      c += 6;
      break;
    default:
      out.closeSubpath();
      break;
    }
  }
  out.setFillRule(rule == BDF_FILL_EVENODD ? Qt::OddEvenFill : Qt::WindingFill);
  return out;
}

/* A bitmap as a QImage: its pixels copied, and kept by its id. */
QImage image_of(bdf_qt *bq, const bdf_canvas_bitmap *b) {
  if (b->id != 0) {
    auto it = bq->images.constFind(b->id * 8);
    if (it != bq->images.constEnd()) {
      return it.value();
    }
  }
  QImage img = QImage(b->pixels, b->width, b->height, qsizetype(4) * b->width,
                      QImage::Format_RGBA8888_Premultiplied)
                   .copy();
  if (b->id != 0) {
    bq->images.insert(b->id * 8, img);
  }
  return img;
}

/* The cell of a pattern that does not repeat along both axes: the image
 * with its first and last rows, or columns, once more around it, so that
 * its edge is not mixed with its far side where it is interpolated. */
QImage padded_of(bdf_qt *bq, const bdf_canvas_bitmap *b, uint8_t repeat, int *pad_x, int *pad_y) {
  *pad_x = repeat == BDF_REPEAT_Y || repeat == BDF_NO_REPEAT;
  *pad_y = repeat == BDF_REPEAT_X || repeat == BDF_NO_REPEAT;
  const quint64 key = b->id * 8 + 1 + repeat;
  if (b->id != 0) {
    auto it = bq->images.constFind(key);
    if (it != bq->images.constEnd()) {
      return it.value();
    }
  }
  const int w = b->width + 2 * *pad_x, h = b->height + 2 * *pad_y;
  QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
  for (int y = 0; y < h && !img.isNull(); y++) {
    const int sy = std::min(std::max(y - *pad_y, 0), b->height - 1);
    uchar *row = img.scanLine(y);
    for (int x = 0; x < w; x++) {
      const int sx = std::min(std::max(x - *pad_x, 0), b->width - 1);
      std::memcpy(row + 4 * x, b->pixels + 4 * (size_t(sy) * size_t(b->width) + size_t(sx)), 4);
    }
  }
  if (b->id != 0) {
    bq->images.insert(key, img);
  }
  return img;
}

/* The stops of a gradient in the order of their places, as a canvas keeps
 * them; two at one place are a step, which Qt takes when they are a hair
 * apart. turned: the places counted from the other end. */
QGradientStops stops_of(const bdf_canvas_paint *paint, bool turned) {
  QVector<QPair<double, QColor>> stops;
  for (size_t i = 0; i < paint->nstops; i++) {
    double at = double(paint->stops[i].offset);
    at = at < 0 ? 0 : at > 1 ? 1 : at; /* (and what is no number is 0) */
    stops.append({turned ? 1 - at : at, color_of(paint->stops[i].color)});
  }
  if (turned) {
    std::reverse(stops.begin(), stops.end());
  }
  std::stable_sort(stops.begin(), stops.end(),
                   [](const QPair<double, QColor> &a, const QPair<double, QColor> &b) {
                     return a.first < b.first;
                   });
  QGradientStops out;
  double last = -1;
  for (const auto &s : stops) {
    double at = s.first;
    if (at <= last) {
      at = std::min(1.0, last + 1e-9);
    }
    last = at;
    out.append({at, s.second});
  }
  return out;
}

/* What a style draws with, when that is a colour or a gradient. */
QBrush brush_of(const bdf_canvas_style *style) {
  const bdf_canvas_paint *paint = style->paint;
  if (!paint) {
    return QBrush(color_of(style->color));
  }
  switch (paint->kind) {
  case BDF_PAINT_LINEAR:
    if (paint->ncoords >= 4 && paint->nstops > 0) {
      QLinearGradient g(paint->coords[0], paint->coords[1], paint->coords[2], paint->coords[3]);
      g.setStops(stops_of(paint, false));
      return QBrush(g);
    }
    break;
  case BDF_PAINT_RADIAL:
    if (paint->ncoords >= 6 && paint->nstops > 0) {
      /* from the circle (x0, y0, r0) to the circle (x1, y1, r1) */
      QRadialGradient g(QPointF(paint->coords[3], paint->coords[4]), paint->coords[5],
                        QPointF(paint->coords[0], paint->coords[1]), paint->coords[2]);
      g.setStops(stops_of(paint, false));
      return QBrush(g);
    }
    break;
  case BDF_PAINT_CONIC:
    if (paint->ncoords >= 3 && paint->nstops > 0) {
      /* a canvas turns clockwise from its start angle (in radians), Qt the
       * other way (in degrees) */
      QConicalGradient g(QPointF(paint->coords[1], paint->coords[2]),
                         -double(paint->coords[0]) * 180 / 3.14159265358979323846);
      g.setStops(stops_of(paint, true));
      return QBrush(g);
    }
    break;
  default:
    break;
  }
  return QBrush(Qt::transparent);
}

void begin_layer_device(bdf_qt *bq, QRect device, double alpha, QPainter::CompositionMode mode);
void end_layer_now(bdf_qt *bq);

/* The pixels of the device a painter may draw on. */
QRect device_rect(const QPainter *p) {
  const QPaintDevice *d = p->device();
  const qreal ratio = d->devicePixelRatioF();
  return QRect(0, 0, int(std::ceil(d->width() * ratio)), int(std::ceil(d->height() * ratio)));
}

/* Fills or strokes a path (pen: its line, or nullptr to fill) with a style. */
void draw(bdf_qt *bq, const QPainterPath &path, const QPen *pen, const bdf_canvas_style *style) {
  const bool layered = unbounded(style->blend);
  if (layered) {
    /* the shape alone, then the whole of it composited */
    begin_layer_device(bq, device_rect(bq->cur), 1, mode_of(style->blend));
  }
  QPainter *p = bq->cur;
  p->save();
  p->setOpacity(double(style->alpha));
  if (!layered) {
    p->setCompositionMode(mode_of(style->blend));
  }
  QBrush brush;
  const bdf_canvas_paint *paint = style->paint;
  if (paint && paint->kind == BDF_PAINT_PATTERN) {
    if (!paint->image || paint->image->width <= 0 || paint->image->height <= 0) {
      brush = QBrush(Qt::transparent);
    } else {
      /* a pattern is interpolated between its pixels whatever the
       * smoothing of images is, as the rasterizer draws it */
      p->setRenderHint(QPainter::SmoothPixmapTransform, true);
      const QTransform pm = transform_of(paint->matrix);
      const double w = paint->image->width, h = paint->image->height;
      if (paint->repeat == BDF_REPEAT) {
        brush = QBrush(image_of(bq, paint->image));
        brush.setTransform(pm);
      } else {
        /* it shows in a band, or in its one cell */
        int pad_x, pad_y;
        brush = QBrush(padded_of(bq, paint->image, paint->repeat, &pad_x, &pad_y));
        brush.setTransform(QTransform::fromTranslate(-pad_x, -pad_y) * pm);
        const double far = 1e7;
        QPainterPath band;
        if (paint->repeat == BDF_REPEAT_X) {
          band.addRect(QRectF(-far, 0, 2 * far, h));
        } else if (paint->repeat == BDF_REPEAT_Y) {
          band.addRect(QRectF(0, -far, w, 2 * far));
        } else {
          band.addRect(QRectF(0, 0, w, h));
        }
        p->setClipPath(pm.map(band), Qt::IntersectClip);
      }
    }
  } else {
    brush = brush_of(style);
  }
  if (pen) {
    QPen with(*pen);
    with.setBrush(brush);
    p->strokePath(path, with);
  } else {
    p->fillPath(path, brush);
  }
  p->restore();
  if (layered) {
    end_layer_now(bq);
  }
}

/* ---- The canvas ---------------------------------------------------------- */

/* Runs a canvas function, unless one before it failed. */
template <typename F> void guarded(void *ctx, F f) {
  bdf_qt *bq = static_cast<bdf_qt *>(ctx);
  if (bq->failed) {
    return;
  }
  try {
    f(bq);
  } catch (...) {
    bq->failed = true;
  }
}

void save(void *ctx) {
  guarded(ctx, [](bdf_qt *bq) {
    bq->cur->save();
    bq->saves += bq->layers.empty();
  });
}

void restore(void *ctx) {
  guarded(ctx, [](bdf_qt *bq) {
    bq->cur->restore();
    bq->saves -= bq->layers.empty();
  });
}

void transform(void *ctx, const double m[6]) {
  guarded(ctx, [&](bdf_qt *bq) { bq->cur->setTransform(transform_of(m), true); });
}

void clip(void *ctx, const bdf_canvas_path *path, uint8_t rule) {
  guarded(ctx, [&](bdf_qt *bq) {
    if (path->nverbs == 0) {
      /* nothing is left of the clip */
      bq->cur->setClipRect(QRectF(0, 0, 0, 0), Qt::IntersectClip);
      return;
    }
    bq->cur->setClipPath(path_of(path, rule), Qt::IntersectClip);
  });
}

void fill(void *ctx, const bdf_canvas_path *path, uint8_t rule, const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_qt *bq) { draw(bq, path_of(path, rule), nullptr, style); });
}

void stroke_line(bdf_qt *bq, const bdf_canvas_path *path, const bdf_canvas_line *line,
                 const bdf_canvas_style *style) {
  if (!(line->width > 0)) {
    return; /* (a pen of no width is a hairline to Qt) */
  }
  QPen pen;
  pen.setWidthF(line->width);
  pen.setCapStyle(line->cap == BDF_CAP_ROUND    ? Qt::RoundCap
                  : line->cap == BDF_CAP_SQUARE ? Qt::SquareCap
                                                : Qt::FlatCap);
  pen.setJoinStyle(line->join == BDF_JOIN_ROUND   ? Qt::RoundJoin
                   : line->join == BDF_JOIN_BEVEL ? Qt::BevelJoin
                                                  : Qt::SvgMiterJoin);
  /* A canvas bevels a join whose miter is longer than the limit times the
   * width, from tip to tip; Qt measures from where the outlines of the two
   * lines end, in widths: sqrt(limit² - 1) / 2 is the same corner. */
  const double limit = line->miter;
  pen.setMiterLimit(limit > 1 ? std::sqrt(limit * limit - 1) / 2 : 0);
  if (line->dash && line->ndash >= 2) {
    /* Qt counts dashes in widths of the pen */
    QVector<qreal> dashes;
    for (size_t i = 0; i + 1 < line->ndash; i += 2) {
      dashes.append(double(line->dash[i]) / line->width);
      dashes.append(double(line->dash[i + 1]) / line->width);
    }
    pen.setDashPattern(dashes);
    pen.setDashOffset(line->dash_offset / line->width);
  }
  draw(bq, path_of(path, BDF_FILL_NONZERO), &pen, style);
}

void stroke(void *ctx, const bdf_canvas_path *path, const bdf_canvas_line *line,
            const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_qt *bq) { stroke_line(bq, path, line, style); });
}

void draw_image(bdf_qt *bq, const bdf_canvas_bitmap *bitmap, const double src[4],
                const double dst[4], const bdf_canvas_style *style) {
  if (bitmap->width <= 0 || bitmap->height <= 0) {
    return;
  }
  const QImage img = image_of(bq, bitmap);
  QPainter *p = bq->cur;
  p->save();
  p->setOpacity(double(style->alpha));
  p->setCompositionMode(mode_of(style->blend));
  p->setRenderHint(QPainter::SmoothPixmapTransform, style->smoothing != 0);
  p->drawImage(QRectF(dst[0], dst[1], dst[2], dst[3]), img, QRectF(src[0], src[1], src[2], src[3]));
  p->restore();
}

void image(void *ctx, const bdf_canvas_bitmap *bitmap, const double src[4], const double dst[4],
           const bdf_canvas_style *style) {
  guarded(ctx, [&](bdf_qt *bq) { draw_image(bq, bitmap, src, dst, style); });
}

/* A layer over a rectangle of the device of the painter drawn on. */
void begin_layer_device(bdf_qt *bq, QRect device, double alpha, QPainter::CompositionMode mode) {
  QPainter *outer = bq->cur;
  device = device.intersected(device_rect(outer));
  if (device.isEmpty()) {
    device = QRect(device.topLeft(), QSize(1, 1));
  }
  auto layer = std::make_unique<Layer>();
  layer->image = QImage(device.size(), QImage::Format_ARGB32_Premultiplied);
  layer->image.fill(Qt::transparent);
  layer->outer = outer;
  layer->origin = device.topLeft();
  layer->alpha = alpha;
  layer->mode = mode;
  layer->painter = std::make_unique<QPainter>(&layer->image);
  layer->painter->setRenderHints(outer->renderHints());
  /* the coordinates of the outer painter, on the pixels of the layer */
  layer->painter->setTransform(outer->deviceTransform() *
                               QTransform::fromTranslate(-device.x(), -device.y()));
  bq->cur = layer->painter.get();
  bq->layers.push_back(std::move(layer));
}

void begin_layer(void *ctx, const double bounds[4], float alpha, uint8_t blend) {
  guarded(ctx, [&](bdf_qt *bq) {
    const QRectF device =
        bq->cur->deviceTransform().mapRect(QRectF(bounds[0], bounds[1], bounds[2], bounds[3]));
    begin_layer_device(bq, device.toAlignedRect(), double(alpha), mode_of(blend));
  });
}

void end_layer_now(bdf_qt *bq) {
  if (bq->layers.empty()) {
    return;
  }
  std::unique_ptr<Layer> layer = std::move(bq->layers.back());
  bq->layers.pop_back();
  layer->painter->end();
  QPainter *outer = layer->outer;
  bq->cur = outer;
  outer->save();
  /* pixel for pixel on the device, through the clip there is */
  outer->setTransform(outer->deviceTransform().inverted(), true);
  outer->setOpacity(layer->alpha);
  outer->setCompositionMode(layer->mode);
  outer->drawImage(layer->origin, layer->image);
  outer->restore();
}

void end_layer(void *ctx) {
  guarded(ctx, [](bdf_qt *bq) { end_layer_now(bq); });
}

/* The pixels a unit of the painter is, for what libbdf draws itself. */
double scale_of(const QPainter *p) {
  const QTransform t = p->deviceTransform();
  const double det = std::fabs(t.m11() * t.m22() - t.m12() * t.m21());
  return det > 0 ? std::sqrt(det) : 1;
}

template <typename Play> bdf_status play(bdf_qt *bq, QPainter *painter, bdf_error *err, Play f) {
  bdf_canvas canvas;
  bdf_qt_canvas(bq, painter, &canvas);
  bq->failed = false;
  bq->saves = 0;
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  bdf_status st = f(&canvas, scale_of(painter));
  if (bq->failed) {
    /* what a function that failed left open: its layers, and the saves of
     * the painter */
    while (!bq->layers.empty()) {
      bq->layers.back()->painter->end();
      bq->layers.pop_back();
    }
    for (; bq->saves > 0; bq->saves--) {
      painter->restore();
    }
    if (st == BDF_OK) {
      st = BDF_ERR_NOMEM;
      if (err) {
        err->status = st;
        err->offset = 0;
        err->message = "out of memory";
      }
    }
  }
  bq->cur = painter;
  painter->restore();
  return st;
}

} // namespace

bdf_qt *bdf_qt_new(bdf_renderer *renderer) {
  bdf_qt *bq = new (std::nothrow) bdf_qt;
  if (bq) {
    bq->renderer = renderer;
  }
  return bq;
}

void bdf_qt_free(bdf_qt *bq) { delete bq; }

void bdf_qt_canvas(bdf_qt *bq, QPainter *painter, bdf_canvas *out) {
  std::memset(out, 0, sizeof *out);
  bq->cur = painter;
  out->ctx = bq;
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

bdf_status bdf_qt_draw_page(bdf_qt *bq, size_t view, size_t page, QPainter *painter,
                            bdf_error *err) {
  return play(bq, painter, err, [&](const bdf_canvas *canvas, double scale) {
    return bdf_play_page(bq->renderer, view, page, scale, canvas, err);
  });
}

bdf_status bdf_qt_draw_part(bdf_qt *bq, const uint8_t hash[16], QPainter *painter,
                            bdf_error *err) {
  return play(bq, painter, err, [&](const bdf_canvas *canvas, double scale) {
    return bdf_play_part(bq->renderer, hash, scale, canvas, err);
  });
}
