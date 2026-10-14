/* Compares what the Core Graphics adapter draws of documents with what
 * libbdf's rasterizer draws of them:
 * compare_coregraphics [font.ttf ...] [doc.bdf ...] */
#include "bdf/bdf_coregraphics.h"

#include "../tools/compare.h"

static int draw(void *ctx, bdf_renderer *r, size_t view, size_t page, const float *region,
                double sx, double sy, int w, int h, uint8_t *pixels) {
  (void)ctx;
  bdf_cg *cg = bdf_cg_new(r);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(pixels, (size_t)w, (size_t)h, 8, 4 * (size_t)w, space,
                                         (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(space);
  if (!c) {
    return 0;
  }
  CGContextSetRGBFillColor(c, 1, 1, 1, 1);
  CGContextFillRect(c, CGRectMake(0, 0, w, h));
  /* the page's origin at the top left, y down */
  CGContextTranslateCTM(c, 0, h);
  CGContextScaleCTM(c, sx, -sy);
  bdf_error err;
  int ok = (region ? bdf_cg_draw_region(cg, view, page, region, c, &err)
                   : bdf_cg_draw_page(cg, view, page, c, &err)) == BDF_OK;
  CGContextRelease(c);
  bdf_cg_free(cg);
  return ok;
}

int main(int argc, char **argv) {
  /* an image that is enlarged is interpolated between its pixels in a way
   * of Core Graphics' own: the mean is further off where a pattern of four
   * pixels fills a third of a page */
  return bdf_compare_main(argc, argv, "coregraphics", draw, NULL, 0, 8, 0.02);
}
