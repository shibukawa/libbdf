/*
 * The Object part decoder and the instruction iterator: a port of decode.go,
 * path.go, paint.go and font.go of the reference implementation, with the
 * same checks in the same order, so that the two accept and reject the same
 * bytes (the differential tests under go/ hold them to it).
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

/* ---- Tables ------------------------------------------------------------- */

static const int bdfi_verb_args[] = {2, 2, 4, 6, 0, 4, 8, 5, 5};

int bdf_verb_args(uint8_t verb) {
  if ((size_t)verb >= sizeof bdfi_verb_args / sizeof bdfi_verb_args[0]) {
    return -1;
  }
  return bdfi_verb_args[verb];
}

/*
 * Signature characters: f=f32, b=u8, c=u32, v=varuint, s=string ref,
 * B=varuint length and that many bytes. Special signatures: "D" (DASH),
 * "R" (FILL_PATH_RUN), "X" (EXT).
 */
typedef struct bdfi_op_info {
  const char *name;
  const char *sig;
} bdfi_op_info;

static const bdfi_op_info bdfi_op_table[256] = {
    [BDF_OP_SAVE] = {"SAVE", ""},
    [BDF_OP_RESTORE] = {"RESTORE", ""},
    [BDF_OP_TRANSFORM] = {"TRANSFORM", "ffffff"},
    [BDF_OP_TRANSLATE] = {"TRANSLATE", "ff"},
    [BDF_OP_SCALE] = {"SCALE", "ff"},
    [BDF_OP_CLIP_PATH] = {"CLIP_PATH", "vb"},
    [BDF_OP_CLIP_RECT] = {"CLIP_RECT", "ffff"},
    [BDF_OP_FILL_COLOR] = {"FILL_COLOR", "c"},
    [BDF_OP_FILL_PAINT] = {"FILL_PAINT", "v"},
    [BDF_OP_STROKE_COLOR] = {"STROKE_COLOR", "c"},
    [BDF_OP_STROKE_PAINT] = {"STROKE_PAINT", "v"},
    [BDF_OP_LINE] = {"LINE", "fbbf"},
    [BDF_OP_DASH] = {"DASH", "D"},
    [BDF_OP_ALPHA] = {"ALPHA", "f"},
    [BDF_OP_BLEND] = {"BLEND", "b"},
    [BDF_OP_SHADOW] = {"SHADOW", "cfff"},
    [BDF_OP_FILTER] = {"FILTER", "s"},
    [BDF_OP_FONT] = {"FONT", "vf"},
    [BDF_OP_TEXT_STYLE] = {"TEXT_STYLE", "bbbf"},
    [BDF_OP_FILL_RECT] = {"FILL_RECT", "ffff"},
    [BDF_OP_STROKE_RECT] = {"STROKE_RECT", "ffff"},
    [BDF_OP_FILL_PATH] = {"FILL_PATH", "vb"},
    [BDF_OP_STROKE_PATH] = {"STROKE_PATH", "v"},
    [BDF_OP_FILL_PATH_AT] = {"FILL_PATH_AT", "vbff"},
    [BDF_OP_FILL_PATH_RUN] = {"FILL_PATH_RUN", "R"},
    [BDF_OP_CLEAR_RECT] = {"CLEAR_RECT", "ffff"},
    [BDF_OP_FILL_TEXT] = {"FILL_TEXT", "sfff"},
    [BDF_OP_STROKE_TEXT] = {"STROKE_TEXT", "sfff"},
    [BDF_OP_IMAGE] = {"IMAGE", "vffff"},
    [BDF_OP_IMAGE_SUB] = {"IMAGE_SUB", "vffffffff"},
    [BDF_OP_SMOOTHING] = {"SMOOTHING", "bb"},
    [BDF_OP_USE] = {"USE", "v"},
    [BDF_OP_USE_AT] = {"USE_AT", "vff"},
    [BDF_OP_GROUP_BEGIN] = {"GROUP_BEGIN", "fbffff"},
    [BDF_OP_GROUP_END] = {"GROUP_END", ""},
    [BDF_OP_MASK_BEGIN] = {"MASK_BEGIN", "bcB"},
    [BDF_OP_MASK_END] = {"MASK_END", ""},
    [BDF_OP_LINK] = {"LINK", "ffffs"},
    [BDF_OP_MARK] = {"MARK", "bs"},
    [BDF_OP_EXT] = {"EXT", "X"},
};

const char *bdf_op_name(uint8_t op) {
  return bdfi_op_table[op].name ? bdfi_op_table[op].name : "";
}

/* ---- Paths -------------------------------------------------------------- */

void bdfi_path_entry_clear(bdfi_path_entry *p) {
  free(p->verbs);
  free(p->args);
  memset(p, 0, sizeof *p);
}

void bdfi_path_entry_view(const bdfi_path_entry *p, bdf_path *out) {
  out->external = p->external;
  out->verbs = p->verbs;
  out->nverbs = p->nverbs;
  out->args = p->args;
  out->nargs = p->nargs;
  out->hash = p->external ? p->hash : NULL;
  out->index = p->index;
}

int bdfi_decode_path(bdfi_reader *r, bdfi_path_entry *p) {
  memset(p, 0, sizeof *p);
  size_t n = bdfi_rd_count(r, 1, "bad path verb count");
  const uint8_t *verbs = bdfi_rd_bytes(r, n);
  if (r->failed) {
    return 0;
  }
  if (n > 0) {
    p->verbs = malloc(n);
    if (!p->verbs) {
      return -1;
    }
    memcpy(p->verbs, verbs, n);
    p->nverbs = n;
  }
  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    int k = bdf_verb_args(verbs[i]);
    if (k < 0) {
      bdfi_rd_fail(r, "unknown path verb");
      return 0;
    }
    total += (size_t)k;
  }
  if (total > SIZE_MAX / 4 || !bdfi_rd_need(r, 4 * total)) {
    bdfi_rd_fail(r, "unexpected end of data");
    return 0;
  }
  if (total > 0) {
    p->args = malloc(total * sizeof(float));
    if (!p->args) {
      return -1;
    }
    for (size_t i = 0; i < total; i++) {
      p->args[i] = bdfi_rd_f32(r);
    }
    p->nargs = total;
  }
  return 0;
}

/* ---- Object ------------------------------------------------------------- */

typedef struct bdfi_paint_entry {
  uint8_t kind;
  float coords[6];
  size_t ncoords;
  bdf_stop *stops;
  size_t nstops;
  uint64_t image;
  uint8_t repeat;
  float matrix[6];
} bdfi_paint_entry;

typedef struct bdfi_font_entry {
  uint8_t kind;
  uint8_t hash[16];
  char *family;
  size_t family_len;
  uint16_t weight;
  uint8_t style;
} bdfi_font_entry;

struct bdf_object {
  uint16_t opset;
  float bbox[4];
  size_t nstrings;
  char **strings;
  size_t *string_lens;
  size_t npaths;
  bdfi_path_entry *paths;
  size_t npaints;
  bdfi_paint_entry *paints;
  size_t nfonts;
  bdfi_font_entry *fonts;
  size_t nimages;
  uint8_t *images; /* 16 bytes each */
  size_t nobjects;
  uint8_t *objects; /* 16 bytes each */
  size_t nops;
  uint8_t *ops;
};

/* bdfi_read_string reads a length-prefixed string into a NUL-terminated copy. */
static char *bdfi_read_string(bdfi_reader *r, size_t *len, int *oom) {
  size_t n = bdfi_rd_count(r, 1, "bad string length");
  const uint8_t *p = bdfi_rd_bytes(r, n);
  if (r->failed) {
    return NULL;
  }
  char *s = malloc(n + 1);
  if (!s) {
    *oom = 1;
    return NULL;
  }
  if (n > 0) {
    memcpy(s, p, n);
  }
  s[n] = '\0';
  *len = n;
  return s;
}

static void bdfi_read_hash(bdfi_reader *r, uint8_t out[16]) {
  const uint8_t *p = bdfi_rd_bytes(r, 16);
  if (p) {
    memcpy(out, p, 16);
  }
}

void bdf_object_free(bdf_object *o) {
  if (!o) {
    return;
  }
  for (size_t i = 0; i < o->nstrings; i++) {
    free(o->strings[i]);
  }
  free(o->strings);
  free(o->string_lens);
  for (size_t i = 0; i < o->npaths; i++) {
    bdfi_path_entry_clear(&o->paths[i]);
  }
  free(o->paths);
  for (size_t i = 0; i < o->npaints; i++) {
    free(o->paints[i].stops);
  }
  free(o->paints);
  for (size_t i = 0; i < o->nfonts; i++) {
    free(o->fonts[i].family);
  }
  free(o->fonts);
  free(o->images);
  free(o->objects);
  free(o->ops);
  free(o);
}

static const size_t bdfi_paint_coords[] = {4, 6, 3};

static void bdfi_decode_paint(bdfi_reader *r, bdfi_paint_entry *p, int *oom) {
  p->kind = bdfi_rd_u8(r);
  if (p->kind == BDF_PAINT_PATTERN) {
    p->image = bdfi_rd_varuint(r);
    p->repeat = bdfi_rd_u8(r);
    for (int i = 0; i < 6; i++) {
      p->matrix[i] = bdfi_rd_f32(r);
    }
    return;
  }
  if ((size_t)p->kind >= sizeof bdfi_paint_coords / sizeof bdfi_paint_coords[0]) {
    bdfi_rd_fail(r, "unknown paint kind");
    return;
  }
  p->ncoords = bdfi_paint_coords[p->kind];
  for (size_t i = 0; i < p->ncoords; i++) {
    p->coords[i] = bdfi_rd_f32(r);
  }
  size_t n = bdfi_rd_count(r, 8, "bad stop count");
  p->stops = bdfi_alloc_items(n, sizeof(bdf_stop), oom);
  if (*oom) {
    return;
  }
  p->nstops = n;
  for (size_t i = 0; i < n; i++) {
    p->stops[i].offset = bdfi_rd_f32(r);
    p->stops[i].color = bdfi_rd_u32(r);
  }
}

static void bdfi_decode_font(bdfi_reader *r, bdfi_font_entry *f, int *oom) {
  f->kind = bdfi_rd_u8(r);
  if (f->kind == BDF_FONT_EMBEDDED) {
    bdfi_read_hash(r, f->hash);
  }
  f->family = bdfi_read_string(r, &f->family_len, oom);
  f->weight = bdfi_rd_u16(r);
  f->style = bdfi_rd_u8(r);
}

bdf_status bdf_object_decode(const uint8_t *data, size_t len, bdf_object **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdfi_reader rd, *r = &rd;
  bdfi_rd_init(r, data, len);
  int oom = 0;
  bdf_object *o = calloc(1, sizeof *o);
  if (!o) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }

  /* The reference reports these three before any shortage of data, so a
   * truncated header is "unsupported opset 0", not "unexpected end". */
  const char *msg = NULL;
  if (len < 4 || memcmp(data, "BOBJ", 4) != 0) {
    msg = "not an object part";
  } else {
    r->pos = 4;
    o->opset = bdfi_rd_u16(r);
    if (o->opset != BDF_OPSET_VERSION) {
      msg = "unsupported opset";
    } else if (bdfi_rd_u16(r) != 0) {
      msg = "unsupported object flags";
    }
  }
  if (msg) {
    bdf_object_free(o);
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, msg);
    return BDF_ERR_FORMAT;
  }
  for (int i = 0; i < 4; i++) {
    o->bbox[i] = bdfi_rd_f32(r);
  }

  size_t n = bdfi_rd_count(r, 1, "bad count");
  o->strings = bdfi_alloc_items(n, sizeof(char *), &oom);
  o->string_lens = bdfi_alloc_items(n, sizeof(size_t), &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    o->strings[i] = bdfi_read_string(r, &o->string_lens[i], &oom);
    if (oom) {
      goto nomem;
    }
    o->nstrings = i + 1;
  }

  n = bdfi_rd_count(r, 1, "bad count");
  o->paths = bdfi_alloc_items(n, sizeof(bdfi_path_entry), &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    bdfi_path_entry *p = &o->paths[i];
    o->npaths = i + 1;
    if (bdfi_rd_u8(r) == 0) {
      if (bdfi_decode_path(r, p) < 0) {
        goto nomem;
      }
    } else {
      p->external = 1;
      bdfi_read_hash(r, p->hash);
      p->index = (uint32_t)bdfi_rd_varuint(r);
    }
  }

  n = bdfi_rd_count(r, 1, "bad count");
  o->paints = bdfi_alloc_items(n, sizeof(bdfi_paint_entry), &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    o->npaints = i + 1;
    bdfi_decode_paint(r, &o->paints[i], &oom);
    if (oom) {
      goto nomem;
    }
  }

  n = bdfi_rd_count(r, 1, "bad count");
  o->fonts = bdfi_alloc_items(n, sizeof(bdfi_font_entry), &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    o->nfonts = i + 1;
    bdfi_decode_font(r, &o->fonts[i], &oom);
    if (oom) {
      goto nomem;
    }
  }

  n = bdfi_rd_count(r, 1, "bad count");
  o->images = bdfi_alloc_items(n, 16, &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    bdfi_read_hash(r, o->images + 16 * i);
    o->nimages = i + 1;
  }

  n = bdfi_rd_count(r, 1, "bad count");
  o->objects = bdfi_alloc_items(n, 16, &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    bdfi_read_hash(r, o->objects + 16 * i);
    o->nobjects = i + 1;
  }

  n = bdfi_rd_count(r, 1, "bad count");
  const uint8_t *ops = bdfi_rd_bytes(r, n);
  if (r->failed) {
    goto fail;
  }
  o->ops = bdfi_alloc_items(n, 1, &oom);
  if (oom) {
    goto nomem;
  }
  if (n > 0) {
    memcpy(o->ops, ops, n);
  }
  o->nops = n;
  *out = o;
  return BDF_OK;

fail:
  bdf_object_free(o);
  bdfi_error_from_reader(err, r);
  return BDF_ERR_FORMAT;
nomem:
  bdf_object_free(o);
  bdfi_error_set(err, BDF_ERR_NOMEM, r->pos, "out of memory");
  return BDF_ERR_NOMEM;
}

uint16_t bdf_object_opset(const bdf_object *o) { return o ? o->opset : 0; }

void bdf_object_bbox(const bdf_object *o, float out[4]) {
  if (!out) {
    return;
  }
  for (int i = 0; i < 4; i++) {
    out[i] = o ? o->bbox[i] : 0;
  }
}

size_t bdf_object_string_count(const bdf_object *o) { return o ? o->nstrings : 0; }

const char *bdf_object_string(const bdf_object *o, size_t i, size_t *len) {
  if (!o || i >= o->nstrings) {
    if (len) {
      *len = 0;
    }
    return NULL;
  }
  if (len) {
    *len = o->string_lens[i];
  }
  return o->strings[i];
}

size_t bdf_object_path_count(const bdf_object *o) { return o ? o->npaths : 0; }

bdf_status bdf_object_path(const bdf_object *o, size_t i, bdf_path *out) {
  if (!o || !out || i >= o->npaths) {
    return BDF_ERR_ARG;
  }
  bdfi_path_entry_view(&o->paths[i], out);
  return BDF_OK;
}

size_t bdf_object_paint_count(const bdf_object *o) { return o ? o->npaints : 0; }

bdf_status bdf_object_paint(const bdf_object *o, size_t i, bdf_paint *out) {
  if (!o || !out || i >= o->npaints) {
    return BDF_ERR_ARG;
  }
  const bdfi_paint_entry *p = &o->paints[i];
  memset(out, 0, sizeof *out);
  out->kind = p->kind;
  out->coords = p->ncoords ? p->coords : NULL;
  out->ncoords = p->ncoords;
  out->stops = p->stops;
  out->nstops = p->nstops;
  out->image = p->image;
  out->repeat = p->repeat;
  memcpy(out->matrix, p->matrix, sizeof out->matrix);
  return BDF_OK;
}

size_t bdf_object_font_count(const bdf_object *o) { return o ? o->nfonts : 0; }

bdf_status bdf_object_font(const bdf_object *o, size_t i, bdf_font *out) {
  if (!o || !out || i >= o->nfonts) {
    return BDF_ERR_ARG;
  }
  const bdfi_font_entry *f = &o->fonts[i];
  out->kind = f->kind;
  out->hash = f->kind == BDF_FONT_EMBEDDED ? f->hash : NULL;
  out->family = f->family;
  out->family_len = f->family_len;
  out->weight = f->weight;
  out->style = f->style;
  return BDF_OK;
}

size_t bdf_object_image_count(const bdf_object *o) { return o ? o->nimages : 0; }

const uint8_t *bdf_object_image(const bdf_object *o, size_t i) {
  return o && i < o->nimages ? o->images + 16 * i : NULL;
}

size_t bdf_object_object_count(const bdf_object *o) { return o ? o->nobjects : 0; }

const uint8_t *bdf_object_object(const bdf_object *o, size_t i) {
  return o && i < o->nobjects ? o->objects + 16 * i : NULL;
}

const uint8_t *bdf_object_ops(const bdf_object *o, size_t *len) {
  if (len) {
    *len = o ? o->nops : 0;
  }
  return o ? o->ops : NULL;
}

/* ---- Instructions ------------------------------------------------------- */

void bdfi_instr_init(bdf_instr *it, const bdf_object *o) {
  memset(it, 0, sizeof *it);
  it->obj = o;
  bdfi_rd_init(&it->r, o->ops, o->nops);
}

void bdfi_instr_clear(bdf_instr *it) {
  free(it->f32s);
  free(it->glyphs);
  it->f32s = NULL;
  it->glyphs = NULL;
  it->f32cap = it->glyphcap = 0;
}

bdf_instr *bdf_instr_new(const bdf_object *o) {
  if (!o) {
    return NULL;
  }
  bdf_instr *it = malloc(sizeof *it);
  if (!it) {
    return NULL;
  }
  bdfi_instr_init(it, o);
  return it;
}

void bdf_instr_free(bdf_instr *it) {
  if (!it) {
    return;
  }
  bdfi_instr_clear(it);
  free(it);
}

void bdf_instr_rewind(bdf_instr *it) {
  if (it) {
    bdfi_rd_init(&it->r, it->obj->ops, it->obj->nops);
    it->nargs = 0;
  }
}

static bdfi_arg *bdfi_push(bdf_instr *it, bdf_arg_kind kind) {
  bdfi_arg *a = &it->args[it->nargs++];
  memset(a, 0, sizeof *a);
  a->kind = kind;
  return a;
}

/* bdfi_grow makes *buf hold at least n items of size bytes. */
static int bdfi_grow(void **buf, size_t *cap, size_t n, size_t size) {
  if (n <= *cap) {
    return 0;
  }
  if (n > SIZE_MAX / size) {
    return -1;
  }
  void *p = realloc(*buf, n * size);
  if (!p) {
    return -1;
  }
  *buf = p;
  *cap = n;
  return 0;
}

int bdf_instr_next(bdf_instr *it, bdf_error *err) {
  bdfi_error_clear(err);
  if (!it) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return -1;
  }
  bdfi_reader *r = &it->r;
  it->nargs = 0;
  if (bdfi_rd_eof(r)) {
    if (r->failed) {
      bdfi_error_from_reader(err, r);
      return -1;
    }
    return 0;
  }
  it->offset = r->pos;
  it->op = bdfi_rd_u8(r);
  const bdfi_op_info *info = &bdfi_op_table[it->op];
  if (!info->name) {
    bdfi_rd_fail_at(r, it->offset, "unknown opcode");
    bdfi_error_from_reader(err, r);
    return -1;
  }
  const char *sig = info->sig;
  if (sig[0] == 'D') {
    size_t n = bdfi_rd_count(r, 4, "bad dash count");
    if (!r->failed && bdfi_grow((void **)&it->f32s, &it->f32cap, n, sizeof(float)) < 0) {
      bdfi_error_set(err, BDF_ERR_NOMEM, r->pos, "out of memory");
      return -1;
    }
    for (size_t i = 0; i < n; i++) {
      it->f32s[i] = bdfi_rd_f32(r);
    }
    bdfi_arg *a = bdfi_push(it, BDF_ARG_F32S);
    a->p = it->f32s;
    a->n = n;
    bdfi_push(it, BDF_ARG_F32)->f = bdfi_rd_f32(r);
  } else if (sig[0] == 'R') {
    bdfi_push(it, BDF_ARG_U8)->u = bdfi_rd_u8(r);
    size_t n = bdfi_rd_count(r, 9, "bad run count");
    if (!r->failed && bdfi_grow((void **)&it->glyphs, &it->glyphcap, n, sizeof(bdf_glyph)) < 0) {
      bdfi_error_set(err, BDF_ERR_NOMEM, r->pos, "out of memory");
      return -1;
    }
    for (size_t i = 0; i < n; i++) {
      it->glyphs[i].path = bdfi_rd_varuint(r);
      it->glyphs[i].x = bdfi_rd_f32(r);
      it->glyphs[i].y = bdfi_rd_f32(r);
    }
    bdfi_arg *a = bdfi_push(it, BDF_ARG_GLYPHS);
    a->p = it->glyphs;
    a->n = n;
  } else if (sig[0] == 'X') {
    size_t n = bdfi_rd_u32(r);
    bdfi_arg *a = bdfi_push(it, BDF_ARG_BYTES);
    a->p = bdfi_rd_bytes(r, n);
    a->n = n;
  } else {
    for (const char *c = sig; *c; c++) {
      switch (*c) {
      case 'f':
        bdfi_push(it, BDF_ARG_F32)->f = bdfi_rd_f32(r);
        break;
      case 'b':
        bdfi_push(it, BDF_ARG_U8)->u = bdfi_rd_u8(r);
        break;
      case 'c':
        bdfi_push(it, BDF_ARG_U32)->u = bdfi_rd_u32(r);
        break;
      case 'v':
        bdfi_push(it, BDF_ARG_VARUINT)->u = bdfi_rd_varuint(r);
        break;
      case 's': {
        uint64_t i = bdfi_rd_varuint(r);
        if (i >= it->obj->nstrings) {
          bdfi_rd_fail(r, "bad string ref");
        } else {
          bdfi_arg *a = bdfi_push(it, BDF_ARG_STRING);
          a->p = it->obj->strings[i];
          a->n = it->obj->string_lens[i];
        }
        break;
      }
      case 'B': {
        size_t n = bdfi_rd_count(r, 1, "bad byte count");
        bdfi_arg *a = bdfi_push(it, BDF_ARG_BYTES);
        a->p = bdfi_rd_bytes(r, n);
        a->n = n;
        break;
      }
      }
    }
  }
  if (r->failed) {
    it->nargs = 0;
    bdfi_error_from_reader(err, r);
    return -1;
  }
  return 1;
}

uint8_t bdf_instr_op(const bdf_instr *it) { return it ? it->op : 0; }
size_t bdf_instr_offset(const bdf_instr *it) { return it ? it->offset : 0; }
size_t bdf_instr_arg_count(const bdf_instr *it) { return it ? it->nargs : 0; }

/* bdfi_arg_at returns operand i when it is of the kind asked for;
 * BDF_ARG_NONE asks for any kind, and BDF_ARG_VARUINT for any of the
 * integer kinds. */
static const bdfi_arg *bdfi_arg_at(const bdf_instr *it, size_t i, bdf_arg_kind kind) {
  if (!it || i >= it->nargs) {
    return NULL;
  }
  const bdfi_arg *a = &it->args[i];
  switch (kind) {
  case BDF_ARG_NONE:
    return a;
  case BDF_ARG_VARUINT:
    return a->kind == BDF_ARG_U8 || a->kind == BDF_ARG_U32 || a->kind == BDF_ARG_VARUINT ? a : NULL;
  default:
    return a->kind == kind ? a : NULL;
  }
}

bdf_arg_kind bdf_instr_arg_kind_at(const bdf_instr *it, size_t i) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_NONE);
  return a ? a->kind : BDF_ARG_NONE;
}

float bdf_instr_f32(const bdf_instr *it, size_t i) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_F32);
  return a ? a->f : 0;
}

uint64_t bdf_instr_u64(const bdf_instr *it, size_t i) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_VARUINT);
  return a ? a->u : 0;
}

const char *bdf_instr_string(const bdf_instr *it, size_t i, size_t *len) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_STRING);
  if (len) {
    *len = a ? a->n : 0;
  }
  return a ? (const char *)a->p : NULL;
}

const uint8_t *bdf_instr_bytes(const bdf_instr *it, size_t i, size_t *len) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_BYTES);
  if (len) {
    *len = a ? a->n : 0;
  }
  return a ? (const uint8_t *)a->p : NULL;
}

const float *bdf_instr_f32s(const bdf_instr *it, size_t i, size_t *n) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_F32S);
  if (n) {
    *n = a ? a->n : 0;
  }
  return a ? (const float *)a->p : NULL;
}

const bdf_glyph *bdf_instr_glyphs(const bdf_instr *it, size_t i, size_t *n) {
  const bdfi_arg *a = bdfi_arg_at(it, i, BDF_ARG_GLYPHS);
  if (n) {
    *n = a ? a->n : 0;
  }
  return a ? (const bdf_glyph *)a->p : NULL;
}

/* ---- Path collection ---------------------------------------------------- */

struct bdf_paths {
  size_t n;
  bdfi_path_entry *paths;
};

void bdf_paths_free(bdf_paths *ps) {
  if (!ps) {
    return;
  }
  for (size_t i = 0; i < ps->n; i++) {
    bdfi_path_entry_clear(&ps->paths[i]);
  }
  free(ps->paths);
  free(ps);
}

bdf_status bdf_paths_decode(const uint8_t *data, size_t len, bdf_paths **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdfi_reader rd, *r = &rd;
  bdfi_rd_init(r, data, len);
  bdf_paths *ps = calloc(1, sizeof *ps);
  if (!ps) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  int oom = 0;
  size_t n = bdfi_rd_count(r, 1, "bad path collection count");
  ps->paths = bdfi_alloc_items(n, sizeof(bdfi_path_entry), &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    ps->n = i + 1;
    if (bdfi_decode_path(r, &ps->paths[i]) < 0) {
      goto nomem;
    }
  }
  if (r->failed) {
    bdf_paths_free(ps);
    bdfi_error_from_reader(err, r);
    return BDF_ERR_FORMAT;
  }
  *out = ps;
  return BDF_OK;
nomem:
  bdf_paths_free(ps);
  bdfi_error_set(err, BDF_ERR_NOMEM, r->pos, "out of memory");
  return BDF_ERR_NOMEM;
}

size_t bdf_paths_count(const bdf_paths *ps) { return ps ? ps->n : 0; }

bdf_status bdf_paths_get(const bdf_paths *ps, size_t i, bdf_path *out) {
  if (!ps || !out || i >= ps->n) {
    return BDF_ERR_ARG;
  }
  bdfi_path_entry_view(&ps->paths[i], out);
  return BDF_OK;
}
