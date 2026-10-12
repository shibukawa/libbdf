/* Tests of the single-file container: the reference's manifest range and
 * part range cases, inflate limits, and reading parts. */
#include "testutil.h"

#include "inflate_vectors.h"

#define HA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void test_manifest_range(void) {
  /* TestOpenSingleManifestRange: the header's manifest range out of the file */
  struct {
    const char *name;
    uint64_t moff, mlen;
  } cases[] = {
      {"negative length", BDF_HEADER_SIZE, (uint64_t)1 << 63},
      {"negative offset", (uint64_t)1 << 63, 8},
      {"sum overflows", INT64_MAX, INT64_MAX},
      {"offset and length cancel", UINT64_MAX - 7, 16},
      {"past the end", BDF_HEADER_SIZE, 1 << 20},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    wr w = {0};
    w_header(&w, 0, cases[i].moff, cases[i].mlen, 0);
    w_text(&w, "{\"bdf\":1,\"parts\":[]}");
    bdf_doc *doc = NULL;
    bdf_error err;
    if (bdf_doc_open_memory(w.b, w.n, &doc, &err) != BDF_ERR_FORMAT) {
      failures++;
      fprintf(stderr, "%s: opened\n", cases[i].name);
      bdf_doc_free(doc);
    }
    free(w.b);
  }
  /* the header alone, and less than the header */
  wr w = {0};
  w_header(&w, 0, BDF_HEADER_SIZE, 0, 0);
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(bdf_doc_open_memory(w.b, w.n, &doc, &err) == BDF_ERR_FORMAT); /* an empty manifest */
  CHECK(bdf_doc_open_memory(w.b, 20, &doc, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "short header");
  CHECK(bdf_doc_open_memory(NULL, 0, &doc, &err) == BDF_ERR_FORMAT);
  CHECK(bdf_doc_open_memory(w.b, w.n, NULL, &err) == BDF_ERR_ARG);
  free(w.b);
}

static void test_part_range(void) {
  /* TestPartRange: entries that reach outside the parts region */
  const char *entries[] = {
      "{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"identity\",\"len\":-1,\"size\":10}",
      "{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"identity\",\"len\":10,\"size\":10,\"off\":-4}",
      "{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"identity\",\"len\":10,\"size\":10,\"off\":9223372036854775807}",
      "{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"identity\",\"len\":11,\"size\":11}",
  };
  for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
    char mj[512];
    snprintf(mj, sizeof mj, "{\"bdf\":1,\"parts\":[%s]}", entries[i]);
    wr w;
    bdf_doc *doc = NULL;
    bdf_error err;
    CHECK(open_single(mj, "0123456789", 10, &doc, &err, &w) == BDF_OK);
    if (doc) {
      uint8_t h[16], *data;
      size_t len;
      memset(h, 0xaa, 16);
      if (bdf_doc_part(doc, h, &data, &len, &err) != BDF_ERR_FORMAT) {
        failures++;
        fprintf(stderr, "part range case %zu: read\n", i);
        bdf_free(data);
      } else {
        CHECK_MSG(err, "part out of range");
      }
      bdf_doc_free(doc);
    }
    free(w.b);
  }
}

static void test_parts(void) {
  /* two parts: one stored as it is, one deflated; the file is tagged "pt" */
  wr parts = {0};
  w_text(&parts, "0123456789");
  w_bytes(&parts, hello_fixed, sizeof hello_fixed);
  char mj[1024];
  snprintf(mj, sizeof mj,
           "{\"bdf\":1,\"opset\":1,\"unit\":\"pt\",\"views\":[{\"id\":\"v\",\"kind\":\"fixed\","
           "\"pages\":[{\"w\":10,\"h\":10,\"layers\":[{\"role\":\"body\",\"obj\":\"" HB "\"}]}]}],"
           "\"parts\":[{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"identity\",\"len\":10,\"size\":10,\"off\":0},"
           "{\"h\":\"" HB "\",\"t\":\"obj\",\"enc\":\"deflate-raw\",\"len\":%zu,\"size\":11,\"off\":10}]}",
           sizeof hello_fixed);
  wr w;
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(open_single(mj, parts.b, parts.n, &doc, &err, &w) == BDF_OK);
  if (doc) {
    uint8_t h[16], *data;
    size_t len;
    memset(h, 0xaa, 16);
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_OK && len == 10 &&
          memcmp(data, "0123456789", 10) == 0);
    bdf_free(data);
    /* the layer's object, through the manifest */
    bdf_layer l;
    CHECK(bdf_doc_layer(doc, 0, 0, 0, &l) == BDF_OK);
    CHECK(bdf_doc_part(doc, l.obj, &data, &len, &err) == BDF_OK && len == 11 &&
          memcmp(data, "hello world", 11) == 0);
    bdf_free(data);
    bdf_part_entry pe;
    CHECK(bdf_doc_part_find(doc, l.obj, &pe) && pe.enc == BDF_ENC_DEFLATE_RAW && pe.size == 11);
    bdf_doc_free(doc);
  }
  free(w.b);
  free(parts.b);

  /* TestInflateLimits: a part that inflates to more than stated, and one as stated */
  snprintf(mj, sizeof mj,
           "{\"bdf\":1,\"parts\":[{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"deflate-raw\",\"len\":%zu,"
           "\"size\":1024}]}",
           sizeof zeros_packed);
  CHECK(open_single(mj, zeros_packed, sizeof zeros_packed, &doc, &err, &w) == BDF_OK);
  if (doc) {
    uint8_t h[16], *data;
    size_t len;
    memset(h, 0xaa, 16);
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "data inflates to more than its stated size");
    bdf_doc_free(doc);
  }
  free(w.b);
  snprintf(mj, sizeof mj,
           "{\"bdf\":1,\"parts\":[{\"h\":\"" HA "\",\"t\":\"img\",\"enc\":\"deflate-raw\",\"len\":%zu,"
           "\"size\":%d}]}",
           sizeof zeros_packed, ZEROS_PLAIN_LEN);
  CHECK(open_single(mj, zeros_packed, sizeof zeros_packed, &doc, &err, &w) == BDF_OK);
  if (doc) {
    uint8_t h[16], *data;
    size_t len;
    memset(h, 0xaa, 16);
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_OK && len == ZEROS_PLAIN_LEN);
    bdf_free(data);
    bdf_doc_free(doc);
  }
  free(w.b);

  /* an identity part is returned as stored, whatever size says; an unknown
   * encoding and a negative size are errors */
  snprintf(mj, sizeof mj,
           "{\"bdf\":1,\"parts\":[{\"h\":\"" HA "\",\"t\":\"img\",\"len\":4,\"size\":99},"
           "{\"h\":\"" HB "\",\"t\":\"img\",\"enc\":\"brotli\",\"len\":4,\"size\":4},"
           "{\"h\":\"cccccccccccccccccccccccccccccccc\",\"t\":\"img\",\"enc\":\"deflate-raw\",\"len\":4,\"size\":-1}]}");
  CHECK(open_single(mj, "abcd", 4, &doc, &err, &w) == BDF_OK);
  if (doc) {
    uint8_t h[16], *data;
    size_t len;
    memset(h, 0xaa, 16);
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_OK && len == 4);
    bdf_free(data);
    memset(h, 0xbb, 16);
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "unknown encoding");
    memset(h, 0xcc, 16);
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
    CHECK_MSG(err, "negative part size");
    bdf_doc_free(doc);
  }
  free(w.b);
}

static void test_compressed_manifest(void) {
  const char *mj = "{\"bdf\":1,\"views\":[{\"id\":\"v\",\"kind\":\"scroll\"}],\"parts\":[]}";
  wr packed = {0};
  w_stored(&packed, mj, strlen(mj));
  wr w = {0};
  w_header(&w, 0, BDF_HEADER_SIZE, packed.n, 1);
  w_bytes(&w, packed.b, packed.n);
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(bdf_doc_open_memory(w.b, w.n, &doc, &err) == BDF_OK);
  if (doc) {
    bdf_view v;
    CHECK(bdf_doc_view_count(doc) == 1 && bdf_doc_view(doc, 0, &v) == BDF_OK &&
          v.kind_code == BDF_VIEW_SCROLL);
    bdf_doc_free(doc);
  }
  /* corrupt the stored block's complement */
  w.b[BDF_HEADER_SIZE + 3] ^= 0xFF;
  CHECK(bdf_doc_open_memory(w.b, w.n, &doc, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "corrupt deflate stream");
  free(w.b);
  free(packed.b);
}

static void test_tiles_index(void) {
  /* records of [u32 tx, u32 ty, 16 bytes], sorted by (ty, tx) */
  wr idx = {0};
  const uint32_t coords[][2] = {{0, 0}, {1, 0}, {5, 0}, {0, 1}, {3, 1}, {2, 7}};
  for (size_t i = 0; i < 6; i++) {
    w_u32(&idx, coords[i][0]);
    w_u32(&idx, coords[i][1]);
    for (int k = 0; k < 16; k++) {
      w_u8(&idx, (uint8_t)i);
    }
  }
  uint8_t out[16];
  CHECK(bdf_tiles_index_find(idx.b, idx.n, 3, 1, out) == 1 && out[0] == 4);
  CHECK(bdf_tiles_index_find(idx.b, idx.n, 0, 0, out) == 1 && out[0] == 0);
  CHECK(bdf_tiles_index_find(idx.b, idx.n, 2, 7, out) == 1 && out[0] == 5);
  CHECK(bdf_tiles_index_find(idx.b, idx.n, 4, 1, out) == 0);
  CHECK(bdf_tiles_index_find(idx.b, idx.n, 0, 2, out) == 0);
  CHECK(bdf_tiles_index_find(idx.b, idx.n - 1, 0, 0, out) == 0); /* not whole records */
  free(idx.b);

  char hex[33];
  uint8_t h[16];
  CHECK(bdf_hash_parse("00112233445566778899aAbBcCdDeEfF", 32, h) == BDF_OK && h[15] == 0xff);
  bdf_hash_format(h, hex);
  CHECK(strcmp(hex, "00112233445566778899aabbccddeeff") == 0);
  CHECK(bdf_hash_parse("0011", 4, h) == BDF_ERR_FORMAT);
  CHECK(bdf_hash_parse("0011223344556677889900112233445g", 32, h) == BDF_ERR_FORMAT);
}

int main(void) {
  test_manifest_range();
  test_part_range();
  test_parts();
  test_compressed_manifest();
  test_tiles_index();
  return finish();
}
