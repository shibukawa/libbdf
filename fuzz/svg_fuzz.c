/* A libFuzzer target over SVG images, through what draws them: the input is
 * a byte of options and an SVG document, which a document of one page draws
 * whole, in part and as a pattern, at two scales, with a font for its text.
 *
 * The fuzz build lowers the pixels of a raster and the elements drawn of an
 * image (BDF_MAX_SVG_RASTER_PIXELS, BDF_MAX_SVG_ELEMENTS): a fuzzer learns
 * nothing from the seconds an image at the limits takes. */
#include "../tests/testutil.h"

#include "../tests/render_vectors.h" /* a font of boxes */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1) {
    return 0;
  }
  uint8_t opts = data[0];
  data++;
  size--;
  docb d = {0};
  wr part = {0}, keep;
  /* what is drawn as an SVG image starts with a "<" */
  if (size == 0 || data[0] != '<') {
    w_bytes(&part, "<svg xmlns=\"http://www.w3.org/2000/svg\">", 40);
  }
  w_bytes(&part, data, size);
  docb_add(&d, 1, "img", &part);
  ob o = {0};
  size_t im = ob_image(&o, 1);
  ob_smoothing(&o, opts & 1);
  ob_draw_image(&o, im, 0, 0, opts & 2 ? 4 : 32, opts & 2 ? 3 : 24);
  const float src[4] = {1, 1, (float)(opts >> 4), 3};
  ob_draw_image_sub(&o, im, src, 16, 0, 8, 8);
  ob_fill_paint(&o, ob_pattern(&o, im, (uint8_t)(opts >> 2 & 3), opts & 2 ? 0.125f : 1, 3, 2));
  ob_rect(&o, BDF_OP_FILL_RECT, 0, 12, 32, 12);
  docb_object(&d, 2, &o);
  char views[256], name[33];
  part_name(2, name);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":[{\"w\":32,\"h\":24,\"layers\":[{\"role\":"
           "\"body\",\"obj\":\"%s\"}]}]}]",
           name);
  bdf_doc *doc = docb_open(&d, views, &keep);
  if (!doc) {
    abort(); /* the document is this file's: it opens */
  }
  bdf_error err;
  bdf_fonts *fonts = NULL;
  if (bdf_fonts_new(&fonts, &err) != BDF_OK) {
    abort();
  }
  if (opts & 0x80) {
    bdf_fonts_add(fonts, rv_font_boxes, sizeof rv_font_boxes, &err);
  }
  bdf_render_options options = {0, 0, fonts};
  bdf_renderer *r = NULL;
  bdf_image img;
  if (bdf_renderer_new(doc, &options, &r, &err) == BDF_OK) {
    /* twice: the second drawing takes another raster of the image, and what
     * the first kept of its elements */
    for (int i = 0; i < 2; i++) {
      if (bdf_render_page(r, 0, 0, i ? 3 : 1, &img, &err) == BDF_OK) {
        volatile unsigned sink = 0;
        size_t n = 4 * (size_t)img.width * (size_t)img.height;
        if (img.width <= 0 || img.height <= 0 || !img.pixels) {
          abort();
        }
        for (size_t k = 0; k < n; k++) {
          sink += img.pixels[k];
        }
        bdf_image_free(&img);
      }
    }
    for (size_t k = 0; k < bdf_renderer_warning_count(r); k++) {
      if (!bdf_renderer_warning(r, k)[0]) {
        abort();
      }
    }
    bdf_renderer_free(r);
  }
  bdf_fonts_free(fonts);
  bdf_doc_free(doc);
  free(keep.b);
  return 0;
}
