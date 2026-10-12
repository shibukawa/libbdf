/* Compares what the QPainter adapter draws of documents with what libbdf's
 * rasterizer draws of them: compare_qt [font.ttf ...] [doc.bdf ...] */
#include "bdf/bdf_qt.h"

#include <QImage>

#include <cstring>

#include "../tools/compare.h"

static int draw(void *ctx, bdf_renderer *r, size_t view, size_t page, double sx, double sy, int w,
                int h, uint8_t *pixels) {
  (void)ctx;
  QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
  if (image.isNull()) {
    return 0;
  }
  image.fill(Qt::white);
  bdf_qt *bq = bdf_qt_new(r);
  bdf_error err;
  bool ok;
  {
    QPainter p(&image);
    p.scale(sx, sy);
    ok = bq && bdf_qt_draw_page(bq, view, page, &p, &err) == BDF_OK;
  }
  bdf_qt_free(bq);
  for (int y = 0; y < h; y++) {
    std::memcpy(pixels + 4 * size_t(y) * size_t(w), image.constScanLine(y), 4 * size_t(w));
  }
  return ok;
}

int main(int argc, char **argv) {
  /* QPainter has no shadows */
  return bdf_compare_main(argc, argv, "qt", draw, nullptr, BDF_COMPARE_NO_SHADOWS, 4, 0.02);
}
