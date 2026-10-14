/* Compares what the cairo adapter draws of documents with what libbdf's
 * rasterizer draws of them: compare_cairo [font.ttf ...] [doc.bdf ...] */
#include "bdf/bdf_cairo.h"

#include "../tools/compare.h"

static int draw(void *ctx, bdf_renderer *r, size_t view, size_t page, const float *region,
                double sx, double sy, int w, int h, uint8_t *pixels) {
  (void)ctx;
  bdf_cairo *bc = bdf_cairo_new(r);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);
  cairo_scale(cr, sx, sy);
  bdf_error err;
  int ok = (region ? bdf_cairo_draw_region(bc, view, page, region, cr, &err)
                   : bdf_cairo_draw_page(bc, view, page, cr, &err)) == BDF_OK;
  cairo_destroy(cr);
  bdf_cairo_free(bc);
  cairo_surface_flush(s);
  const unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; y++) {
    const uint32_t *row = (const uint32_t *)(const void *)(data + (size_t)y * (size_t)stride);
    for (int x = 0; x < w; x++) {
      uint8_t *o = pixels + 4 * ((size_t)y * (size_t)w + (size_t)x);
      o[0] = (uint8_t)(row[x] >> 16);
      o[1] = (uint8_t)(row[x] >> 8);
      o[2] = (uint8_t)row[x];
      o[3] = (uint8_t)(row[x] >> 24);
    }
  }
  cairo_surface_destroy(s);
  return ok;
}

int main(int argc, char **argv) {
  /* cairo has no shadows */
  return bdf_compare_main(argc, argv, "cairo", draw, NULL, BDF_COMPARE_NO_SHADOWS, 4, 0.02);
}
