/* Small limits exercise rejections without allocating enormous buffers. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#undef BDF_MAX_PART_SIZE
#undef BDF_MAX_MANIFEST_SIZE
#undef BDF_MAX_RENDER_PIXELS
#undef BDF_MAX_JSON_DEPTH
#undef BDF_MAX_WOFF2_SIZE
#undef BDF_MAX_FACE_SEGMENTS
#define BDF_MAX_FACE_SEGMENTS 64
#define BDF_MAX_PART_SIZE     1024
#define BDF_MAX_MANIFEST_SIZE 512
#define BDF_MAX_RENDER_PIXELS 16
#define BDF_MAX_JSON_DEPTH    32
#define BDF_MAX_WOFF2_SIZE    512
#define BDF_IMPLEMENTATION
#include "bdf/bdf.h"
#undef BDF_IMPLEMENTATION
#include "inflate_vectors.h"
#include "render_vectors.h"
#include "testutil.h"

typedef struct stream_input {
  const uint8_t *data;
  size_t len, pos, chunk;
} stream_input;

static int more_input(bdfi_pull *in, size_t want) {
  stream_input *s = in->ctx;
  size_t n = s->len - s->pos;
  if (n > s->chunk)
    n = s->chunk;
  if (n > want)
    n = want;
  in->p = s->data + s->pos;
  in->pos = 0;
  in->n = n;
  s->pos += n;
  return n != 0;
}

typedef struct stream_output {
  const uint8_t *want;
  size_t len, pos;
  int reject;
} stream_output;

static int check_output(void *ctx, const uint8_t *p, size_t n) {
  stream_output *s = ctx;
  if (s->reject)
    return -1;
  CHECK(n <= s->len - s->pos);
  if (n > s->len - s->pos)
    return -1;
  CHECK(memcmp(p, s->want + s->pos, n) == 0);
  s->pos += n;
  return 0;
}

static void check_stream(const uint8_t *data, size_t n, size_t limit) {
  uint8_t *plain = NULL;
  size_t size = 0;
  bdf_error err;
  CHECK(bdf_inflate(data, n, limit, &plain, &size, &err) == BDF_OK);
  const size_t chunks[] = {1, 7, 4096};
  for (size_t c = 0; c < 3; c++) {
    for (int reject = 0; reject < 2; reject++) {
      stream_input src = {data, n, 0, chunks[c]};
      bdfi_pull in = {NULL, 0, 0, more_input, &src};
      stream_output dst = {plain, size, 0, reject};
      size_t count = 0;
      bdf_status st = bdfi_inflate_sink(&in, limit, check_output, &dst, &count);
      CHECK(st == (reject ? BDF_ERR_FORMAT : BDF_OK));
      CHECK(reject || (count == size && dst.pos == size));
    }
  }
  bdf_free(plain);
}

static void put_bits(wr *w, uint32_t *bits, unsigned *have, uint32_t value, unsigned n) {
  *bits |= value << *have;
  *have += n;
  while (*have >= 8) {
    w_u8(w, (uint8_t)*bits);
    *bits >>= 8;
    *have -= 8;
  }
}

static void stream_tests(void) {
  check_stream(hello_fixed, sizeof hello_fixed, 64);
  check_stream(text_dynamic, sizeof text_dynamic, TEXT_PLAIN_LEN);
  check_stream(zeros_packed, sizeof zeros_packed, ZEROS_PLAIN_LEN);
  /* A back-reference at the maximum distance, crossing the window end. */
  wr w = {0};
  w_u8(&w, 0); /* non-final stored block */
  w_u16(&w, 32768);
  w_u16(&w, 32767);
  for (size_t i = 0; i < 32768; i++)
    w_u8(&w, (uint8_t)i);
  uint32_t bits = 0;
  unsigned have = 0;
  put_bits(&w, &bits, &have, 3, 3);    /* final fixed-Huffman block */
  put_bits(&w, &bits, &have, 0xa3, 8); /* symbol 285: 258 bytes */
  put_bits(&w, &bits, &have, 0x17, 5); /* distance symbol 29 */
  put_bits(&w, &bits, &have, 8191, 13);
  put_bits(&w, &bits, &have, 0, 7); /* end of block */
  if (have)
    w_u8(&w, (uint8_t)bits);
  check_stream(w.b, w.n, 32768 + 258);
  free(w.b);
}

static int reads;

static int read_part(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *len) {
  (void)ctx;
  (void)name;
  (void)buf;
  (void)cap;
  (void)len;
  reads++;
  return -1;
}

static void part_limits(void) {
  const char *manifests[] = {
      "{\"bdf\":1,\"opset\":1,\"parts\":[{\"h\":\"00000000000000000000000000000000\","
      "\"t\":\"obj\",\"enc\":\"identity\",\"len\":1025,\"size\":1025}]}",
      "{\"bdf\":1,\"opset\":1,\"parts\":[{\"h\":\"00000000000000000000000000000000\","
      "\"t\":\"obj\",\"enc\":\"deflate-raw\",\"len\":1,\"size\":1025}]}"};
  bdf_source src = {0};
  src.read_part = read_part;
  for (size_t i = 0; i < 2; i++) {
    bdf_doc *doc = NULL;
    bdf_error err;
    CHECK(bdf_doc_open_split((const uint8_t *)manifests[i], strlen(manifests[i]), &src, &doc,
                             &err) == BDF_OK);
    uint8_t hash[16] = {0}, *out = NULL;
    size_t len = 99;
    reads = 0;
    CHECK(bdf_doc_part(doc, hash, &out, &len, &err) == BDF_ERR_TOO_LARGE);
    CHECK(out == NULL && len == 0 && reads == 0);
    bdf_doc_free(doc);
  }
}

/* A face keeps the outlines of its glyphs up to BDF_MAX_FACE_SEGMENTS
 * segments, then lets them go: asked for again, a glyph is read again, the
 * same. */
static void face_outlines(void) {
  size_t evicted = 0;
  for (size_t fi = 0; fi < sizeof rv_font / sizeof rv_font[0]; fi++) {
    bdf_status st;
    bdfi_face *f = bdfi_face_new(rv_fonts + rv_font[fi].off, rv_font[fi].len, 0, 0, &st);
    if (!f) {
      continue;
    }
    int oom = 0;
    for (int pass = 0; pass < 2; pass++) {
      for (uint16_t gid = 0; gid < 300 && !oom; gid++) {
        size_t before = f->kept;
        bdfi_rpath *p = bdfi_face_outline(f, gid, &oom);
        if (!p) {
          continue;
        }
        size_t cost = p->nsegs + p->nsubs;
        if (f->kept < before + cost) {
          evicted++;
        }
        /* used before the next is asked for, as the drawers use it */
        bdfi_rpath *again = bdfi_face_outline(f, gid, &oom);
        CHECK(again && again->nsegs == p->nsegs && again->nsubs == p->nsubs);
        /* one outline over the budget is kept alone */
        CHECK(f->kept <= BDF_MAX_FACE_SEGMENTS || f->kept == cost);
      }
    }
    CHECK(!oom);
    bdfi_face_free(f);
  }
  CHECK(evicted > 0); /* the budget was reached */
}

/* The stops of a gradient sorted by merging when there are many: the same
 * table as sorting the few by insertion, whatever order they come in, and
 * stops of one offset in the order they came. */
static void gradient_stops(void) {
  bdf_stop few[4] = {{0, 0x000000ff}, {0.5f, 0xff0000ff}, {0.5f, 0x0000ffff}, {1, 0xffffffff}};
  bdf_stop many[40], shuffled[40];
  memcpy(many, few, sizeof few);
  for (size_t i = 4; i < 40; i++) {
    many[i].offset = 0.999f - (float)(i - 4) * 0.002f; /* beyond 0.9, descending */
    many[i].color = (uint32_t)(0x01010101u * (uint32_t)i) | 0xff;
  }
  bdfi_lut a, b;
  bdfi_lut_build(&a, few, 4);
  bdfi_lut_build(&b, many, 40);
  /* up to 0.5 the tables are the same, and at 0.5 the first of the two
   * stops there, the red one, in both */
  for (int i = 0; i <= 512; i++) {
    CHECK(memcmp(a.c[i], b.c[i], sizeof a.c[i]) == 0);
  }
  CHECK(a.c[512][0] == 1 && a.c[512][2] == 0);
  /* distinct offsets in any order: one sorted order, so one table */
  for (size_t i = 0; i < 40; i++) {
    many[i].offset = (float)i / 39;
    many[i].color = (uint32_t)(0x01010101u * (uint32_t)(i * 7)) | 0xff;
  }
  for (size_t i = 0; i < 40; i++) {
    shuffled[i] = many[(i * 17 + 5) % 40];
  }
  bdfi_lut_build(&a, many, 40);
  bdfi_lut_build(&b, shuffled, 40);
  CHECK(memcmp(&a, &b, sizeof a) == 0);
}

int main(void) {
  stream_tests();
  part_limits();
  face_outlines();
  gradient_stops();
  bdfi_arena arena = {0};
  CHECK(bdfi_arena_strdup(&arena, "", SIZE_MAX) == NULL);
  CHECK(bdfi_malloc_array(SIZE_MAX / 2 + 1, 2) == NULL);
  CHECK(bdfi_realloc_array(NULL, (size_t)PTRDIFF_MAX / 4 + 1, 4) == NULL);
  int oom = 0;
  CHECK(bdfi_alloc_items((size_t)PTRDIFF_MAX / 4 + 1, 4, &oom) == NULL && oom);

  bdf_doc *doc = NULL;
  bdf_error err;
  uint8_t too_large[513] = {0};
  bdf_source src = {0};
  src.read_part = read_part;
  CHECK(bdf_doc_open_split(too_large, sizeof too_large, &src, &doc, &err) == BDF_ERR_FORMAT);
  CHECK(doc == NULL);
  bdfi_json json;
  uint8_t nested[80];
  memset(nested, '[', 40);
  memset(nested + 40, ']', 40);
  CHECK(bdfi_json_parse(nested, sizeof nested, &json, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "JSON nesting too deep");

  ob o = {0};
  o.bbox[2] = o.bbox[3] = 5;
  bdf_object *obj = ob_decode(&o);
  bdf_image image;
  CHECK(bdf_render_object(obj, 1, NULL, &image, &err) == BDF_ERR_TOO_LARGE);
  CHECK(image.pixels == NULL);
  bdf_object_free(obj);

  bdfi_wbuf w = {0};
  uint8_t data[512] = {0};
  bdfi_wbuf_bytes(&w, data, sizeof data);
  CHECK(w.n == 512 && !w.oom && !w.limited);
  bdfi_wbuf_u8(&w, 0);
  CHECK(w.n == 512 && w.limited);
  free(w.b);
  return finish();
}
