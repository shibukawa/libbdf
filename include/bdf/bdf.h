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
#define BDF_VERSION_MINOR  2
#define BDF_VERSION_PATCH  0
#define BDF_VERSION_STRING "0.2.0"

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
  BDF_ERR_UNSUPPORTED = 4 /* well-formed, but beyond this build or this library: a WOFF2 font
                             without a Brotli decoder, a font collection, a WOFF 1.0 font */
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

/* ---- Limits of the format (spec §3.2, §4) ------------------------------- */

#define BDF_MAX_MANIFEST_SIZE (256u << 20) /* a manifest is at most 256 MiB */
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

/* bdf_doc_encrypted reports an encryption block in the manifest; locked, that
 * it is encrypted and not unlocked (unlocking is not implemented yet, so the
 * two agree). The views of a locked document are those of its outer
 * manifest: none; its parts are the sealed parts, as stored. */
BDF_API int bdf_doc_encrypted(const bdf_doc *doc);
BDF_API int bdf_doc_locked(const bdf_doc *doc);
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
  size_t ncols, nrows;
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
 * NUL bytes of their own, so use the length. NULL when i is out of range. */
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

#ifdef __cplusplus
}
#endif

#endif /* BDF_H */
