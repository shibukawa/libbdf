/*
 * Text extraction: the runs of an object and the objects it draws, a port of
 * text.go of the reference implementation (the same walk, the same
 * separators, the same limits), and what the reference leaves to its
 * TypeScript reader (packages/core/src/text.ts): the LINK areas and the
 * structure the MARKs give the text (headings, lists, tables, figures).
 */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

/* The most states a walk keeps saved at a time, as the renderer keeps them
 * (BDFI_MAX_STATES of the rasterizer): a SAVE is one byte, and a state a
 * hundred, so a part of nothing but SAVEs would otherwise ask for a hundred
 * times its size. Those beyond are not kept, and their RESTOREs restore
 * nothing. */
#define BDFI_TX_MAX_STATES 1024

/* A double as a float, as Go converts one: beyond the range of a float it
 * is infinite (C leaves the conversion undefined there). */
static float bdfi_tx_f32(double v) {
  return v > FLT_MAX ? INFINITY : v < -FLT_MAX ? -INFINITY : (float)v;
}

/* The runs, the links and the nodes of a walk are kept in memory, so all
 * three are bounded. */
#define BDFI_MAX_TEXT_RUNS  ((size_t)1 << 22)
#define BDFI_MAX_TEXT_LINKS ((size_t)1 << 22)
#define BDFI_MAX_TEXT_NODES ((size_t)1 << 22)
/* Every drawing grows every open figure: the figures that grow are bounded,
 * and one opened inside that many others keeps the size of nothing. */
#define BDFI_MAX_OPEN_FIGURES 64

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

/*
 * The bounds of figures are computed as the TypeScript reader computes
 * them, in doubles and without fused multiply-adds, so that they come out
 * the same to the bit: a second transform, kept beside the float one the
 * positions of the runs use.
 */
static double bdfi_muld(double a, double b) {
  volatile double p = a * b;
  return p;
}

typedef struct bdfi_rect {
  double x, y, w, h;
} bdfi_rect;

static int bdfi_negative(double v) {
  uint64_t u;
  memcpy(&u, &v, sizeof u);
  return (u >> 63) != 0;
}

/* Math.min, Math.max and Math.abs: NaN when an operand is, and -0 below +0. */
static double bdfi_js_min(double a, double b) {
  if (a != a) {
    return a;
  }
  if (b != b) {
    return b;
  }
  if (a == 0 && b == 0) {
    return bdfi_negative(a) ? a : b;
  }
  return a < b ? a : b;
}

static double bdfi_js_max(double a, double b) {
  if (a != a) {
    return a;
  }
  if (b != b) {
    return b;
  }
  if (a == 0 && b == 0) {
    return bdfi_negative(a) ? b : a;
  }
  return a > b ? a : b;
}

static double bdfi_js_abs(double v) { return v < 0 ? -v : v == 0 ? 0.0 : v; }

static void bdfi_dmat_mul(const double m[6], const double n[6], double out[6]) {
  double r[6];
  r[0] = bdfi_muld(m[0], n[0]) + bdfi_muld(m[2], n[1]);
  r[1] = bdfi_muld(m[1], n[0]) + bdfi_muld(m[3], n[1]);
  r[2] = bdfi_muld(m[0], n[2]) + bdfi_muld(m[2], n[3]);
  r[3] = bdfi_muld(m[1], n[2]) + bdfi_muld(m[3], n[3]);
  r[4] = bdfi_muld(m[0], n[4]) + bdfi_muld(m[2], n[5]);
  r[4] = r[4] + m[4];
  r[5] = bdfi_muld(m[1], n[4]) + bdfi_muld(m[3], n[5]);
  r[5] = r[5] + m[5];
  memcpy(out, r, sizeof r);
}

/* bdfi_box_of is the bounding box of a rectangle moved by m. */
static bdfi_rect bdfi_box_of(const double m[6], double x, double y, double w, double h) {
  const double px[4] = {x, x + w, x, x + w};
  const double py[4] = {y, y, y + h, y + h};
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < 4; i++) {
    double tx = bdfi_muld(m[0], px[i]) + bdfi_muld(m[2], py[i]);
    double ty = bdfi_muld(m[1], px[i]) + bdfi_muld(m[3], py[i]);
    tx = tx + m[4];
    ty = ty + m[5];
    x0 = bdfi_js_min(x0, tx);
    y0 = bdfi_js_min(y0, ty);
    x1 = bdfi_js_max(x1, tx);
    y1 = bdfi_js_max(y1, ty);
  }
  bdfi_rect r = {x0, y0, x1 - x0, y1 - y0};
  return r;
}

/* bdfi_intersect is what two rectangles share: 0 when they share nothing. */
static int bdfi_intersect(const bdfi_rect *a, const bdfi_rect *b, bdfi_rect *out) {
  double x0 = bdfi_js_max(a->x, b->x), y0 = bdfi_js_max(a->y, b->y);
  double x1 = bdfi_js_min(a->x + a->w, b->x + b->w), y1 = bdfi_js_min(a->y + a->h, b->y + b->h);
  if (!(x1 >= x0 && y1 >= y0)) {
    return 0;
  }
  out->x = x0;
  out->y = y0;
  out->w = x1 - x0;
  out->h = y1 - y0;
  return 1;
}

static bdfi_rect bdfi_union(const bdfi_rect *a, const bdfi_rect *b) {
  double x0 = bdfi_js_min(a->x, b->x), y0 = bdfi_js_min(a->y, b->y);
  bdfi_rect r = {x0, y0, bdfi_js_max(a->x + a->w, b->x + b->w) - x0,
                 bdfi_js_max(a->y + a->h, b->y + b->h) - y0};
  return r;
}

/* bdfi_path_box is the bounding box of the control points of an inline path
 * (arcs and ellipses by their extent): 0 for a path with no points. */
static int bdfi_path_box(const bdf_path *p, bdfi_rect *out) {
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
#define BDFI_ADD(px, py)                                                                           \
  do {                                                                                             \
    double ax = (px), ay = (py);                                                                   \
    x0 = bdfi_js_min(x0, ax);                                                                      \
    y0 = bdfi_js_min(y0, ay);                                                                      \
    x1 = bdfi_js_max(x1, ax);                                                                      \
    y1 = bdfi_js_max(y1, ay);                                                                      \
  } while (0)
  const float *a = p->args;
  size_t i = 0, n = p->nargs;
  for (size_t v = 0; v < p->nverbs; v++) {
    size_t need = 0;
    switch (p->verbs[v]) {
    case BDF_VERB_MOVE:
    case BDF_VERB_LINE:
      need = 2;
      break;
    case BDF_VERB_QUAD:
    case BDF_VERB_RECT:
      need = 4;
      break;
    case BDF_VERB_CUBIC:
      need = 6;
      break;
    case BDF_VERB_ELLIPSE:
      need = 8;
      break;
    case BDF_VERB_ARC_TO:
    case BDF_VERB_ROUND_RECT:
      need = 5;
      break;
    default:
      break;
    }
    if (need > n - i) {
      break; /* the decoder checked the counts; a table that lies ends here */
    }
    switch (p->verbs[v]) {
    case BDF_VERB_MOVE:
    case BDF_VERB_LINE:
      BDFI_ADD(a[i], a[i + 1]);
      break;
    case BDF_VERB_QUAD:
    case BDF_VERB_ARC_TO:
      BDFI_ADD(a[i], a[i + 1]);
      BDFI_ADD(a[i + 2], a[i + 3]);
      break;
    case BDF_VERB_CUBIC:
      BDFI_ADD(a[i], a[i + 1]);
      BDFI_ADD(a[i + 2], a[i + 3]);
      BDFI_ADD(a[i + 4], a[i + 5]);
      break;
    case BDF_VERB_RECT:
    case BDF_VERB_ROUND_RECT:
      BDFI_ADD(a[i], a[i + 1]);
      BDFI_ADD((double)a[i] + (double)a[i + 2], (double)a[i + 1] + (double)a[i + 3]);
      break;
    case BDF_VERB_ELLIPSE: {
      double r = bdfi_js_max(bdfi_js_abs(a[i + 2]), bdfi_js_abs(a[i + 3]));
      BDFI_ADD((double)a[i] - r, (double)a[i + 1] - r);
      BDFI_ADD((double)a[i] + r, (double)a[i + 1] + r);
      break;
    }
    default:
      break;
    }
    i += need;
  }
#undef BDFI_ADD
  if (!(x1 >= x0)) {
    return 0;
  }
  out->x = x0;
  out->y = y0;
  out->w = x1 - x0;
  out->h = y1 - y0;
  return 1;
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
    uint8_t *keys = bdfi_realloc_array(s->keys, cap, 16);
    if (!keys) {
      return -1;
    }
    s->keys = keys;
    s->cap_keys = cap;
  }
  if (s->nnodes == s->cap_nodes) {
    size_t cap = s->cap_nodes ? s->cap_nodes * 2 : 16;
    bdfi_cb_node *nodes = bdfi_realloc_array(s->nodes, cap, sizeof *nodes);
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
  int64_t node; /* its leaf; -1 when the structure was not asked for */
  const char *lang;
  size_t lang_len;
} bdfi_run;

typedef struct bdfi_link {
  float x, y, w, h;
  const char *url;
  size_t url_len;
  int64_t after;
  int64_t node;
} bdfi_link;

typedef struct bdfi_node {
  uint8_t kind; /* BDF_NODE_* */
  uint8_t level;
  uint8_t has_cell, scope;
  uint8_t has_bounds;
  uint32_t row, col, rows, cols;
  int64_t parent;
  const char *alt;
  size_t alt_len;
  bdfi_rect bounds;
  bdfi_rect origin; /* a figure: where it was opened, its bounds when it draws nothing */
} bdfi_node;

struct bdf_text {
  bdfi_run *runs;
  size_t nruns, cap_runs;
  bdfi_link *links;
  size_t nlinks, cap_links;
  bdfi_node *nodes;
  size_t nnodes, cap_nodes;
  void *owner;
  void (*free_owner)(void *);
};

void bdf_text_free(bdf_text *t) {
  if (!t) {
    return;
  }
  free(t->runs);
  free(t->links);
  free(t->nodes);
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
  out->node = r->node;
  out->lang = r->lang ? r->lang : "";
  out->lang_len = r->lang_len;
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
  out->node = l->node;
  return BDF_OK;
}

size_t bdf_text_node_count(const bdf_text *t) { return t ? t->nnodes : 0; }

bdf_status bdf_text_node_at(const bdf_text *t, size_t i, bdf_text_node *out) {
  if (!t || !out || i >= t->nnodes) {
    return BDF_ERR_ARG;
  }
  const bdfi_node *n = &t->nodes[i];
  memset(out, 0, sizeof *out);
  out->kind = n->kind;
  out->parent = n->parent;
  out->level = n->level;
  out->has_cell = n->has_cell;
  out->row = n->row;
  out->col = n->col;
  out->rows = n->rows;
  out->cols = n->cols;
  out->scope = n->scope;
  out->alt = n->alt ? n->alt : "";
  out->alt_len = n->alt_len;
  out->has_bounds = n->has_bounds;
  if (n->has_bounds) {
    out->x = bdfi_tx_f32(n->bounds.x);
    out->y = bdfi_tx_f32(n->bounds.y);
    out->w = bdfi_tx_f32(n->bounds.w);
    out->h = bdfi_tx_f32(n->bounds.h);
  }
  return BDF_OK;
}

/* ---- The walk ----------------------------------------------------------- */

typedef struct bdfi_tstate {
  float m[6];
  const bdf_object *font_obj;
  size_t font_index;
  float size;
  uint8_t align;
  /* for the bounds of figures: the transform in doubles, and the clip */
  uint8_t has_clip;
  double dm[6];
  bdfi_rect clip;
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
  int links; /* the links and the structure are asked for */
  int oom;
  int too_many_nodes;
  /* the structure (spec §7.8), which runs on through USE in walk order */
  size_t *open; /* the open containers, outermost first */
  size_t nopen, cap_open;
  int64_t leaf;     /* the leaf the next run joins; -1: a new one */
  int has_reserved; /* a leaf MARK chose the kind of the next leaf */
  bdfi_node reserved;
  const char *lang;
  size_t lang_len;
  size_t figures[BDFI_MAX_OPEN_FIGURES]; /* the open figures, which grow with what is drawn */
  size_t nfigures;
} bdfi_tx;

/* The outcomes of walking one object. */
enum {
  BDFI_WALK_OK = 0,
  BDFI_WALK_FORMAT = 1, /* its op stream is malformed: the walk of it ends */
  BDFI_WALK_STOP = 2    /* a limit was passed or memory ran out: everything ends */
};

static int bdfi_tx_stopped(const bdfi_tx *t) { return t->oom || t->limits->err != NULL; }

/* ---- The structure ------------------------------------------------------ */

/* bdfi_js_space is the length of the white space character at s[i] (the \s
 * of a JavaScript regular expression, and what Number() trims), or 0. */
static size_t bdfi_js_space(const char *s, size_t n, size_t i) {
  const uint8_t *b = (const uint8_t *)s;
  if (i >= n) {
    return 0;
  }
  if (b[i] == ' ' || (b[i] >= 0x09 && b[i] <= 0x0D)) {
    return 1;
  }
  if (b[i] == 0xC2 && n - i >= 2 && b[i + 1] == 0xA0) {
    return 2;
  }
  if (n - i < 3) {
    return 0;
  }
  uint32_t c = ((uint32_t)b[i] << 16) | ((uint32_t)b[i + 1] << 8) | b[i + 2];
  if (c == 0xE19A80 || (c >= 0xE28080 && c <= 0xE2808A) || c == 0xE280A8 || c == 0xE280A9 ||
      c == 0xE280AF || c == 0xE2819F || c == 0xE38080 || c == 0xEFBBBF) {
    return 3;
  }
  return 0;
}

static int bdfi_is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int bdfi_is_digit(char c) { return c >= '0' && c <= '9'; }

/* bdfi_clamp_u32 is a JavaScript number as an index: what does not fit is
 * the largest. */
static uint32_t bdfi_clamp_u32(double v) {
  if (!(v > 0)) {
    return 0;
  }
  return v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
}

/* bdfi_a1 reads the letters and digits of a cell reference at *i: the row
 * and the column from 0, as doubles (a reference may name any number). */
static int bdfi_a1(const char *s, size_t n, size_t *i, double *row, double *col) {
  size_t k = *i;
  double c = 0, r = 0;
  if (k >= n || !bdfi_is_alpha(s[k])) {
    return 0;
  }
  for (; k < n && bdfi_is_alpha(s[k]); k++) {
    c = c * 26 + (double)((s[k] & 0xDF) - 64);
  }
  if (k >= n || !bdfi_is_digit(s[k])) {
    return 0;
  }
  for (; k < n && bdfi_is_digit(s[k]); k++) {
    r = r * 10 + (double)(s[k] - '0');
  }
  *i = k;
  *row = r - 1;
  *col = c - 1;
  return 1;
}

/* bdfi_cell_ref parses a CELL payload into a node: an A1 reference or a
 * range, then " col" or " row" for a header cell (parseCellRef of the
 * TypeScript reader). 0 when the payload is none. */
static int bdfi_cell_ref(const char *s, size_t n, bdfi_node *out) {
  size_t i = 0, w;
  while ((w = bdfi_js_space(s, n, i)) > 0) {
    i += w;
  }
  double row, col, row2, col2;
  if (!bdfi_a1(s, n, &i, &row, &col)) {
    return 0;
  }
  row2 = row;
  col2 = col;
  if (i < n && s[i] == ':') {
    i++;
    if (!bdfi_a1(s, n, &i, &row2, &col2)) {
      return 0;
    }
  }
  uint8_t scope = BDF_SCOPE_NONE;
  size_t k = i;
  while ((w = bdfi_js_space(s, n, k)) > 0) {
    k += w;
  }
  if (k > i && n - k >= 3 && (memcmp(s + k, "col", 3) == 0 || memcmp(s + k, "row", 3) == 0)) {
    scope = s[k] == 'c' ? BDF_SCOPE_COL : BDF_SCOPE_ROW;
    k += 3;
    while ((w = bdfi_js_space(s, n, k)) > 0) {
      k += w;
    }
  }
  if (k != n || row < 0) {
    return 0;
  }
  out->has_cell = 1;
  out->row = bdfi_clamp_u32(row);
  out->col = bdfi_clamp_u32(col);
  out->rows = bdfi_clamp_u32(bdfi_js_max(1, row2 - row + 1));
  out->cols = bdfi_clamp_u32(bdfi_js_max(1, col2 - col + 1));
  out->scope = scope;
  return 1;
}

/*
 * bdfi_heading_level is the level a HEADING payload names, 1 to 6, or 0: the
 * payload as JavaScript's Number() reads it, which takes "3", " 3 ", "3.0",
 * "30e-1" and "0x3" alike. (A decimal that only rounds to a whole number
 * names none here.)
 */
static uint8_t bdfi_heading_level(const char *s, size_t n) {
  size_t i = 0, w;
  while ((w = bdfi_js_space(s, n, i)) > 0) {
    i += w;
  }
  /* the end without the white space after it */
  size_t end = i;
  for (size_t k = i; k < n;) {
    w = bdfi_js_space(s, n, k);
    k += w ? w : 1;
    if (!w) {
      end = k;
    }
  }
  n = end;
  if (i >= n) {
    return 0;
  }
  if (n - i > 2 && s[i] == '0' && bdfi_is_alpha(s[i + 1]) && (s[i + 1] | 0x20) != 'e') {
    char c = (char)(s[i + 1] | 0x20);
    unsigned base = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 0;
    if (base == 0) {
      return 0;
    }
    unsigned v = 0;
    for (size_t k = i + 2; k < n; k++) {
      char d = (char)(s[k] | 0x20);
      unsigned digit = bdfi_is_digit(s[k]) ? (unsigned)(s[k] - '0')
                       : d >= 'a' && d <= 'f' ? (unsigned)(d - 'a') + 10
                                              : 99;
      if (digit >= base) {
        return 0;
      }
      v = v > 6 ? 7 : v * base + digit;
    }
    return v >= 1 && v <= 6 ? (uint8_t)v : 0;
  }
  int negative = 0;
  if (s[i] == '+' || s[i] == '-') {
    negative = s[i] == '-';
    i++;
  }
  /* a whole number from 1 to 6 has one digit that is not zero, in the place
   * of the ones once the exponent has moved the point */
  size_t int_from = i;
  while (i < n && bdfi_is_digit(s[i])) {
    i++;
  }
  size_t int_to = i, frac_from = i, frac_to = i;
  if (i < n && s[i] == '.') {
    i++;
    frac_from = i;
    while (i < n && bdfi_is_digit(s[i])) {
      i++;
    }
    frac_to = i;
  }
  if (int_to == int_from && frac_to == frac_from) {
    return 0;
  }
  int64_t exponent = 0;
  if (i < n && (s[i] | 0x20) == 'e') {
    i++;
    int minus = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
      minus = s[i] == '-';
      i++;
    }
    if (i >= n || !bdfi_is_digit(s[i])) {
      return 0;
    }
    for (; i < n && bdfi_is_digit(s[i]); i++) {
      exponent = exponent > INT32_MAX ? exponent : exponent * 10 + (s[i] - '0');
    }
    if (minus) {
      exponent = -exponent;
    }
  }
  if (i != n || negative) {
    return 0;
  }
  int digit = 0, digits = 0;
  int64_t place = 0;
  for (size_t k = int_from; k < int_to && digits < 2; k++) {
    if (s[k] != '0') {
      digit = s[k] - '0';
      place = (int64_t)(int_to - 1 - k);
      digits++;
    }
  }
  for (size_t k = frac_from; k < frac_to && digits < 2; k++) {
    if (s[k] != '0') {
      digit = s[k] - '0';
      place = -(int64_t)(k - frac_from) - 1;
      digits++;
    }
  }
  if (digits != 1 || place + exponent != 0) {
    return 0;
  }
  return digit >= 1 && digit <= 6 ? (uint8_t)digit : 0;
}

static int64_t bdfi_tx_top(const bdfi_tx *t) {
  return t->nopen ? (int64_t)t->open[t->nopen - 1] : -1;
}

/* The kind of the innermost open container, or -1. */
static int bdfi_tx_top_kind(const bdfi_tx *t) {
  return t->nopen ? t->text->nodes[t->open[t->nopen - 1]].kind : -1;
}

/* bdfi_tx_add appends a node: its index, or -1 (a limit, or no memory). */
static int64_t bdfi_tx_add(bdfi_tx *t, const bdfi_node *n) {
  bdf_text *out = t->text;
  if (out->nnodes >= BDFI_MAX_TEXT_NODES) {
    t->too_many_nodes = 1;
    bdfi_limits_fail(t->limits, "too many structure nodes");
    return -1;
  }
  if (out->nnodes == out->cap_nodes) {
    size_t cap = out->cap_nodes ? out->cap_nodes * 2 : 32;
    bdfi_node *nodes = bdfi_realloc_array(out->nodes, cap, sizeof *nodes);
    if (!nodes) {
      t->oom = 1;
      return -1;
    }
    out->nodes = nodes;
    out->cap_nodes = cap;
  }
  out->nodes[out->nnodes] = *n;
  return (int64_t)out->nnodes++;
}

/* bdfi_tx_open opens a container. st is the state at a FIGURE mark. */
static void bdfi_tx_open(bdfi_tx *t, const bdfi_node *tmpl, const bdfi_tstate *st) {
  bdfi_node n = *tmpl;
  n.parent = bdfi_tx_top(t);
  if (n.kind == BDF_NODE_FIGURE && st) {
    n.origin.x = st->dm[4];
    n.origin.y = st->dm[5];
  }
  int64_t i = bdfi_tx_add(t, &n);
  if (i < 0) {
    return;
  }
  if (t->nopen == t->cap_open) {
    size_t cap = t->cap_open ? t->cap_open * 2 : 16;
    size_t *open = bdfi_realloc_array(t->open, cap, sizeof *open);
    if (!open) {
      t->oom = 1;
      return;
    }
    t->open = open;
    t->cap_open = cap;
  }
  t->open[t->nopen++] = (size_t)i;
  t->leaf = -1;
  t->has_reserved = 0;
  if (n.kind == BDF_NODE_FIGURE && t->nfigures < BDFI_MAX_OPEN_FIGURES) {
    t->figures[t->nfigures++] = (size_t)i;
  }
}

static void bdfi_tx_close_top(bdfi_tx *t) {
  t->leaf = -1;
  t->has_reserved = 0;
  if (t->nopen == 0) {
    return;
  }
  size_t i = t->open[--t->nopen];
  bdfi_node *n = &t->text->nodes[i];
  if (n->kind != BDF_NODE_FIGURE) {
    return;
  }
  size_t kept = 0;
  for (size_t k = 0; k < t->nfigures; k++) {
    if (t->figures[k] != i) {
      t->figures[kept++] = t->figures[k];
    }
  }
  t->nfigures = kept;
  if (!n->has_bounds) {
    n->bounds = n->origin; /* it drew nothing: no size, where it was opened */
    n->has_bounds = 1;
  }
}

/* bdfi_tx_leaf is the leaf of the next run, made from the kind a MARK
 * reserved, or a paragraph. */
static int64_t bdfi_tx_leaf(bdfi_tx *t) {
  if (t->leaf >= 0 && !t->has_reserved) {
    return t->leaf;
  }
  bdfi_node n;
  memset(&n, 0, sizeof n);
  n.kind = BDF_NODE_PARAGRAPH;
  if (t->has_reserved) {
    n = t->reserved;
  }
  t->has_reserved = 0;
  /* runs right in a list make an item of their own, in a table its caption */
  if (bdfi_tx_top_kind(t) == BDF_NODE_LIST) {
    bdfi_node item;
    memset(&item, 0, sizeof item);
    item.kind = BDF_NODE_ITEM;
    bdfi_tx_open(t, &item, NULL);
  }
  if (bdfi_tx_top_kind(t) == BDF_NODE_TABLE) {
    memset(&n, 0, sizeof n);
    n.kind = BDF_NODE_CAPTION;
  }
  n.parent = bdfi_tx_top(t);
  t->leaf = bdfi_tx_add(t, &n);
  return t->leaf;
}

/* bdfi_tx_structure applies a structure MARK (spec §7.8). */
static void bdfi_tx_structure(bdfi_tx *t, uint8_t kind, const char *payload, size_t len,
                              const bdfi_tstate *st) {
  bdfi_node n;
  memset(&n, 0, sizeof n);
  switch (kind) {
  case BDF_MARK_PARAGRAPH:
  case BDF_MARK_BOX:
    n.kind = BDF_NODE_PARAGRAPH;
    t->reserved = n;
    t->has_reserved = 1;
    break;
  case BDF_MARK_HEADING:
    n.kind = BDF_NODE_HEADING;
    n.level = bdfi_heading_level(payload, len);
    t->reserved = n;
    t->has_reserved = 1;
    break;
  case BDF_MARK_LIST:
    n.kind = BDF_NODE_LIST;
    bdfi_tx_open(t, &n, NULL);
    break;
  case BDF_MARK_LIST_ITEM:
    if (bdfi_tx_top_kind(t) == BDF_NODE_ITEM) {
      bdfi_tx_close_top(t);
    }
    if (bdfi_tx_top_kind(t) == BDF_NODE_LIST) {
      n.kind = BDF_NODE_ITEM;
      bdfi_tx_open(t, &n, NULL);
    } else {
      n.kind = BDF_NODE_PARAGRAPH;
      t->reserved = n;
      t->has_reserved = 1;
    }
    break;
  case BDF_MARK_TABLE:
    n.kind = BDF_NODE_TABLE;
    bdfi_tx_open(t, &n, NULL);
    break;
  case BDF_MARK_CELL:
    n.kind = BDF_NODE_CELL;
    (void)bdfi_cell_ref(payload, len, &n);
    if (bdfi_tx_top_kind(t) == BDF_NODE_CELL) {
      bdfi_tx_close_top(t);
    }
    if (bdfi_tx_top_kind(t) == BDF_NODE_TABLE) {
      bdfi_tx_open(t, &n, NULL);
    } else {
      t->reserved = n;
      t->has_reserved = 1;
    }
    break;
  case BDF_MARK_FIGURE:
    n.kind = BDF_NODE_FIGURE;
    n.alt = payload;
    n.alt_len = len;
    bdfi_tx_open(t, &n, st);
    break;
  case BDF_MARK_END: {
    int k = bdfi_tx_top_kind(t);
    if (k == BDF_NODE_ITEM || k == BDF_NODE_CELL) {
      bdfi_tx_close_top(t);
    }
    if (t->nopen > 0) {
      bdfi_tx_close_top(t);
    }
    break;
  }
  default:
    break;
  }
}

/* bdfi_tx_grow adds a drawn area, in the space of st, to the open figures. */
static void bdfi_tx_grow(bdfi_tx *t, const bdfi_tstate *st, double x, double y, double w,
                         double h) {
  if (t->nfigures == 0) {
    return;
  }
  bdfi_rect b = bdfi_box_of(st->dm, x, y, w, h);
  if (st->has_clip && !bdfi_intersect(&b, &st->clip, &b)) {
    return;
  }
  for (size_t k = 0; k < t->nfigures; k++) {
    bdfi_node *n = &t->text->nodes[t->figures[k]];
    n->bounds = n->has_bounds ? bdfi_union(&n->bounds, &b) : b;
    n->has_bounds = 1;
  }
}

/* bdfi_tx_grow_path adds an inline path drawn at an offset. */
static void bdfi_tx_grow_path(bdfi_tx *t, const bdfi_tstate *st, const bdf_object *o, uint64_t path,
                              double dx, double dy) {
  bdf_path p;
  bdfi_rect b;
  if (t->nfigures == 0 || path >= bdf_object_path_count(o) ||
      bdf_object_path(o, (size_t)path, &p) != BDF_OK || p.external || !bdfi_path_box(&p, &b)) {
    return;
  }
  bdfi_tx_grow(t, st, b.x + dx, b.y + dy, b.w, b.h);
}

/* bdfi_tx_clip narrows the clip of st to a rectangle in its space. */
static void bdfi_tx_clip(bdfi_tstate *st, double x, double y, double w, double h) {
  bdfi_rect b = bdfi_box_of(st->dm, x, y, w, h);
  if (st->has_clip && !bdfi_intersect(&b, &st->clip, &b)) {
    memset(&b, 0, sizeof b); /* nothing is left to draw into */
  }
  st->clip = b;
  st->has_clip = 1;
}

/* bdfi_utf16_len is the length of UTF-8 text in UTF-16 code units as a
 * decoder that replaces what is malformed makes it (String.length). */
static size_t bdfi_utf16_len(const char *s, size_t n) {
  size_t units = 0, len;
  for (size_t i = 0; i < n; i += len) {
    units += bdfi_utf8_next(s, n, i, &len) > 0xFFFF ? 2 : 1;
  }
  return units;
}

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

/* bdfi_tx_emit adds a run. gx is x as the TypeScript reader has it, a
 * double: the same number but for an ALT_TEXT a child object draws, whose x
 * is a sum, which the reference rounds to a float and that reader does not. */
static void bdfi_tx_emit_at(bdfi_tx *t, const char *text, size_t len, float x, float y,
                            float advance, const bdfi_tstate *st, int alt, double gx) {
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
    bdfi_run *runs = bdfi_realloc_array(out->runs, cap, sizeof *runs);
    if (!runs) {
      t->oom = 1;
      return;
    }
    out->runs = runs;
    out->cap_runs = cap;
  }
  run.node = -1;
  if (t->links) {
    run.node = bdfi_tx_leaf(t);
    if (run.node < 0) {
      return;
    }
    run.lang = t->lang;
    run.lang_len = t->lang_len;
    if (t->nfigures > 0) {
      /* the extent of the text: its advance, or half its size a character */
      double size = (double)st->size;
      double w = advance > 0 ? (double)advance : bdfi_muld(size, (double)bdfi_utf16_len(text, len)) * 0.5;
      double anchor = st->align == BDF_ALIGN_RIGHT ? -w : st->align == BDF_ALIGN_CENTER ? -w / 2 : 0;
      bdfi_tx_grow(t, st, gx + anchor, (double)y - bdfi_muld(size, 0.8), w, size);
    }
  }
  out->runs[out->nruns++] = run;
}

static void bdfi_tx_emit(bdfi_tx *t, const char *text, size_t len, float x, float y, float advance,
                         const bdfi_tstate *st, int alt) {
  bdfi_tx_emit_at(t, text, len, x, y, advance, st, alt, (double)x);
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
    bdfi_link *links = bdfi_realloc_array(out->links, cap, sizeof *links);
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
  l->node = t->leaf >= 0 ? t->leaf : bdfi_tx_top(t);
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

/* bdfi_tx_walk walks an object. from holds what it starts with: the
 * transforms and the clip; the font, the size and the alignment start anew. */
static int bdfi_tx_walk(bdfi_tx *t, const bdf_object *o, const bdfi_tstate *from, int reused,
                        bdf_error *err) {
  bdfi_tstate st;
  memset(&st, 0, sizeof st);
  memcpy(st.m, from->m, sizeof st.m);
  memcpy(st.dm, from->dm, sizeof st.dm);
  st.has_clip = from->has_clip;
  st.clip = from->clip;
  st.size = 10;
  bdfi_tstate *stack = NULL;
  size_t nstack = 0, cap_stack = 0, unsaved = 0;
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
      if (nstack >= BDFI_TX_MAX_STATES) {
        unsaved++;
        break;
      }
      if (nstack == cap_stack) {
        size_t cap = cap_stack ? cap_stack * 2 : 8;
        bdfi_tstate *grown = bdfi_realloc_array(stack, cap, sizeof *grown);
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
      if (unsaved > 0) {
        unsaved--;
      } else if (nstack > 0) {
        st = stack[--nstack];
      }
      break;
    case BDF_OP_TRANSFORM:
    case BDF_OP_TRANSLATE:
    case BDF_OP_SCALE: {
      float n[6] = {1, 0, 0, 1, 0, 0};
      if (op == BDF_OP_TRANSFORM) {
        for (size_t i = 0; i < 6; i++) {
          n[i] = bdf_instr_f32(&it, i);
        }
      } else if (op == BDF_OP_TRANSLATE) {
        n[4] = bdf_instr_f32(&it, 0);
        n[5] = bdf_instr_f32(&it, 1);
      } else {
        n[0] = bdf_instr_f32(&it, 0);
        n[3] = bdf_instr_f32(&it, 1);
      }
      bdfi_mat_mul(st.m, n, st.m);
      if (t->links) {
        const double dn[6] = {n[0], n[1], n[2], n[3], n[4], n[5]};
        bdfi_dmat_mul(st.dm, dn, st.dm);
      }
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
        if (t->links) {
          size_t len;
          const char *payload = bdf_instr_string(&it, 1, &len);
          bdfi_tx_structure(t, kind, payload, len, &st);
        }
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
      case BDF_MARK_LANG:
        /* no separator, and a waiting ALT_TEXT still belongs to the next
         * drawing; the language holds for the runs from here on */
        t->lang = bdf_instr_string(&it, 1, &t->lang_len);
        break;
      default:
        /* unknown kinds leave the separator to the next MARK or to the guess
         * from the positions */
        break;
      }
      break;
    }
    /* what is drawn, for the figures that are open, and the clip */
    case BDF_OP_CLIP_RECT:
      if (t->links) {
        bdfi_tx_clip(&st, bdf_instr_f32(&it, 0), bdf_instr_f32(&it, 1), bdf_instr_f32(&it, 2),
                     bdf_instr_f32(&it, 3));
      }
      break;
    case BDF_OP_CLIP_PATH:
      if (t->links) {
        bdf_path p;
        bdfi_rect b;
        uint64_t i = bdf_instr_u64(&it, 0);
        if (i < bdf_object_path_count(o) && bdf_object_path(o, (size_t)i, &p) == BDF_OK &&
            !p.external && bdfi_path_box(&p, &b)) {
          bdfi_tx_clip(&st, b.x, b.y, b.w, b.h);
        }
      }
      break;
    case BDF_OP_FILL_RECT:
    case BDF_OP_STROKE_RECT:
      bdfi_tx_grow(t, &st, bdf_instr_f32(&it, 0), bdf_instr_f32(&it, 1), bdf_instr_f32(&it, 2),
                   bdf_instr_f32(&it, 3));
      break;
    case BDF_OP_FILL_PATH:
    case BDF_OP_STROKE_PATH:
      bdfi_tx_grow_path(t, &st, o, bdf_instr_u64(&it, 0), 0, 0);
      break;
    case BDF_OP_IMAGE:
      bdfi_tx_grow(t, &st, bdf_instr_f32(&it, 1), bdf_instr_f32(&it, 2), bdf_instr_f32(&it, 3),
                   bdf_instr_f32(&it, 4));
      break;
    case BDF_OP_IMAGE_SUB:
      bdfi_tx_grow(t, &st, bdf_instr_f32(&it, 5), bdf_instr_f32(&it, 6), bdf_instr_f32(&it, 7),
                   bdf_instr_f32(&it, 8));
      break;
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
      bdfi_tx_grow_path(t, &st, o, bdf_instr_u64(&it, 0), bdf_instr_f32(&it, 2),
                        bdf_instr_f32(&it, 3));
      if (t->has_alt) {
        t->has_alt = 0;
        bdfi_tx_emit(t, t->alt, t->alt_len, bdf_instr_f32(&it, 2), bdf_instr_f32(&it, 3), 0, &st, 1);
      }
      break;
    case BDF_OP_FILL_PATH_RUN: {
      size_t n = 0;
      const bdf_glyph *g = NULL;
      if (t->has_alt || t->nfigures > 0) {
        g = bdf_instr_glyphs(&it, 1, &n);
      }
      for (size_t k = 0; t->nfigures > 0 && k < n; k++) {
        bdfi_tx_grow_path(t, &st, o, g[k].path, g[k].x, g[k].y);
      }
      if (t->has_alt) {
        t->has_alt = 0;
        if (n > 0) {
          bdfi_tx_emit(t, t->alt, t->alt_len, g[0].x, g[0].y, 0, &st, 1);
        } else {
          bdfi_tx_emit(t, t->alt, t->alt_len, 0, 0, 0, &st, 1);
        }
      }
      break;
    }
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
        /* the text the child draws spans its bbox along the baseline, and
         * what it draws still counts for a figure */
        float advance = 0;
        double gx = x;
        if (child) {
          float bbox[4];
          bdf_object_bbox(child, bbox);
          bdfi_tx_grow(t, &st, (double)bbox[0] + (double)x, (double)bbox[1] + (double)y, bbox[2],
                       bbox[3]);
          gx = (double)x + (double)bbox[0];
          x = x + bbox[0];
          advance = bbox[2];
        }
        t->has_alt = 0;
        bdfi_tx_emit_at(t, t->alt, t->alt_len, x, y, advance, &st, 1, gx);
        break;
      }
      if (!child) {
        break;
      }
      if (t->depth >= BDF_MAX_USE_DEPTH) {
        bdfi_limits_fail(t->limits, "objects draw objects too deep");
        break;
      }
      bdfi_tstate cs = st;
      if (op == BDF_OP_USE_AT) {
        const float n[6] = {1, 0, 0, 1, x, y};
        bdfi_mat_mul(cs.m, n, cs.m);
      }
      if (t->links) {
        const double dn[6] = {1, 0, 0, 1, x, y};
        bdfi_dmat_mul(cs.dm, dn, cs.dm);
      }
      t->depth++;
      /* a child whose stream is malformed keeps the runs before the damage,
       * and the walk goes on after it, as in the reference */
      (void)bdfi_tx_walk(t, child, &cs, reused || again, NULL);
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
  t.leaf = -1;
  bdfi_tstate st;
  memset(&st, 0, sizeof st);
  st.m[0] = st.m[3] = 1;
  st.dm[0] = st.dm[3] = 1;
  st.size = 10;
  bdf_error werr;
  bdfi_error_clear(&werr);
  int rc = bdfi_tx_walk(&t, obj, &st, 0, &werr);
  bdf_status status = BDF_OK;
  if (t.oom) {
    status = BDF_ERR_NOMEM;
  } else if (limits->err) {
    status = BDF_ERR_FORMAT;
  } else if (rc == BDFI_WALK_FORMAT) {
    free(t.open);
    bdf_text_free(text);
    if (err) {
      *err = werr;
    }
    return BDF_ERR_FORMAT;
  }
  if (status == BDF_OK) {
    /* An ALT_TEXT nothing took is the last run. The reference does not report
     * the limit of runs when this one passes it; the limit holds from then
     * on. What is still open is closed at the end of the object. */
    bdfi_tx_flush_alt(&t, &st);
    while (t.nopen > 0) {
      bdfi_tx_close_top(&t);
    }
    if (t.oom) {
      status = BDF_ERR_NOMEM;
    } else if (t.too_many_nodes) {
      status = BDF_ERR_FORMAT; /* the nodes, not the runs, are too many */
    }
  }
  free(t.open);
  if (status != BDF_OK) {
    bdf_text_free(text);
    bdfi_error_set(err, status, 0, status == BDF_ERR_NOMEM ? "out of memory" : limits->err);
    return status;
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
