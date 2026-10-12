/*
 * Text extraction: the runs of an object and the objects it draws, a port of
 * text.go of the reference implementation (the same walk, the same
 * separators, the same limits), and the LINK areas, which the reference
 * leaves to its viewer.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

/* The runs and the links of a walk are kept in memory, so both are bounded. */
#define BDFI_MAX_TEXT_RUNS  ((size_t)1 << 22)
#define BDFI_MAX_TEXT_LINKS ((size_t)1 << 22)

/* ---- Arithmetic --------------------------------------------------------- */

/*
 * The float product, rounded to float before anything is added to it. The
 * volatile keeps compilers from contracting the multiplication with the
 * addition after it into a fused multiply-add, which rounds once where the
 * two operations round twice: positions come out the same on every platform
 * and compiler, and the same as the reference computes them where Go does
 * not fuse either (amd64).
 */
static float bdfi_mulf(float a, float b) {
  volatile float p = a * b;
  return p;
}

/* bdfi_mat_mul is m × n, as text.go's mul. */
static void bdfi_mat_mul(const float m[6], const float n[6], float out[6]) {
  float r[6];
  r[0] = bdfi_mulf(m[0], n[0]) + bdfi_mulf(m[2], n[1]);
  r[1] = bdfi_mulf(m[1], n[0]) + bdfi_mulf(m[3], n[1]);
  r[2] = bdfi_mulf(m[0], n[2]) + bdfi_mulf(m[2], n[3]);
  r[3] = bdfi_mulf(m[1], n[2]) + bdfi_mulf(m[3], n[3]);
  r[4] = bdfi_mulf(m[0], n[4]) + bdfi_mulf(m[2], n[5]);
  r[4] = r[4] + m[4];
  r[5] = bdfi_mulf(m[1], n[4]) + bdfi_mulf(m[3], n[5]);
  r[5] = r[5] + m[5];
  memcpy(out, r, sizeof r);
}

static void bdfi_mat_apply(const float m[6], float x, float y, float *ox, float *oy) {
  float tx = bdfi_mulf(m[0], x) + bdfi_mulf(m[2], y);
  float ty = bdfi_mulf(m[1], x) + bdfi_mulf(m[3], y);
  *ox = tx + m[4];
  *oy = ty + m[5];
}

/* ---- A set of part names ------------------------------------------------ */

static int bdfi_key_bit(const uint8_t *key, uint32_t bit) {
  return (key[bit >> 3] >> (7 - (bit & 7))) & 1;
}

void bdfi_hash_set_free(bdfi_hash_set *s) {
  free(s->nodes);
  free(s->keys);
  memset(s, 0, sizeof *s);
}

int bdfi_hash_set_add(bdfi_hash_set *s, const uint8_t key[16]) {
  uint32_t bit = 0;
  if (s->nkeys > 0) {
    /* the key the tree leads to: the one that shares the longest prefix */
    int32_t p = s->root;
    while (p >= 0) {
      p = s->nodes[p].child[bdfi_key_bit(key, s->nodes[p].bit)];
    }
    const uint8_t *best = s->keys + 16 * (size_t)(~p);
    size_t i = 0;
    while (i < 16 && best[i] == key[i]) {
      i++;
    }
    if (i == 16) {
      return 1;
    }
    uint8_t x = best[i] ^ key[i];
    bit = (uint32_t)i * 8;
    while ((x & 0x80) == 0) {
      x = (uint8_t)(x << 1);
      bit++;
    }
  }
  if (s->nkeys >= (size_t)INT32_MAX) {
    return -1;
  }
  if (s->nkeys == s->cap_keys) {
    size_t cap = s->cap_keys ? s->cap_keys * 2 : 16;
    uint8_t *keys = realloc(s->keys, cap * 16);
    if (!keys) {
      return -1;
    }
    s->keys = keys;
    s->cap_keys = cap;
  }
  if (s->nnodes == s->cap_nodes) {
    size_t cap = s->cap_nodes ? s->cap_nodes * 2 : 16;
    bdfi_cb_node *nodes = realloc(s->nodes, cap * sizeof *nodes);
    if (!nodes) {
      return -1;
    }
    s->nodes = nodes;
    s->cap_nodes = cap;
  }
  int32_t leaf = ~(int32_t)s->nkeys;
  memcpy(s->keys + 16 * s->nkeys, key, 16);
  s->nkeys++;
  if (s->nkeys == 1) {
    s->root = leaf;
    return 0;
  }
  /* a node for the bit goes where the path first tests a later bit */
  int32_t *where = &s->root;
  while (*where >= 0 && s->nodes[*where].bit < bit) {
    where = &s->nodes[*where].child[bdfi_key_bit(key, s->nodes[*where].bit)];
  }
  int dir = bdfi_key_bit(key, bit);
  bdfi_cb_node *n = &s->nodes[s->nnodes];
  n->bit = bit;
  n->child[dir] = leaf;
  n->child[1 - dir] = *where;
  *where = (int32_t)s->nnodes;
  s->nnodes++;
  return 0;
}

/* ---- Limits ------------------------------------------------------------- */

void bdfi_use_limits_init(bdfi_use_limits *l) { memset(l, 0, sizeof *l); }

void bdfi_use_limits_free(bdfi_use_limits *l) { bdfi_hash_set_free(&l->seen); }

static void bdfi_limits_fail(bdfi_use_limits *l, const char *msg) {
  if (!l->err) {
    l->err = msg;
  }
}

/* ---- The text ----------------------------------------------------------- */

typedef struct bdfi_run {
  const char *text;
  size_t text_len;
  float x, y, advance, size;
  float m[6];
  const bdf_object *font_obj; /* NULL: no font was set */
  size_t font_index;
  uint32_t ordinal;
  uint8_t align, sep, alt;
} bdfi_run;

typedef struct bdfi_link {
  float x, y, w, h;
  const char *url;
  size_t url_len;
  int64_t after;
} bdfi_link;

struct bdf_text {
  bdfi_run *runs;
  size_t nruns, cap_runs;
  bdfi_link *links;
  size_t nlinks, cap_links;
  void *owner;
  void (*free_owner)(void *);
};

void bdf_text_free(bdf_text *t) {
  if (!t) {
    return;
  }
  free(t->runs);
  free(t->links);
  if (t->free_owner) {
    t->free_owner(t->owner);
  }
  free(t);
}

void bdfi_text_adopt(bdf_text *t, void *owner, void (*free_owner)(void *)) {
  t->owner = owner;
  t->free_owner = free_owner;
}

size_t bdf_text_run_count(const bdf_text *t) { return t ? t->nruns : 0; }

bdf_status bdf_text_run_at(const bdf_text *t, size_t i, bdf_text_run *out) {
  if (!t || !out || i >= t->nruns) {
    return BDF_ERR_ARG;
  }
  const bdfi_run *r = &t->runs[i];
  memset(out, 0, sizeof *out);
  out->text = r->text ? r->text : "";
  out->text_len = r->text_len;
  out->x = r->x;
  out->y = r->y;
  out->advance = r->advance;
  out->size = r->size;
  memcpy(out->matrix, r->m, sizeof out->matrix);
  if (r->font_obj && bdf_object_font(r->font_obj, r->font_index, &out->font) == BDF_OK) {
    out->has_font = 1;
  }
  out->align = r->align;
  out->sep = r->sep;
  out->alt_text = r->alt;
  out->ordinal = r->ordinal;
  return BDF_OK;
}

size_t bdf_text_link_count(const bdf_text *t) { return t ? t->nlinks : 0; }

bdf_status bdf_text_link_at(const bdf_text *t, size_t i, bdf_text_link *out) {
  if (!t || !out || i >= t->nlinks) {
    return BDF_ERR_ARG;
  }
  const bdfi_link *l = &t->links[i];
  out->x = l->x;
  out->y = l->y;
  out->w = l->w;
  out->h = l->h;
  out->url = l->url ? l->url : "";
  out->url_len = l->url_len;
  out->after = l->after;
  return BDF_OK;
}

/* ---- The walk ----------------------------------------------------------- */

typedef struct bdfi_tstate {
  float m[6];
  const bdf_object *font_obj;
  size_t font_index;
  float size;
  uint8_t align;
} bdfi_tstate;

typedef struct bdfi_tx {
  bdf_text *text;
  bdf_resolve_fn resolve;
  void *ctx;
  bdfi_use_limits *limits;
  uint8_t pending; /* the separator a MARK asks for the next run */
  int has_mark;
  int has_alt; /* an ALT_TEXT waits for the drawing it describes */
  const char *alt;
  size_t alt_len;
  int depth; /* of the object being walked: 0 is the top-level one */
  int links;
  int oom;
} bdfi_tx;

/* The outcomes of walking one object. */
enum {
  BDFI_WALK_OK = 0,
  BDFI_WALK_FORMAT = 1, /* its op stream is malformed: the walk of it ends */
  BDFI_WALK_STOP = 2    /* a limit was passed or memory ran out: everything ends */
};

static int bdfi_tx_stopped(const bdfi_tx *t) { return t->oom || t->limits->err != NULL; }

/* bdfi_guess_sep estimates the separator between runs that no MARK tells of. */
static uint8_t bdfi_guess_sep(const bdfi_run *prev, const bdfi_run *cur) {
  float size = prev->size;
  if (size <= 0) {
    size = 10;
  }
  float dy = cur->y - prev->y;
  double ady = dy < 0 ? -(double)dy : (double)dy;
  if (ady > (double)size * 0.5) {
    return BDF_SEP_SPACE;
  }
  float end = prev->x + prev->advance;
  float gap = cur->x - end;
  if (prev->advance == 0 || gap > bdfi_mulf(0.2f, size)) {
    return BDF_SEP_SPACE;
  }
  return BDF_SEP_NONE;
}

static void bdfi_tx_emit(bdfi_tx *t, const char *text, size_t len, float x, float y, float advance,
                         const bdfi_tstate *st, int alt) {
  bdf_text *out = t->text;
  bdfi_run run;
  memset(&run, 0, sizeof run);
  run.text = text;
  run.text_len = len;
  run.advance = advance;
  run.size = st->size;
  run.font_obj = st->font_obj;
  run.font_index = st->font_index;
  run.align = st->align;
  memcpy(run.m, st->m, sizeof run.m);
  bdfi_mat_apply(st->m, x, y, &run.x, &run.y);
  run.ordinal = (uint32_t)out->nruns;
  run.alt = alt ? 1 : 0;
  if (t->has_mark) {
    run.sep = t->pending;
  } else if (out->nruns > 0) {
    run.sep = bdfi_guess_sep(&out->runs[out->nruns - 1], &run);
  }
  t->pending = BDF_SEP_NONE;
  t->has_mark = 0;
  if (out->nruns >= BDFI_MAX_TEXT_RUNS) {
    bdfi_limits_fail(t->limits, "too many text runs");
    return;
  }
  if (out->nruns == out->cap_runs) {
    size_t cap = out->cap_runs ? out->cap_runs * 2 : 64;
    bdfi_run *runs = realloc(out->runs, cap * sizeof *runs);
    if (!runs) {
      t->oom = 1;
      return;
    }
    out->runs = runs;
    out->cap_runs = cap;
  }
  out->runs[out->nruns++] = run;
}

/* bdfi_tx_flush_alt emits an ALT_TEXT that no drawing took, without a place. */
static void bdfi_tx_flush_alt(bdfi_tx *t, const bdfi_tstate *st) {
  if (t->has_alt) {
    t->has_alt = 0;
    bdfi_tx_emit(t, t->alt, t->alt_len, 0, 0, 0, st, 1);
  }
}

static void bdfi_tx_mark(bdfi_tx *t, uint8_t sep) {
  if (!t->has_mark || sep > t->pending) {
    t->pending = sep;
  }
  t->has_mark = 1;
}

/* bdfi_tx_link records the bounding box of a LINK's rectangle. */
static void bdfi_tx_link(bdfi_tx *t, const bdfi_tstate *st, float x, float y, float w, float h,
                         const char *url, size_t url_len) {
  bdf_text *out = t->text;
  if (out->nlinks >= BDFI_MAX_TEXT_LINKS) {
    bdfi_limits_fail(t->limits, "too many links");
    return;
  }
  if (out->nlinks == out->cap_links) {
    size_t cap = out->cap_links ? out->cap_links * 2 : 16;
    bdfi_link *links = realloc(out->links, cap * sizeof *links);
    if (!links) {
      t->oom = 1;
      return;
    }
    out->links = links;
    out->cap_links = cap;
  }
  const float px[4] = {x, x + w, x, x + w};
  const float py[4] = {y, y, y + h, y + h};
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  for (int i = 0; i < 4; i++) {
    float tx, ty;
    bdfi_mat_apply(st->m, px[i], py[i], &tx, &ty);
    if (i == 0 || tx < x0) {
      x0 = tx;
    }
    if (i == 0 || tx > x1) {
      x1 = tx;
    }
    if (i == 0 || ty < y0) {
      y0 = ty;
    }
    if (i == 0 || ty > y1) {
      y1 = ty;
    }
  }
  bdfi_link *l = &out->links[out->nlinks++];
  l->x = x0;
  l->y = y0;
  l->w = x1 - x0;
  l->h = y1 - y0;
  l->url = url;
  l->url_len = url_len;
  l->after = (int64_t)out->nruns - 1;
}

/* bdfi_tx_child returns the object an instruction draws, NULL for one that
 * is missing, and whether it was walked before. */
static const bdf_object *bdfi_tx_child(bdfi_tx *t, const bdf_object *o, const bdf_instr *it,
                                       int *again) {
  *again = 0;
  uint64_t i = bdf_instr_u64(it, 0);
  if (i >= bdf_object_object_count(o) || !t->resolve) {
    return NULL;
  }
  const uint8_t *hash = bdf_object_object(o, (size_t)i);
  const bdf_object *c = t->resolve(t->ctx, hash);
  if (!c) {
    return NULL;
  }
  int seen = bdfi_hash_set_add(&t->limits->seen, hash);
  if (seen < 0) {
    t->oom = 1;
    return NULL;
  }
  *again = seen;
  return c;
}

static int bdfi_tx_walk(bdfi_tx *t, const bdf_object *o, const float m[6], int reused,
                        bdf_error *err) {
  bdfi_tstate st;
  memset(&st, 0, sizeof st);
  memcpy(st.m, m, sizeof st.m);
  st.size = 10;
  bdfi_tstate *stack = NULL;
  size_t nstack = 0, cap_stack = 0;
  int64_t masking = 0; /* inside MASK_BEGIN … MASK_END: a soft mask, not content */
  uint64_t max_reused =
      t->limits->max_reused ? t->limits->max_reused : (uint64_t)BDF_MAX_REUSED_INSTRUCTIONS;
  bdf_instr it;
  bdfi_instr_init(&it, o);
  int result = BDFI_WALK_OK;
  for (;;) {
    bdf_error e;
    int rc = bdf_instr_next(&it, &e);
    if (rc == 0) {
      break;
    }
    if (reused && ++t->limits->reused > max_reused) {
      bdfi_limits_fail(t->limits, "objects are drawn too many times");
      break;
    }
    if (rc < 0) {
      if (e.status == BDF_ERR_NOMEM) {
        t->oom = 1;
      } else {
        result = BDFI_WALK_FORMAT;
        if (err) {
          *err = e;
        }
      }
      break;
    }
    uint8_t op = bdf_instr_op(&it);
    if (op == BDF_OP_MASK_BEGIN) {
      masking++;
      continue;
    }
    if (op == BDF_OP_MASK_END) {
      masking--;
      continue;
    }
    if (masking > 0) {
      continue;
    }
    switch (op) {
    case BDF_OP_SAVE:
      if (nstack == cap_stack) {
        size_t cap = cap_stack ? cap_stack * 2 : 8;
        bdfi_tstate *grown = cap > SIZE_MAX / sizeof *grown ? NULL : realloc(stack, cap * sizeof *grown);
        if (!grown) {
          t->oom = 1;
          break;
        }
        stack = grown;
        cap_stack = cap;
      }
      stack[nstack++] = st;
      break;
    case BDF_OP_RESTORE:
      if (nstack > 0) {
        st = stack[--nstack];
      }
      break;
    case BDF_OP_TRANSFORM: {
      float n[6];
      for (size_t i = 0; i < 6; i++) {
        n[i] = bdf_instr_f32(&it, i);
      }
      bdfi_mat_mul(st.m, n, st.m);
      break;
    }
    case BDF_OP_TRANSLATE: {
      const float n[6] = {1, 0, 0, 1, bdf_instr_f32(&it, 0), bdf_instr_f32(&it, 1)};
      bdfi_mat_mul(st.m, n, st.m);
      break;
    }
    case BDF_OP_SCALE: {
      const float n[6] = {bdf_instr_f32(&it, 0), 0, 0, bdf_instr_f32(&it, 1), 0, 0};
      bdfi_mat_mul(st.m, n, st.m);
      break;
    }
    case BDF_OP_FONT: {
      uint64_t i = bdf_instr_u64(&it, 0);
      if (i < bdf_object_font_count(o)) {
        st.font_obj = o;
        st.font_index = (size_t)i;
      }
      st.size = bdf_instr_f32(&it, 1);
      break;
    }
    case BDF_OP_TEXT_STYLE:
      st.align = (uint8_t)bdf_instr_u64(&it, 0);
      break;
    case BDF_OP_MARK: {
      uint8_t kind = (uint8_t)bdf_instr_u64(&it, 0);
      switch (kind) {
      case BDF_MARK_LINE:
        bdfi_tx_flush_alt(t, &st);
        bdfi_tx_mark(t, BDF_SEP_SPACE);
        break;
      case BDF_MARK_PARAGRAPH:
      case BDF_MARK_CELL:
      case BDF_MARK_BOX:
      case BDF_MARK_HEADING:
      case BDF_MARK_LIST:
      case BDF_MARK_LIST_ITEM:
      case BDF_MARK_TABLE:
      case BDF_MARK_FIGURE:
      case BDF_MARK_END:
        bdfi_tx_flush_alt(t, &st);
        bdfi_tx_mark(t, BDF_SEP_BREAK);
        break;
      case BDF_MARK_WRAP:
        bdfi_tx_flush_alt(t, &st);
        bdfi_tx_mark(t, BDF_SEP_NONE);
        break;
      case BDF_MARK_ALT_TEXT:
        bdfi_tx_flush_alt(t, &st);
        t->alt = bdf_instr_string(&it, 1, &t->alt_len);
        t->has_alt = 1;
        break;
      default:
        /* LANG and unknown kinds leave the separator to the next MARK or to
         * the guess from the positions */
        break;
      }
      break;
    }
    case BDF_OP_FILL_TEXT:
    case BDF_OP_STROKE_TEXT: {
      float x = bdf_instr_f32(&it, 1), y = bdf_instr_f32(&it, 2), adv = bdf_instr_f32(&it, 3);
      if (t->has_alt) {
        /* the drawing right after an ALT_TEXT renders that text */
        t->has_alt = 0;
        bdfi_tx_emit(t, t->alt, t->alt_len, x, y, adv, &st, 1);
      } else {
        size_t len;
        const char *s = bdf_instr_string(&it, 0, &len);
        bdfi_tx_emit(t, s, len, x, y, adv, &st, 0);
      }
      break;
    }
    case BDF_OP_FILL_PATH_AT:
      if (t->has_alt) {
        t->has_alt = 0;
        bdfi_tx_emit(t, t->alt, t->alt_len, bdf_instr_f32(&it, 2), bdf_instr_f32(&it, 3), 0, &st, 1);
      }
      break;
    case BDF_OP_FILL_PATH_RUN:
      if (t->has_alt) {
        size_t n;
        const bdf_glyph *g = bdf_instr_glyphs(&it, 1, &n);
        t->has_alt = 0;
        if (n > 0) {
          bdfi_tx_emit(t, t->alt, t->alt_len, g[0].x, g[0].y, 0, &st, 1);
        } else {
          bdfi_tx_emit(t, t->alt, t->alt_len, 0, 0, 0, &st, 1);
        }
      }
      break;
    case BDF_OP_USE:
    case BDF_OP_USE_AT: {
      float x = 0, y = 0;
      if (op == BDF_OP_USE_AT) {
        x = bdf_instr_f32(&it, 1);
        y = bdf_instr_f32(&it, 2);
      }
      int again;
      const bdf_object *child = bdfi_tx_child(t, o, &it, &again);
      if (t->has_alt) {
        /* the text the child draws spans its bbox along the baseline */
        float advance = 0;
        if (child) {
          float bbox[4];
          bdf_object_bbox(child, bbox);
          x = x + bbox[0];
          advance = bbox[2];
        }
        t->has_alt = 0;
        bdfi_tx_emit(t, t->alt, t->alt_len, x, y, advance, &st, 1);
        break;
      }
      if (!child) {
        break;
      }
      if (t->depth >= BDF_MAX_USE_DEPTH) {
        bdfi_limits_fail(t->limits, "objects draw objects too deep");
        break;
      }
      float cm[6];
      memcpy(cm, st.m, sizeof cm);
      if (op == BDF_OP_USE_AT) {
        const float n[6] = {1, 0, 0, 1, x, y};
        bdfi_mat_mul(cm, n, cm);
      }
      t->depth++;
      /* a child whose stream is malformed keeps the runs before the damage,
       * and the walk goes on after it, as in the reference */
      (void)bdfi_tx_walk(t, child, cm, reused || again, NULL);
      t->depth--;
      break;
    }
    case BDF_OP_LINK:
      if (t->links) {
        size_t len;
        const char *url = bdf_instr_string(&it, 4, &len);
        bdfi_tx_link(t, &st, bdf_instr_f32(&it, 0), bdf_instr_f32(&it, 1), bdf_instr_f32(&it, 2),
                     bdf_instr_f32(&it, 3), url, len);
      }
      break;
    default:
      break;
    }
    if (bdfi_tx_stopped(t)) {
      break;
    }
  }
  bdfi_instr_clear(&it);
  free(stack);
  return bdfi_tx_stopped(t) ? BDFI_WALK_STOP : result;
}

bdf_status bdfi_text_extract(const bdf_object *obj, bdf_resolve_fn resolve, void *ctx,
                             bdfi_use_limits *limits, int links, bdf_text **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!obj || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  if (limits->err) {
    /* a limit passed in an earlier walk with these limits holds for this one */
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, limits->err);
    return BDF_ERR_FORMAT;
  }
  bdf_text *text = calloc(1, sizeof *text);
  if (!text) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  bdfi_tx t;
  memset(&t, 0, sizeof t);
  t.text = text;
  t.resolve = resolve;
  t.ctx = ctx;
  t.limits = limits;
  t.links = links;
  const float identity[6] = {1, 0, 0, 1, 0, 0};
  bdf_error werr;
  bdfi_error_clear(&werr);
  int rc = bdfi_tx_walk(&t, obj, identity, 0, &werr);
  if (t.oom) {
    bdf_text_free(text);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  if (limits->err) {
    bdf_text_free(text);
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, limits->err);
    return BDF_ERR_FORMAT;
  }
  if (rc == BDFI_WALK_FORMAT) {
    bdf_text_free(text);
    if (err) {
      *err = werr;
    }
    return BDF_ERR_FORMAT;
  }
  /* An ALT_TEXT nothing took is the last run. The reference does not report
   * the limit of runs when this one passes it; the limit holds from then on. */
  bdfi_tstate st;
  memset(&st, 0, sizeof st);
  memcpy(st.m, identity, sizeof st.m);
  st.size = 10;
  bdfi_tx_flush_alt(&t, &st);
  if (t.oom) {
    bdf_text_free(text);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  *out = text;
  return BDF_OK;
}

bdf_status bdf_text_extract(const bdf_object *obj, bdf_resolve_fn resolve, void *ctx,
                            bdf_text **out, bdf_error *err) {
  bdfi_use_limits limits;
  bdfi_use_limits_init(&limits);
  bdf_status st = bdfi_text_extract(obj, resolve, ctx, &limits, 1, out, err);
  bdfi_use_limits_free(&limits);
  return st;
}

/* ---- Link targets (spec §7.7) ------------------------------------------- */

/* bdfi_page_number reads 1 to 7 digits that are all of s: the number, or -1. */
static int64_t bdfi_page_number(const char *s, size_t n) {
  if (n < 1 || n > 7) {
    return -1;
  }
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return -1;
    }
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

static int bdfi_has_prefix(const char *s, size_t n, const char *prefix) {
  size_t k = strlen(prefix);
  return n >= k && memcmp(s, prefix, k) == 0;
}

/* bdfi_has_scheme compares the start of a url with a scheme and its colon,
 * ignoring ASCII case. */
static int bdfi_has_scheme(const char *s, size_t n, const char *scheme) {
  size_t k = strlen(scheme);
  if (n < k) {
    return 0;
  }
  for (size_t i = 0; i < k; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != scheme[i]) {
      return 0;
    }
  }
  return 1;
}

static int bdfi_hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* bdfi_percent_byte reads %XX at s[i]: the byte, or -1. */
static int bdfi_percent_byte(const char *s, size_t n, size_t i) {
  if (n - i < 3 || s[i] != '%') {
    return -1;
  }
  int hi = bdfi_hex_digit(s[i + 1]), lo = bdfi_hex_digit(s[i + 2]);
  return hi < 0 || lo < 0 ? -1 : (hi << 4) | lo;
}

/*
 * bdfi_decode_id decodes a view id as the reference's viewer does: "+" is a
 * space, and %XX sequences must spell valid UTF-8 (decodeURIComponent). With
 * buf NULL it only checks and measures. It returns the length, or -1.
 */
static int64_t bdfi_decode_id(const char *s, size_t n, char *buf, size_t cap) {
  size_t out = 0;
  size_t i = 0;
  while (i < n) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    size_t nb = 1;
    if (s[i] == '+') {
      bytes[0] = ' ';
      i++;
    } else if (s[i] == '%') {
      int b = bdfi_percent_byte(s, n, i);
      if (b < 0) {
        return -1;
      }
      bytes[0] = (uint8_t)b;
      i += 3;
      if (b >= 0x80) {
        /* the rest of the sequence is percent-encoded too, and valid */
        nb = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 ? 2 : 0;
        if (nb == 0 || b > 0xF4) {
          return -1;
        }
        for (size_t k = 1; k < nb; k++) {
          int c = bdfi_percent_byte(s, n, i);
          if (c < 0x80 || c > 0xBF) {
            return -1;
          }
          bytes[k] = (uint8_t)c;
          i += 3;
        }
        if ((bytes[0] == 0xE0 && bytes[1] < 0xA0) || (bytes[0] == 0xED && bytes[1] > 0x9F) ||
            (bytes[0] == 0xF0 && bytes[1] < 0x90) || (bytes[0] == 0xF4 && bytes[1] > 0x8F)) {
          return -1; /* overlong, a surrogate, or beyond U+10FFFF */
        }
      }
    } else {
      bytes[0] = (uint8_t)s[i];
      i++;
    }
    if (buf) {
      if (cap - out < nb) {
        return -1;
      }
      memcpy(buf + out, bytes, nb);
    }
    out += nb;
  }
  return (int64_t)out;
}

bdf_link_kind bdf_link_parse(const char *url, size_t len, bdf_link_target *out) {
  bdf_link_target t;
  memset(&t, 0, sizeof t);
  t.kind = BDF_LINK_NONE;
  if (url && len > 0 && url[0] == '#') {
    if (bdfi_has_prefix(url, len, "#page=")) {
      int64_t page = bdfi_page_number(url + 6, len - 6);
      if (page >= 1) {
        t.kind = BDF_LINK_PAGE;
        t.page = page;
      }
    } else if (bdfi_has_prefix(url, len, "#view=")) {
      /* the id runs to the first '&'; nothing but "&page=N" may follow, and
       * the id holds no '#' */
      const char *id = url + 6;
      size_t rest = len - 6, id_len = 0;
      while (id_len < rest && id[id_len] != '&' && id[id_len] != '#') {
        id_len++;
      }
      int64_t page = 0;
      int ok = id_len > 0;
      if (ok && id_len < rest) {
        const char *tail = id + id_len;
        size_t tail_len = rest - id_len;
        page = bdfi_has_prefix(tail, tail_len, "&page=") ? bdfi_page_number(tail + 6, tail_len - 6) : -1;
        ok = page >= 1;
      }
      if (ok && bdfi_decode_id(id, id_len, NULL, 0) >= 0) {
        t.kind = BDF_LINK_VIEW;
        t.page = page;
        t.view = id;
        t.view_len = id_len;
      }
    }
  } else if (url && (bdfi_has_scheme(url, len, "http:") || bdfi_has_scheme(url, len, "https:") ||
                     bdfi_has_scheme(url, len, "mailto:"))) {
    t.kind = BDF_LINK_URL;
  }
  if (out) {
    *out = t;
  }
  return t.kind;
}

int64_t bdf_link_view_id(const bdf_link_target *target, char *buf, size_t cap) {
  if (!target || target->kind != BDF_LINK_VIEW || !target->view || (!buf && cap > 0)) {
    return -1;
  }
  int64_t n = bdfi_decode_id(target->view, target->view_len, NULL, 0);
  if (n < 0 || (uint64_t)n > cap) {
    return -1;
  }
  return bdfi_decode_id(target->view, target->view_len, buf, cap);
}
