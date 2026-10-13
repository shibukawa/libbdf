/* Compares what the Skia adapter draws of documents with what libbdf's
 * rasterizer draws of them: compare_skia [font.ttf ...] [doc.bdf ...] */
#include "bdf/bdf_skia.h"

#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include "../tools/compare.h"

static int draw(void *ctx, bdf_renderer *r, size_t view, size_t page, double sx, double sy, int w,
                int h, uint8_t *pixels) {
  (void)ctx;
  /* straight into the pixels of the comparison: premultiplied RGBA */
  const SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, size_t(4) * size_t(w));
  bdf_skia *bs = bdf_skia_new(r);
  if (!surface || !bs) {
    bdf_skia_free(bs);
    return 0;
  }
  SkCanvas *canvas = surface->getCanvas();
  canvas->clear(SK_ColorWHITE);
  canvas->scale(SkScalar(sx), SkScalar(sy));
  bdf_error err;
  const bool ok = bdf_skia_draw_page(bs, view, page, canvas, &err) == BDF_OK;
  bdf_skia_free(bs);
  return ok;
}

int main(int argc, char **argv) {
  /* Skia blurs a shadow in its own way, and the pages built in have none
   * that is blurred */
  return bdf_compare_main(argc, argv, "skia", draw, nullptr, 0, 4, 0.02);
}
