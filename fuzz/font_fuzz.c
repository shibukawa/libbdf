/* A libFuzzer target over the fonts of the rasterizer: the input is a byte
 * of options and a font program, which a document carries and draws text
 * in (filled and stroked, in both directions, made bold and slanted), and
 * which a renderer is also given as one of the application's fonts, to be
 * found by name and for the characters the first lacks. */
#include "../tests/testutil.h"

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
  w_bytes(&part, data, size);
  docb_add(&d, 1, "font", &part);
  ob o = {0};
  size_t carried = ob_font_part(&o, 1, opts & 1 ? 700 : 400, opts & 2 ? BDF_STYLE_ITALIC : BDF_STYLE_NORMAL);
  size_t named = ob_font(&o, opts & 4 ? "serif" : "No Such, 'Test', monospace");
  static const char *const strings[4] = {"Hamburgefonstiv 0123", "a\tb \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d 12",
                                         "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9d\x92\x9c", "A"};
  ob_set_font(&o, carried, 8 + (float)(opts >> 6) * 9);
  ob_text_style(&o, (uint8_t)(opts >> 3 & 3), (uint8_t)(opts >> 5 & 3), opts & 8 ? BDF_DIR_RTL : BDF_DIR_LTR, 0.5f);
  ob_text(&o, strings[opts >> 4 & 3], 4, 20, opts & 16 ? 60 : 0);
  ob_stroke_text(&o, strings[opts >> 2 & 3], 4, 44, 0);
  ob_set_font(&o, named, 12);
  ob_text(&o, strings[opts & 3], 4, 62, 0);
  docb_object(&d, 2, &o);
  char views[256], name[33];
  part_name(2, name);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":[{\"w\":96,\"h\":72,\"layers\":[{\"role\":"
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
  bdf_fonts_add(fonts, data, size, &err); /* a font or not */
  bdf_render_options with = {0, 0, fonts};
  for (int pass = 0; pass < 2; pass++) {
    bdf_renderer *r = NULL;
    bdf_image img;
    if (bdf_renderer_new(doc, pass ? &with : NULL, &r, &err) != BDF_OK) {
      continue;
    }
    if (bdf_render_page(r, 0, 0, 1, &img, &err) == BDF_OK) {
      volatile unsigned sink = 0;
      size_t n = 4 * (size_t)img.width * (size_t)img.height;
      for (size_t k = 0; k < n; k++) {
        sink += img.pixels[k];
      }
      bdf_image_free(&img);
    }
    bdf_renderer_free(r);
  }
  bdf_fonts_free(fonts);
  bdf_doc_free(doc);
  free(keep.b);
  return 0;
}
