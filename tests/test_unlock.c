/* Tests of encrypted documents (spec §3.5) and segments (§3.6) on a
 * document of four pages the reference wrote in the clear, encrypted with a
 * password, sealed for a key, and in segments (unlock_vectors.h, written by
 * go/genvectors). */
#include "testutil.h"

#include "unlock_vectors.h"

static bdf_doc *open_mem(const uint8_t *data, size_t len) {
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(data, len, &doc, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "open: %s\n", err.message);
    exit(1);
  }
  return doc;
}

/* text_is checks the runs of a page's object: its own line and the shared one. */
static void text_is(const bdf_doc *doc, size_t page, const char *first, const char *second) {
  bdf_layer layer;
  bdf_text *text = NULL;
  bdf_error err;
  if (bdf_doc_layer(doc, 0, page, 0, &layer) != BDF_OK) {
    failures++;
    fprintf(stderr, "page %zu has no layer\n", page);
    return;
  }
  CHECK(memcmp(layer.obj, uv_page_objects + 16 * page, 16) == 0);
  if (bdf_doc_text(doc, layer.obj, &text, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "page %zu: %s\n", page, err.message);
    return;
  }
  bdf_text_run a, b;
  CHECK(bdf_text_run_count(text) == 2);
  if (bdf_text_run_at(text, 0, &a) == BDF_OK && bdf_text_run_at(text, 1, &b) == BDF_OK) {
    CHECK(strcmp(a.text, first) == 0);
    CHECK(strcmp(b.text, second) == 0);
  } else {
    failures++;
  }
  bdf_text_free(text);
}

/* same_parts checks that every part of the clear document reads the same
 * from doc. */
static void same_parts(const bdf_doc *doc, const bdf_doc *plain) {
  size_t n = bdf_doc_part_count(plain);
  CHECK(bdf_doc_part_count(doc) == n);
  for (size_t i = 0; i < n; i++) {
    bdf_part_entry e, got;
    uint8_t *a = NULL, *b = NULL;
    size_t an = 0, bn = 0;
    bdf_error err;
    CHECK(bdf_doc_part_entry(plain, i, &e) == BDF_OK);
    CHECK(bdf_doc_part_find(doc, e.hash, &got) == 1);
    CHECK(got.size == e.size && got.type_len == e.type_len && got.sealed != NULL);
    CHECK(bdf_doc_part(plain, e.hash, &a, &an, &err) == BDF_OK);
    CHECK(bdf_doc_part(doc, e.hash, &b, &bn, &err) == BDF_OK);
    CHECK(an == bn && an > 0 && memcmp(a, b, an) == 0);
    bdf_free(a);
    bdf_free(b);
  }
}

static void check_whole(const bdf_doc *doc, const bdf_doc *plain) {
  bdf_view view;
  CHECK(bdf_doc_encrypted(doc) && !bdf_doc_locked(doc));
  CHECK(bdf_doc_view_count(doc) == 1);
  CHECK(bdf_doc_view(doc, 0, &view) == BDF_OK && view.npages == 4);
  CHECK(bdf_doc_dc_count(doc, "title") == 1);
  size_t n;
  const char *title = bdf_doc_dc(doc, "title", 0, &n);
  CHECK(n == 12 && memcmp(title, "secret title", 12) == 0);
  same_parts(doc, plain);
  text_is(doc, 0, "page 1", "shared");
  text_is(doc, 3, "page 4", "shared");
}

static void test_password(void) {
  bdf_doc *plain = open_mem(uv_plain, uv_plain_len);
  bdf_doc *doc = open_mem(uv_locked, uv_locked_len);
  bdf_error err;
  CHECK(!bdf_doc_encrypted(plain) && !bdf_doc_locked(plain));
  CHECK(bdf_doc_encrypted(doc) && bdf_doc_locked(doc));
  /* locked: no views, no title, and the parts are the sealed ones */
  CHECK(bdf_doc_view_count(doc) == 0);
  CHECK(bdf_doc_dc_count(doc, "title") == 0);
  CHECK(bdf_doc_part_find(doc, uv_shared, NULL) == 0);
  uint8_t *out;
  size_t n;
  CHECK(bdf_doc_part(doc, uv_shared, &out, &n, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "unknown part");
  for (size_t i = 0; i < bdf_doc_part_count(doc); i++) {
    bdf_part_entry e;
    CHECK(bdf_doc_part_entry(doc, i, &e) == BDF_OK);
    CHECK(e.type_len == 6 && memcmp(e.type, "sealed", 6) == 0 && e.sealed == NULL);
  }
  /* the title is nowhere in the file */
  int found = 0;
  for (size_t i = 0; i + 12 <= uv_locked_len; i++) {
    found |= memcmp(uv_locked + i, "secret title", 12) == 0;
  }
  CHECK(!found);

  /* wrong passwords leave it locked, and another can be tried */
  CHECK(bdf_doc_unlock(doc, "wrong", 5, &err) == BDF_ERR_PASSWORD);
  CHECK_MSG(err, "wrong password");
  CHECK(bdf_doc_unlock(doc, "", 0, &err) == BDF_ERR_PASSWORD);
  CHECK(bdf_doc_unlock(doc, NULL, 0, &err) == BDF_ERR_PASSWORD);
  CHECK(bdf_doc_unlock(doc, (const char *)uv_password, uv_password_len - 1, &err) ==
        BDF_ERR_PASSWORD);
  CHECK(bdf_doc_unlock_ecdh(doc, uv_private, &err) == BDF_ERR_PASSWORD);
  CHECK_MSG(err, "the document is not sealed for this key");
  CHECK(bdf_doc_locked(doc) && bdf_doc_view_count(doc) == 0);
  CHECK(bdf_doc_unlock(NULL, "x", 1, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_unlock(doc, NULL, 1, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_unlock(plain, "x", 1, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "the document is not locked");

  /* the password as it decomposes opens the document: it is put in form KC */
  CHECK(bdf_doc_unlock(doc, (const char *)uv_password_nfd, uv_password_nfd_len, &err) == BDF_OK);
  check_whole(doc, plain);
  CHECK(bdf_doc_unlock(doc, (const char *)uv_password, uv_password_len, &err) == BDF_ERR_ARG);
  bdf_doc_free(doc);

  /* and so does the password in full-width letters and half-width katakana,
   * with an ideographic space */
  doc = open_mem(uv_locked, uv_locked_len);
  CHECK(bdf_doc_unlock(doc, (const char *)uv_password_wide, uv_password_wide_len, &err) == BDF_OK);
  check_whole(doc, plain);
  bdf_doc_free(doc);

  doc = open_mem(uv_locked, uv_locked_len);
  CHECK(bdf_doc_unlock(doc, (const char *)uv_password, uv_password_len, &err) == BDF_OK);
  check_whole(doc, plain);
  bdf_doc_free(doc);
  bdf_doc_free(plain);
}

static void test_ecdh(void) {
  bdf_doc *plain = open_mem(uv_plain, uv_plain_len);
  bdf_doc *doc = open_mem(uv_sealed, uv_sealed_len);
  bdf_error err;
  uint8_t pub[65], key[32];
  CHECK(bdf_p256_public_key(uv_private, pub) == BDF_OK && memcmp(pub, uv_public, 65) == 0);
  memset(key, 0, sizeof key);
  CHECK(bdf_p256_public_key(key, pub) == BDF_ERR_ARG);
  CHECK(bdf_p256_public_key(NULL, pub) == BDF_ERR_ARG);
  CHECK(bdf_p256_public_key(uv_private, NULL) == BDF_ERR_ARG);

  CHECK(bdf_doc_locked(doc));
  CHECK(bdf_doc_unlock(doc, (const char *)uv_password, uv_password_len, &err) == BDF_ERR_PASSWORD);
  CHECK(bdf_doc_unlock_ecdh(doc, key, &err) == BDF_ERR_ARG); /* zero is no key */
  CHECK_MSG(err, "the key is not a P-256 private key");
  key[31] = 1;
  CHECK(bdf_doc_unlock_ecdh(doc, key, &err) == BDF_ERR_PASSWORD);
  CHECK(bdf_doc_unlock_ecdh(doc, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_unlock_ecdh(NULL, uv_private, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_unlock_ecdh(plain, uv_private, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_locked(doc));
  CHECK(bdf_doc_unlock_ecdh(doc, uv_private, &err) == BDF_OK);
  check_whole(doc, plain);
  CHECK(bdf_doc_unlock_ecdh(doc, uv_private, &err) == BDF_ERR_ARG);
  bdf_doc_free(doc);
  bdf_doc_free(plain);
}

/* test_damage changes every byte after the header of the encrypted
 * document in turn: nothing damaged opens as if it were whole. */
static void test_damage(void) {
  bdf_doc *plain = open_mem(uv_plain, uv_plain_len);
  uint8_t *copy = malloc(uv_locked_len);
  size_t opened = 0, refused = 0;
  bdf_header h;
  bdf_error herr;
  CHECK(bdf_header_parse(uv_locked, uv_locked_len, uv_locked_len, &h, &herr) == BDF_OK);
  size_t parts_at = (size_t)(h.manifest_off + h.manifest_len);
  for (size_t at = BDF_HEADER_SIZE; at < uv_locked_len; at++) {
    memcpy(copy, uv_locked, uv_locked_len);
    copy[at] ^= 0x04;
    bdf_doc *doc = NULL;
    bdf_error err;
    if (bdf_doc_open_memory(copy, uv_locked_len, &doc, &err) != BDF_OK) {
      refused++;
      continue;
    }
    if (bdf_doc_unlock(doc, (const char *)uv_password, uv_password_len, &err) != BDF_OK) {
      CHECK(bdf_doc_locked(doc) && bdf_doc_view_count(doc) == 0);
      refused++;
      bdf_doc_free(doc);
      continue;
    }
    /* the manifest opened: the change is in a sealed part, which then
     * fails, or in the stored manifest (deflated: a changed bit may move
     * several parts, or none) */
    opened++;
    size_t bad = 0;
    for (size_t i = 0; i < bdf_doc_part_count(plain); i++) {
      bdf_part_entry e;
      uint8_t *a = NULL, *b = NULL;
      size_t an = 0, bn = 0;
      bdf_doc_part_entry(plain, i, &e);
      CHECK(bdf_doc_part(plain, e.hash, &a, &an, &err) == BDF_OK);
      if (bdf_doc_part(doc, e.hash, &b, &bn, &err) == BDF_OK) {
        CHECK(an == bn && memcmp(a, b, an) == 0); /* never other bytes */
      } else {
        bad++;
      }
      bdf_free(a);
      bdf_free(b);
    }
    CHECK(at < parts_at || bad == 1);
    bdf_doc_free(doc);
  }
  CHECK(opened > 0 && refused > 0);
  free(copy);
  bdf_doc_free(plain);
}

static bdf_doc *open_segment(const uint8_t *data, size_t len) {
  bdf_doc *doc = open_mem(data, len);
  bdf_error err;
  if (bdf_doc_locked(doc) && bdf_doc_unlock_ecdh(doc, uv_private, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "segment: %s\n", err.message);
  }
  return doc;
}

static size_t layers_of(const bdf_doc *doc, size_t page) {
  bdf_page p;
  return bdf_doc_page(doc, 0, page, &p) == BDF_OK ? p.nlayers : 99;
}

static void test_segments(void) {
  bdf_error err;
  bdf_segment s;
  bdf_doc *first = open_mem(uv_seg0, uv_seg0_len);
  /* a locked segment says nothing of what it carries */
  CHECK(bdf_doc_locked(first) && !bdf_doc_segment(first, &s));
  CHECK(bdf_doc_unlock_ecdh(first, uv_private, &err) == BDF_OK);
  CHECK(bdf_doc_segment(first, &s) && s.from == 0 && s.to == 2 && s.view_len == 5 &&
        memcmp(s.view, "pages", 5) == 0);
  /* all four pages are there, two of them with what they draw */
  bdf_view view;
  CHECK(bdf_doc_view(first, 0, &view) == BDF_OK && view.npages == 4);
  CHECK(layers_of(first, 0) == 1 && layers_of(first, 1) == 1);
  CHECK(layers_of(first, 2) == 0 && layers_of(first, 3) == 0);
  CHECK(bdf_doc_part_count(first) == 3); /* two pages and the shared object */
  CHECK(bdf_doc_part_find(first, uv_shared, NULL) == 1);
  CHECK(bdf_doc_part_find(first, uv_page_objects + 32, NULL) == 0);
  text_is(first, 0, "page 1", "shared");
  text_is(first, 1, "page 2", "shared");

  /* the second segment alone lacks the object its pages share */
  bdf_doc *second = open_segment(uv_seg1, uv_seg1_len);
  CHECK(bdf_doc_segment(second, &s) && s.from == 2 && s.to == 4);
  CHECK(bdf_doc_part_count(second) == 2);
  CHECK(bdf_doc_part_find(second, uv_shared, NULL) == 0);
  bdf_layer layer;
  bdf_text *text = NULL;
  CHECK(bdf_doc_layer(second, 0, 2, 0, &layer) == BDF_OK);
  CHECK(bdf_doc_text(second, layer.obj, &text, &err) == BDF_OK);
  CHECK(bdf_text_run_count(text) == 1); /* an object that is not there draws nothing */
  bdf_text_free(text);

  /* arguments a merge refuses */
  bdf_doc *plain = open_mem(uv_plain, uv_plain_len);
  bdf_doc *locked = open_mem(uv_seg1, uv_seg1_len);
  CHECK(bdf_doc_add_segment(NULL, second, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_add_segment(first, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_add_segment(first, first, &err) == BDF_ERR_ARG);
  CHECK(bdf_doc_add_segment(first, plain, &err) == BDF_ERR_ARG);
  CHECK_MSG(err, "not a segment document");
  CHECK(bdf_doc_add_segment(first, locked, &err) == BDF_ERR_ARG); /* unlock it first */
  CHECK(bdf_doc_add_segment(locked, second, &err) == BDF_ERR_ARG);
  bdf_doc_free(locked);
  CHECK(layers_of(first, 2) == 0 && bdf_doc_part_count(first) == 3);

  /* put together: the pages of both, and the parts of both */
  CHECK(bdf_doc_add_segment(first, second, &err) == BDF_OK);
  CHECK(bdf_doc_add_segment(first, second, &err) == BDF_ERR_ARG); /* it is first's now */
  bdf_doc *third = open_segment(uv_seg1_clear, uv_seg1_clear_len);
  CHECK(bdf_doc_add_segment(second, third, &err) == BDF_ERR_ARG); /* and takes none itself */
  for (size_t p = 0; p < 4; p++) {
    CHECK(layers_of(first, p) == 1);
  }
  CHECK(bdf_doc_part_count(first) == 5);
  size_t seen_shared = 0;
  for (size_t i = 0; i < bdf_doc_part_count(first); i++) {
    bdf_part_entry e;
    uint8_t *a = NULL, *b = NULL;
    size_t an = 0, bn = 0;
    CHECK(bdf_doc_part_entry(first, i, &e) == BDF_OK);
    seen_shared += memcmp(e.hash, uv_shared, 16) == 0;
    CHECK(bdf_doc_part(first, e.hash, &a, &an, &err) == BDF_OK);
    CHECK(bdf_doc_part(plain, e.hash, &b, &bn, &err) == BDF_OK);
    CHECK(an == bn && an > 0 && memcmp(a, b, an) == 0);
    bdf_free(a);
    bdf_free(b);
  }
  CHECK(seen_shared == 1);
  bdf_part_entry e;
  CHECK(bdf_doc_part_entry(first, 5, &e) == BDF_ERR_ARG);
  text_is(first, 0, "page 1", "shared");
  text_is(first, 2, "page 3", "shared");
  text_is(first, 3, "page 4", "shared");
  /* the same segment in the clear, on top: its pages and parts again */
  CHECK(!bdf_doc_encrypted(third) && bdf_doc_segment(third, &s) && s.from == 2);
  CHECK(bdf_doc_add_segment(first, third, &err) == BDF_OK);
  CHECK(bdf_doc_part_count(first) == 7);
  text_is(first, 2, "page 3", "shared");
  text_is(first, 3, "page 4", "shared");
  /* the text of the whole view, as a search index reads it */
  bdf_search_text *st = NULL;
  CHECK(bdf_doc_search_text(first, &st, &err) == BDF_OK);
  bdf_search_text_free(st);
  bdf_doc_free(first);

  /* a whole document takes segments too, the clear one or an encrypted one */
  bdf_doc *more = open_segment(uv_seg1, uv_seg1_len);
  CHECK(bdf_doc_add_segment(plain, more, &err) == BDF_OK);
  text_is(plain, 3, "page 4", "shared");
  bdf_doc_free(plain);

  /* a segment of another document: other views */
  wr w;
  bdf_doc *other = NULL;
  CHECK(open_single("{\"bdf\":1,\"views\":[{\"id\":\"pages\",\"kind\":\"fixed\",\"pages\":["
                    "{\"w\":1,\"h\":1,\"layers\":[]}]}],\"parts\":[]}",
                    NULL, 0, &other, &err, &w) == BDF_OK);
  bdf_doc *seg = open_segment(uv_seg0, uv_seg0_len);
  CHECK(bdf_doc_add_segment(other, seg, &err) == BDF_ERR_FORMAT);
  CHECK_MSG(err, "the segment is of another document");
  bdf_doc_free(seg); /* refused: still the caller's */
  bdf_doc_free(other);
  free(w.b);
}

int main(void) {
  test_password();
  test_ecdh();
  test_damage();
  test_segments();
  return finish();
}
