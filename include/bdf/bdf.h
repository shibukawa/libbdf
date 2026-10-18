/*
 * libbdf — a C reader for BDF (Browser-specific Document Format) v1.
 *
 * The format is specified by docs/spec.md of github.com/shibukawa/bdf. This
 * header is the whole public ABI; it follows a few rules so that it binds
 * cleanly from C++, Swift, C# (P/Invoke), Go (cgo or purego) and Python:
 *
 *   - every handle is an opaque pointer with a paired *_free function;
 *   - no struct is passed or returned by value; small plain structs are
 *     filled through a pointer, and never own memory;
 *   - strings carry their length, and are UTF-8;
 *   - nothing is global: two documents on two threads never touch each other.
 *
 * Pointers returned by accessors point into the handle they were asked
 * from, and stay valid until that handle is freed.
 *
 * Single-header build: tools/amalgamate.py joins the sources into one bdf.h.
 * Define BDF_IMPLEMENTATION in exactly one C translation unit before
 * including it; define BDF_STATIC as well to keep every function out of the
 * link namespace.
 */
#ifndef BDF_H
#define BDF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BDF_STATIC)
#define BDF_API static inline /* inline: the functions a program does not call do not warn */
#elif defined(_WIN32) && defined(BDF_SHARED)
#ifdef BDF_BUILDING
#define BDF_API __declspec(dllexport)
#else
#define BDF_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define BDF_API __attribute__((visibility("default")))
#else
#define BDF_API
#endif

#define BDF_VERSION_MAJOR  0
#define BDF_VERSION_MINOR  8
#define BDF_VERSION_PATCH  0
#define BDF_VERSION_STRING "0.8.0"

/* The wire format version and the instruction set (opset) this library reads. */
#define BDF_FORMAT_VERSION 1
#define BDF_OPSET_VERSION  1

/* bdf_version returns BDF_VERSION_STRING of the library that is linked. */
BDF_API const char *bdf_version(void);

typedef enum bdf_status {
  BDF_OK = 0,
  BDF_ERR_FORMAT = 1,     /* malformed data; bdf_error says where */
  BDF_ERR_ARG = 2,        /* a NULL or out-of-range argument */
  BDF_ERR_NOMEM = 3,
  BDF_ERR_UNSUPPORTED = 4, /* well-formed, but beyond this build or this library: a WOFF2 font
                              without a Brotli decoder, a font collection, a WOFF 1.0 font */
  BDF_ERR_PASSWORD = 5,    /* the password, or the key, does not open the document */
  BDF_ERR_TOO_LARGE = 6    /* input or output exceeds a configured resource limit */
} bdf_status;

/*
 * bdf_error reports where a decoder stopped. The message is a static string
 * and is never freed. A decoder of a part gives the offset into the data it
 * was given; the instruction iterator gives the offset into the op stream;
 * the manifest reader gives 0.
 */
typedef struct bdf_error {
  bdf_status status;
  size_t offset;
  const char *message; /* "" when status is BDF_OK */
} bdf_error;

/* bdf_free frees a buffer a bdf_* function returned as the caller's
 * (bdf_inflate, bdf_doc_part). */
BDF_API void bdf_free(void *p);

/* ---- Format and resource limits (spec §3.2, §4) ------------------------- */

#ifndef BDF_MAX_MANIFEST_SIZE
#define BDF_MAX_MANIFEST_SIZE (256u << 20) /* a manifest is at most 256 MiB */
#endif
/* Resource policy: bounds both stored and decoded parts. Builds for small
 * devices may lower this and the manifest/render limits. These are per
 * buffer limits, not a bound on the process's total heap. */
#ifndef BDF_MAX_PART_SIZE
#define BDF_MAX_PART_SIZE (256u << 20)
#endif
#define BDF_MAX_PAGE_SIZE     (1 << 24)    /* page sides and bodies, in units */
#define BDF_MAX_SHEET_ENTRIES (1 << 24)    /* rows, and columns, of a sheet */
#define BDF_MIN_ENTRY_SIZE    (1.0 / 64)   /* the smallest row or column that is not hidden */
#define BDF_DEFAULT_TILE      2048         /* the tile size of a sheet that states none */

/* ---- Single-file container (spec §3.3) ---------------------------------- */

#define BDF_HEADER_SIZE    32
#define BDF_FLAG_ENCRYPTED 0x0001u

typedef struct bdf_header {
  uint16_t version;      /* BDF_FORMAT_VERSION */
  uint16_t flags;        /* BDF_FLAG_ENCRYPTED or 0 */
  uint64_t manifest_off; /* from the start of the file */
  uint64_t manifest_len; /* stored (compressed) length */
  uint8_t manifest_enc;  /* 0 identity, 1 deflate-raw */
} bdf_header;

/*
 * bdf_header_parse checks the 32-byte header of a single-file BDF whose whole
 * size is file_size bytes, with the checks of the reference reader: magic,
 * version, defined flags, reserved bytes, and the manifest's range and size.
 * data holds at least the first len bytes of the file.
 */
BDF_API bdf_status bdf_header_parse(const uint8_t *data, size_t len, uint64_t file_size,
                                    bdf_header *out, bdf_error *err);

/* ---- Part names --------------------------------------------------------- */

/* bdf_hash_parse reads the 32-character hex name of a part (spec §3.1). */
BDF_API bdf_status bdf_hash_parse(const char *s, size_t len, uint8_t out[16]);
/* bdf_hash_format writes the lowercase hex name and a NUL. */
BDF_API void bdf_hash_format(const uint8_t hash[16], char out[33]);

/* ---- deflate-raw (spec §3.2) -------------------------------------------- */

/*
 * bdf_inflate inflates raw deflate data (RFC 1951) into a buffer the caller
 * frees with bdf_free. The output is bounded by limit: data that inflates to
 * more is an error, as the format asks of readers. Nothing past the final
 * block is read.
 */
BDF_API bdf_status bdf_inflate(const uint8_t *data, size_t len, size_t limit, uint8_t **out,
                               size_t *out_len, bdf_error *err);

/* ---- Fonts (spec §6.1) -------------------------------------------------- */

/* bdf_woff2_available reports whether this build decodes WOFF2 fonts (a
 * Brotli decoder is compiled in). */
BDF_API int bdf_woff2_available(void);

/*
 * bdf_woff2_decode unpacks a WOFF2 font into an sfnt font (TrueType or
 * CFF-flavored OpenType) the caller frees with bdf_free, rebuilding the
 * transformed glyf, loca and hmtx tables as the reference does: loca in the
 * long format with head.indexToLocFormat set to 1, the tables sorted by tag.
 * Font collections are BDF_ERR_UNSUPPORTED.
 */
BDF_API bdf_status bdf_woff2_decode(const uint8_t *data, size_t len, uint8_t **out,
                                    size_t *out_len, bdf_error *err);

/*
 * bdf_font_sfnt returns the bytes of a font part as an sfnt font a font
 * engine loads, in a buffer the caller frees with bdf_free: WOFF2 decoded,
 * TrueType and OpenType (including collections) copied as they are. WOFF
 * 1.0 is BDF_ERR_UNSUPPORTED; anything else is not a font.
 */
BDF_API bdf_status bdf_font_sfnt(const uint8_t *data, size_t len, uint8_t **out,
                                 size_t *out_len, bdf_error *err);

/* ---- Documents (spec §3, §4) -------------------------------------------- */

typedef struct bdf_doc bdf_doc;

/*
 * bdf_source supplies the bytes of a document. For the single-file form
 * read_at and size are used; for the split form read_part. ctx is passed
 * back to the callbacks, and the source struct is copied, so it may be a
 * local; what ctx points to must outlive the document.
 */
typedef struct bdf_source {
  void *ctx;
  /* read_at copies len bytes at off into buf: 0, or -1 when it cannot. */
  int (*read_at)(void *ctx, uint64_t off, uint8_t *buf, size_t len);
  /* size is the size of the single file. */
  uint64_t size;
  /*
   * read_part reads the stored bytes of a part of the split form: name is the
   * 32-character hex name under parts/, NUL-terminated. With buf NULL it only
   * sets *size to the number of bytes; otherwise it copies up to cap bytes and
   * sets *size to what it copied. It returns 0, or -1 when there is no such
   * part or it cannot be read.
   */
  int (*read_part)(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *size);
} bdf_source;

/* bdf_doc_open opens the single-file form from a source (read_at, size). */
BDF_API bdf_status bdf_doc_open(const bdf_source *src, bdf_doc **out, bdf_error *err);
/* bdf_doc_open_memory opens the single-file form held in memory; data must
 * outlive the document (it is not copied). */
BDF_API bdf_status bdf_doc_open_memory(const uint8_t *data, size_t len, bdf_doc **out,
                                       bdf_error *err);
/* bdf_doc_open_split opens the split form from its manifest.json and a
 * source whose read_part fetches the parts. */
BDF_API bdf_status bdf_doc_open_split(const uint8_t *manifest, size_t len, const bdf_source *src,
                                      bdf_doc **out, bdf_error *err);
BDF_API void bdf_doc_free(bdf_doc *doc);

/* bdf_doc_encrypted reports an encrypted document (spec §3.5); locked, one
 * that is not unlocked yet. The views of a locked document are those of its
 * outer manifest: none; its parts are the sealed parts, as stored. */
BDF_API int bdf_doc_encrypted(const bdf_doc *doc);
BDF_API int bdf_doc_locked(const bdf_doc *doc);

/*
 * bdf_doc_unlock opens an encrypted document with its password (UTF-8; it is
 * normalised to NFKC, as the format asks, so full-width and half-width
 * spellings are one password): the sealed manifest becomes the
 * manifest of the document, with its views, and bdf_doc_part opens the
 * parts. BDF_ERR_PASSWORD when no key slot opens with the password; the
 * document then stays locked, and another password can be tried. A document
 * that is not locked is BDF_ERR_ARG. Deriving the key takes a moment: the
 * format's 600,000 iterations are a few tenths of a second, and a file may
 * ask for up to ten million.
 *
 * Unlock a document before it is shared between threads.
 */
BDF_API bdf_status bdf_doc_unlock(bdf_doc *doc, const char *password, size_t len, bdf_error *err);
/* The iterations of the key derivation a document may ask for, over all its
 * password slots together: the format's limit. A build for a slow processor
 * may define a lower one; a document that asks for more is then refused as
 * malformed. */
#ifndef BDF_MAX_KDF_ITERATIONS
#define BDF_MAX_KDF_ITERATIONS 10000000
#endif
/*
 * bdf_doc_unlock_ecdh opens a document sealed for one reader (an ecdh key
 * slot; a segment a server sealed for one request, spec §3.6) with the
 * reader's private key: a P-256 scalar of 32 bytes, big-endian.
 * BDF_ERR_PASSWORD when no key slot opens with it.
 */
BDF_API bdf_status bdf_doc_unlock_ecdh(bdf_doc *doc, const uint8_t private_key[32],
                                       bdf_error *err);
/* bdf_p256_public_key is the public key of a private key (32 random bytes
 * that make a number from 1 to the order of the curve less 1), as the
 * uncompressed point of 65 bytes a segment request carries. BDF_ERR_ARG for a
 * private key out of range: draw another. */
BDF_API bdf_status bdf_p256_public_key(const uint8_t private_key[32], uint8_t out[65]);

/*
 * bdf_doc_add_segment puts the pages a segment document carries into doc
 * (spec §3.6): doc is the first segment that came, or a document of the same
 * views and pages, and segment one that came later, unlocked. Its parts are
 * added to those of doc, and its pages may draw parts earlier segments
 * brought. A segment of other views or pages is refused as one of another
 * document. On success doc owns segment: it is freed with doc, and is not
 * to be used or freed on its own. A document that was given away so takes no
 * segments itself and cannot be given again (BDF_ERR_ARG): add every segment
 * to the same document.
 */
BDF_API bdf_status bdf_doc_add_segment(bdf_doc *doc, bdf_doc *segment, bdf_error *err);
BDF_API int bdf_doc_split(const bdf_doc *doc);

/* bdf_doc_opset is the manifest's opset: 0 when it states none. */
BDF_API int64_t bdf_doc_opset(const bdf_doc *doc);
BDF_API const char *bdf_doc_unit(const bdf_doc *doc, size_t *len);

typedef struct bdf_meta {
  const char *source; /* the input format ("pdf", "pptx", ...), or "" */
  size_t source_len;
  const char *generator;
  size_t generator_len;
} bdf_meta;

BDF_API void bdf_doc_meta(const bdf_doc *doc, bdf_meta *out);
/* The Dublin Core elements of meta.dc (spec §4.3), by element name
 * ("title", "creator", ... "created", "modified"), each with its values. */
BDF_API size_t bdf_doc_dc_count(const bdf_doc *doc, const char *element);
BDF_API const char *bdf_doc_dc(const bdf_doc *doc, const char *element, size_t i, size_t *len);

typedef struct bdf_segment {
  const char *view; /* the view the segment belongs to */
  size_t view_len;
  int64_t from, to; /* the pages carried, [from, to) */
} bdf_segment;

/* bdf_doc_segment fills out and returns 1 for a segment document (spec §3.6). */
BDF_API int bdf_doc_segment(const bdf_doc *doc, bdf_segment *out);

typedef enum bdf_view_kind {
  BDF_VIEW_UNKNOWN = 0,
  BDF_VIEW_FIXED = 1,
  BDF_VIEW_FLOW = 2,
  BDF_VIEW_SHEET = 3,
  BDF_VIEW_SCROLL = 4
} bdf_view_kind;

typedef struct bdf_view {
  const char *id;
  size_t id_len;
  const char *kind; /* as written */
  size_t kind_len;
  bdf_view_kind kind_code;
  const char *title;
  size_t title_len;
  const char *direction; /* "ltr", "rtl" or "" */
  size_t direction_len;
  int rtl;
  const char *text_index; /* the name of the text index part, or "" */
  size_t text_index_len;
  size_t npages;
  int has_continuous;
  float gap;
  /* sheets */
  float tile;        /* the tile size: as stated, or BDF_DEFAULT_TILE */
  float tile_stated; /* as stated; 0 when none */
  size_t ncols, nrows; /* the runs of columns and of rows (bdf_doc_view_run), not their number */
  int has_freeze;
  int64_t freeze_cols, freeze_rows;
  int gridlines;
  size_t ntiles;         /* inline tiles */
  const char *tiles_ref; /* the name of the tile index part, or "" */
  size_t tiles_ref_len;
  /* play (spec §4.4): part names, or "" */
  int has_play;
  const char *seq;
  size_t seq_len;
  const char *audio;
  size_t audio_len;
  const char *audio_type;
  size_t audio_type_len;
  const char *cues;
  size_t cues_len;
} bdf_view;

BDF_API size_t bdf_doc_view_count(const bdf_doc *doc);
BDF_API bdf_status bdf_doc_view(const bdf_doc *doc, size_t i, bdf_view *out);
/* bdf_doc_view_run writes [count, size] of run i of the columns (rows 0) or
 * rows (rows 1) of a sheet. */
BDF_API bdf_status bdf_doc_view_run(const bdf_doc *doc, size_t view, int rows, size_t i,
                                    float out[2]);

typedef struct bdf_page {
  float w, h;
  int has_body;
  float body[4]; /* x y w h */
  size_t nlayers;
} bdf_page;

BDF_API bdf_status bdf_doc_page(const bdf_doc *doc, size_t view, size_t page, bdf_page *out);

typedef struct bdf_layer {
  const char *role; /* "body", "master", ...; unknown roles are drawn as body */
  size_t role_len;
  const uint8_t *obj; /* the 16-byte name of the object part */
} bdf_layer;

BDF_API bdf_status bdf_doc_layer(const bdf_doc *doc, size_t view, size_t page, size_t layer,
                                 bdf_layer *out);

typedef struct bdf_tile {
  const char *key; /* "tx,ty" */
  size_t key_len;
  const char *obj; /* the hex name of the tile's object part */
  size_t obj_len;
} bdf_tile;

/* bdf_doc_tile is inline tile i of a sheet (bdf_view.ntiles of them). */
BDF_API bdf_status bdf_doc_tile(const bdf_doc *doc, size_t view, size_t i, bdf_tile *out);
/* bdf_tile_key_parse reads "tx,ty": 1, or 0 when the key is not of that form. */
BDF_API int bdf_tile_key_parse(const char *key, size_t len, uint32_t *tx, uint32_t *ty);
/* bdf_tiles_index_find looks a tile up in a tile index part (spec §4.1):
 * 1 and the object's name, or 0. */
BDF_API int bdf_tiles_index_find(const uint8_t *idx, size_t len, uint32_t tx, uint32_t ty,
                                 uint8_t out[16]);

enum { BDF_ENC_IDENTITY = 0, BDF_ENC_DEFLATE_RAW = 1, BDF_ENC_UNKNOWN = -1 };

typedef struct bdf_part_entry {
  const uint8_t *hash; /* 16 bytes */
  const char *type;    /* "obj", "font", "img", "path", "idx", "seq", "audio", "sealed" */
  size_t type_len;
  int enc; /* BDF_ENC_* */
  const char *enc_name;
  size_t enc_name_len;
  int64_t len;  /* stored length */
  int64_t size; /* decoded length */
  int64_t off;  /* single form: from the start of the parts region */
  const uint8_t *sealed; /* encrypted documents: the sealed part, or NULL */
} bdf_part_entry;

/* The parts of a document: those of its manifest (once unlocked, of the
 * sealed one), and after them those of the segments it took, in the order
 * they were added. A part two of them carry is listed twice and read from
 * the first. */
BDF_API size_t bdf_doc_part_count(const bdf_doc *doc);
BDF_API bdf_status bdf_doc_part_entry(const bdf_doc *doc, size_t i, bdf_part_entry *out);
/* bdf_doc_part_find fills out (when not NULL) and returns 1 when the manifest
 * has the part. */
BDF_API int bdf_doc_part_find(const bdf_doc *doc, const uint8_t hash[16], bdf_part_entry *out);
/*
 * bdf_doc_part reads a part and decodes it (inflates deflate-raw, bounded by
 * the size the manifest states) into a buffer the caller frees with
 * bdf_free. Parts of a locked document are its sealed parts, as stored.
 */
BDF_API bdf_status bdf_doc_part(const bdf_doc *doc, const uint8_t hash[16], uint8_t **out,
                                size_t *out_len, bdf_error *err);

/* The encryption block of an encrypted document's outer manifest (spec §3.5). */
typedef struct bdf_key_slot {
  const char *type; /* "password" or "ecdh" */
  size_t type_len;
  const char *kdf;
  size_t kdf_len;
  int64_t iter;
  const uint8_t *salt;
  size_t salt_len;
  const char *crv;
  size_t crv_len;
  const uint8_t *epk;
  size_t epk_len;
  const uint8_t *key; /* the wrapped content key */
  size_t key_len;
} bdf_key_slot;

BDF_API const char *bdf_doc_cipher(const bdf_doc *doc, size_t *len);
BDF_API size_t bdf_doc_key_count(const bdf_doc *doc);
BDF_API bdf_status bdf_doc_key(const bdf_doc *doc, size_t i, bdf_key_slot *out);
/* bdf_doc_sealed_manifest is the name of the sealed manifest part and its
 * encoding, or NULL for a document that is not encrypted. */
BDF_API const uint8_t *bdf_doc_sealed_manifest(const bdf_doc *doc, const char **enc,
                                               size_t *enc_len);

/* ---- Constants of opset 1 (spec §7) ------------------------------------- */

enum {
  BDF_OP_SAVE = 0x01,
  BDF_OP_RESTORE = 0x02,
  BDF_OP_TRANSFORM = 0x03,
  BDF_OP_TRANSLATE = 0x04,
  BDF_OP_SCALE = 0x05,
  BDF_OP_CLIP_PATH = 0x06,
  BDF_OP_CLIP_RECT = 0x07,
  BDF_OP_FILL_COLOR = 0x10,
  BDF_OP_FILL_PAINT = 0x11,
  BDF_OP_STROKE_COLOR = 0x12,
  BDF_OP_STROKE_PAINT = 0x13,
  BDF_OP_LINE = 0x14,
  BDF_OP_DASH = 0x15,
  BDF_OP_ALPHA = 0x16,
  BDF_OP_BLEND = 0x17,
  BDF_OP_SHADOW = 0x18,
  BDF_OP_FILTER = 0x19,
  BDF_OP_FONT = 0x1A,
  BDF_OP_TEXT_STYLE = 0x1B,
  BDF_OP_FILL_RECT = 0x20,
  BDF_OP_STROKE_RECT = 0x21,
  BDF_OP_FILL_PATH = 0x22,
  BDF_OP_STROKE_PATH = 0x23,
  BDF_OP_FILL_PATH_AT = 0x24,
  BDF_OP_FILL_PATH_RUN = 0x25,
  BDF_OP_CLEAR_RECT = 0x26,
  BDF_OP_FILL_TEXT = 0x30,
  BDF_OP_STROKE_TEXT = 0x31,
  BDF_OP_IMAGE = 0x40,
  BDF_OP_IMAGE_SUB = 0x41,
  BDF_OP_SMOOTHING = 0x42,
  BDF_OP_USE = 0x50,
  BDF_OP_USE_AT = 0x51,
  BDF_OP_GROUP_BEGIN = 0x52,
  BDF_OP_GROUP_END = 0x53,
  BDF_OP_MASK_BEGIN = 0x54,
  BDF_OP_MASK_END = 0x55,
  BDF_OP_LINK = 0x70,
  BDF_OP_MARK = 0x71,
  BDF_OP_EXT = 0xFF
};

enum { BDF_FILL_NONZERO = 0, BDF_FILL_EVENODD = 1 };
enum { BDF_CAP_BUTT = 0, BDF_CAP_ROUND = 1, BDF_CAP_SQUARE = 2 };
enum { BDF_JOIN_MITER = 0, BDF_JOIN_ROUND = 1, BDF_JOIN_BEVEL = 2 };
enum {
  BDF_ALIGN_LEFT = 0,
  BDF_ALIGN_RIGHT = 1,
  BDF_ALIGN_CENTER = 2,
  BDF_ALIGN_START = 3,
  BDF_ALIGN_END = 4
};
enum {
  BDF_BASELINE_ALPHABETIC = 0,
  BDF_BASELINE_TOP = 1,
  BDF_BASELINE_MIDDLE = 2,
  BDF_BASELINE_BOTTOM = 3,
  BDF_BASELINE_HANGING = 4,
  BDF_BASELINE_IDEOGRAPHIC = 5
};
enum { BDF_DIR_INHERIT = 0, BDF_DIR_LTR = 1, BDF_DIR_RTL = 2 };
enum { BDF_MASK_ALPHA = 0, BDF_MASK_LUMINOSITY = 1 };

/* Blend modes (globalCompositeOperation), spec appendix A. */
enum {
  BDF_BLEND_SOURCE_OVER = 0,
  BDF_BLEND_MULTIPLY = 1,
  BDF_BLEND_SCREEN = 2,
  BDF_BLEND_OVERLAY = 3,
  BDF_BLEND_DARKEN = 4,
  BDF_BLEND_LIGHTEN = 5,
  BDF_BLEND_COLOR_DODGE = 6,
  BDF_BLEND_COLOR_BURN = 7,
  BDF_BLEND_HARD_LIGHT = 8,
  BDF_BLEND_SOFT_LIGHT = 9,
  BDF_BLEND_DIFFERENCE = 10,
  BDF_BLEND_EXCLUSION = 11,
  BDF_BLEND_HUE = 12,
  BDF_BLEND_SATURATION = 13,
  BDF_BLEND_COLOR = 14,
  BDF_BLEND_LUMINOSITY = 15,
  BDF_BLEND_DESTINATION_OVER = 16,
  BDF_BLEND_DESTINATION_IN = 17,
  BDF_BLEND_DESTINATION_OUT = 18,
  BDF_BLEND_SOURCE_IN = 19,
  BDF_BLEND_SOURCE_OUT = 20,
  BDF_BLEND_SOURCE_ATOP = 21,
  BDF_BLEND_DESTINATION_ATOP = 22,
  BDF_BLEND_XOR = 23,
  BDF_BLEND_COPY = 24,
  BDF_BLEND_LIGHTER = 25
};

/* Path verbs (spec §6.3) and the number of f32 operands of each. */
enum {
  BDF_VERB_MOVE = 0,      /* x y */
  BDF_VERB_LINE = 1,      /* x y */
  BDF_VERB_QUAD = 2,      /* cx cy x y */
  BDF_VERB_CUBIC = 3,     /* c1x c1y c2x c2y x y */
  BDF_VERB_CLOSE = 4,     /* – */
  BDF_VERB_RECT = 5,      /* x y w h */
  BDF_VERB_ELLIPSE = 6,   /* x y rx ry rot a0 a1 ccw */
  BDF_VERB_ARC_TO = 7,    /* x1 y1 x2 y2 r */
  BDF_VERB_ROUND_RECT = 8 /* x y w h r */
};

/* bdf_verb_args returns the number of f32 operands of a verb, or -1. */
BDF_API int bdf_verb_args(uint8_t verb);

/* Paint kinds (spec §6.4). */
enum { BDF_PAINT_LINEAR = 0, BDF_PAINT_RADIAL = 1, BDF_PAINT_CONIC = 2, BDF_PAINT_PATTERN = 3 };
enum { BDF_REPEAT = 0, BDF_REPEAT_X = 1, BDF_REPEAT_Y = 2, BDF_NO_REPEAT = 3 };

/* Font kinds (spec §6.1). */
enum { BDF_FONT_EMBEDDED = 0, BDF_FONT_SYSTEM = 1 };
enum { BDF_STYLE_NORMAL = 0, BDF_STYLE_ITALIC = 1, BDF_STYLE_OBLIQUE = 2 };

/* bdf_op_name returns the name of an opcode as the spec writes it
 * ("FILL_PATH"), or "" for a byte that is no opcode. */
BDF_API const char *bdf_op_name(uint8_t op);

/* ---- Resources ---------------------------------------------------------- */

/* bdf_path is one entry of an object's path table, or one path of a path
 * collection part. The pointers belong to the object or collection. */
typedef struct bdf_path {
  int external;         /* 0: verbs and args; 1: hash and index into a path collection part */
  const uint8_t *verbs; /* BDF_VERB_* */
  size_t nverbs;
  const float *args; /* the operands of all verbs, in verb order */
  size_t nargs;
  const uint8_t *hash; /* 16 bytes; external only */
  uint32_t index;      /* external only */
} bdf_path;

typedef struct bdf_stop {
  float offset;
  uint32_t color; /* 0xRRGGBBAA */
} bdf_stop;

typedef struct bdf_paint {
  uint8_t kind;        /* BDF_PAINT_* */
  const float *coords; /* linear: x0 y0 x1 y1; radial: x0 y0 r0 x1 y1 r1; conic: angle x y */
  size_t ncoords;
  const bdf_stop *stops; /* gradients only */
  size_t nstops;
  uint64_t image;  /* pattern only: index into the object's image table */
  uint8_t repeat;  /* pattern only: BDF_REPEAT* */
  float matrix[6]; /* pattern only */
} bdf_paint;

typedef struct bdf_font {
  uint8_t kind;        /* BDF_FONT_* */
  const uint8_t *hash; /* 16 bytes; embedded only, else NULL */
  const char *family;  /* UTF-8 CSS family list; NUL-terminated as a convenience */
  size_t family_len;
  uint16_t weight; /* 100–900 */
  uint8_t style;   /* BDF_STYLE_* */
} bdf_font;

/* ---- Object part (spec §5) ---------------------------------------------- */

typedef struct bdf_object bdf_object;

/*
 * bdf_object_decode parses an Object part: its header, resource tables and
 * the bytes of its op stream, with the checks of the reference reader (every
 * count is bounded by the data that follows it, so the memory an object takes
 * is bounded by its size). The instructions themselves are checked as they
 * are read with bdf_instr_next. The object copies what it needs, so data may
 * be freed after the call.
 */
BDF_API bdf_status bdf_object_decode(const uint8_t *data, size_t len, bdf_object **out,
                                     bdf_error *err);
BDF_API void bdf_object_free(bdf_object *obj);

BDF_API uint16_t bdf_object_opset(const bdf_object *obj);
/* bdf_object_bbox writes x, y, w, h: the range the object draws, in its own
 * coordinates. */
BDF_API void bdf_object_bbox(const bdf_object *obj, float out[4]);

BDF_API size_t bdf_object_string_count(const bdf_object *obj);
/* The strings are UTF-8 and NUL-terminated as a convenience; they may hold
 * NUL bytes of their own, so use the length. NULL when i is out of range.
 * A byte order mark at the start of a string of a part is not part of it
 * (the format says so): here and in the runs of a text index, the string is
 * what follows the mark. */
BDF_API const char *bdf_object_string(const bdf_object *obj, size_t i, size_t *len);

BDF_API size_t bdf_object_path_count(const bdf_object *obj);
BDF_API bdf_status bdf_object_path(const bdf_object *obj, size_t i, bdf_path *out);

BDF_API size_t bdf_object_paint_count(const bdf_object *obj);
BDF_API bdf_status bdf_object_paint(const bdf_object *obj, size_t i, bdf_paint *out);

BDF_API size_t bdf_object_font_count(const bdf_object *obj);
BDF_API bdf_status bdf_object_font(const bdf_object *obj, size_t i, bdf_font *out);

/* The image and child-object tables hold 16-byte part names. NULL when i is
 * out of range. */
BDF_API size_t bdf_object_image_count(const bdf_object *obj);
BDF_API const uint8_t *bdf_object_image(const bdf_object *obj, size_t i);
BDF_API size_t bdf_object_object_count(const bdf_object *obj);
BDF_API const uint8_t *bdf_object_object(const bdf_object *obj, size_t i);

/* bdf_object_ops returns the raw op stream. */
BDF_API const uint8_t *bdf_object_ops(const bdf_object *obj, size_t *len);

/* ---- Instructions (spec §7) --------------------------------------------- */

/*
 * A bdf_instr walks the op stream of an object one instruction at a time.
 * The operands of the current instruction are reached by index, in the
 * order the spec lists them; each has a kind. An unknown opcode, an operand
 * past the end of the stream, a string reference past the string table, or
 * a count larger than the stream can hold is an error, after which the walk
 * is over. The object must outlive the iterator.
 */
typedef struct bdf_instr bdf_instr;

typedef enum bdf_arg_kind {
  BDF_ARG_NONE = 0,
  BDF_ARG_F32 = 1,     /* bdf_instr_f32 */
  BDF_ARG_U8 = 2,      /* bdf_instr_u64 */
  BDF_ARG_U32 = 3,     /* bdf_instr_u64: a colour 0xRRGGBBAA, or the backdrop of MASK_BEGIN */
  BDF_ARG_VARUINT = 4, /* bdf_instr_u64: a reference or a count */
  BDF_ARG_STRING = 5,  /* bdf_instr_string: the string a str operand refers to */
  BDF_ARG_BYTES = 6,   /* bdf_instr_bytes: MASK_BEGIN transfer, EXT payload */
  BDF_ARG_F32S = 7,    /* bdf_instr_f32s: DASH segments */
  BDF_ARG_GLYPHS = 8   /* bdf_instr_glyphs: FILL_PATH_RUN */
} bdf_arg_kind;

typedef struct bdf_glyph {
  uint64_t path; /* index into the object's path table */
  float x, y;
} bdf_glyph;

BDF_API bdf_instr *bdf_instr_new(const bdf_object *obj); /* NULL when out of memory */
BDF_API void bdf_instr_free(bdf_instr *it);
BDF_API void bdf_instr_rewind(bdf_instr *it);

/* bdf_instr_next moves to the next instruction: 1 when there is one, 0 at the
 * end of the stream, -1 on an error (err says where; the walk is over). */
BDF_API int bdf_instr_next(bdf_instr *it, bdf_error *err);

BDF_API uint8_t bdf_instr_op(const bdf_instr *it);
/* bdf_instr_offset is the offset of the opcode in the op stream. */
BDF_API size_t bdf_instr_offset(const bdf_instr *it);
BDF_API size_t bdf_instr_arg_count(const bdf_instr *it);
BDF_API bdf_arg_kind bdf_instr_arg_kind_at(const bdf_instr *it, size_t i);
BDF_API float bdf_instr_f32(const bdf_instr *it, size_t i);
BDF_API uint64_t bdf_instr_u64(const bdf_instr *it, size_t i);
BDF_API const char *bdf_instr_string(const bdf_instr *it, size_t i, size_t *len);
BDF_API const uint8_t *bdf_instr_bytes(const bdf_instr *it, size_t i, size_t *len);
BDF_API const float *bdf_instr_f32s(const bdf_instr *it, size_t i, size_t *n);
BDF_API const bdf_glyph *bdf_instr_glyphs(const bdf_instr *it, size_t i, size_t *n);

/* ---- Path collection part (spec §6.3) ----------------------------------- */

typedef struct bdf_paths bdf_paths;

BDF_API bdf_status bdf_paths_decode(const uint8_t *data, size_t len, bdf_paths **out,
                                    bdf_error *err);
BDF_API void bdf_paths_free(bdf_paths *paths);
BDF_API size_t bdf_paths_count(const bdf_paths *paths);
/* The path at i; never external. */
BDF_API bdf_status bdf_paths_get(const bdf_paths *paths, size_t i, bdf_path *out);

/* ---- Text (spec §7.4, §7.7, §7.8) --------------------------------------- */

/* MARK kinds (spec §7.8). */
enum {
  BDF_MARK_PARAGRAPH = 0,
  BDF_MARK_LINE = 1,
  BDF_MARK_CELL = 2,
  BDF_MARK_BOX = 3,
  BDF_MARK_ALT_TEXT = 4,
  BDF_MARK_WRAP = 5,
  BDF_MARK_HEADING = 6,
  BDF_MARK_LIST = 7,
  BDF_MARK_LIST_ITEM = 8,
  BDF_MARK_TABLE = 9,
  BDF_MARK_FIGURE = 10,
  BDF_MARK_END = 11,
  BDF_MARK_LANG = 12
};

/* Separators between consecutive runs (spec §7.9): nothing, a space, a
 * paragraph break. */
enum { BDF_SEP_NONE = 0, BDF_SEP_SPACE = 1, BDF_SEP_BREAK = 2 };

/* Limits of walking the objects an object draws with USE (spec §7.6): how
 * deep, and how many instructions are read from objects that were walked
 * before. A walk past either fails. A build of the library may define the
 * second lower. */
#define BDF_MAX_USE_DEPTH 64
#ifndef BDF_MAX_REUSED_INSTRUCTIONS
#define BDF_MAX_REUSED_INSTRUCTIONS (1 << 27)
#endif

/*
 * bdf_text holds the text runs, the links and the structure of an object and
 * the objects it draws, in walk order: what a text layer, a selection, a
 * search or a screen reader needs. The runs are those of the reference
 * implementation's ExtractText, with the same order, ordinals and
 * separators; the structure is that of its TypeScript reader
 * (extractContent).
 */
typedef struct bdf_text bdf_text;

/* bdf_resolve_fn returns the decoded object of a part name, or NULL when
 * there is none (the child is skipped). */
typedef const bdf_object *(*bdf_resolve_fn)(void *ctx, const uint8_t hash[16]);

/*
 * bdf_text_extract walks an object. resolve gives the objects it draws with
 * USE (NULL: none are walked). The strings of the text point into the
 * objects, which must outlive it.
 */
BDF_API bdf_status bdf_text_extract(const bdf_object *obj, bdf_resolve_fn resolve, void *ctx,
                                    bdf_text **out, bdf_error *err);
BDF_API void bdf_text_free(bdf_text *text);

typedef struct bdf_text_run {
  const char *text; /* UTF-8, NUL-terminated as a convenience */
  size_t text_len;
  float x, y;      /* the anchor (the x, y of FILL_TEXT) after the transform */
  float advance;   /* the expected width, in the units of the drawing; 0 when unknown */
  float size;      /* the font size */
  float matrix[6]; /* the transform at the time of drawing: a b c d e f */
  int has_font;
  bdf_font font;    /* when has_font */
  uint8_t align;    /* BDF_ALIGN_* */
  uint8_t sep;      /* BDF_SEP_*: the separator from the run before */
  int alt_text;     /* the text of an ALT_TEXT mark: nothing was drawn with it */
  uint32_t ordinal; /* the position in the walk; matches the text index part */
  int64_t node;     /* the structure node the run belongs to (bdf_text_node_at) */
  const char *lang; /* the language a MARK LANG set (BCP 47); "" for the document's */
  size_t lang_len;
} bdf_text_run;

BDF_API size_t bdf_text_run_count(const bdf_text *text);
BDF_API bdf_status bdf_text_run_at(const bdf_text *text, size_t i, bdf_text_run *out);

/* A LINK area: the bounding box of its rectangle after the transform. */
typedef struct bdf_text_link {
  float x, y, w, h;
  const char *url; /* as written; see bdf_link_parse */
  size_t url_len;
  int64_t after; /* the index of the last run before the LINK, -1 before the first */
  int64_t node;  /* the structure node open at the LINK, -1 at the top level */
} bdf_text_link;

BDF_API size_t bdf_text_link_count(const bdf_text *text);
BDF_API bdf_status bdf_text_link_at(const bdf_text *text, size_t i, bdf_text_link *out);

/*
 * The structure the MARKs give the text (spec §7.8), for reading aloud.
 * Leaves hold runs: a paragraph, a heading, the caption of a table, and a
 * cell outside a table (a sheet's). Containers hold nodes: a list, an item,
 * a table, a cell inside a table, a figure. Nodes are in the order they
 * open, and each names its parent.
 */
typedef enum bdf_node_kind {
  BDF_NODE_PARAGRAPH = 0,
  BDF_NODE_HEADING = 1,
  BDF_NODE_CAPTION = 2,
  BDF_NODE_LIST = 3,
  BDF_NODE_ITEM = 4,
  BDF_NODE_TABLE = 5,
  BDF_NODE_CELL = 6,
  BDF_NODE_FIGURE = 7
} bdf_node_kind;

enum { BDF_SCOPE_NONE = 0, BDF_SCOPE_COL = 1, BDF_SCOPE_ROW = 2 };

typedef struct bdf_text_node {
  uint8_t kind;   /* BDF_NODE_* */
  int64_t parent; /* the node around it, -1 at the top level */
  uint8_t level;  /* a heading: 1 to 6, or 0 when its mark names no level */
  /* a cell whose mark names a place: from 0, and how many rows and columns
   * it spans; in a table from the table's first cell, else in the sheet */
  int has_cell;
  uint32_t row, col, rows, cols;
  uint8_t scope;   /* BDF_SCOPE_*: a cell that heads its column or row */
  const char *alt; /* a figure: what to read for it; "" for a decoration */
  size_t alt_len;
  /* a figure: the box around what it draws, clipped, in the space of the
   * object; of no size, where the figure opens, when it draws nothing */
  int has_bounds;
  float x, y, w, h;
} bdf_text_node;

BDF_API size_t bdf_text_node_count(const bdf_text *text);
BDF_API bdf_status bdf_text_node_at(const bdf_text *text, size_t i, bdf_text_node *out);

/* What a LINK url may be (spec §7.7); a reader makes no link of anything else. */
typedef enum bdf_link_kind {
  BDF_LINK_NONE = 0, /* not a link a reader follows */
  BDF_LINK_URL = 1,  /* an http, https or mailto URL */
  BDF_LINK_PAGE = 2, /* "#page=N": a page of the same view */
  BDF_LINK_VIEW = 3  /* "#view=ID" or "#view=ID&page=N": another view */
} bdf_link_kind;

typedef struct bdf_link_target {
  bdf_link_kind kind;
  int64_t page;     /* PAGE, and VIEW with a page: from 1; 0 when a VIEW link names none */
  const char *view; /* VIEW: the id as the url writes it (URL-encoded), inside url */
  size_t view_len;
} bdf_link_target;

/* bdf_link_parse classifies a LINK url; out may be NULL. */
BDF_API bdf_link_kind bdf_link_parse(const char *url, size_t len, bdf_link_target *out);
/* bdf_link_view_id decodes the id of a VIEW link (percent-encoding, and "+"
 * as a space) into buf: its length, or -1 when it does not decode or does
 * not fit in cap bytes. No NUL is written. */
BDF_API int64_t bdf_link_view_id(const bdf_link_target *target, char *buf, size_t cap);
/* bdf_doc_view_find is the index of the view with an id, or -1. */
BDF_API int64_t bdf_doc_view_find(const bdf_doc *doc, const char *id, size_t len);

/* bdf_doc_object reads and decodes an object part; free it with bdf_object_free. */
BDF_API bdf_status bdf_doc_object(const bdf_doc *doc, const uint8_t hash[16], bdf_object **out,
                                  bdf_error *err);
/*
 * bdf_doc_text is the text of a top-level object of a document (the object
 * of a page's layer, or of a tile), with the objects it draws read from the
 * document. The text owns those objects.
 */
BDF_API bdf_status bdf_doc_text(const bdf_doc *doc, const uint8_t hash[16], bdf_text **out,
                                bdf_error *err);

/* ---- Text index (spec §7.9) --------------------------------------------- */

/* The runs of a view in reading order: a text index part, or the same made
 * from the objects of a view. */
typedef struct bdf_text_index bdf_text_index;

typedef struct bdf_index_run {
  uint32_t a, b;    /* page and layer; in a sheet, tile x and y */
  uint32_t ordinal; /* bdf_text_run.ordinal in that object */
  uint8_t sep;      /* BDF_SEP_* */
  const char *text;
  size_t text_len;
} bdf_index_run;

BDF_API bdf_status bdf_text_index_decode(const uint8_t *data, size_t len, bdf_text_index **out,
                                         bdf_error *err);
BDF_API void bdf_text_index_free(bdf_text_index *index);
BDF_API size_t bdf_text_index_count(const bdf_text_index *index);
BDF_API bdf_status bdf_text_index_run(const bdf_text_index *index, size_t i, bdf_index_run *out);
/* bdf_text_index_plain joins the runs into plain text (a space or a line
 * break where a run's separator says so), NUL-terminated, in a buffer the
 * caller frees with bdf_free. */
BDF_API bdf_status bdf_text_index_plain(const bdf_text_index *index, char **out, size_t *out_len,
                                        bdf_error *err);

/*
 * bdf_doc_view_text is the text of a view: its text index part when it has
 * one and extract is 0, and otherwise extracted from its objects (the
 * layers of its pages; in a sheet its inline tiles, each keeping the runs
 * anchored in it). The limits of BDF_MAX_* count across the view.
 */
BDF_API bdf_status bdf_doc_view_text(const bdf_doc *doc, size_t view, int extract,
                                     bdf_text_index **out, bdf_error *err);

/*
 * The text of a document for a search index outside it, page by page: that
 * of bdf_doc_view_text for each view, without the pages that have none, and
 * without a scroll view when the document also has a flow view (the same
 * text laid out both ways). Normalisation (NFKC, case, kana) is left to the
 * search engine. A locked document has no views, and so no text.
 */
typedef struct bdf_search_text bdf_search_text;

typedef struct bdf_search_view {
  const char *id;
  size_t id_len;
  const char *kind;
  size_t kind_len;
  const char *title;
  size_t title_len;
  size_t npages;
} bdf_search_view;

typedef struct bdf_search_page {
  int64_t page; /* from 1, for links like #page=N; 0 for a whole sheet */
  const char *text;
  size_t text_len;
} bdf_search_page;

BDF_API bdf_status bdf_doc_search_text(const bdf_doc *doc, bdf_search_text **out, bdf_error *err);
BDF_API void bdf_search_text_free(bdf_search_text *st);
BDF_API size_t bdf_search_text_view_count(const bdf_search_text *st);
BDF_API bdf_status bdf_search_text_view(const bdf_search_text *st, size_t i, bdf_search_view *out);
BDF_API bdf_status bdf_search_text_page(const bdf_search_text *st, size_t view, size_t i,
                                        bdf_search_page *out);

/* ---- Search -------------------------------------------------------------- */

/*
 * bdf_text_search finds text in the runs of a text index (a view's part, or
 * bdf_doc_view_text) the way the reference's viewer does: characters are
 * taken for what they are to a reader (NFKC: full and half width, ligatures,
 * circled and squared forms, a letter and the accent after it as the one
 * letter; katakana as hiragana; the minus sign as a hyphen; and, unless the
 * search is case-sensitive, capitals as small letters), and a phrase is
 * found across the runs of a line and across lines, but not across a
 * paragraph break. The text of a run and a query are each normalised as a
 * whole; a run is normalised by itself, so a letter at the end of one and an
 * accent at the start of the next stay two. A hit covers whole characters as
 * a reader sees them: a letter with its accents, a Hangul syllable. The
 * index must outlive the search.
 */
typedef struct bdf_text_search bdf_text_search;

BDF_API bdf_status bdf_text_search_new(const bdf_text_index *index, int case_sensitive,
                                       bdf_text_search **out, bdf_error *err);
BDF_API void bdf_text_search_free(bdf_text_search *search);
/* bdf_text_search_plain is the text that is searched: normalised, a space or
 * a line break where a run's separator says so. NUL-terminated, in a buffer
 * the caller frees with bdf_free. */
BDF_API bdf_status bdf_text_search_plain(const bdf_text_search *search, char **out, size_t *out_len,
                                         bdf_error *err);
/* bdf_text_search_normalize is text as a search has it (a query before its
 * white space is joined), in a buffer the caller frees with bdf_free. */
BDF_API bdf_status bdf_text_search_normalize(const char *s, size_t len, int case_sensitive,
                                             char **out, size_t *out_len, bdf_error *err);

/* What the reference's viewer asks for: at most 1000 hits, 40 characters of
 * context on each side. */
#define BDF_SEARCH_LIMIT   1000
#define BDF_SEARCH_CONTEXT 40

typedef struct bdf_text_hits bdf_text_hits;

/*
 * bdf_text_search_find is the hits of a query, in the order of the text, at
 * most limit of them (hits may overlap: each starts a character after the
 * one before at the earliest). White space of any kind and length in the
 * query is one space. context is how many characters of the searchable text
 * each hit keeps on each side of it. An empty query has no hits.
 */
BDF_API bdf_status bdf_text_search_find(const bdf_text_search *search, const char *query,
                                        size_t len, size_t limit, size_t context,
                                        bdf_text_hits **out, bdf_error *err);
BDF_API void bdf_text_hits_free(bdf_text_hits *hits);
BDF_API size_t bdf_text_hits_count(const bdf_text_hits *hits);

typedef struct bdf_text_hit {
  const char *text; /* what was found, as the document has it (not normalised) */
  size_t text_len;
  const char *context; /* the searchable text around it; a break shows as " ⏎ " */
  size_t context_len;
  size_t nsegments; /* the runs it lies in: one or more in a row */
} bdf_text_hit;

BDF_API bdf_status bdf_text_hits_at(const bdf_text_hits *hits, size_t i, bdf_text_hit *out);

/* The part of a hit inside one run. */
typedef struct bdf_text_hit_segment {
  size_t run;        /* the index of the run in the text index */
  uint32_t a, b;     /* that run's page and layer, or tile x and y */
  uint32_t ordinal;  /* and its ordinal in the object (bdf_text_run.ordinal) */
  size_t start, end; /* the bytes of the run's text the hit covers: [start, end) */
} bdf_text_hit_segment;

BDF_API bdf_status bdf_text_hits_segment(const bdf_text_hits *hits, size_t i, size_t k,
                                         bdf_text_hit_segment *out);

/* ---- Rendering ----------------------------------------------------------- */

/*
 * bdf_renderer draws the views of a document into images without a browser:
 * the pages of fixed and flow views, stretches of scroll views and regions
 * of sheets, for thumbnails, previews and prints. It is a port of the
 * reference implementation's raster/imagebdf, and draws the same pixels: a
 * software rasterizer that runs the instructions of the objects as a Canvas
 * 2D would (anti-aliased paths, strokes with joins, caps and dashes, clips,
 * gradients, groups with their alpha and blend mode, soft masks, shadows).
 * What it leaves out it warns of (bdf_renderer_warning).
 *
 * A renderer keeps what it decodes for the next drawing. It is not to be
 * used from two threads at a time; the document must outlive it.
 */
typedef struct bdf_renderer bdf_renderer;

/*
 * bdf_fonts is a set of fonts for the text that names its font instead of
 * carrying it (HTML, Markdown and EPUB documents, and others converted
 * with system fonts) and for the characters an embedded font lacks. The
 * renderer finds a face for a family in it as the reference implementation
 * does: the family itself, a family that looks alike or has its metrics, a
 * generic family of the same kind, and at last any face. libbdf reads no
 * font directories: the application adds the files it wants drawn with, in
 * the order they are to be preferred in.
 *
 * bdf_fonts_add copies a font file that is in memory (TrueType, OpenType,
 * or a collection of them); bdf_fonts_add_file takes a path, reads the
 * names of its faces now and the glyphs when a renderer first draws with
 * one. The path is UTF-8 on every system (on Windows a path that is not, or
 * that names no file so, is then tried in the system's code page). Both
 * return BDF_ERR_FORMAT for what has no face that can be read. A
 * set is not changed once a renderer has it; renderers on several threads
 * may share one, which must outlive them.
 */
typedef struct bdf_fonts bdf_fonts;

BDF_API bdf_status bdf_fonts_new(bdf_fonts **out, bdf_error *err);
BDF_API void bdf_fonts_free(bdf_fonts *fonts);
BDF_API bdf_status bdf_fonts_add(bdf_fonts *fonts, const uint8_t *data, size_t len,
                                 bdf_error *err);
BDF_API bdf_status bdf_fonts_add_file(bdf_fonts *fonts, const char *path, bdf_error *err);
/* bdf_fonts_count returns the number of faces in the set. */
BDF_API size_t bdf_fonts_count(const bdf_fonts *fonts);

typedef struct bdf_render_options {
  /* The colour behind the content, 0xRRGGBBAA, when has_background: else
   * white for pages and regions, and nothing for bdf_render_object. A
   * background of 0 is a transparent page. */
  int has_background;
  uint32_t background;
  /* The fonts for text that names its font, or NULL: such text is then not
   * drawn, with a warning. */
  const bdf_fonts *fonts;
} bdf_render_options;

BDF_API bdf_status bdf_renderer_new(const bdf_doc *doc, const bdf_render_options *opts,
                                    bdf_renderer **out, bdf_error *err);
BDF_API void bdf_renderer_free(bdf_renderer *renderer);

/* An image a renderer drew: 4 bytes a pixel, red, green, blue and alpha,
 * the colours multiplied by the alpha, rows from the top without padding. */
typedef struct bdf_image {
  int width, height;
  int x, y;        /* bdf_render_object: where the first pixel lies from the origin of the object */
  uint8_t *pixels; /* width × height × 4 bytes; free the image with bdf_image_free */
} bdf_image;

BDF_API void bdf_image_free(bdf_image *image);

/* The most pixels of an image a renderer draws (a canvas takes 16 bytes a
 * pixel while it is drawn): a larger one is BDF_ERR_TOO_LARGE. */
#ifndef BDF_MAX_RENDER_PIXELS
#define BDF_MAX_RENDER_PIXELS (64 << 20)
#endif

/* bdf_render_page draws a whole page of a fixed or flow view (or a strip of
 * a scroll view) at scale device pixels per unit. The units of a page are
 * points, so a scale of dpi / 72 draws at dpi pixels per inch. */
BDF_API bdf_status bdf_render_page(bdf_renderer *renderer, size_t view, size_t page, double scale,
                                   bdf_image *out, bdf_error *err);
/* bdf_render_page_size draws a whole page into exactly w × h pixels,
 * stretched when they have another aspect ratio. */
BDF_API bdf_status bdf_render_page_size(bdf_renderer *renderer, size_t view, size_t page, int w,
                                        int h, bdf_image *out, bdf_error *err);
/*
 * bdf_render_region draws a rectangle (x, y, w, h in units) of a view into
 * a w × h image: of a page of a fixed or flow view, of the continuous
 * layout of a scroll view (its strips stacked, in units from the top), or
 * of a sheet (page is ignored in the last two).
 */
BDF_API bdf_status bdf_render_region(bdf_renderer *renderer, size_t view, size_t page,
                                     const float region[4], int w, int h, bdf_image *out,
                                     bdf_error *err);
/* bdf_sheet_size is the size of a sheet in units, from its rows and columns. */
BDF_API void bdf_sheet_size(const bdf_doc *doc, size_t view, double *w, double *h);
/*
 * bdf_render_object draws an object that needs no other part (a formula
 * laid out as paths) at scale device pixels per unit. The image holds the
 * bounding box of the object grown to whole pixels; x and y say where its
 * first pixel lies from where the origin of the object is drawn.
 */
BDF_API bdf_status bdf_render_object(const bdf_object *obj, double scale,
                                     const bdf_render_options *opts, bdf_image *out,
                                     bdf_error *err);
/* What could not be drawn as the viewer would draw it, each once. */
BDF_API size_t bdf_renderer_warning_count(const bdf_renderer *renderer);
BDF_API const char *bdf_renderer_warning(const bdf_renderer *renderer, size_t i);

/* ---- Drawing through another canvas -------------------------------------- */

/*
 * bdf_play_page draws a page through the functions of a bdf_canvas instead
 * of into pixels of its own: the way to draw a document with Skia,
 * CoreGraphics, cairo, QPainter or any other 2D library (adapters/ has
 * canvases for these). The renderer does what belongs to the format, and
 * the canvas only what a 2D library has under one name or another:
 *
 *   - it runs the instructions of the page's objects and of the objects
 *     they draw, keeps the drawing state, and hands every drawing over with
 *     the whole style it is drawn in (colour or paint, alpha, composite
 *     operation, shadow, line);
 *   - paths come as moves, lines, quadratic and cubic curves and closes;
 *   - images come decoded, as premultiplied RGBA;
 *   - text comes as the outlines of its glyphs, laid out as bdf_render_page
 *     lays it out, unless the canvas draws text itself (text);
 *   - soft masks are drawn by libbdf and come as an image to multiply what
 *     was drawn by.
 *
 * Coordinates are those of the page: the canvas is expected to map them to
 * its pixels (scale says how many pixels a unit is, for the images libbdf
 * draws itself) and to have filled the background. The page is clipped to
 * its rectangle.
 *
 * A canvas function must not call back into the renderer. Pointers given to
 * it are valid during the call only, except that a bitmap with an id other
 * than 0 has the same pixels whenever the same id is given again while the
 * renderer lives: a canvas may keep what it made of them by that id.
 */

/* A path: verbs is BDF_VERB_MOVE, _LINE, _QUAD, _CUBIC and _CLOSE only, and
 * coords holds their 2, 2, 4, 6 and 0 numbers one after the other. Every
 * move is followed by a line or a curve at least. A path of no verbs is
 * given to clip alone, where it clips everything away. */
typedef struct bdf_canvas_path {
  const uint8_t *verbs;
  size_t nverbs;
  const double *coords;
  size_t ncoords;
} bdf_canvas_path;

typedef struct bdf_canvas_bitmap {
  uint64_t id; /* 0: the pixels are of this call alone */
  int width, height;
  const uint8_t *pixels; /* premultiplied RGBA, 4*width bytes a row */
} bdf_canvas_bitmap;

/* A gradient, or a pattern of an image. */
typedef struct bdf_canvas_paint {
  uint8_t kind;        /* BDF_PAINT_* */
  const float *coords; /* gradients: as in bdf_paint */
  size_t ncoords;
  const bdf_stop *stops;
  size_t nstops;
  const bdf_canvas_bitmap *image; /* patterns */
  uint8_t repeat;                 /* BDF_REPEAT* */
  double matrix[6];               /* pattern: its space in the user space */
} bdf_canvas_paint;

/* What a drawing is drawn with. */
typedef struct bdf_canvas_style {
  uint32_t color;                /* 0xRRGGBBAA, not premultiplied; without a paint */
  const bdf_canvas_paint *paint; /* or NULL */
  float alpha;                   /* of the whole drawing, on top of the colour's */
  uint8_t blend;                 /* BDF_BLEND_* */
  /* a shadow when the colour has alpha and blur or an offset is not 0: in
   * units of the page, whatever the transform is (as Canvas 2D has them) */
  uint32_t shadow_color;
  double shadow_blur, shadow_dx, shadow_dy;
  int smoothing; /* images: interpolate between their pixels */
} bdf_canvas_style;

typedef struct bdf_canvas_line {
  double width;
  uint8_t cap, join; /* BDF_CAP_*, BDF_JOIN_* */
  double miter;
  const float *dash; /* on, off, ... in user space; NULL: solid */
  size_t ndash;
  double dash_offset;
} bdf_canvas_line;

/* A string for a canvas that draws text itself. */
typedef struct bdf_canvas_text {
  const char *text; /* UTF-8 */
  size_t len;
  double x, y;      /* the anchor */
  double advance;   /* the width the string had where it was written, or 0 */
  int stroke;       /* outlined (with the line and the stroke style) instead of filled */
  const bdf_font *font;     /* the font asked for; NULL: none was set (sans-serif) */
  const uint8_t *font_data; /* an embedded font as an sfnt file (unpacked from WOFF2), or NULL */
  size_t font_len;
  uint64_t font_id; /* as the id of a bitmap: the same for the same font_data */
  double size;
  uint8_t align, baseline, dir; /* BDF_ALIGN_*, BDF_BASELINE_*, BDF_DIR_* */
  double spacing;               /* between letters */
} bdf_canvas_text;

typedef struct bdf_canvas {
  void *ctx; /* given to every function */

  /* The state of the canvas that libbdf leaves to it: the transform and the
   * clip. save and restore nest, and layers nest with them. */
  void (*save)(void *ctx);
  void (*restore)(void *ctx);
  /* Concatenates a matrix: the new user space in the old one. */
  void (*transform)(void *ctx, const double m[6]);
  /* Intersects the clip with a path (rule: BDF_FILL_*). */
  void (*clip)(void *ctx, const bdf_canvas_path *path, uint8_t rule);

  void (*fill)(void *ctx, const bdf_canvas_path *path, uint8_t rule, const bdf_canvas_style *style);
  void (*stroke)(void *ctx, const bdf_canvas_path *path, const bdf_canvas_line *line,
                 const bdf_canvas_style *style);
  /* Draws the part src (x, y, w, h in its pixels, within it) of a bitmap
   * into the rectangle dst (x, y, w, h, both positive) of the user space.
   * Of the style, the alpha, the blend, the shadow and smoothing apply. */
  void (*image)(void *ctx, const bdf_canvas_bitmap *bitmap, const double src[4], const double dst[4],
                const bdf_canvas_style *style);

  /* What is drawn until end_layer is drawn on a transparent canvas of its
   * own and then composited as one: of the style, the alpha, the blend and
   * the shadow apply (a group drawn under a shadow casts it as a whole, as
   * a canvas does). bounds (x, y, w, h in user space) holds all of what is
   * drawn; the shadow may fall outside them. begin_layer saves the state as
   * save does, and end_layer restores it. */
  void (*begin_layer)(void *ctx, const double bounds[4], const bdf_canvas_style *style);
  void (*end_layer)(void *ctx);

  /* Optional (NULL: libbdf draws the outlines of the glyphs through fill
   * and stroke). Returns 0 to have libbdf draw this string after all. */
  int (*text)(void *ctx, const bdf_canvas_text *text, const bdf_canvas_line *line,
              const bdf_canvas_style *style);
} bdf_canvas;

/*
 * bdf_play_page draws a page of a fixed or flow view, or a strip of a
 * scroll view, through canvas. scale is the device pixels a unit of the
 * page takes on the canvas (1 when unknown). What cannot be drawn is among
 * the renderer's warnings afterwards.
 */
BDF_API bdf_status bdf_play_page(bdf_renderer *renderer, size_t view, size_t page, double scale,
                                 const bdf_canvas *canvas, bdf_error *err);
/* bdf_play_part does the same with one object part of the document, in the
 * object's own coordinates and without a clip: a tile of a sheet, a master. */
BDF_API bdf_status bdf_play_part(bdf_renderer *renderer, const uint8_t hash[16], double scale,
                                 const bdf_canvas *canvas, bdf_error *err);
/*
 * bdf_play_region draws a rectangle (x, y, w, h in units) of a view through
 * canvas, as bdf_render_region draws it into pixels: of a page of a fixed
 * or flow view, of the continuous layout of a scroll view, or of a sheet
 * with its gridlines (page is ignored in the last two). The corner of the
 * rectangle is at the origin of the canvas and nothing is drawn outside
 * it; the background is the canvas's to paint. scale is the device pixels
 * a unit takes on the canvas: the gridlines are a device pixel wide, on
 * whole pixels from the origin.
 */
BDF_API bdf_status bdf_play_region(bdf_renderer *renderer, size_t view, size_t page,
                                   const float region[4], double scale, const bdf_canvas *canvas,
                                   bdf_error *err);

/* ---- Cue index (spec §4.4) ---------------------------------------------- */

typedef struct bdf_cues bdf_cues;

/* A rectangle of a page the playing position moves through: a system of a
 * score, or a line of lyrics. */
typedef struct bdf_cue_system {
  uint32_t page; /* from 0 */
  float x, y, w, h;
} bdf_cue_system;

/* At tick (of the Standard MIDI File, or milliseconds of an audio file) the
 * playing position is at x on a system. */
typedef struct bdf_cue {
  uint32_t tick;
  uint32_t system;
  float x;
} bdf_cue;

BDF_API bdf_status bdf_cues_decode(const uint8_t *data, size_t len, bdf_cues **out, bdf_error *err);
BDF_API void bdf_cues_free(bdf_cues *cues);
BDF_API size_t bdf_cues_system_count(const bdf_cues *cues);
BDF_API bdf_status bdf_cues_system(const bdf_cues *cues, size_t i, bdf_cue_system *out);
BDF_API size_t bdf_cues_count(const bdf_cues *cues);
BDF_API bdf_status bdf_cues_cue(const bdf_cues *cues, size_t i, bdf_cue *out);

#ifdef __cplusplus
}
#endif

#endif /* BDF_H */
