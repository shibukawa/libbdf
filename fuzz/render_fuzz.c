/* A libFuzzer target over the rasterizer. The first byte chooses the
 * background and the scale; the rest is an object, which is drawn alone
 * when it needs no other part, or a document, of which the first pages,
 * strips and a region of each sheet are drawn into small images.
 *
 * The fuzz build lowers what a drawing may take at its limits
 * (BDF_MAX_REUSED_DRAWN, BDF_MAX_SHADOW_BLUR). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "bdf/bdf.h"

/* Reads every byte of an image: it has as many as it says. */
static void check(const bdf_image *img) {
  volatile unsigned sink = 0;
  size_t n = 4 * (size_t)img->width * (size_t)img->height;
  if (img->width <= 0 || img->height <= 0 || !img->pixels) {
    abort();
  }
  for (size_t i = 0; i < n; i++) {
    sink += img->pixels[i];
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2) {
    return 0;
  }
  bdf_render_options opts = {data[0] & 1, data[0] & 2 ? 0xffffffffu : 0x20408080u, NULL};
  double scale = 0.25 + (double)(data[0] >> 2 & 7) * 0.25;
  data++;
  size--;
  bdf_error err;
  bdf_image img;
  bdf_object *obj = NULL;
  if (bdf_object_decode(data, size, &obj, &err) == BDF_OK) {
    float box[4];
    bdf_object_bbox(obj, box);
    /* a picture of a few thousand pixels says as much as one of millions */
    if (box[2] > 0 && box[3] > 0 && box[2] * scale <= 256 && box[3] * scale <= 256 &&
        bdf_render_object(obj, scale, &opts, &img, &err) == BDF_OK) {
      check(&img);
      bdf_image_free(&img);
    }
    bdf_object_free(obj);
  }
  bdf_doc *doc = NULL;
  if (bdf_doc_open_memory(data, size, &doc, &err) != BDF_OK) {
    return 0;
  }
  bdf_renderer *r = NULL;
  if (bdf_renderer_new(doc, &opts, &r, &err) == BDF_OK) {
    size_t views = bdf_doc_view_count(doc);
    for (size_t v = 0; v < views && v < 3; v++) {
      bdf_view view;
      if (bdf_doc_view(doc, v, &view) != BDF_OK) {
        continue;
      }
      const float region[4] = {0, 0, 300, 200};
      if (view.kind_code == BDF_VIEW_SHEET || view.kind_code == BDF_VIEW_SCROLL) {
        if (bdf_render_region(r, v, 0, region, 60, 40, &img, &err) == BDF_OK) {
          check(&img);
          bdf_image_free(&img);
        }
      }
      for (size_t p = 0; p < view.npages && p < 2; p++) {
        if (bdf_render_page_size(r, v, p, 48, 64, &img, &err) == BDF_OK) {
          check(&img);
          bdf_image_free(&img);
        }
      }
    }
    for (size_t i = 0; i < bdf_renderer_warning_count(r); i++) {
      if (!bdf_renderer_warning(r, i)[0]) {
        abort();
      }
    }
    bdf_renderer_free(r);
  }
  bdf_doc_free(doc);
  return 0;
}
