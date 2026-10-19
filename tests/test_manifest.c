/* Tests of the manifest reader: TestManifestNumbers of the reference, what
 * encoding/json accepts and rejects, and the accessors. */
#include "testutil.h"

#define LAYERS "\"layers\":[]"

static void expect_reject(const char *views, const char *why) {
  char mj[1024];
  snprintf(mj, sizeof mj, "{\"bdf\":1,\"views\":%s,\"parts\":[]}", views);
  bdf_error err;
  if (manifest_opens(mj, &err)) {
    failures++;
    fprintf(stderr, "%s: accepted\n", why);
  }
}

static void test_numbers(void) {
  /* TestManifestNumbers of the reference implementation */
  expect_reject("[null]", "null view");
  expect_reject("[{\"id\":\"v\",\"kind\":\"fixed\",\"pages\":[null]}]", "null page");
  expect_reject("[{\"id\":\"v\",\"kind\":\"fixed\",\"pages\":[{\"w\":-5,\"h\":80000000," LAYERS "}]}]",
                "negative page");
  expect_reject("[{\"id\":\"v\",\"kind\":\"fixed\",\"pages\":[{\"w\":100,\"h\":1e9," LAYERS "}]}]",
                "huge page");
  expect_reject("[{\"id\":\"v\",\"kind\":\"flow\",\"pages\":[{\"w\":100,\"h\":100,\"body\":{\"x\":0,"
                "\"y\":-1e30,\"w\":10,\"h\":10}," LAYERS "}]}]",
                "page body");
  expect_reject("[{\"id\":\"v\",\"kind\":\"flow\",\"continuous\":{\"gap\":1e30}}]", "gap");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"tile\":0.000001}]", "tiny tile");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"rows\":[[16777216,10],[1,10]]}]", "many rows");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"rows\":[[1e11,0]]}]", "rows beyond f32");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"cols\":[[1.5,10]]}]", "half a column");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"cols\":[[-1,10]]}]", "negative columns");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"cols\":[[400000,0.0000001]]}]", "thin columns");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"rows\":[[4,-10]]}]", "negative row size");
  expect_reject("[{\"id\":\"v\",\"kind\":\"sheet\",\"freeze\":{\"rows\":-1}}]", "negative freeze");

  /* what writers write: pages, hidden rows, the tile of the default size */
  const char *mj =
      "{\"bdf\":1,\"views\":[{\"id\":\"p\",\"kind\":\"flow\",\"continuous\":{\"gap\":12},"
      "\"pages\":[{\"w\":595.28,\"h\":841.89,\"body\":{\"x\":72,\"y\":72,\"w\":451,\"h\":697}," LAYERS
      "}]},{\"id\":\"s\",\"kind\":\"sheet\",\"tile\":2048,\"cols\":[[3,64],[16381,48]],"
      "\"rows\":[[1,0],[1048575,15]],\"freeze\":{\"rows\":1}},{\"id\":\"t\",\"kind\":\"sheet\"}],"
      "\"parts\":[]}";
  wr w;
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(open_single(mj, NULL, 0, &doc, &err, &w) == BDF_OK);
  if (doc) {
    bdf_view v;
    CHECK(bdf_doc_view_count(doc) == 3);
    CHECK(bdf_doc_view(doc, 0, &v) == BDF_OK && v.kind_code == BDF_VIEW_FLOW && v.npages == 1);
    CHECK(v.has_continuous && v.gap == 12);
    bdf_page pg;
    CHECK(bdf_doc_page(doc, 0, 0, &pg) == BDF_OK && pg.w == 595.28f && pg.has_body &&
          pg.body[0] == 72 && pg.nlayers == 0);
    CHECK(bdf_doc_view(doc, 1, &v) == BDF_OK && v.kind_code == BDF_VIEW_SHEET && v.tile == 2048);
    CHECK(v.ncols == 2 && v.nrows == 2 && v.has_freeze && v.freeze_rows == 1 && v.freeze_cols == 0);
    float run[2];
    CHECK(bdf_doc_view_run(doc, 1, 0, 1, run) == BDF_OK && run[0] == 16381 && run[1] == 48);
    CHECK(bdf_doc_view_run(doc, 1, 1, 0, run) == BDF_OK && run[0] == 1 && run[1] == 0);
    CHECK(bdf_doc_view_run(doc, 1, 1, 2, run) == BDF_ERR_ARG);
    CHECK(bdf_doc_view(doc, 2, &v) == BDF_OK && v.tile == BDF_DEFAULT_TILE && v.tile_stated == 0);
    CHECK(bdf_doc_view(doc, 3, &v) == BDF_ERR_ARG);
    bdf_doc_free(doc);
  }
  free(w.b);
}

static void test_json(void) {
  bdf_error err;
  /* syntax */
  CHECK(!manifest_opens("{\"bdf\":1,\"parts\":[],}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"parts\":[]} x", &err));
  CHECK(!manifest_opens("", &err));
  CHECK(!manifest_opens("[]", &err));
  CHECK(!manifest_opens("null", &err)); /* version 0 */
  CHECK(!manifest_opens("{\"bdf\":01}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"unit\":\"p\tt\"}", &err)); /* a raw control character */
  CHECK(manifest_opens(" \t\r\n{\"bdf\":1}\n", &err));
  /* versions */
  CHECK(!manifest_opens("{\"bdf\":2,\"parts\":[]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1.0,\"parts\":[]}", &err)); /* not an integer literal */
  CHECK(!manifest_opens("{\"bdf\":1e0,\"parts\":[]}", &err));
  CHECK(!manifest_opens("{\"bdf\":\"1\",\"parts\":[]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"opset\":2}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"opset\":-1}", &err));
  CHECK(manifest_opens("{\"bdf\":1,\"opset\":1}", &err));
  CHECK(manifest_opens("{\"bdf\":1,\"opset\":0}", &err));
  /* the last of duplicate members counts; unknown members and null are nothing */
  CHECK(manifest_opens("{\"bdf\":2,\"bdf\":1,\"future\":{\"deep\":[[[[1]]]]},\"views\":null}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"bdf\":2}", &err));
  /* a null leaves a number, a string or a struct as it was (encoding/json
   * ignores it there), and empties a slice, a pointer or a map */
  CHECK(manifest_opens("{\"bdf\":1,\"bdf\":null}", &err));
  CHECK(manifest_opens("{\"bdf\":null,\"bdf\":1}", &err));
  CHECK(!manifest_opens("{\"bdf\":2,\"bdf\":null}", &err));
  CHECK(manifest_opens("{\"bdf\":1,\"views\":[{\"id\":5}],\"views\":null}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":null,\"views\":[{\"id\":5}]}", &err));
  CHECK(manifest_opens("{\"bdf\":1,\"views\":[{\"id\":\"v\",\"kind\":\"fixed\","
                       "\"continuous\":{\"gap\":1e30},\"continuous\":null}]}",
                       &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"id\":\"v\",\"kind\":\"fixed\","
                        "\"pages\":[{\"w\":1e9,\"w\":null,\"h\":1,\"layers\":[]}]}]}",
                        &err));
  CHECK(manifest_opens("{\"bdf\":1,\"meta\":{\"source\":\"x\"},\"meta\":null}", &err));
  {
    /* a part keeps its length through a null that follows it */
    wr w;
    bdf_doc *doc = NULL;
    CHECK(open_single("{\"bdf\":1,\"parts\":[{\"h\":\"00000000000000000000000000000000\","
                      "\"t\":\"obj\",\"enc\":\"identity\",\"len\":5,\"len\":null,\"size\":5,"
                      "\"off\":0}]}",
                      "hello", 5, &doc, &err, &w) == BDF_OK);
    if (doc) {
      bdf_part_entry e;
      CHECK(bdf_doc_part_entry(doc, 0, &e) == BDF_OK && e.len == 5);
      bdf_doc_free(doc);
    }
    free(w.b);
  }
  /* members of the wrong type */
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":{}}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[5]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"id\":5}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"gridlines\":1}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"pages\":[{\"w\":\"5\"}]}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"pages\":[{\"w\":1e39}]}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"cols\":[5]}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"views\":[{\"freeze\":{\"rows\":1.5}}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"parts\":[{\"h\":5}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"parts\":[{\"h\":\"xyz\"}]}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"parts\":[{\"h\":\"00000000000000000000000000000000\",\"len\":1e3}]}",
                        &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"meta\":{\"dc\":{\"title\":5}}}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"meta\":{\"dc\":{\"title\":[5]}}}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"encryption\":5}", &err));
  CHECK(!manifest_opens("{\"bdf\":1,\"encryption\":{\"keys\":[{\"salt\":\"not base64!\"}]}}", &err));
  /* and what encoding/json takes */
  CHECK(manifest_opens("{\"bdf\":1,\"parts\":[{\"h\":\"0123456789ABCDEF0123456789abcdef\"}]}", &err));
  CHECK(manifest_opens("{\"bdf\":1,\"views\":[{\"cols\":[[26]],\"rows\":[[1,2,3]],\"pages\":[{\"layers\":[null]}]}]}",
                       &err));
  CHECK(manifest_opens("{\"bdf\":1,\"parts\":[null]}", &err));
  CHECK(manifest_opens("{\"bdf\":1,\"Views\":[null]}", &err)); /* not the member: ignored */
  CHECK(manifest_opens("{\"bdf\":1,\"views\":[{\"pages\":[{\"w\":100,\"h\":100,\"body\":null}]}]}", &err));
}

static void test_fields(void) {
  const char *mj =
      "{\"bdf\":1,\"opset\":1,\"unit\":\"pt\","
      "\"meta\":{\"dc\":{\"title\":\"\\u5831\\u544a\\u66f8 \\\"Q\\\"\",\"creator\":[\"a\",\"b\"],"
      "\"language\":\"ja\",\"subject\":[]},\"source\":\"pptx\",\"generator\":\"bdf-go/0.1\"},"
      "\"segment\":{\"view\":\"pages\",\"from\":10,\"to\":20},"
      "\"views\":[{\"id\":\"pages\",\"kind\":\"fixed\",\"title\":\"T\",\"direction\":\"rtl\","
      "\"textIndex\":\"11111111111111111111111111111111\","
      "\"pages\":[{\"w\":960,\"h\":540,\"layers\":[{\"role\":\"master\","
      "\"obj\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},{\"role\":\"body\","
      "\"obj\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}]}],"
      "\"play\":{\"seq\":\"cccccccccccccccccccccccccccccccc\",\"cues\":\"dddddddddddddddddddddddddddddddd\"}},"
      "{\"id\":\"s\",\"kind\":\"sheet\",\"gridlines\":true,\"tiles\":{\"0,0\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\","
      "\"12,3\":\"ffffffffffffffffffffffffffffffff\"},\"tilesRef\":\"99999999999999999999999999999999\"}],"
      "\"parts\":[{\"h\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"t\":\"obj\",\"enc\":\"deflate-raw\","
      "\"len\":10,\"size\":20,\"off\":0},{\"h\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"t\":\"obj\","
      "\"enc\":\"identity\",\"len\":5,\"size\":5,\"off\":10}]}";
  wr w;
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(open_single(mj, NULL, 0, &doc, &err, &w) == BDF_OK);
  if (!doc) {
    fprintf(stderr, "open: %s\n", err.message);
    free(w.b);
    return;
  }
  size_t len;
  CHECK(bdf_doc_opset(doc) == 1);
  CHECK(strcmp(bdf_doc_unit(doc, &len), "pt") == 0 && len == 2);
  bdf_meta meta;
  bdf_doc_meta(doc, &meta);
  CHECK(strcmp(meta.source, "pptx") == 0 && strcmp(meta.generator, "bdf-go/0.1") == 0);
  CHECK(bdf_doc_dc_count(doc, "title") == 1 && bdf_doc_dc_count(doc, "creator") == 2);
  CHECK(bdf_doc_dc_count(doc, "subject") == 0 && bdf_doc_dc_count(doc, "nonsense") == 0);
  const char *title = bdf_doc_dc(doc, "title", 0, &len);
  CHECK(title && strcmp(title, "\xe5\xa0\xb1\xe5\x91\x8a\xe6\x9b\xb8 \"Q\"") == 0 && len == 13);
  CHECK(strcmp(bdf_doc_dc(doc, "creator", 1, &len), "b") == 0);
  CHECK(bdf_doc_dc(doc, "creator", 2, &len) == NULL);
  bdf_segment seg;
  CHECK(bdf_doc_segment(doc, &seg) == 1 && strcmp(seg.view, "pages") == 0 && seg.from == 10 &&
        seg.to == 20);
  CHECK(!bdf_doc_encrypted(doc) && !bdf_doc_locked(doc) && !bdf_doc_split(doc));

  bdf_view v;
  CHECK(bdf_doc_view_count(doc) == 2);
  CHECK(bdf_doc_view(doc, 0, &v) == BDF_OK);
  CHECK(strcmp(v.id, "pages") == 0 && v.kind_code == BDF_VIEW_FIXED && strcmp(v.title, "T") == 0);
  CHECK(v.rtl && strcmp(v.direction, "rtl") == 0);
  CHECK(v.text_index_len == 32 && v.text_index[0] == '1');
  CHECK(v.npages == 1 && !v.has_continuous && v.has_play && v.seq_len == 32 && v.audio_len == 0 &&
        v.cues[0] == 'd');
  bdf_page pg;
  CHECK(bdf_doc_page(doc, 0, 0, &pg) == BDF_OK && pg.w == 960 && pg.h == 540 && !pg.has_body &&
        pg.nlayers == 2);
  bdf_layer l;
  CHECK(bdf_doc_layer(doc, 0, 0, 1, &l) == BDF_OK && strcmp(l.role, "body") == 0 && l.obj[0] == 0xbb);
  CHECK(bdf_doc_layer(doc, 0, 0, 2, &l) == BDF_ERR_ARG);
  CHECK(bdf_doc_view(doc, 1, &v) == BDF_OK && v.gridlines && v.ntiles == 2 && v.tiles_ref_len == 32);
  bdf_tile t;
  uint32_t tx, ty;
  CHECK(bdf_doc_tile(doc, 1, 1, &t) == BDF_OK && strcmp(t.key, "12,3") == 0 && t.obj[0] == 'f');
  CHECK(bdf_tile_key_parse(t.key, t.key_len, &tx, &ty) && tx == 12 && ty == 3);
  CHECK(!bdf_tile_key_parse("12", 2, &tx, &ty) && !bdf_tile_key_parse("1,2,3", 5, &tx, &ty) &&
        !bdf_tile_key_parse("a,1", 3, &tx, &ty) && !bdf_tile_key_parse("1,", 2, &tx, &ty));
  CHECK(!bdf_tile_key_parse(NULL, 3, &tx, &ty) && !bdf_tile_key_parse("1,2", 3, NULL, &ty) &&
        !bdf_tile_key_parse("1,2", 3, &tx, NULL) && !bdf_tile_key_parse(NULL, 0, &tx, &ty));

  bdf_part_entry pe;
  CHECK(bdf_doc_part_count(doc) == 2);
  CHECK(bdf_doc_part_entry(doc, 0, &pe) == BDF_OK && pe.enc == BDF_ENC_DEFLATE_RAW && pe.len == 10 &&
        pe.size == 20 && pe.off == 0 && strcmp(pe.type, "obj") == 0 && pe.sealed == NULL);
  uint8_t h[16];
  memset(h, 0xbb, 16);
  CHECK(bdf_doc_part_find(doc, h, &pe) == 1 && pe.enc == BDF_ENC_IDENTITY && pe.off == 10);
  h[0] = 0;
  CHECK(bdf_doc_part_find(doc, h, &pe) == 0);
  bdf_doc_free(doc);
  free(w.b);
}

/* encrypted_opens reports whether a manifest with an encryption block opens
 * under the encrypted flag. */
static int encrypted_opens(const char *manifest) {
  wr w = {0};
  w_header(&w, BDF_FLAG_ENCRYPTED, BDF_HEADER_SIZE, strlen(manifest), 0);
  w_text(&w, manifest);
  bdf_doc *doc = NULL;
  bdf_error err;
  bdf_status st = bdf_doc_open_memory(w.b, w.n, &doc, &err);
  bdf_doc_free(doc);
  free(w.b);
  return st == BDF_OK;
}

static void test_encryption(void) {
  const char *outer =
      "{\"bdf\":1,\"encryption\":{\"cipher\":\"A256GCM\",\"keys\":[{\"type\":\"password\","
      "\"kdf\":\"PBKDF2-SHA256\",\"iter\":600000,\"salt\":\"AAECAwQFBgcICQoLDA0ODw==\","
      "\"key\":\"QUJD\"}],\"manifest\":{\"part\":\"abababababababababababababababab\","
      "\"enc\":\"deflate-raw\"}},\"parts\":[{\"h\":\"abababababababababababababababab\","
      "\"t\":\"sealed\",\"enc\":\"identity\",\"len\":3,\"size\":3,\"off\":0}]}";
  wr w = {0};
  w_header(&w, BDF_FLAG_ENCRYPTED, BDF_HEADER_SIZE, strlen(outer), 0);
  w_text(&w, outer);
  w_text(&w, "xyz");
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(bdf_doc_open_memory(w.b, w.n, &doc, &err) == BDF_OK);
  if (doc) {
    CHECK(bdf_doc_encrypted(doc) && bdf_doc_locked(doc) && bdf_doc_view_count(doc) == 0);
    size_t len;
    CHECK(strcmp(bdf_doc_cipher(doc, &len), "A256GCM") == 0);
    bdf_key_slot k;
    CHECK(bdf_doc_key_count(doc) == 1 && bdf_doc_key(doc, 0, &k) == BDF_OK);
    CHECK(strcmp(k.type, "password") == 0 && k.iter == 600000 && k.salt_len == 16 && k.salt[15] == 15);
    CHECK(k.key_len == 3 && memcmp(k.key, "ABC", 3) == 0 && k.epk == NULL && k.crv_len == 0);
    const char *enc;
    const uint8_t *sm = bdf_doc_sealed_manifest(doc, &enc, &len);
    CHECK(sm && sm[0] == 0xab && strcmp(enc, "deflate-raw") == 0);
    /* the parts of a locked document are the sealed parts, as stored */
    uint8_t h[16];
    memset(h, 0xab, 16);
    uint8_t *data;
    CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_OK && len == 3 && memcmp(data, "xyz", 3) == 0);
    bdf_free(data);
    bdf_doc_free(doc);
  }
  /* the flag and the block must agree */
  w.b[6] = 0;
  CHECK(bdf_doc_open_memory(w.b, w.n, &doc, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "encryption flag does not match the manifest");
  free(w.b);
  wr p = single("{\"bdf\":1}", NULL, 0);
  p.b[6] = BDF_FLAG_ENCRYPTED;
  CHECK(bdf_doc_open_memory(p.b, p.n, &doc, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "encryption flag does not match the manifest");
  free(p.b);
  /* padding that is not canonical is still base64; whitespace is not */
  CHECK(encrypted_opens("{\"bdf\":1,\"encryption\":{\"keys\":[{\"key\":\"AB==\"}]}}"));
  CHECK(!encrypted_opens("{\"bdf\":1,\"encryption\":{\"keys\":[{\"key\":\"AB=\"}]}}"));
  CHECK(!encrypted_opens("{\"bdf\":1,\"encryption\":{\"keys\":[{\"key\":\"QU JD\"}]}}"));
}

/* ---- The split form ----------------------------------------------------- */

typedef struct fake_part {
  const char *name;
  const uint8_t *data;
  size_t len;
} fake_part;

typedef struct fake_store {
  const fake_part *parts;
  size_t n;
} fake_store;

static int fake_read_part(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *size) {
  const fake_store *s = ctx;
  for (size_t i = 0; i < s->n; i++) {
    if (strcmp(s->parts[i].name, name) == 0) {
      if (buf) {
        size_t n = s->parts[i].len < cap ? s->parts[i].len : cap;
        memcpy(buf, s->parts[i].data, n);
        *size = n;
      } else {
        *size = s->parts[i].len;
      }
      return 0;
    }
  }
  return -1;
}

static void test_split(void) {
  static const uint8_t hello_fixed[13] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28,
                                          0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00};
  const fake_part parts[] = {
      {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", (const uint8_t *)"0123456789", 10},
      {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", hello_fixed, sizeof hello_fixed},
      {"cccccccccccccccccccccccccccccccc", (const uint8_t *)"short", 5},
  };
  fake_store store = {parts, 3};
  bdf_source src;
  memset(&src, 0, sizeof src);
  src.ctx = &store;
  src.read_part = fake_read_part;
  const char *mj =
      "{\"bdf\":1,\"parts\":["
      "{\"h\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"t\":\"img\",\"enc\":\"identity\",\"len\":10,\"size\":10},"
      "{\"h\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"t\":\"obj\",\"enc\":\"deflate-raw\",\"len\":13,\"size\":11},"
      "{\"h\":\"cccccccccccccccccccccccccccccccc\",\"t\":\"img\",\"enc\":\"identity\",\"len\":6,\"size\":6},"
      "{\"h\":\"dddddddddddddddddddddddddddddddd\",\"t\":\"img\",\"enc\":\"identity\",\"len\":1,\"size\":1},"
      "{\"h\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\",\"t\":\"img\",\"enc\":\"gzip\",\"len\":1,\"size\":1},"
      "{\"h\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"t\":\"obj\",\"enc\":\"deflate-raw\",\"len\":13,\"size\":4}"
      "]}";
  bdf_doc *doc = NULL;
  bdf_error err;
  CHECK(bdf_doc_open_split((const uint8_t *)mj, strlen(mj), &src, &doc, &err) == BDF_OK);
  if (!doc) {
    return;
  }
  CHECK(bdf_doc_split(doc));
  uint8_t h[16], *data;
  size_t len;
  memset(h, 0xaa, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_OK && len == 10 && memcmp(data, "0123456789", 10) == 0);
  bdf_free(data);
  /* the last entry of a hash is the one that counts: size 4 makes "hello world" too large */
  memset(h, 0xbb, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "data inflates to more than its stated size");
  memset(h, 0xcc, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "part is not its stated length");
  memset(h, 0xdd, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "cannot read the part");
  memset(h, 0xee, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "cannot read the part"); /* the store has no such part; the encoding is never reached */
  memset(h, 0x11, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "unknown part");
  bdf_doc_free(doc);

  /* the first entry alone inflates to the stated size */
  const char *mj2 =
      "{\"bdf\":1,\"parts\":[{\"h\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"t\":\"obj\","
      "\"enc\":\"deflate-raw\",\"len\":13,\"size\":11}]}";
  CHECK(bdf_doc_open_split((const uint8_t *)mj2, strlen(mj2), &src, &doc, &err) == BDF_OK);
  memset(h, 0xbb, 16);
  CHECK(bdf_doc_part(doc, h, &data, &len, &err) == BDF_OK && len == 11 && memcmp(data, "hello world", 11) == 0);
  bdf_free(data);
  bdf_doc_free(doc);
  CHECK(bdf_doc_open_split((const uint8_t *)mj2, strlen(mj2), NULL, &doc, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_open_split((const uint8_t *)"{", 1, &src, &doc, &err) == BDF_ERR_FORMAT);
}

int main(void) {
  test_numbers();
  test_json();
  test_fields();
  test_encryption();
  test_split();
  return finish();
}
