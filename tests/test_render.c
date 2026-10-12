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
    bdf_render_options opts = {rv_object[i].has_background, rv_object[i].background};
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
  bdf_render_options white = {1, 0xffffffff};
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
  CHECK(strcmp(bdf_renderer_warning(r, 1), "text is not drawn") == 0);
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
  test_reference_objects();
  test_reference_document();
  return finish();
}
