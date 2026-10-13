/* Tests of the rasterizer through the public API: drawings whose pixels can
 * be told by hand, and random drawings and a random document with what the
 * reference's raster/imagebdf makes of them (render_vectors.h, written by
 * go/genvectors on amd64). The second is the test of the claim that libbdf
 * draws the same pixels on every platform and compiler: the hashes are the
 * same whoever builds this. */
#include "testutil.h"

#include "render_vectors.h"

static uint64_t fnv(const uint8_t *data, size_t n) {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ data[i]) * 1099511628211ull;
  }
  return h;
}

/* The pixel at (x, y) as 0xRRGGBBAA. */
static uint32_t pixel(const bdf_image *img, int x, int y) {
  const uint8_t *p = img->pixels + 4 * ((size_t)y * (size_t)img->width + (size_t)x);
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Whether a renderer has warned of something. */
static int warned(const bdf_renderer *r, const char *what) {
  for (size_t i = 0; i < bdf_renderer_warning_count(r); i++) {
    if (strstr(bdf_renderer_warning(r, i), what)) {
      return 1;
    }
  }
  return 0;
}

static bdf_object *box_object(ob *o, float w, float h) {
  o->bbox[2] = w;
  o->bbox[3] = h;
  return ob_decode(o);
}

static void test_reference_objects(void) {
  for (size_t i = 0; i < RV_OBJECTS; i++) {
    bdf_object *obj = NULL;
    bdf_error err;
    bdf_image img;
    bdf_render_options opts = {rv_object[i].has_background, rv_object[i].background, NULL};
    CHECK(bdf_object_decode(rv_objects + rv_object[i].off, rv_object[i].len, &obj, &err) == BDF_OK);
    if (bdf_render_object(obj, rv_object[i].scale, &opts, &img, &err) != BDF_OK) {
      failures++;
      fprintf(stderr, "object %zu: %s\n", i, err.message);
      bdf_object_free(obj);
      continue;
    }
    if (img.width != rv_object[i].w || img.height != rv_object[i].h || img.x != rv_object[i].x ||
        img.y != rv_object[i].y) {
      failures++;
      fprintf(stderr, "object %zu: %d×%d at (%d, %d)\n", i, img.width, img.height, img.x, img.y);
    } else if (fnv(img.pixels, 4 * (size_t)img.width * (size_t)img.height) != rv_object[i].hash) {
      failures++;
      fprintf(stderr, "object %zu: the pixels are not those the reference draws\n", i);
    }
    bdf_image_free(&img);
    bdf_object_free(obj);
  }
}

static void test_reference_document(void) {
  bdf_doc *doc = NULL;
  bdf_renderer *r = NULL;
  bdf_error err;
  CHECK(bdf_doc_open_memory(rv_document, rv_document_len, &doc, &err) == BDF_OK);
  CHECK(bdf_renderer_new(doc, NULL, &r, &err) == BDF_OK);
  /* twice: what a renderer keeps from a drawing changes nothing of the next */
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < RV_RENDERS; i++) {
      bdf_image img;
      bdf_status st;
      if (rv_render[i].scale > 0) {
        st = bdf_render_page(r, rv_render[i].view, rv_render[i].page, rv_render[i].scale, &img, &err);
      } else {
        st = bdf_render_region(r, rv_render[i].view, rv_render[i].page, rv_render[i].region,
                               rv_render[i].w, rv_render[i].h, &img, &err);
      }
      if (st != BDF_OK) {
        failures++;
        fprintf(stderr, "render %zu: %s\n", i, err.message);
        continue;
      }
      if (img.width != rv_render[i].w || img.height != rv_render[i].h) {
        failures++;
        fprintf(stderr, "render %zu: %d×%d, want %d×%d\n", i, img.width, img.height, rv_render[i].w,
                rv_render[i].h);
      } else if (fnv(img.pixels, 4 * (size_t)img.width * (size_t)img.height) != rv_render[i].hash) {
        failures++;
        fprintf(stderr, "render %zu (pass %d): the pixels are not those the reference draws\n", i, pass);
      }
      bdf_image_free(&img);
    }
  }
  bdf_renderer_free(r);
  bdf_doc_free(doc);
}

/* The documents that draw images, one for each format: the decoders give
 * the reference's pixels, and its sampling makes the same of them. */
static void test_reference_images(void) {
  for (size_t d = 0; d < RV_IMAGE_DOCUMENTS; d++) {
    bdf_doc *doc = NULL;
    bdf_renderer *r = NULL;
    bdf_error err;
    CHECK(bdf_doc_open_memory(rv_image_documents + rv_image_document[d].off, rv_image_document[d].len, &doc,
                              &err) == BDF_OK);
    CHECK(bdf_renderer_new(doc, NULL, &r, &err) == BDF_OK);
    for (size_t i = 0; i < RV_IMAGE_RENDERS; i++) {
      if (rv_image_render[i].doc != d) {
        continue;
      }
      bdf_image img;
      bdf_status st;
      if (rv_image_render[i].scale > 0) {
        st = bdf_render_page(r, rv_image_render[i].view, rv_image_render[i].page, rv_image_render[i].scale,
                             &img, &err);
      } else {
        st = bdf_render_region(r, rv_image_render[i].view, rv_image_render[i].page,
                               rv_image_render[i].region, rv_image_render[i].w, rv_image_render[i].h, &img,
                               &err);
      }
      if (st != BDF_OK) {
        failures++;
        fprintf(stderr, "%s images, render %zu: %s\n", rv_image_document[d].format, i, err.message);
        continue;
      }
      if (img.width != rv_image_render[i].w || img.height != rv_image_render[i].h) {
        failures++;
        fprintf(stderr, "%s images, render %zu: %d×%d, want %d×%d\n", rv_image_document[d].format, i,
                img.width, img.height, rv_image_render[i].w, rv_image_render[i].h);
      } else if (fnv(img.pixels, 4 * (size_t)img.width * (size_t)img.height) != rv_image_render[i].hash) {
        failures++;
        fprintf(stderr, "%s images, render %zu: the pixels are not those the reference draws\n",
                rv_image_document[d].format, i);
      }
      bdf_image_free(&img);
    }
    bdf_renderer_free(r);
    bdf_doc_free(doc);
  }
}

/* The documents that draw text, with the fonts made for the tests: the
 * outlines of the fonts, the faces found for the families, and the layout
 * are the reference's. */
static void test_reference_text(void) {
  bdf_error err;
  bdf_fonts *fonts = NULL;
  CHECK(bdf_fonts_new(&fonts, &err) == BDF_OK);
  for (size_t i = 0; i < RV_FONTS; i++) {
    CHECK(bdf_fonts_add(fonts, rv_fonts + rv_font[i].off, rv_font[i].len, &err) == BDF_OK);
  }
  CHECK(bdf_fonts_count(fonts) == RV_FONTS);
  bdf_render_options opts = {0, 0, fonts};
  for (size_t d = 0; d < RV_TEXT_DOCUMENTS; d++) {
    bdf_doc *doc = NULL;
    bdf_renderer *r = NULL;
    CHECK(bdf_doc_open_memory(rv_text_documents + rv_text_document[d].off, rv_text_document[d].len, &doc,
                              &err) == BDF_OK);
    CHECK(bdf_renderer_new(doc, &opts, &r, &err) == BDF_OK);
    for (size_t i = 0; i < RV_TEXT_RENDERS; i++) {
      if (rv_text_render[i].doc != d) {
        continue;
      }
      bdf_image img;
      bdf_status st;
      if (rv_text_render[i].scale > 0) {
        st = bdf_render_page(r, rv_text_render[i].view, rv_text_render[i].page, rv_text_render[i].scale, &img,
                             &err);
      } else {
        st = bdf_render_region(r, rv_text_render[i].view, rv_text_render[i].page, rv_text_render[i].region,
                               rv_text_render[i].w, rv_text_render[i].h, &img, &err);
      }
      if (st != BDF_OK) {
        failures++;
        fprintf(stderr, "text document %zu, render %zu: %s\n", d, i, err.message);
        continue;
      }
      if (img.width != rv_text_render[i].w || img.height != rv_text_render[i].h) {
        failures++;
        fprintf(stderr, "text document %zu, render %zu: %d×%d, want %d×%d\n", d, i, img.width, img.height,
                rv_text_render[i].w, rv_text_render[i].h);
      } else if (fnv(img.pixels, 4 * (size_t)img.width * (size_t)img.height) != rv_text_render[i].hash) {
        failures++;
        fprintf(stderr, "text document %zu, render %zu: the pixels are not those the reference draws\n", d, i);
      }
      bdf_image_free(&img);
    }
    bdf_renderer_free(r);
    bdf_doc_free(doc);
  }
  bdf_fonts_free(fonts);
}

/* Documents with SVG images, as the reference draws them (with the same
 * fonts for their text). */
static void test_reference_svg(void) {
  bdf_error err;
  bdf_fonts *fonts = NULL;
  CHECK(bdf_fonts_new(&fonts, &err) == BDF_OK);
  for (size_t i = 0; i < RV_FONTS; i++) {
    CHECK(bdf_fonts_add(fonts, rv_fonts + rv_font[i].off, rv_font[i].len, &err) == BDF_OK);
  }
  CHECK(bdf_fonts_count(fonts) == RV_FONTS);
  bdf_render_options opts = {0, 0, fonts};
  for (size_t d = 0; d < RV_SVG_DOCUMENTS; d++) {
    bdf_doc *doc = NULL;
    bdf_renderer *r = NULL;
    CHECK(bdf_doc_open_memory(rv_svg_documents + rv_svg_document[d].off, rv_svg_document[d].len, &doc,
                              &err) == BDF_OK);
    CHECK(bdf_renderer_new(doc, &opts, &r, &err) == BDF_OK);
    for (size_t i = 0; i < RV_SVG_RENDERS; i++) {
      if (rv_svg_render[i].doc != d) {
        continue;
      }
      bdf_image img;
      bdf_status st;
      if (rv_svg_render[i].scale > 0) {
        st = bdf_render_page(r, rv_svg_render[i].view, rv_svg_render[i].page, rv_svg_render[i].scale, &img,
                             &err);
      } else {
        st = bdf_render_region(r, rv_svg_render[i].view, rv_svg_render[i].page, rv_svg_render[i].region,
                               rv_svg_render[i].w, rv_svg_render[i].h, &img, &err);
      }
      if (st != BDF_OK) {
        failures++;
        fprintf(stderr, "SVG document %zu, render %zu: %s\n", d, i, err.message);
        continue;
      }
      if (img.width != rv_svg_render[i].w || img.height != rv_svg_render[i].h) {
        failures++;
        fprintf(stderr, "SVG document %zu, render %zu: %d×%d, want %d×%d\n", d, i, img.width, img.height,
                rv_svg_render[i].w, rv_svg_render[i].h);
      } else if (fnv(img.pixels, 4 * (size_t)img.width * (size_t)img.height) != rv_svg_render[i].hash) {
        failures++;
        fprintf(stderr, "SVG document %zu, render %zu: the pixels are not those the reference draws\n", d, i);
      }
      bdf_image_free(&img);
    }
    bdf_renderer_free(r);
    bdf_doc_free(doc);
  }
  bdf_fonts_free(fonts);
}

static void add_part(docb *d, uint32_t id, const char *type, const void *bytes, size_t n) {
  wr w = {0};
  w_bytes(&w, bytes, n);
  docb_add(d, id, type, &w);
}

/* Text in a font whose glyphs are boxes (render_vectors.h says which): where
 * a string is drawn, aligned, on which baseline, spaced, fitted to an
 * advance, in a face made bold or slanted, stroked; in a font the document
 * carries and in one the renderer is given; and what is said of the fonts
 * that cannot be drawn with. */
static void test_text(void) {
  docb d = {0};
  wr keep;
  bdf_error err;
  bdf_image img;
  add_part(&d, 1, "font", rv_font_boxes, sizeof rv_font_boxes);
  add_part(&d, 2, "font", "wOFF and nothing more", 21);
  add_part(&d, 3, "font", rv_font_boxes, 100); /* cut short */

  /* 20: the font the document carries */
  ob a = {0};
  size_t boxes = ob_font_part(&a, 1, 400, BDF_STYLE_NORMAL);
  ob_set_font(&a, boxes, 10);
  ob_text(&a, "AB", 0, 10, 0);
  ob_text_style(&a, BDF_ALIGN_RIGHT, BDF_BASELINE_ALPHABETIC, BDF_DIR_INHERIT, 0);
  ob_text(&a, "B", 30, 28, 0);
  ob_text_style(&a, BDF_ALIGN_LEFT, BDF_BASELINE_TOP, BDF_DIR_INHERIT, 0);
  ob_text(&a, "B", 20, 0, 0);
  ob_text_style(&a, BDF_ALIGN_LEFT, BDF_BASELINE_ALPHABETIC, BDF_DIR_INHERIT, 2);
  ob_text(&a, "AB", 0, 28, 0);
  docb_object(&d, 20, &a);

  /* 21: fitted to an advance, bold, slanted, stroked */
  ob b = {0};
  size_t plain = ob_font_part(&b, 1, 400, BDF_STYLE_NORMAL);
  size_t bold = ob_font_part(&b, 1, 700, BDF_STYLE_NORMAL);
  size_t slanted = ob_font_part(&b, 1, 400, BDF_STYLE_ITALIC);
  ob_set_font(&b, plain, 10);
  ob_text(&b, "A", 0, 10, 20);
  ob_set_font(&b, bold, 10);
  ob_text(&b, "A", 20, 10, 0);
  ob_set_font(&b, slanted, 10);
  ob_text(&b, "A", 0, 28, 0);
  ob_set_font(&b, plain, 10);
  ob_stroke_text(&b, "A", 20, 28, 0);
  docb_object(&d, 21, &b);

  /* 22: a font by name, and one that is not there */
  ob c = {0};
  ob_set_font(&c, ob_font(&c, "'Boxes', serif"), 10);
  ob_text(&c, "A", 0, 10, 0);
  ob_set_font(&c, ob_font(&c, "No Such Family"), 10);
  ob_text(&c, "A", 10, 10, 0);
  docb_object(&d, 22, &c);

  /* 23: fonts that cannot be drawn with */
  ob e = {0};
  for (uint32_t id = 2; id <= 4; id++) {
    ob_set_font(&e, ob_font_part(&e, id, 400, BDF_STYLE_NORMAL), 10); /* 4 is not there */
    ob_text(&e, "A", 0, 10, 0);
  }
  docb_object(&d, 23, &e);

  char views[1024], n20[33], n21[33], n22[33], n23[33];
  part_name(20, n20);
  part_name(21, n21);
  part_name(22, n22);
  part_name(23, n23);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":["
           "{\"w\":30,\"h\":30,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":40,\"h\":30,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":20,\"h\":12,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":20,\"h\":12,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}]}]",
           n20, n21, n22, n23);
  bdf_doc *doc = docb_open(&d, views, &keep);
  bdf_renderer *r = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &r, &err) == BDF_OK);
  const uint32_t ink = 0x000000ff, paper = 0xffffffff;

  CHECK(bdf_render_page(r, 0, 0, 1, &img, &err) == BDF_OK);
  /* "AB" on the baseline y = 10: A is 8 wide and 7 high from x = 1 */
  CHECK(pixel(&img, 1, 3) == ink && pixel(&img, 8, 9) == ink && pixel(&img, 0, 5) == paper && pixel(&img, 9, 5) == paper);
  CHECK(pixel(&img, 5, 2) == paper && pixel(&img, 5, 10) == paper);
  /* and B after its advance of 10: 5 wide, from 2 below the baseline to 8 above */
  CHECK(pixel(&img, 10, 2) == ink && pixel(&img, 14, 11) == ink && pixel(&img, 15, 5) == paper);
  CHECK(pixel(&img, 10, 12) == paper && pixel(&img, 10, 1) == paper);
  /* "B" that ends at x = 30 on the baseline y = 28 */
  CHECK(pixel(&img, 25, 20) == ink && pixel(&img, 29, 29) == ink && pixel(&img, 24, 25) == paper);
  /* "B" hanging from the top of its em box at y = 0: the baseline is 8 below */
  CHECK(pixel(&img, 20, 0) == ink && pixel(&img, 24, 9) == ink && pixel(&img, 20, 10) == paper && pixel(&img, 25, 5) == paper);
  /* "AB" spaced by 2: B starts at 12 */
  CHECK(pixel(&img, 1, 21) == ink && pixel(&img, 10, 25) == paper && pixel(&img, 11, 25) == paper && pixel(&img, 12, 25) == ink);
  CHECK(pixel(&img, 16, 25) == ink && pixel(&img, 17, 25) == paper);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);

  CHECK(bdf_render_page(r, 0, 1, 1, &img, &err) == BDF_OK);
  /* "A" of an advance of 10 fitted to 20: twice as wide, and no higher */
  CHECK(pixel(&img, 2, 3) == ink && pixel(&img, 17, 9) == ink && pixel(&img, 1, 5) == paper && pixel(&img, 18, 5) == paper);
  CHECK(pixel(&img, 9, 2) == paper);
  /* made bold: the box is its own, and the pixels around it are shaded */
  CHECK(pixel(&img, 21, 3) == ink && pixel(&img, 28, 9) == ink);
  CHECK(pixel(&img, 20, 6) != paper && pixel(&img, 20, 6) != ink && pixel(&img, 29, 6) != paper && pixel(&img, 24, 2) != paper);
  CHECK(pixel(&img, 19, 6) == paper && pixel(&img, 24, 1) == paper);
  /* slanted by a quarter of the height: the top row is 1.5 to 1.75 to the
   * right of the baseline, where the box stands on x = 1 to 9 */
  CHECK(pixel(&img, 0, 27) == paper && pixel(&img, 1, 27) != paper && pixel(&img, 2, 27) == ink && pixel(&img, 10, 27) == paper);
  CHECK(pixel(&img, 1, 21) == paper && pixel(&img, 2, 21) != paper && pixel(&img, 2, 21) != ink && pixel(&img, 3, 21) == ink);
  CHECK(pixel(&img, 9, 21) == ink && pixel(&img, 10, 21) != paper && pixel(&img, 10, 21) != ink && pixel(&img, 11, 21) == paper);
  /* stroked: a line one wide around the box, half of it on either side */
  CHECK(pixel(&img, 25, 24) == paper && pixel(&img, 20, 24) == pixel(&img, 21, 24) && pixel(&img, 20, 24) != paper);
  CHECK(pixel(&img, 20, 24) != ink && pixel(&img, 22, 24) == paper && pixel(&img, 19, 24) == paper);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);

  /* without fonts, text that names its font is not drawn */
  CHECK(bdf_render_page(r, 0, 2, 1, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 5, 6) == paper && pixel(&img, 15, 6) == paper);
  CHECK(warned(r, "no font for \"'Boxes', serif\": its text is not drawn"));
  CHECK(warned(r, "no font for \"No Such Family\": its text is not drawn"));
  CHECK(bdf_renderer_warning_count(r) == 2);
  bdf_image_free(&img);
  CHECK(bdf_render_page(r, 0, 3, 1, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 5, 6) == paper);
  CHECK(warned(r, "WOFF 1 fonts are not drawn"));
  CHECK(warned(r, "font 00000003000000000000000000000000 cannot be read"));
  CHECK(warned(r, "font 00000004000000000000000000000000 is missing"));
  CHECK(warned(r, "no font for \"\": its text is not drawn"));
  CHECK(bdf_renderer_warning_count(r) == 6);
  bdf_image_free(&img);
  bdf_renderer_free(r);

  /* with the font: by its name, and in place of a family that is not there */
  bdf_fonts *fonts = NULL;
  CHECK(bdf_fonts_new(&fonts, &err) == BDF_OK);
  CHECK(bdf_fonts_add(fonts, rv_font_boxes, 100, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_fonts_add(fonts, (const uint8_t *)"no font", 7, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_fonts_add_file(fonts, "/no/such/font.ttf", &err) == BDF_ERR_FORMAT);
  CHECK(bdf_fonts_count(fonts) == 0);
  CHECK(bdf_fonts_add(fonts, rv_font_boxes, sizeof rv_font_boxes, &err) == BDF_OK);
  CHECK(bdf_fonts_count(fonts) == 1);
  CHECK(bdf_fonts_new(NULL, &err) == BDF_ERR_ARG && bdf_fonts_add(NULL, rv_font_boxes, 1, &err) == BDF_ERR_ARG);
  CHECK(bdf_fonts_count(NULL) == 0);
  bdf_render_options opts = {0, 0, fonts};
  CHECK(bdf_renderer_new(doc, &opts, &r, &err) == BDF_OK);
  CHECK(bdf_render_page(r, 0, 2, 1, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 1, 3) == ink && pixel(&img, 8, 9) == ink && pixel(&img, 9, 5) == paper);
  CHECK(pixel(&img, 11, 3) == ink && pixel(&img, 18, 9) == ink && pixel(&img, 19, 5) == paper);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);
  /* and a font that cannot be read is drawn in its stead */
  CHECK(bdf_render_page(r, 0, 3, 1, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 1, 3) == ink && pixel(&img, 8, 9) == ink);
  CHECK(bdf_renderer_warning_count(r) == 3);
  bdf_image_free(&img);
  bdf_renderer_free(r);
  bdf_fonts_free(fonts);
  bdf_fonts_free(NULL);
  bdf_doc_free(doc);
  free(keep.b);
}

static void test_pixels(void) {
  bdf_error err;
  bdf_image img;
  /* a rectangle on whole pixels: its colour, and nothing around it */
  ob o = {0};
  ob_fill_color(&o, 0xff0000ff);
  ob_rect(&o, BDF_OP_FILL_RECT, 2, 3, 4, 5);
  /* half a pixel over two: half the alpha in each, the colour multiplied */
  ob_fill_color(&o, 0x0000ffff);
  ob_rect(&o, BDF_OP_FILL_RECT, 7.5f, 0, 1, 1);
  /* a colour that is half transparent over the red */
  ob_fill_color(&o, 0x00ff0080);
  ob_rect(&o, BDF_OP_FILL_RECT, 2, 3, 1, 1);
  bdf_object *obj = box_object(&o, 10, 10);
  CHECK(bdf_render_object(obj, 1, NULL, &img, &err) == BDF_OK);
  CHECK(img.width == 10 && img.height == 10 && img.x == 0 && img.y == 0);
  CHECK(pixel(&img, 0, 0) == 0);
  CHECK(pixel(&img, 1, 3) == 0 && pixel(&img, 6, 3) == 0 && pixel(&img, 2, 2) == 0 && pixel(&img, 2, 8) == 0);
  CHECK(pixel(&img, 3, 3) == 0xff0000ff && pixel(&img, 5, 7) == 0xff0000ff);
  CHECK(pixel(&img, 7, 0) == 0x00008080 && pixel(&img, 8, 0) == 0x00008080);
  /* 128/255 of green over red: the red left is 1 - 128/255 */
  CHECK(pixel(&img, 2, 3) == 0x7f8000ff);
  bdf_image_free(&img);

  /* at twice the scale, on a background */
  bdf_render_options white = {1, 0xffffffff, NULL};
  CHECK(bdf_render_object(obj, 2, &white, &img, &err) == BDF_OK);
  CHECK(img.width == 20 && img.height == 20);
  CHECK(pixel(&img, 0, 0) == 0xffffffff && pixel(&img, 6, 7) == 0xff0000ff && pixel(&img, 11, 15) == 0xff0000ff);
  CHECK(pixel(&img, 12, 15) == 0xffffffff);
  CHECK(pixel(&img, 15, 0) == 0x0000ffff && pixel(&img, 16, 1) == 0x0000ffff && pixel(&img, 17, 0) == 0xffffffff);
  bdf_image_free(&img);
  bdf_object_free(obj);

  /* the image holds the bounding box, wherever it lies from the origin */
  ob b = {0};
  b.bbox[0] = -3.5f;
  b.bbox[1] = 2;
  ob_fill_color(&b, 0x000000ff);
  ob_rect(&b, BDF_OP_FILL_RECT, -3, 2, 1, 1);
  obj = box_object(&b, 5, 3);
  CHECK(bdf_render_object(obj, 1, NULL, &img, &err) == BDF_OK);
  CHECK(img.x == -4 && img.y == 2 && img.width == 6 && img.height == 3);
  CHECK(pixel(&img, 1, 0) == 0x000000ff && pixel(&img, 0, 0) == 0 && pixel(&img, 2, 0) == 0);
  bdf_image_free(&img);
  bdf_object_free(obj);

  /* a clip, a transform kept and restored, and a group at half its alpha */
  ob c = {0};
  ob_op(&c, BDF_OP_SAVE);
  ob_rect(&c, BDF_OP_CLIP_RECT, 0, 0, 4, 4);
  ob_translate(&c, 2, 2);
  ob_fill_color(&c, 0x00ff00ff);
  ob_rect(&c, BDF_OP_FILL_RECT, 0, 0, 6, 6);
  ob_op(&c, BDF_OP_RESTORE);
  ob_op(&c, BDF_OP_GROUP_BEGIN);
  w_f32(&c.ops, 0.5f);
  w_u8(&c.ops, BDF_BLEND_SOURCE_OVER);
  w_f32(&c.ops, 0);
  w_f32(&c.ops, 0);
  w_f32(&c.ops, 8);
  w_f32(&c.ops, 8);
  ob_fill_color(&c, 0xffffffff);
  ob_rect(&c, BDF_OP_FILL_RECT, 6, 6, 2, 2);
  ob_op(&c, BDF_OP_GROUP_END);
  obj = box_object(&c, 8, 8);
  CHECK(bdf_render_object(obj, 1, NULL, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 2, 2) == 0x00ff00ff && pixel(&img, 3, 3) == 0x00ff00ff);
  CHECK(pixel(&img, 4, 4) == 0 && pixel(&img, 1, 1) == 0 && pixel(&img, 5, 2) == 0);
  CHECK(pixel(&img, 6, 6) == 0x80808080 && pixel(&img, 7, 7) == 0x80808080 && pixel(&img, 5, 6) == 0);
  bdf_image_free(&img);
  bdf_object_free(obj);
}

/* 2x2: red, green; blue, white at half alpha */
static const uint8_t png_2x2[76] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78, 0x9c, 0x51, 0x17, 0x92,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
/* 2x1: red, blue, with an EXIF orientation of 6 (turned a quarter clockwise) */
static const uint8_t png_turned[109] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf4, 0x22, 0x7f,
    0x8a, 0x00, 0x00, 0x00, 0x1a, 0x65, 0x58, 0x49, 0x66, 0x4d, 0x4d, 0x00, 0x2a, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x01, 0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xd6, 0x67, 0x4b, 0x69, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x42, 0xff, 0x01, 0x0f, 0xf9, 0x03, 0xfd, 0x98, 0x79, 0xd7,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
/* a header that states 20000 x 20000 pixels */
static const uint8_t png_huge[45] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x4e, 0x20, 0x00, 0x00, 0x4e, 0x20, 0x08, 0x06, 0x00, 0x00, 0x00, 0xe3, 0x70, 0x46,
    0x39, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
/* 2x1, gray: black, white */
static const uint8_t png_gray[68] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0xd1, 0x49, 0x20,
    0x56, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0xf8, 0x0f, 0x00,
    0x01, 0x02, 0x01, 0x00, 0xd1, 0x1a, 0xcb, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82,
};

static void add_image(docb *d, uint32_t id, const void *bytes, size_t n) {
  wr w = {0};
  w_bytes(&w, bytes, n);
  docb_add(d, id, "img", &w);
}

/* Images: drawn pixel for pixel and smoothed, a part of one, as patterns,
 * turned by their EXIF orientation, and what is said of those that are not
 * drawn. */
static void test_images(void) {
  docb d = {0};
  wr keep;
  bdf_error err;
  bdf_image img;
  static const char avif[] = "\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1miaf";
  static const char svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
  add_image(&d, 1, png_2x2, sizeof png_2x2);
  add_image(&d, 2, png_turned, sizeof png_turned);
  add_image(&d, 3, png_huge, sizeof png_huge);
  add_image(&d, 4, png_gray, sizeof png_gray);
  add_image(&d, 5, avif, sizeof avif - 1);
  add_image(&d, 6, svg, sizeof svg - 1);
  add_image(&d, 7, png_2x2, sizeof png_2x2 - 20); /* cut short */
  add_image(&d, 8, "what is this", 12);

  /* 20: on black, the image pixel for pixel, its green pixel alone, the
   * image as a pattern that repeats and as one that does not */
  ob a = {0};
  size_t im = ob_image(&a, 1);
  size_t tiles = ob_pattern(&a, im, BDF_REPEAT, 1, 0, 0);
  size_t once = ob_pattern(&a, im, BDF_NO_REPEAT, 1, 5, 5);
  ob_fill_color(&a, 0x000000ff);
  ob_rect(&a, BDF_OP_FILL_RECT, 0, 0, 8, 8);
  ob_smoothing(&a, 0);
  ob_draw_image(&a, im, 0, 0, 4, 4);
  const float green[4] = {1, 0, 1, 1};
  ob_draw_image_sub(&a, im, green, 4, 0, 2, 2);
  ob_fill_paint(&a, tiles);
  ob_rect(&a, BDF_OP_FILL_RECT, 0, 4, 4, 4);
  ob_fill_paint(&a, once);
  ob_rect(&a, BDF_OP_FILL_RECT, 4, 4, 4, 4);
  docb_object(&d, 20, &a);

  /* 21: two gray pixels stretched over eight, smoothed (as images are until
   * an object says otherwise), and the turned image */
  ob b = {0};
  size_t gray = ob_image(&b, 4), turned = ob_image(&b, 2);
  ob_draw_image(&b, gray, 0, 0, 8, 1);
  ob_smoothing(&b, 0);
  ob_draw_image(&b, turned, 0, 2, 1, 2);
  docb_object(&d, 21, &b);

  /* 22: what is not drawn */
  ob c = {0};
  for (uint32_t id = 3; id <= 9; id++) {
    if (id != 4) {
      ob_draw_image(&c, ob_image(&c, id), 0, 0, 2, 2); /* 9 is not there */
    }
  }
  ob_draw_image(&c, 40, 0, 0, 2, 2);
  ob_fill_paint(&c, ob_pattern(&c, 40, BDF_REPEAT, 1, 0, 0)); /* a pattern of no image: nothing */
  ob_rect(&c, BDF_OP_FILL_RECT, 0, 0, 2, 2);
  docb_object(&d, 22, &c);

  char views[1024], n20[33], n21[33], n22[33];
  part_name(20, n20);
  part_name(21, n21);
  part_name(22, n22);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":["
           "{\"w\":8,\"h\":8,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":8,\"h\":4,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":2,\"h\":2,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}]}]",
           n20, n21, n22);
  bdf_doc *doc = docb_open(&d, views, &keep);
  bdf_renderer *r = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &r, &err) == BDF_OK);

  const uint32_t red = 0xff0000ff, lime = 0x00ff00ff, blue = 0x0000ffff, black = 0x000000ff;
  const uint32_t half = 0x808080ff; /* white at half alpha, on black */
  CHECK(bdf_render_page(r, 0, 0, 1, &img, &err) == BDF_OK);
  CHECK(img.width == 8 && img.height == 8);
  /* each pixel of the image is four of the page */
  CHECK(pixel(&img, 0, 0) == red && pixel(&img, 1, 1) == red && pixel(&img, 2, 0) == lime && pixel(&img, 3, 1) == lime);
  CHECK(pixel(&img, 0, 2) == blue && pixel(&img, 1, 3) == blue && pixel(&img, 2, 2) == half && pixel(&img, 3, 3) == half);
  /* the part */
  CHECK(pixel(&img, 4, 0) == lime && pixel(&img, 5, 1) == lime && pixel(&img, 6, 0) == black && pixel(&img, 4, 2) == black);
  /* the pattern, from the origin of the page */
  CHECK(pixel(&img, 0, 4) == red && pixel(&img, 1, 4) == lime && pixel(&img, 2, 4) == red && pixel(&img, 3, 4) == lime);
  CHECK(pixel(&img, 0, 5) == blue && pixel(&img, 1, 5) == half && pixel(&img, 0, 6) == red && pixel(&img, 3, 7) == half);
  /* and the one that is drawn once, where its matrix puts it */
  CHECK(pixel(&img, 5, 5) == red && pixel(&img, 6, 5) == lime && pixel(&img, 5, 6) == blue && pixel(&img, 6, 6) == half);
  CHECK(pixel(&img, 4, 4) == black && pixel(&img, 7, 5) == black && pixel(&img, 5, 7) == black && pixel(&img, 4, 5) == black);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);
  /* twice the size: the image is decoded once and drawn again */
  CHECK(bdf_render_page(r, 0, 0, 2, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 3, 3) == red && pixel(&img, 4, 3) == lime && pixel(&img, 3, 4) == blue && pixel(&img, 7, 7) == half);
  bdf_image_free(&img);

  CHECK(bdf_render_page(r, 0, 1, 1, &img, &err) == BDF_OK);
  /* black to white between the centres of the two pixels, and held outside them */
  static const uint8_t ramp[8] = {0, 0, 32, 96, 159, 223, 255, 255};
  for (int x = 0; x < 8; x++) {
    uint32_t v = ramp[x];
    CHECK(pixel(&img, x, 0) == (v << 24 | v << 16 | v << 8 | 0xff));
  }
  /* a quarter turn clockwise: the red pixel on the left is on top */
  CHECK(pixel(&img, 0, 2) == red && pixel(&img, 0, 3) == blue && pixel(&img, 1, 2) == 0xffffffff);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);

  CHECK(bdf_render_page(r, 0, 2, 1, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 0, 0) == 0xffffffff && pixel(&img, 1, 1) == 0xffffffff);
  CHECK(warned(r, "an image of more than 104857600 pixels is not drawn"));
  CHECK(warned(r, "AVIF images are not drawn"));
  CHECK(warned(r, "an image (png) cannot be decoded"));
  CHECK(warned(r, "an image () cannot be decoded"));
  CHECK(warned(r, "image 00000009000000000000000000000000 is missing"));
  CHECK(warned(r, "bad image reference"));
  /* (the SVG image among them is an empty one: it draws nothing, and is read) */
  CHECK(bdf_renderer_warning_count(r) == 6);
  bdf_image_free(&img);
  /* said once, however often they are drawn */
  CHECK(bdf_render_page(r, 0, 2, 1, &img, &err) == BDF_OK);
  CHECK(bdf_renderer_warning_count(r) == 6);
  bdf_image_free(&img);

  bdf_renderer_free(r);
  bdf_doc_free(doc);
  free(keep.b);
}

/* SVG images: shapes, a translucent element, a clip path, use, a transform,
 * path data, a nested viewport, a gradient, text in a font of the
 * application, an image inside; an SVG image in part and as a pattern; and
 * what is said of what is not drawn. */
static void test_svg(void) {
  static const char shapes[] =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
      "width=\"16\" height=\"16\">"
      "<defs>"
      "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#f00\"/>"
      "<stop offset=\"1\" stop-color=\"#00f\"/></linearGradient>"
      "<clipPath id=\"c\"><rect x=\"8\" y=\"0\" width=\"4\" height=\"4\"/></clipPath>"
      "<rect id=\"r\" width=\"2\" height=\"2\" fill=\"#0f0\"/>"
      "</defs>"
      "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"#f00\"/>"
      "<rect x=\"4\" y=\"0\" width=\"4\" height=\"4\" fill=\"blue\" opacity=\"0.5\"/>"
      "<rect x=\"6\" y=\"0\" width=\"10\" height=\"10\" clip-path=\"url(#c)\"/>"
      "<use xlink:href=\"#r\" x=\"12\" y=\"0\"/>"
      "<g transform=\"translate(0 4) scale(2)\" fill=\"#ff0\"><rect width=\"2\" height=\"2\"/></g>"
      /* the data of a path ends at what is no command: here after its Z */
      "<path d=\"M4 4H8V8H4Z5 L16 16 0 16\" style=\"fill: #0ff\"/>"
      "<circle cx=\"13\" cy=\"6\" r=\"2\" fill=\"none\" stroke=\"#f0f\" stroke-width=\"0.5\"/>"
      "<rect x=\"0\" y=\"8\" width=\"8\" height=\"4\" fill=\"url(#g)\"/>"
      "<svg x=\"8\" y=\"8\" width=\"4\" height=\"4\" viewBox=\"0 0 2 2\">"
      "<rect width=\"1\" height=\"1\" fill=\"#f00\"/>"
      "<rect x=\"1\" y=\"1\" width=\"9\" height=\"9\" fill=\"#00f\"/></svg>"
      "<text x=\"0\" y=\"16\" font-family=\"Boxes\" font-size=\"4\">A</text>"
      "<image x=\"12\" y=\"12\" width=\"4\" height=\"4\" href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAE0lEQVR42mP4z8DwHwyBNAg0AABJSQl4nFEXkgAAAABJRU5ErkJggg==\"/>"
      "</svg>";
  static const char left_out[] =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\">"
      "<defs><pattern id=\"p\" width=\"1\" height=\"1\"><rect width=\"1\" height=\"1\"/></pattern>"
      "<mask id=\"m\"/></defs>"
      "<rect width=\"4\" height=\"4\" fill=\"url(#p)\"/>"
      "<rect width=\"2\" height=\"2\" fill=\"url(#p) #f00\"/>"
      "<rect x=\"2\" width=\"2\" height=\"2\" mask=\"url(#m)\" fill=\"#00f\"/>"
      "<image width=\"1\" height=\"1\" href=\"other.png\"/>"
      "<image width=\"1\" height=\"1\" href=\"data:image/png;base64,AAAA\"/>"
      "</svg>";
  static const char stripes[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"2\">"
                                "<rect width=\"1\" height=\"2\" fill=\"#f00\"/></svg>";
  static const char page[] = "<html><body>not an image</body></html>";
  docb d = {0};
  wr keep;
  bdf_error err;
  bdf_image img;
  add_image(&d, 1, shapes, sizeof shapes - 1);
  add_image(&d, 2, left_out, sizeof left_out - 1);
  add_image(&d, 3, stripes, sizeof stripes - 1);
  add_image(&d, 4, page, sizeof page - 1);

  /* 20: the image at its size */
  ob a = {0};
  ob_draw_image(&a, ob_image(&a, 1), 0, 0, 16, 16);
  docb_object(&d, 20, &a);
  /* 21: its top left corner, four times as large; and stripes as a pattern */
  ob b = {0};
  const float corner[4] = {0, 0, 4, 4};
  ob_draw_image_sub(&b, ob_image(&b, 1), corner, 0, 0, 16, 12);
  ob_fill_paint(&b, ob_pattern(&b, ob_image(&b, 3), BDF_REPEAT, 1, 0, 0));
  ob_rect(&b, BDF_OP_FILL_RECT, 0, 12, 16, 4);
  docb_object(&d, 21, &b);
  /* 22: what is not drawn */
  ob c = {0};
  ob_draw_image(&c, ob_image(&c, 2), 0, 0, 4, 4);
  ob_draw_image(&c, ob_image(&c, 4), 0, 0, 4, 4);
  docb_object(&d, 22, &c);

  char views[1024], n20[33], n21[33], n22[33];
  part_name(20, n20);
  part_name(21, n21);
  part_name(22, n22);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":["
           "{\"w\":16,\"h\":16,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":16,\"h\":16,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":4,\"h\":4,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]}]}]",
           n20, n21, n22);
  bdf_doc *doc = docb_open(&d, views, &keep);
  bdf_fonts *fonts = NULL;
  CHECK(bdf_fonts_new(&fonts, &err) == BDF_OK);
  CHECK(bdf_fonts_add(fonts, rv_font_boxes, sizeof rv_font_boxes, &err) == BDF_OK);
  bdf_render_options opts = {0, 0, fonts};
  bdf_renderer *r = NULL;
  CHECK(bdf_renderer_new(doc, &opts, &r, &err) == BDF_OK);

  const uint32_t red = 0xff0000ff, lime = 0x00ff00ff, blue = 0x0000ffff, black = 0x000000ff;
  const uint32_t white = 0xffffffff, yellow = 0xffff00ff, cyan = 0x00ffffff;
  for (int scale = 1; scale <= 2; scale++) {
    /* twice the size is a raster of its own, of twice the pixels */
    int k = scale;
    CHECK(bdf_render_page(r, 0, 0, scale, &img, &err) == BDF_OK);
    CHECK(img.width == 16 * k && img.height == 16 * k);
    CHECK(pixel(&img, 0, 0) == red && pixel(&img, 4 * k - 1, 4 * k - 1) == red);
    /* blue at half its opacity (128 of 255 in the raster of the image), on
     * the white of the page */
    CHECK(pixel(&img, 4 * k, 0) == 0x7f7fffff && pixel(&img, 8 * k - 1, 4 * k - 1) == 0x7f7fffff);
    /* the black rectangle through its clip path: over the half blue there is none */
    CHECK(pixel(&img, 8 * k, 0) == black && pixel(&img, 11 * k - 1, 4 * k - 1) == black &&
          pixel(&img, 12 * k - 1, 0) == black);
    CHECK(pixel(&img, 7 * k, k) == 0x7f7fffff && pixel(&img, 9 * k, 5 * k) == white);
    /* the rectangle a use element draws, where it puts it */
    CHECK(pixel(&img, 12 * k, 0) == lime && pixel(&img, 14 * k - 1, 2 * k - 1) == lime);
    CHECK(pixel(&img, 14 * k, 0) == white && pixel(&img, 12 * k, 2 * k) == white);
    /* a group moved and made twice as large, with the fill its child takes */
    CHECK(pixel(&img, 0, 4 * k) == yellow && pixel(&img, 4 * k - 1, 8 * k - 1) == yellow);
    /* the path up to its Z: what follows the 5 is not read */
    CHECK(pixel(&img, 4 * k, 4 * k) == cyan && pixel(&img, 8 * k - 1, 8 * k - 1) == cyan);
    CHECK(pixel(&img, 8 * k, 12 * k) == white);
    /* a ring: nothing in its middle */
    CHECK(pixel(&img, 13 * k - 1, 6 * k - 1) == white && pixel(&img, 13 * k - 1, 4 * k) != white);
    /* a gradient over the bounding box, from red to blue */
    uint32_t left = pixel(&img, 0, 9 * k), right = pixel(&img, 8 * k - 1, 9 * k);
    CHECK(left >> 24 > 0xe0 && (left >> 8 & 0xff) < 0x20 && right >> 24 < 0x20 && (right >> 8 & 0xff) > 0xe0);
    /* a viewport of its own: two units of it are four of the image, and what is past it is cut */
    CHECK(pixel(&img, 8 * k, 8 * k) == red && pixel(&img, 10 * k - 1, 10 * k - 1) == red);
    CHECK(pixel(&img, 10 * k, 10 * k) == blue && pixel(&img, 12 * k - 1, 12 * k - 1) == blue);
    CHECK(pixel(&img, 10 * k, 8 * k) == white && pixel(&img, 12 * k, 12 * k - 1) == white);
    /* a box of the font, on its baseline */
    CHECK(pixel(&img, k, 14 * k) == black && pixel(&img, 3 * k - 1, 16 * k - 1) == black);
    CHECK(pixel(&img, 0, 14 * k) != black && pixel(&img, k, 12 * k + k - 1) == white);
    /* the image in it, its four pixels in the corners of its box */
    CHECK(pixel(&img, 12 * k, 12 * k) == red && pixel(&img, 16 * k - 1, 12 * k) == lime);
    CHECK(pixel(&img, 12 * k, 16 * k - 1) == blue && pixel(&img, 16 * k - 1, 16 * k - 1) == white);
    CHECK(bdf_renderer_warning_count(r) == 0);
    bdf_image_free(&img);
  }

  /* a part of it is in the units of its size, and as sharp as it is shown */
  CHECK(bdf_render_page(r, 0, 1, 1, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 0, 0) == red && pixel(&img, 15, 11) == red && pixel(&img, 8, 6) == red);
  /* the pattern: a red stripe and nothing, again and again */
  CHECK(pixel(&img, 0, 12) == red && pixel(&img, 1, 13) == white && pixel(&img, 2, 14) == red &&
        pixel(&img, 15, 15) == white);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);

  CHECK(bdf_render_page(r, 0, 2, 1, &img, &err) == BDF_OK);
  /* the colour after a pattern is drawn in its place, and a mask is not applied */
  CHECK(pixel(&img, 0, 0) == red && pixel(&img, 1, 1) == red && pixel(&img, 2, 0) == blue);
  CHECK(pixel(&img, 0, 2) == white && pixel(&img, 3, 3) == white);
  CHECK(warned(r, "SVG patterns are not drawn"));
  CHECK(warned(r, "SVG masks are not applied"));
  CHECK(warned(r, "SVG images linking to other files are not drawn"));
  CHECK(warned(r, "an image () cannot be decoded"));
  CHECK(warned(r, "an image in an SVG image cannot be decoded"));
  CHECK(warned(r, "an SVG image cannot be read"));
  CHECK(bdf_renderer_warning_count(r) == 6);
  bdf_image_free(&img);

  bdf_renderer_free(r);
  bdf_fonts_free(fonts);
  bdf_doc_free(doc);
  free(keep.b);
}

static void test_pages(void) {
  docb d = {0};
  wr keep;
  bdf_error err;
  bdf_image img;
  /* 1: a square two pages draw; 2 and 3: the pages; 4: text */
  ob shared = {0};
  ob_fill_color(&shared, 0x0000ffff);
  ob_rect(&shared, BDF_OP_FILL_RECT, 0, 0, 2, 2);
  docb_object(&d, 1, &shared);
  ob one = {0};
  ob_use_at(&one, ob_child(&one, 1), 1, 1);
  docb_object(&d, 2, &one);
  ob two = {0};
  ob_use_at(&two, ob_child(&two, 1), 4, 0);
  ob_use(&two, ob_child(&two, 9)); /* an object that is not there */
  docb_object(&d, 3, &two);
  ob words = {0};
  ob_text(&words, "words", 0, 5, 10);
  docb_object(&d, 4, &words);
  char views[1024], n2[33], n3[33], n4[33];
  part_name(2, n2);
  part_name(3, n3);
  part_name(4, n4);
  snprintf(views, sizeof views,
           "[{\"id\":\"p\",\"kind\":\"fixed\",\"pages\":["
           "{\"w\":6,\"h\":4,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"}]},"
           "{\"w\":6,\"h\":4,\"layers\":[{\"role\":\"body\",\"obj\":\"%s\"},{\"role\":\"body\",\"obj\":\"%s\"}]}]},"
           "{\"id\":\"s\",\"kind\":\"sheet\",\"tile\":4,\"cols\":[[3,2]],\"rows\":[[2,3]],\"gridlines\":true,"
           "\"tiles\":{\"1,0\":\"%s\"}}]",
           n2, n3, n4, n2);
  bdf_doc *doc = docb_open(&d, views, &keep);
  bdf_renderer *r = NULL;
  CHECK(bdf_renderer_new(doc, NULL, &r, &err) == BDF_OK);

  /* a page is white where nothing is drawn */
  CHECK(bdf_render_page(r, 0, 0, 1, &img, &err) == BDF_OK);
  CHECK(img.width == 6 && img.height == 4);
  CHECK(pixel(&img, 0, 0) == 0xffffffff && pixel(&img, 1, 1) == 0x0000ffff && pixel(&img, 2, 2) == 0x0000ffff);
  CHECK(pixel(&img, 3, 3) == 0xffffffff);
  CHECK(bdf_renderer_warning_count(r) == 0);
  bdf_image_free(&img);
  /* the second page: what it cannot draw is warned of once, and the rest is drawn */
  CHECK(bdf_render_page(r, 0, 1, 2, &img, &err) == BDF_OK);
  CHECK(img.width == 12 && img.height == 8);
  CHECK(pixel(&img, 8, 0) == 0x0000ffff && pixel(&img, 11, 3) == 0x0000ffff && pixel(&img, 7, 0) == 0xffffffff);
  CHECK(bdf_renderer_warning_count(r) == 2);
  CHECK(strstr(bdf_renderer_warning(r, 0), "is missing") != NULL);
  /* the text is in the font of a canvas, and the renderer was given no fonts */
  CHECK(strcmp(bdf_renderer_warning(r, 1), "no font for \"sans-serif\": its text is not drawn") == 0);
  CHECK(strcmp(bdf_renderer_warning(r, 2), "") == 0);
  bdf_image_free(&img);
  CHECK(bdf_render_page(r, 0, 1, 2, &img, &err) == BDF_OK);
  CHECK(bdf_renderer_warning_count(r) == 2);
  bdf_image_free(&img);

  /* into a size of its own, and a region of it */
  CHECK(bdf_render_page_size(r, 0, 0, 12, 4, &img, &err) == BDF_OK);
  CHECK(img.width == 12 && img.height == 4 && pixel(&img, 2, 1) == 0x0000ffff && pixel(&img, 5, 2) == 0x0000ffff);
  CHECK(pixel(&img, 6, 1) == 0xffffffff);
  bdf_image_free(&img);
  const float part[4] = {1, 1, 2, 2};
  CHECK(bdf_render_region(r, 0, 0, part, 4, 4, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 0, 0) == 0x0000ffff && pixel(&img, 3, 3) == 0x0000ffff);
  bdf_image_free(&img);
  /* what lies outside the page is not the page's white */
  const float wide[4] = {-2, 0, 10, 4};
  CHECK(bdf_render_region(r, 0, 0, wide, 10, 4, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 0, 0) == 0 && pixel(&img, 2, 0) == 0xffffffff && pixel(&img, 8, 3) == 0);
  bdf_image_free(&img);

  /* a sheet: its tile at 4 units, and its gridlines after each column and row */
  double sw, sh;
  bdf_sheet_size(doc, 1, &sw, &sh);
  CHECK(sw == 6 && sh == 6);
  const float cells[4] = {0, 0, 8, 8};
  CHECK(bdf_render_region(r, 1, 0, cells, 8, 8, &img, &err) == BDF_OK);
  CHECK(pixel(&img, 0, 0) == 0xffffffff);
  CHECK(pixel(&img, 2, 0) == 0xd9d9d9ff && pixel(&img, 4, 1) == 0xd9d9d9ff && pixel(&img, 0, 3) == 0xd9d9d9ff);
  CHECK(pixel(&img, 5, 1) == 0x0000ffff && pixel(&img, 6, 2) == 0x0000ffff); /* the tile at (4, 0), its square at (1, 1) */
  CHECK(pixel(&img, 7, 7) == 0xffffffff);
  bdf_image_free(&img);

  /* what is not there, and what is too much */
  CHECK(bdf_render_page(r, 0, 2, 1, &img, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "no such page");
  CHECK(bdf_render_page(r, 5, 0, 1, &img, &err) == BDF_ERR_ARG);
  CHECK(bdf_render_page(r, 1, 0, 1, &img, &err) == BDF_ERR_ARG); /* a sheet has no pages */
  CHECK(bdf_render_page(r, 0, 0, 0, &img, &err) == BDF_ERR_ARG);
  CHECK(bdf_render_page(r, 0, 0, 1e6, &img, &err) == BDF_ERR_TOO_LARGE);
  CHECK(img.pixels == NULL);
  CHECK(bdf_render_page_size(r, 0, 0, 0, 4, &img, &err) == BDF_ERR_ARG);
  CHECK(bdf_render_page_size(r, 0, 0, 100000, 100000, &img, &err) == BDF_ERR_TOO_LARGE);
  const float none[4] = {0, 0, 0, 4};
  CHECK(bdf_render_region(r, 0, 0, none, 4, 4, &img, &err) == BDF_ERR_ARG);
  CHECK(bdf_render_region(r, 0, 0, part, 100000, 100000, &img, &err) == BDF_ERR_TOO_LARGE);
  CHECK(bdf_render_region(r, 0, 9, part, 4, 4, &img, &err) == BDF_ERR_ARG);
  CHECK(bdf_render_page(NULL, 0, 0, 1, &img, &err) == BDF_ERR_ARG);
  CHECK(bdf_render_page(r, 0, 0, 1, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_renderer_new(NULL, NULL, &r, &err) == BDF_ERR_ARG);
  bdf_renderer_free(r);
  bdf_renderer_free(NULL);
  bdf_image_free(NULL);
  CHECK(bdf_renderer_warning_count(NULL) == 0);

  /* an object that needs a document, or draws nothing */
  bdf_object *child = NULL;
  uint8_t *data = NULL;
  size_t len = 0;
  uint8_t name[16];
  CHECK(bdf_hash_parse(n2, 32, name) == BDF_OK);
  CHECK(bdf_doc_part(doc, name, &data, &len, &err) == BDF_OK);
  CHECK(bdf_object_decode(data, len, &child, &err) == BDF_OK);
  CHECK(bdf_render_object(child, 1, NULL, &img, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "the object refers to other parts of a document");
  bdf_object_free(child);
  bdf_free(data);
  ob empty = {0};
  bdf_object *nothing = ob_decode(&empty);
  CHECK(bdf_render_object(nothing, 1, NULL, &img, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "empty object");
  CHECK(bdf_render_object(NULL, 1, NULL, &img, &err) == BDF_ERR_ARG);
  bdf_object_free(nothing);
  bdf_doc_free(doc);
  free(keep.b);
}

int main(void) {
  test_pixels();
  test_pages();
  test_images();
  test_reference_objects();
  test_reference_document();
  test_reference_images();
  test_text();
  test_reference_text();
  test_svg();
  test_reference_svg();
  return finish();
}
