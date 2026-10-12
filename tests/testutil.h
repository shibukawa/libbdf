/* Helpers shared by the tests: a check macro, a small byte writer, and
 * builders of single-file documents. */
#ifndef BDF_TESTUTIL_H
#define BDF_TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdf/bdf.h"

static int failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                     \
    }                                                                                              \
  } while (0)

#define CHECK_MSG(err, msg)                                                                        \
  do {                                                                                             \
    if (strcmp((err).message, (msg)) != 0) {                                                       \
      failures++;                                                                                  \
      fprintf(stderr, "%s:%d: message %s, want %s\n", __FILE__, __LINE__, (err).message, (msg));   \
    }                                                                                              \
  } while (0)

typedef struct wr {
  uint8_t *b;
  size_t n, cap;
} wr;

static inline void w_bytes(wr *w, const void *p, size_t n) {
  if (w->n + n > w->cap) {
    w->cap = (w->n + n) * 2 + 64;
    w->b = realloc(w->b, w->cap);
    if (!w->b) {
      abort();
    }
  }
  if (n > 0) {
    memcpy(w->b + w->n, p, n);
  }
  w->n += n;
}

static inline void w_u8(wr *w, uint8_t v) { w_bytes(w, &v, 1); }

static inline void w_u16(wr *w, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  w_bytes(w, b, 2);
}

static inline void w_u32(wr *w, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  w_bytes(w, b, 4);
}

static inline void w_u64(wr *w, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    w_u8(w, (uint8_t)(v >> (8 * i)));
  }
}

static inline void w_f32(wr *w, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  w_u32(w, u);
}

static inline void w_varuint(wr *w, uint64_t v) {
  while (v >= 0x80) {
    w_u8(w, (uint8_t)(v | 0x80));
    v >>= 7;
  }
  w_u8(w, (uint8_t)v);
}

static inline void w_str(wr *w, const char *s) {
  w_varuint(w, strlen(s));
  w_bytes(w, s, strlen(s));
}

static inline void w_text(wr *w, const char *s) { w_bytes(w, s, strlen(s)); }

/* w_header writes a single-file header with the manifest at offset 32. */
static inline void w_header(wr *w, uint16_t flags, uint64_t moff, uint64_t mlen, uint8_t menc) {
  w_bytes(w, "bdf\0", 4);
  w_u16(w, BDF_FORMAT_VERSION);
  w_u16(w, flags);
  w_u64(w, moff);
  w_u64(w, mlen);
  w_u8(w, menc);
  for (int i = 0; i < 7; i++) {
    w_u8(w, 0);
  }
}

/* w_stored writes bytes as one stored deflate block: valid deflate-raw data
 * without a compressor. */
static inline void w_stored(wr *w, const void *p, size_t n) {
  w_u8(w, 0x01); /* final block, stored */
  w_u16(w, (uint16_t)n);
  w_u16(w, (uint16_t)~n);
  w_bytes(w, p, n);
}

/* single builds a single-file document: the header, the manifest JSON as it
 * is, and the stored bytes of the parts after it. */
static inline wr single(const char *manifest, const void *parts, size_t parts_len) {
  wr w = {0};
  w_header(&w, 0, BDF_HEADER_SIZE, strlen(manifest), 0);
  w_text(&w, manifest);
  w_bytes(&w, parts, parts_len);
  return w;
}

/* open_single opens such a document and returns the status; the error is
 * left in *err and the document in *doc. */
static inline bdf_status open_single(const char *manifest, const void *parts, size_t parts_len,
                              bdf_doc **doc, bdf_error *err, wr *keep) {
  *keep = single(manifest, parts, parts_len);
  return bdf_doc_open_memory(keep->b, keep->n, doc, err);
}

/* manifest_ok reports whether a manifest alone opens, freeing what it made. */
static inline int manifest_opens(const char *manifest, bdf_error *err) {
  wr w;
  bdf_doc *doc = NULL;
  bdf_status st = open_single(manifest, NULL, 0, &doc, err, &w);
  bdf_doc_free(doc);
  free(w.b);
  return st == BDF_OK;
}

static inline int finish(void) {
  if (failures) {
    fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}

#endif /* BDF_TESTUTIL_H */
