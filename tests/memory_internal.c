/* Allocation fault injection and requested-heap high-water marks. The
 * amalgamation lets every allocation in our code use this allocator,
 * without changing the public ABI or relying on platform interposition.
 * Brotli's own allocations are outside these measurements. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef union allocation {
  max_align_t alignment;
  struct {
    size_t size;
  } value;
} allocation;

static size_t calls, fail_at, live, peak, blocks;
static int fail_after;

static void *tracked_malloc(size_t n) {
  calls++;
  if (fail_at && (calls == fail_at || (fail_after && calls > fail_at))) {
    return NULL;
  }
  if (n > SIZE_MAX - sizeof(allocation)) {
    return NULL;
  }
  allocation *a = malloc(sizeof *a + n);
  if (!a) {
    abort();
  }
  a->value.size = n;
  live += n;
  blocks++;
  if (live > peak) {
    peak = live;
  }
  return a + 1;
}

static void tracked_free(void *p) {
  if (p) {
    allocation *a = (allocation *)p - 1;
    live -= a->value.size;
    blocks--;
    free(a);
  }
}

static void *tracked_calloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) {
    return NULL;
  }
  void *p = tracked_malloc(n * size);
  if (p) {
    memset(p, 0, n * size);
  }
  return p;
}

static void *tracked_realloc(void *old, size_t n) {
  /* Always move: expose stale pointers and include the transient copy in
   * peak memory. A failed realloc leaves the old allocation alive. */
  void *p = tracked_malloc(n);
  if (p && old) {
    size_t size = ((allocation *)old - 1)->value.size;
    memcpy(p, old, size < n ? size : n);
    tracked_free(old);
  }
  return p;
}

#define malloc  tracked_malloc
#define calloc  tracked_calloc
#define realloc tracked_realloc
#define free    tracked_free
#define BDF_IMPLEMENTATION
#include "bdf/bdf.h"
#undef BDF_IMPLEMENTATION
#undef malloc
#undef calloc
#undef realloc
#undef free

#include "render_vectors.h"
#include "testutil.h"

static wr png_fixture;

static void png_chunk(wr *w, const char *type, const uint8_t *p, size_t n) {
  uint8_t bytes[4];
  bdfi_put32(bytes, (uint32_t)n);
  w_bytes(w, bytes, 4);
  w_bytes(w, type, 4);
  w_bytes(w, p, n);
  uint32_t crc = bdfi_png_crc(0xffffffffu, (const uint8_t *)type, 4);
  bdfi_put32(bytes, ~bdfi_png_crc(crc, p, n));
  w_bytes(w, bytes, 4);
}

static void make_png(void) {
  const size_t width = 1024, height = 1024, stride = width * 8 + 1;
  size_t n = stride * height;
  uint8_t *raw = malloc(n);
  memset(raw, 0xff, n);
  for (size_t y = 0; y < height; y++) {
    raw[y * stride] = 0;
  }
  wr z = {0};
  w_u8(&z, 0x78);
  w_u8(&z, 0x01);
  for (size_t off = 0; off < n;) {
    size_t take = n - off < 65535 ? n - off : 65535;
    w_u8(&z, off + take == n);
    w_u16(&z, (uint16_t)take);
    w_u16(&z, (uint16_t)~take);
    w_bytes(&z, raw + off, take);
    off += take;
  }
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) {
    a = (a + raw[i]) % 65521;
    b = (b + a) % 65521;
  }
  uint8_t sum[4];
  bdfi_put32(sum, b << 16 | a);
  w_bytes(&z, sum, 4);
  uint8_t ihdr[13] = {0};
  bdfi_put32(ihdr, (uint32_t)width);
  bdfi_put32(ihdr + 4, (uint32_t)height);
  ihdr[8] = 16;
  ihdr[9] = 6;
  w_bytes(&png_fixture, "\x89PNG\r\n\x1a\n", 8);
  png_chunk(&png_fixture, "IHDR", ihdr, sizeof ihdr);
  png_chunk(&png_fixture, "IDAT", z.b, z.n);
  png_chunk(&png_fixture, "IEND", NULL, 0);
  free(z.b);
  free(raw);
}

static void png_image(void) {
  bdfi_bitmap image;
  bdf_status st = bdfi_png_decode(png_fixture.b, png_fixture.n, &image);
  if (st == BDF_OK) {
    CHECK(image.w == 1024 && image.h == 1024);
    for (size_t i = 0; i < 4 * 1024 * 1024; i++) {
      CHECK(image.pix[i] == 255);
    }
    bdf_free(image.pix);
  } else {
    CHECK(fail_at && st == BDF_ERR_NOMEM);
    CHECK(image.pix == NULL && image.w == 0 && image.h == 0);
  }
}

static void cache_growth(void) {
  /* Model adding a segment between draws; realloc always moves here. */
  bdf_doc doc;
  memset(&doc, 0, sizeof doc);
  bdfi_part parts[2];
  memset(parts, 0, sizeof parts);
  parts[1].hash[0] = 1;
  size_t order[2] = {0, 1};
  doc.m.parts = parts;
  doc.m.nparts = doc.total_parts = 1;
  doc.order = order;
  bdf_renderer *r = bdfi_renderer_alloc(&doc, NULL);
  if (!r) {
    return;
  }
  bdfi_robj *slot = bdfi_r_part_slot(r, parts[0].hash);
  if (slot) {
    bdfi_picture *p = tracked_calloc(1, sizeof *p);
    if (p) {
      p->w = p->h = 1;
      if (bdfi_r_keep(r, p, &slot->pic, &slot->pic_state)) {
        slot->pic = p;
        slot->pic_state = 1;
        doc.m.nparts = doc.total_parts = 2;
        if (bdfi_r_part_slot(r, parts[1].hash)) {
          r->drawing++;
          r->room = 1;
          CHECK(bdfi_r_make_room(r, 1));
          CHECK(r->parts[0].pic == NULL && r->parts[0].pic_state == 0);
        }
      } else {
        bdfi_picture_free(p);
      }
    }
  }
  bdf_renderer_free(r);
}

static void sorting(void) {
  bdfi_manifest manifest;
  memset(&manifest, 0, sizeof manifest);
  manifest.nparts = 4096;
  manifest.parts = calloc(manifest.nparts, sizeof *manifest.parts);
  for (size_t i = 0; i < manifest.nparts; i++) {
    manifest.parts[i].hash[0] = (uint8_t)((i * 73) % 128);
  }
  int oom = 0;
  size_t *order = bdfi_order_of(&manifest, &oom);
  if (order) {
    for (size_t i = 1; i < manifest.nparts; i++) {
      CHECK(memcmp(manifest.parts[order[i - 1]].hash, manifest.parts[order[i]].hash, 16) <= 0);
    }
    for (size_t i = manifest.nparts - 128; i < manifest.nparts; i++) {
      CHECK(bdfi_find_in(&manifest, order, manifest.parts[i].hash) == &manifest.parts[i]);
    }
    uint8_t missing[16] = {255};
    CHECK(bdfi_find_in(&manifest, order, missing) == NULL);
  } else {
    CHECK(fail_at && oom);
  }
  bdf_free(order);
  free(manifest.parts);
}

static void fonts(void) {
  bdf_fonts *set = NULL;
  bdf_error err;
  if (bdf_fonts_new(&set, &err) == BDF_OK) {
    for (size_t i = 0; i < RV_FONTS; i++) {
      bdf_fonts_add(set, rv_fonts + rv_font[i].off, rv_font[i].len, &err);
    }
    bdf_fonts_free(set);
  }
}

static void svg(void) {
  static const char input[] =
      "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>"
      "<style>.a{fill:red;stroke:blue}</style><defs><path id='p' d='M0 0L8 8Z'/></defs>"
      "<g opacity='.5'><use href='#p' class='a'/><text x='1' y='9'>test</text></g></svg>";
  bdf_status st;
  bdfi_svg_image *image = bdfi_svg_image_new((const uint8_t *)input, sizeof input - 1, &st);
  if (!fail_at)
    CHECK(image != NULL && st == BDF_OK);
  bdfi_svg_image_free(image);
}

static void document(void) {
  bdf_doc *doc = NULL;
  bdf_renderer *r = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(rv_document, rv_document_len, &doc, &err) != BDF_OK) {
    CHECK(doc == NULL);
    return;
  }
  bdf_text_index *text = NULL;
  bdf_doc_view_text(doc, 0, 0, &text, &err);
  bdf_text_index_free(text);
  bdf_search_text *search = NULL;
  bdf_doc_search_text(doc, &search, &err);
  bdf_search_text_free(search);
  if (bdf_renderer_new(doc, NULL, &r, &err) == BDF_OK) {
    bdf_image image = {0};
    bdf_render_page_size(r, 0, 0, 32, 32, &image, &err);
    bdf_image_free(&image);
    bdf_renderer_free(r);
  }
  bdf_doc_free(doc);
}

static size_t glyph_points = 3;

static void glyph(void) {
  /* 203 points makes a two-byte coordinate straddle the 1024-byte
   * allocation boundary. A failed growth used to hang in padding. */
  uint8_t full[450] = {0};
  size_t points = glyph_points;
  bdfi_put16(full + 4, 1);
  const uint32_t lengths[7] = {2, 1, (uint32_t)points, (uint32_t)points + 1, 0, 4, 0};
  for (int i = 0; i < 7; i++) {
    bdfi_put32(full + 8 + 4 * i, lengths[i]);
  }
  full[37] = 1;
  full[38] = (uint8_t)points;
  uint8_t *glyf = NULL, *loca = NULL;
  size_t ng = 0, nl = 0;
  bdf_status st = bdfi_woff2_rebuild_glyf(full, 44 + 2 * points, &glyf, &ng, &loca, &nl);
  if (!fail_at) {
    CHECK(st == BDF_OK && ng == ((14 + 5 * points + 3) & ~(size_t)3) && nl == 8);
  }
  bdf_free(glyf);
  bdf_free(loca);
}

static void rendering(void) {
  bdf_object *o = NULL;
  bdf_error err;
  if (bdf_object_decode(rv_objects + rv_object[0].off, rv_object[0].len, &o, &err) != BDF_OK) {
    return;
  }
  bdf_image image = {0};
  bdf_render_object(o, 1, NULL, &image, &err);
  bdf_image_free(&image);
  bdf_object_free(o);
}

static size_t exercise(const char *name, void (*run)(void), int faults) {
  calls = peak = 0;
  fail_at = 0;
  run();
  CHECK(live == 0 && blocks == 0);
  size_t count = calls, high = peak;
  printf("%s: peak=%zu bytes, allocations=%zu\n", name, peak, count);
  if (!faults) {
    return high;
  }
  for (fail_after = 0; fail_after <= 1; fail_after++) {
    for (size_t i = 1; i <= count; i++) {
      calls = peak = 0;
      fail_at = i;
      run();
      if (live || blocks) {
        fprintf(stderr, "%s: failure %zu (persistent=%d), leaked %zu bytes in %zu blocks\n", name,
                i, fail_after, live, blocks);
        abort();
      }
    }
  }
  fail_at = 0;
  return high;
}

static void benchmark(void) {
  make_png();
  clock_t start = clock();
  for (int i = 0; i < 10; i++) {
    png_image();
  }
  printf("PNG RGBA16: %.3f ms/image\n", 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC / 10);
  free(png_fixture.b);
  uint8_t key[32] = {0}, block[16] = {0};
  bdfi_aes aes;
  bdfi_aes256_init(&aes, key);
  start = clock();
  for (int i = 0; i < 65536; i++) {
    bdfi_aes256_encrypt(&aes, block, block);
  }
  printf("AES-256: %.3f ms/MiB (check %u)\n", 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC,
         (unsigned)block[0]);
  bdfi_wipe(&aes, sizeof aes);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
    benchmark();
    return finish();
  }
  int faults = argc < 2 || strcmp(argv[1], "--measure") != 0;
  make_png();
  size_t used = exercise("PNG 1024x1024 RGBA16", png_image, faults);
  CHECK(!faults || used <= 5 * 1024 * 1024);
  free(png_fixture.b);
  used = exercise("WOFF2 small glyph", glyph, faults);
  CHECK(!faults || used <= 16 * 1024);
  if (faults) {
    glyph_points = 203;
    exercise("WOFF2 growth and padding", glyph, 1);
  }
  exercise("document/text/render", document, faults);
  used = exercise("4096 part indices", sorting, faults);
  CHECK(!faults || used <= 4096 * sizeof(size_t));
  exercise("render output", rendering, faults);
  if (faults) {
    exercise("picture cache growth", cache_growth, 1);
    exercise("font database", fonts, 1);
    exercise("SVG document", svg, 1);
  }
  return finish();
}
