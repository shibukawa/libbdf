/*
 * Internal helpers of libbdf, shared between its source files. Not
 * installed. Everything here carries the bdfi_ prefix (BDFI_ for macros),
 * because in the single-header build the whole library is one translation
 * unit of the application's.
 */
#ifndef BDF_INTERNAL_H
#define BDF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "bdf/bdf.h"

/*
 * BDF_INTERNAL marks what the library's files share with each other: nothing
 * in the library build, where the symbols are hidden from the outside, and
 * static in the single-header build (tools/amalgamate.py defines it).
 */
#ifndef BDF_INTERNAL
#define BDF_INTERNAL
#endif

/* ---- Errors ------------------------------------------------------------- */

BDF_INTERNAL void bdfi_error_clear(bdf_error *err);
BDF_INTERNAL void bdfi_error_set(bdf_error *err, bdf_status status, size_t off, const char *msg);

/* ---- A bounded little-endian reader ------------------------------------- */

/*
 * bdfi_reader reads little-endian values from a byte range and remembers the
 * first failure, as the reader of the reference implementation (buf.go)
 * does: after a failure every read returns zero and the position does not
 * move, so a decoder can read a whole structure and check once at the end.
 */
typedef struct bdfi_reader {
  const uint8_t *b;
  size_t len;
  size_t pos;
  int failed;
  size_t err_off;
  const char *err_msg;
} bdfi_reader;

BDF_INTERNAL void bdfi_rd_init(bdfi_reader *r, const uint8_t *b, size_t len);
/* bdfi_rd_fail records the first failure, at the current position. */
BDF_INTERNAL void bdfi_rd_fail(bdfi_reader *r, const char *msg);
BDF_INTERNAL void bdfi_rd_fail_at(bdfi_reader *r, size_t off, const char *msg);
/* bdfi_rd_need reports whether n more bytes can be read, failing otherwise. */
BDF_INTERNAL int bdfi_rd_need(bdfi_reader *r, size_t n);
/*
 * bdfi_rd_count reads the number of items that follow, each at least size
 * bytes long: more than the rest of the data can hold is a failure (msg), so
 * a count is safe to allocate for.
 */
BDF_INTERNAL size_t bdfi_rd_count(bdfi_reader *r, size_t size, const char *msg);
BDF_INTERNAL int bdfi_rd_eof(const bdfi_reader *r);
BDF_INTERNAL uint8_t bdfi_rd_u8(bdfi_reader *r);
BDF_INTERNAL uint16_t bdfi_rd_u16(bdfi_reader *r);
BDF_INTERNAL uint32_t bdfi_rd_u32(bdfi_reader *r);
BDF_INTERNAL uint64_t bdfi_rd_u64(bdfi_reader *r);
BDF_INTERNAL float bdfi_rd_f32(bdfi_reader *r);
/* bdfi_rd_varuint reads an unsigned LEB128 of at most 10 bytes (as Go's
 * encoding/binary does); an overflow or a truncated one is "bad varuint". */
BDF_INTERNAL uint64_t bdfi_rd_varuint(bdfi_reader *r);
/* bdfi_rd_bytes returns n bytes at the position, or NULL after a failure. */
BDF_INTERNAL const uint8_t *bdfi_rd_bytes(bdfi_reader *r, size_t n);
/* bdfi_error_from_reader fills err from the reader's failure. */
BDF_INTERNAL void bdfi_error_from_reader(bdf_error *err, const bdfi_reader *r);

/* ---- Memory ------------------------------------------------------------- */

/* bdfi_alloc_items allocates n zeroed items of size bytes, or sets *oom; n
 * may be 0, which returns NULL without an error. */
BDF_INTERNAL void *bdfi_alloc_items(size_t n, size_t size, int *oom);

/* bdfi_arena hands out memory that is freed all at once. */
typedef struct bdfi_arena_chunk bdfi_arena_chunk;
typedef struct bdfi_arena {
  bdfi_arena_chunk *chunks;
} bdfi_arena;

BDF_INTERNAL void bdfi_arena_init(bdfi_arena *a);
/* bdfi_arena_alloc returns n zeroed bytes, aligned for any type, or NULL
 * when out of memory (n may be 0, which returns a non-NULL pointer). */
BDF_INTERNAL void *bdfi_arena_alloc(bdfi_arena *a, size_t n);
/* bdfi_arena_strdup copies n bytes and a NUL after them. */
BDF_INTERNAL char *bdfi_arena_strdup(bdfi_arena *a, const char *s, size_t n);
BDF_INTERNAL void bdfi_arena_free(bdfi_arena *a);

/* ---- Paths -------------------------------------------------------------- */

/* A path as the object and path collection decoders hold it. */
typedef struct bdfi_path_entry {
  int external;
  uint8_t *verbs;
  size_t nverbs;
  float *args;
  size_t nargs;
  uint8_t hash[16];
  uint32_t index;
} bdfi_path_entry;

/* bdfi_decode_path reads inline path data into *p. It returns 0, or -1 when
 * out of memory; a format failure is left in the reader. */
BDF_INTERNAL int bdfi_decode_path(bdfi_reader *r, bdfi_path_entry *p);
BDF_INTERNAL void bdfi_path_entry_clear(bdfi_path_entry *p);
BDF_INTERNAL void bdfi_path_entry_view(const bdfi_path_entry *p, bdf_path *out);

/* ---- JSON --------------------------------------------------------------- */

/*
 * A small JSON reader for the manifest: RFC 8259, strict about the grammar,
 * lenient about the bytes of strings the way encoding/json is (invalid UTF-8
 * and lone surrogates become U+FFFD). Values are kept in a flat node array
 * and the decoded strings in one buffer, so a document costs memory in
 * proportion to its size.
 */
enum {
  BDFI_JSON_NULL = 1,
  BDFI_JSON_FALSE,
  BDFI_JSON_TRUE,
  BDFI_JSON_NUMBER,
  BDFI_JSON_STRING,
  BDFI_JSON_ARRAY,
  BDFI_JSON_OBJECT
};

typedef struct bdfi_json_node {
  uint32_t kind;
  uint32_t next;    /* the next sibling, or 0 */
  uint32_t child;   /* arrays and objects: the first child, or 0 */
  uint32_t count;   /* arrays and objects: the number of children */
  uint32_t off;     /* strings: the decoded bytes; numbers: the text */
  uint32_t len;     /* their length */
  uint32_t key_off; /* object members: the decoded key */
  uint32_t key_len;
  uint32_t has_key;
} bdfi_json_node;

typedef struct bdfi_json {
  bdfi_json_node *nodes;
  uint32_t n, cap;
  uint8_t *buf;
  uint32_t buf_n, buf_cap;
} bdfi_json;

#define BDFI_JSON_MAX_DEPTH 1024

/* bdfi_json_parse parses one JSON value (and nothing after it) into *j,
 * which the caller zero-initialises and frees with bdfi_json_free. */
BDF_INTERNAL bdf_status bdfi_json_parse(const uint8_t *data, size_t len, bdfi_json *j,
                                        bdf_error *err);
BDF_INTERNAL void bdfi_json_free(bdfi_json *j);
BDF_INTERNAL const bdfi_json_node *bdfi_json_root(const bdfi_json *j);
/* bdfi_json_member returns the last member of an object named key, or NULL. */
BDF_INTERNAL const bdfi_json_node *bdfi_json_member(const bdfi_json *j, const bdfi_json_node *obj,
                                                    const char *key);
BDF_INTERNAL const bdfi_json_node *bdfi_json_first(const bdfi_json *j, const bdfi_json_node *c);
BDF_INTERNAL const bdfi_json_node *bdfi_json_next(const bdfi_json *j, const bdfi_json_node *n);
/* bdfi_json_text returns the bytes of a string, or the text of a number, or
 * the key of a member when key is set. A NUL follows them in the buffer,
 * beyond the length, so they serve as C strings when they hold no NUL. */
BDF_INTERNAL const char *bdfi_json_text(const bdfi_json *j, const bdfi_json_node *n, int key,
                                        size_t *len);
/* bdfi_json_number converts a number: 0, or -1 when it overflows a double. */
BDF_INTERNAL int bdfi_json_number(const bdfi_json *j, const bdfi_json_node *n, double *out);
/* bdfi_json_int64 converts a number that is an integer literal: 0, or -1
 * when it has a fraction or an exponent, or does not fit. */
BDF_INTERNAL int bdfi_json_int64(const bdfi_json *j, const bdfi_json_node *n, int64_t *out);

/* bdfi_base64_decode decodes standard base64 with padding into the arena:
 * 0, or -1 when the text is not base64. */
BDF_INTERNAL int bdfi_base64_decode(const char *s, size_t n, bdfi_arena *a, uint8_t **out,
                                    size_t *out_len);

/* ---- Manifest ----------------------------------------------------------- */

typedef struct bdfi_str {
  char *p; /* NUL-terminated; NULL when absent */
  size_t n;
} bdfi_str;

typedef struct bdfi_layer {
  bdfi_str role;
  uint8_t obj[16];
} bdfi_layer;

typedef struct bdfi_page {
  float w, h;
  int has_body;
  float body[4];
  size_t nlayers;
  bdfi_layer *layers;
} bdfi_page;

typedef struct bdfi_tile {
  bdfi_str key;
  bdfi_str obj;
} bdfi_tile;

typedef struct bdfi_view {
  bdfi_str id, kind, title, text_index, direction;
  size_t npages;
  bdfi_page *pages;
  int has_continuous;
  float gap;
  float tile; /* as stated; 0 when none */
  size_t ncols, nrows;
  float (*cols)[2];
  float (*rows)[2];
  int has_freeze;
  int64_t freeze_cols, freeze_rows;
  int gridlines;
  size_t ntiles;
  bdfi_tile *tiles;
  bdfi_str tiles_ref;
  int has_play;
  bdfi_str seq, audio, audio_type, cues;
} bdfi_view;

typedef struct bdfi_part {
  uint8_t hash[16];
  bdfi_str type, enc;
  int64_t len, size, off;
  int has_sealed;
  uint8_t sealed[16];
} bdfi_part;

typedef struct bdfi_key {
  bdfi_str type, kdf, crv;
  int64_t iter;
  uint8_t *salt, *epk, *key;
  size_t salt_len, epk_len, key_len;
} bdfi_key;

#define BDFI_DC_TERMS 17
/* bdfi_dc_term is the name of Dublin Core element i, in the order of spec §4.3. */
BDF_INTERNAL const char *bdfi_dc_term(size_t i);

typedef struct bdfi_dc {
  size_t n;
  bdfi_str *values;
} bdfi_dc;

typedef struct bdfi_manifest {
  bdfi_arena arena;
  int64_t bdf, opset;
  bdfi_str unit, source, generator;
  bdfi_dc dc[BDFI_DC_TERMS];
  int has_segment;
  bdfi_str segment_view;
  int64_t segment_from, segment_to;
  size_t nviews;
  bdfi_view *views;
  size_t nparts;
  bdfi_part *parts;
  int has_encryption;
  bdfi_str cipher;
  size_t nkeys;
  bdfi_key *keys;
  uint8_t sealed_manifest[16];
  bdfi_str sealed_manifest_enc;
} bdfi_manifest;

/* bdfi_manifest_parse reads and checks a manifest into *m, which the caller
 * zero-initialises; on failure *m holds nothing to free. */
BDF_INTERNAL bdf_status bdfi_manifest_parse(const uint8_t *json, size_t len, bdfi_manifest *m,
                                            bdf_error *err);
BDF_INTERNAL void bdfi_manifest_free(bdfi_manifest *m);

#endif /* BDF_INTERNAL_H */
