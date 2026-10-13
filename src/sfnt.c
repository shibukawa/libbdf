/*
 * Font programs for the rasterizer: what the reference reads of an sfnt
 * font to draw text with it (internal/sfnt of bdf: sfnt.go, outline.go and
 * the CFF half of bounds.go), ported so that a font gives the same glyphs,
 * advances and outlines here. A font is read as leniently as there: a
 * table that is cut short reads as zeroes past its end, and what cannot be
 * read is a glyph without an outline rather than an error.
 *
 * Where the reference indexes past a table (an INDEX of a CFF font whose
 * last offset is cut off) it panics, and the image being drawn fails with
 * it; here such a font has no outlines, and the rest of the page is drawn.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* ---- Reading ------------------------------------------------------------- */

/* A table, or any stretch of the font: there is none when p is NULL (a
 * table of no bytes that is there has a p). */
typedef struct bdfi_span {
  const uint8_t *p;
  size_t n;
} bdfi_span;

/* be16 and be32 of the reference: 0 past the end. */
static uint32_t bdfi_sf16(bdfi_span b, int64_t i) {
  if (i < 0 || (uint64_t)i + 1 >= b.n) {
    return 0;
  }
  return (uint32_t)b.p[i] << 8 | b.p[i + 1];
}

static uint32_t bdfi_sf32(bdfi_span b, int64_t i) {
  if (i < 0 || (uint64_t)i + 3 >= b.n) {
    return 0;
  }
  return (uint32_t)b.p[i] << 24 | (uint32_t)b.p[i + 1] << 16 | (uint32_t)b.p[i + 2] << 8 |
         b.p[i + 3];
}

static int32_t bdfi_sf_i16(bdfi_span b, int64_t i) { return (int16_t)bdfi_sf16(b, i); }

static bdfi_span bdfi_span_from(bdfi_span b, size_t off) {
  bdfi_span s = {b.p + off, b.n - off};
  return s;
}

/* ---- The character map --------------------------------------------------- */


typedef struct bdfi_cmap_pair {
  uint32_t c, g; /* a character and its glyph */
} bdfi_cmap_pair;

/* What reads a subtable: it counts the characters the subtables of a font
 * map together, and collects the mappings of one of them when asked to. */
typedef struct bdfi_cmap_read {
  int64_t left; /* maxCmap: below zero, the font maps too many */
  bdfi_cmap_pair *pairs;
  size_t n, cap;
  int collect, oom;
} bdfi_cmap_read;

#define BDFI_MAX_CMAP ((int64_t)4 * 0x110000)

static void bdfi_cmap_put(bdfi_cmap_read *r, uint32_t c, uint32_t g) {
  if (!r->collect || r->oom) {
    return;
  }
  if (r->n == r->cap) {
    size_t cap = r->cap ? 2 * r->cap : 256;
    bdfi_cmap_pair *p = bdfi_realloc_array(r->pairs, cap, sizeof *p);
    if (!p) {
      r->oom = 1;
      return;
    }
    r->pairs = p;
    r->cap = cap;
  }
  r->pairs[r->n].c = c;
  r->pairs[r->n].g = g;
  r->n++;
}

/* parseCmapSubtable: 0 for a format that is not read (and for a subtable
 * that maps more characters than are left to map). */
static int bdfi_cmap_subtable(bdfi_span b, bdfi_cmap_read *r) {
  switch (bdfi_sf16(b, 0)) {
  case 0:
    r->left -= 256;
    for (size_t c = 0; c < 256 && 6 + c < b.n; c++) {
      if (b.p[6 + c] != 0) {
        bdfi_cmap_put(r, (uint32_t)c, b.p[6 + c]);
      }
    }
    return 1;
  case 4: {
    int64_t seg_x2 = bdfi_sf16(b, 6);
    int64_t ends = 14, starts = 16 + seg_x2, deltas = 16 + 2 * seg_x2, range_offs = 16 + 3 * seg_x2;
    for (int64_t s = 0; s < seg_x2 / 2; s++) {
      uint32_t end = bdfi_sf16(b, ends + s * 2), start = bdfi_sf16(b, starts + s * 2);
      uint32_t delta = bdfi_sf16(b, deltas + s * 2);
      int64_t ro = bdfi_sf16(b, range_offs + s * 2);
      if (start > end) {
        continue;
      }
      /* segments do not overlap in a font that is well made: those that do
       * count as many times as they map a character */
      r->left -= (int64_t)(end - start) + 1;
      if (r->left < 0) {
        return 0;
      }
      for (uint32_t c = start; c <= end && c != 0xffff; c++) {
        uint32_t g;
        if (ro == 0) {
          g = (c + delta) & 0xffff;
        } else {
          int64_t addr = range_offs + s * 2 + ro + (int64_t)(c - start) * 2;
          if ((uint64_t)addr + 1 >= b.n) {
            continue;
          }
          g = bdfi_sf16(b, addr);
          if (g != 0) {
            g = (g + delta) & 0xffff;
          }
        }
        if (g != 0) {
          bdfi_cmap_put(r, c, g);
        }
      }
    }
    return 1;
  }
  case 6: {
    uint32_t first = bdfi_sf16(b, 6), count = bdfi_sf16(b, 8);
    r->left -= count;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t g = bdfi_sf16(b, 10 + (int64_t)i * 2);
      if (g != 0) {
        bdfi_cmap_put(r, first + i, g);
      }
    }
    return 1;
  }
  case 12: {
    /* at most as many mappings as there are code points: groups that
     * overlap or reach past Unicode must not make the loop run long */
    int64_t budget = 0x110000;
    int64_t ngroups = bdfi_sf32(b, 12);
    for (int64_t i = 0; i < ngroups && (uint64_t)(16 + i * 12 + 12) <= b.n && budget > 0; i++) {
      int64_t rec = 16 + i * 12;
      uint32_t start = bdfi_sf32(b, rec), end = bdfi_sf32(b, rec + 4), gid = bdfi_sf32(b, rec + 8);
      if (start > 0x10ffff || end < start) {
        continue;
      }
      if (end > 0x10ffff) {
        end = 0x10ffff;
      }
      if (end > start + 0xffff) {
        end = start + 0xffff;
      }
      for (uint32_t c = start; c <= end && budget > 0; c++) {
        bdfi_cmap_put(r, c, (gid + (c - start)) & 0xffff); /* glyph 0 too: it hides what was mapped */
        budget--;
      }
    }
    r->left -= 0x110000 - budget;
    return 1;
  }
  default:
    return 0;
  }
}

static int bdfi_cmap_by_offset(const void *a, const void *b) {
  const uint32_t *x = a, *y = b;
  return x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : x[1] < y[1] ? -1 : x[1] > y[1];
}

/*
 * parseCmap: the best Unicode subtable of the font ((3,10), then (3,1),
 * then platform 0), as pairs in the order of their characters. Every
 * subtable is read, as the reference reads them, for the bound on what they
 * map together: BDF_ERR_FORMAT when they map too much. *found says whether
 * the font has such a subtable at all.
 */
static bdf_status bdfi_cmap_parse(bdfi_span cm, bdfi_cmap_pair **out, size_t *nout, int *found) {
  *out = NULL;
  *nout = 0;
  *found = 0;
  if (cm.n < 4) {
    return BDF_OK;
  }
  size_t n = bdfi_sf16(cm, 2);
  if (n == 0) {
    return BDF_OK;
  }
  /* the records of several platforms share subtables: each is read once,
   * where the first record that names it stands */
  uint32_t (*order)[2] = bdfi_malloc_array(n, sizeof *order);
  uint32_t *first = bdfi_malloc_array(n, sizeof *first);
  uint8_t *state = bdfi_calloc_array(n, 1); /* of the record that reads: 2 when it is read */
  if (!order || !first || !state) {
    free(order);
    free(first);
    free(state);
    return BDF_ERR_NOMEM;
  }
  for (size_t i = 0; i < n; i++) {
    order[i][0] = bdfi_sf32(cm, 4 + (int64_t)i * 8 + 4);
    order[i][1] = (uint32_t)i;
  }
  qsort(order, n, sizeof *order, bdfi_cmap_by_offset);
  for (size_t k = 0; k < n; k++) {
    first[order[k][1]] =
        k > 0 && order[k][0] == order[k - 1][0] ? first[order[k - 1][1]] : order[k][1];
  }
  free(order);
  bdfi_cmap_read r;
  memset(&r, 0, sizeof r);
  r.left = BDFI_MAX_CMAP;
  int best = -1;
  size_t chosen = n;
  bdf_status st = BDF_OK;
  for (size_t i = 0; i < n; i++) {
    int64_t rec = 4 + (int64_t)i * 8;
    uint32_t pid = bdfi_sf16(cm, rec), eid = bdfi_sf16(cm, rec + 2), off = bdfi_sf32(cm, rec + 4);
    if (off >= cm.n) {
      continue;
    }
    if (first[i] == i) {
      state[i] = bdfi_cmap_subtable(bdfi_span_from(cm, off), &r) ? 2 : 1;
      if (r.left < 0) {
        st = BDF_ERR_FORMAT; /* the cmap table maps too many characters */
        break;
      }
    }
    if (state[first[i]] != 2) {
      continue;
    }
    if ((pid == 3 && (eid == 1 || eid == 10)) || pid == 0) {
      int score = pid == 3 && eid == 10 ? 3 : pid == 3 ? 2 : 1;
      if (score > best) {
        best = score;
        chosen = i;
      }
    }
  }
  free(first);
  free(state);
  if (st != BDF_OK || chosen == n) {
    return st;
  }
  /* the mappings of the one that is used, in the order they are made */
  r.collect = 1;
  r.left = BDFI_MAX_CMAP;
  bdfi_cmap_subtable(bdfi_span_from(cm, bdfi_sf32(cm, 4 + (int64_t)chosen * 8 + 4)), &r);
  bdfi_cmap_pair *a = r.pairs, *b = bdfi_malloc_array(r.n ? r.n : 1, sizeof *b);
  if (r.oom || !b) {
    free(a);
    free(b);
    return BDF_ERR_NOMEM;
  }
  /* by character, with a sort that keeps the order of the mappings of one
   * character: the last of them is the one a map would hold */
  for (size_t width = 1; width < r.n; width *= 2) {
    for (size_t lo = 0; lo < r.n; lo += 2 * width) {
      size_t mid = lo + width < r.n ? lo + width : r.n;
      size_t hi = lo + 2 * width < r.n ? lo + 2 * width : r.n;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        b[k++] = a[j].c < a[i].c ? a[j++] : a[i++];
      }
      while (i < mid) {
        b[k++] = a[i++];
      }
      while (j < hi) {
        b[k++] = a[j++];
      }
    }
    bdfi_cmap_pair *t = a;
    a = b;
    b = t;
  }
  size_t w = 0;
  for (size_t i = 0; i < r.n; i++) {
    if (i + 1 == r.n || a[i + 1].c != a[i].c) {
      b[w++] = a[i];
    }
  }
  free(a);
  *out = b;
  *nout = w;
  *found = 1;
  return BDF_OK;
}

/* ---- CFF outlines -------------------------------------------------------- */

/* An INDEX: count items, and where item i lies in the font (item i is
 * b[at[i]..at[i+1])). */
typedef struct bdfi_cff_index {
  size_t count;
  uint32_t *at; /* count + 1 places, when count > 0 */
} bdfi_cff_index;

/* cffGlyphs: what running the charstrings of a CFF font needs. */
typedef struct bdfi_cff {
  bdfi_span b; /* the CFF table */
  bdfi_cff_index char_strings, global;
  bdfi_cff_index *local; /* the Subrs of each Private DICT */
  size_t nlocal;
  uint8_t *fd_select; /* the Private DICT of each glyph (CID-keyed fonts), or NULL */
} bdfi_cff;

static void bdfi_cff_free(bdfi_cff *c) {
  if (!c) {
    return;
  }
  free(c->char_strings.at);
  free(c->global.at);
  for (size_t i = 0; i < c->nlocal; i++) {
    free(c->local[i].at);
  }
  free(c->local);
  free(c->fd_select);
  free(c);
}

/*
 * cffIndex: the INDEX at pos and the position after it. 0 for one that
 * cannot be read (-1 when memory ran out).
 */
static int bdfi_cff_read_index(bdfi_span b, int64_t pos, bdfi_cff_index *out, int64_t *after) {
  out->count = 0;
  out->at = NULL;
  if (pos < 0 || (uint64_t)pos + 2 > b.n) {
    return 0;
  }
  size_t count = bdfi_sf16(b, pos);
  if (count == 0) {
    *after = pos + 2;
    return 1;
  }
  if ((uint64_t)pos + 3 > b.n) {
    return 0;
  }
  int off_size = b.p[pos + 2];
  if (off_size < 1 || off_size > 4) {
    return 0;
  }
  int64_t offs = pos + 3;
  int64_t data = offs + (int64_t)(count + 1) * off_size - 1;
  /* the reference lets data be the length of the table, and then reads the
   * last byte of the last offset past its end: a panic, here no INDEX */
  if ((uint64_t)data >= b.n) {
    return 0;
  }
  uint32_t *at = bdfi_malloc_array(count + 1, sizeof *at);
  if (!at) {
    return -1;
  }
  for (size_t i = 0; i <= count; i++) {
    uint64_t v = 0;
    for (int k = 0; k < off_size; k++) {
      v = v << 8 | b.p[offs + (int64_t)i * off_size + k];
    }
    v += (uint64_t)data;
    if (v > b.n || (i > 0 && v < at[i - 1])) {
      free(at);
      return 0;
    }
    at[i] = (uint32_t)v;
  }
  out->count = count;
  out->at = at;
  *after = at[count];
  return 1;
}

static bdfi_span bdfi_cff_item(const bdfi_cff *c, const bdfi_cff_index *ix, size_t i) {
  bdfi_span s = {c->b.p + ix->at[i], ix->at[i + 1] - ix->at[i]};
  return s;
}

/*
 * cffDict, for one operator: the operands of its last use in a DICT (12 x
 * is 1200+x). It returns how many there are, or -1 when the DICT does not
 * have it; the first of them come out in v.
 */
static int bdfi_cff_dict(bdfi_span b, int want, double v[2]) {
  double ops[2] = {0, 0};
  int nops = 0, found = -1;
  for (size_t i = 0; i < b.n;) {
    int c = b.p[i];
    double num = 0;
    int is_num = 1;
    if (c <= 21) {
      int op = c;
      i++;
      if (c == 12 && i < b.n) {
        op = 1200 + b.p[i];
        i++;
      }
      if (op == want) {
        found = nops;
        v[0] = ops[0];
        v[1] = ops[1];
      }
      nops = 0;
      continue;
    } else if (c == 28 && i + 2 < b.n) {
      num = (double)bdfi_sf_i16(b, (int64_t)i + 1);
      i += 3;
    } else if (c == 29 && i + 4 < b.n) {
      num = (double)(int32_t)bdfi_sf32(b, (int64_t)i + 1);
      i += 5;
    } else if (c == 30) {
      /* a real number: its nibbles are passed over (no offset is one) */
      i++;
      while (i < b.n) {
        uint8_t nib = b.p[i];
        i++;
        if ((nib & 0x0f) == 0x0f || nib >> 4 == 0x0f) {
          break;
        }
      }
    } else if (c >= 32 && c <= 246) {
      num = (double)(c - 139);
      i++;
    } else if (c >= 247 && c <= 250 && i + 1 < b.n) {
      num = (double)((c - 247) * 256 + b.p[i + 1] + 108);
      i += 2;
    } else if (c >= 251 && c <= 254 && i + 1 < b.n) {
      num = (double)(-(c - 251) * 256 - b.p[i + 1] - 108);
      i += 2;
    } else {
      is_num = 0;
      i++;
    }
    if (is_num) {
      if (nops < 2) {
        ops[nops] = num;
      }
      if (nops < 3) {
        nops++; /* three says "more than two" */
      }
    }
  }
  return found;
}

/* The Subrs of a Private DICT that a Top or Font DICT names with its
 * operator 18 (size and offset), if it has one that can be read. */
static int bdfi_cff_private(bdfi_span b, bdfi_span dict, bdfi_cff_index *out) {
  double v[2], s[2];
  out->count = 0;
  out->at = NULL;
  if (bdfi_cff_dict(dict, 18, v) != 2) {
    return 1;
  }
  int64_t size = (int64_t)v[0], off = (int64_t)v[1];
  if (off < 0 || size < 0 || (uint64_t)(off + size) > b.n) {
    return 1;
  }
  bdfi_span priv = {b.p + off, (size_t)size};
  if (bdfi_cff_dict(priv, 19, s) == 1) {
    int64_t after;
    if (bdfi_cff_read_index(b, off + (int64_t)s[0], out, &after) < 0) {
      return -1;
    }
  }
  return 1;
}

/* parseCFFGlyphs: NULL for a table that cannot be read. */
static bdfi_cff *bdfi_cff_parse(bdfi_span b, int *oom) {
  if (b.n < 4) {
    return NULL;
  }
  bdfi_cff *c = calloc(1, sizeof *c);
  if (!c) {
    *oom = 1;
    return NULL;
  }
  c->b = b;
  bdfi_cff_index names, tops, strings, fds;
  memset(&tops, 0, sizeof tops);
  memset(&fds, 0, sizeof fds);
  int64_t pos;
  int ok = bdfi_cff_read_index(b, b.p[2], &names, &pos);
  free(names.at);
  if (ok == 1) {
    ok = bdfi_cff_read_index(b, pos, &tops, &pos);
    if (ok == 1 && tops.count == 0) {
      ok = 0;
    }
  }
  if (ok == 1) {
    ok = bdfi_cff_read_index(b, pos, &strings, &pos);
    free(strings.at);
  }
  if (ok == 1) {
    ok = bdfi_cff_read_index(b, pos, &c->global, &pos);
  }
  double v[2], fda[2], fdsel[2];
  bdfi_span top = {NULL, 0};
  if (ok == 1) {
    top = bdfi_cff_item(c, &tops, 0);
    if (bdfi_cff_dict(top, 17, v) == 1) {
      ok = bdfi_cff_read_index(b, (int64_t)v[0], &c->char_strings, &pos);
    }
    if (ok == 1 && c->char_strings.count == 0) {
      ok = 0;
    }
  }
  if (ok == 1) {
    if (bdfi_cff_dict(top, 1236, fda) == 1 && bdfi_cff_dict(top, 1237, fdsel) == 1) {
      ok = bdfi_cff_read_index(b, (int64_t)fda[0], &fds, &pos);
      if (ok == 1 && fds.count == 0) {
        ok = 0;
      }
      if (ok == 1) {
        c->local = bdfi_calloc_array(fds.count, sizeof *c->local);
        if (!c->local) {
          ok = -1;
        }
      }
      for (size_t i = 0; ok == 1 && i < fds.count; i++) {
        ok = bdfi_cff_private(b, bdfi_cff_item(c, &fds, i), &c->local[i]);
        c->nlocal = i + 1;
      }
      if (ok == 1) {
        /* cffFDSelect: the Font DICT of every glyph (formats 0 and 3) */
        size_t n = c->char_strings.count;
        int64_t at = (int64_t)fdsel[0];
        c->fd_select = bdfi_calloc_array(n, 1);
        if (!c->fd_select) {
          ok = -1;
        } else if (at >= 0 && (uint64_t)at < b.n) {
          if (b.p[at] == 0) {
            if ((uint64_t)at + 1 + n <= b.n) {
              memcpy(c->fd_select, b.p + at + 1, n);
            }
          } else if (b.p[at] == 3) {
            size_t nr = bdfi_sf16(b, at + 1);
            for (size_t i = 0; i < nr; i++) {
              int64_t r = at + 3 + (int64_t)i * 3;
              if ((uint64_t)r + 5 > b.n) {
                break;
              }
              size_t from = bdfi_sf16(b, r), next = bdfi_sf16(b, r + 3);
              for (size_t g = from; g < next && g < n; g++) {
                c->fd_select[g] = b.p[r + 2];
              }
            }
          }
        }
      }
    } else {
      c->local = bdfi_calloc_array(1, sizeof *c->local);
      if (!c->local) {
        ok = -1;
      } else {
        c->nlocal = 1;
        ok = bdfi_cff_private(b, top, &c->local[0]);
      }
    }
  }
  free(tops.at);
  free(fds.at);
  if (ok != 1) {
    if (ok < 0) {
      *oom = 1;
    }
    bdfi_cff_free(c);
    return NULL;
  }
  return c;
}

static int bdfi_cff_bias(size_t n) { return n < 1240 ? 107 : n < 33900 ? 1131 : 32768; }

/* t2Run: a Type 2 charstring being run, which draws into a path. */
typedef struct bdfi_t2 {
  const bdfi_cff *g;
  const bdfi_cff_index *local;
  bdfi_rpath *path;
  double *stack;
  size_t n, cap;
  double x, y;
  double sx, sy; /* the start of the contour, drawn once something follows it */
  int open, width_seen, done, call_depth, oom;
  int64_t stems, operations;
} bdfi_t2;

static void bdfi_t2_push(bdfi_t2 *t, double v) {
  if (t->n == t->cap) {
    size_t cap = t->cap ? 2 * t->cap : 64;
    double *s = bdfi_realloc_array(t->stack, cap, sizeof *s);
    if (!s) {
      t->oom = 1;
      t->done = 1;
      return;
    }
    t->stack = s;
    t->cap = cap;
  }
  t->stack[t->n++] = v;
}

static void bdfi_t2_close(bdfi_t2 *t) {
  if (t->open) {
    bdfi_rpath_close(t->path);
    t->open = 0;
  }
}

static void bdfi_t2_move(bdfi_t2 *t, double dx, double dy) {
  bdfi_t2_close(t);
  t->x += dx;
  t->y += dy;
  t->sx = t->x;
  t->sy = t->y;
}

static void bdfi_t2_start(bdfi_t2 *t) {
  if (!t->open) {
    bdfi_rpath_move_to(t->path, t->sx, t->sy);
    t->open = 1;
  }
}

static void bdfi_t2_line(bdfi_t2 *t, double dx, double dy) {
  bdfi_t2_start(t);
  t->x += dx;
  t->y += dy;
  bdfi_rpath_line_to(t->path, t->x, t->y);
}

/* A cubic curve from the current point with relative control points. */
static void bdfi_t2_curve(bdfi_t2 *t, double dx1, double dy1, double dx2, double dy2, double dx3,
                          double dy3) {
  bdfi_t2_start(t);
  double x1 = t->x + dx1, y1 = t->y + dy1;
  double x2 = x1 + dx2, y2 = y1 + dy2;
  double x3 = x2 + dx3, y3 = y2 + dy3;
  bdfi_rpath_cubic_to(t->path, x1, y1, x2, y2, x3, y3);
  t->x = x3;
  t->y = y3;
}

/* takeWidth: the advance width that the first stack-clearing operator of a
 * charstring may carry is dropped. It is there when the operator has one
 * operand more than it takes (even: it takes pairs). */
static void bdfi_t2_width(bdfi_t2 *t, size_t want, int even) {
  if (t->width_seen) {
    return;
  }
  t->width_seen = 1;
  if (even ? t->n % 2 == 1 : t->n > want) {
    memmove(t->stack, t->stack + 1, (t->n - 1) * sizeof *t->stack);
    t->n--;
  }
}

/* escape: the two-byte operators. The flexes draw curves; the others
 * (arithmetic, storage) are not used by the fonts this reads. */
static void bdfi_t2_escape(bdfi_t2 *t, int op) {
  const double *s = t->stack;
  size_t n = t->n;
  switch (op) {
  case 35: /* flex */
    if (n >= 12) {
      bdfi_t2_curve(t, s[0], s[1], s[2], s[3], s[4], s[5]);
      bdfi_t2_curve(t, s[6], s[7], s[8], s[9], s[10], s[11]);
    }
    break;
  case 34: /* hflex */
    if (n >= 7) {
      bdfi_t2_curve(t, s[0], 0, s[1], s[2], s[3], 0);
      bdfi_t2_curve(t, s[4], 0, s[5], -s[2], s[6], 0);
    }
    break;
  case 36: /* hflex1 */
    if (n >= 9) {
      bdfi_t2_curve(t, s[0], s[1], s[2], s[3], s[4], 0);
      bdfi_t2_curve(t, s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
    }
    break;
  case 37: /* flex1 */
    if (n >= 11) {
      double dx = s[0] + s[2] + s[4] + s[6] + s[8];
      double dy = s[1] + s[3] + s[5] + s[7] + s[9];
      double x6 = s[10], y6 = s[10];
      if (fabs(dx) > fabs(dy)) {
        y6 = -dy;
      } else {
        x6 = -dx;
      }
      bdfi_t2_curve(t, s[0], s[1], s[2], s[3], s[4], s[5]);
      bdfi_t2_curve(t, s[6], s[7], s[8], s[9], x6, y6);
    }
    break;
  default:
    break;
  }
}

static void bdfi_t2_run(bdfi_t2 *t, bdfi_span cs) {
  if (t->call_depth > 10) {
    t->done = 1;
    return;
  }
  for (size_t i = 0; i < cs.n && !t->done;) {
    t->operations++;
    if (t->operations > 100000) {
      t->done = 1;
      return;
    }
    int v = cs.p[i];
    /* numbers: one that is cut off is passed over without a value */
    if (v == 28) {
      if (i + 2 < cs.n) {
        bdfi_t2_push(t, (double)bdfi_sf_i16(cs, (int64_t)i + 1));
      }
      i += 3;
      continue;
    }
    if (v >= 32 && v <= 246) {
      bdfi_t2_push(t, (double)(v - 139));
      i++;
      continue;
    }
    if (v >= 247 && v <= 250) {
      if (i + 1 < cs.n) {
        bdfi_t2_push(t, (double)((v - 247) * 256 + cs.p[i + 1] + 108));
      }
      i += 2;
      continue;
    }
    if (v >= 251 && v <= 254) {
      if (i + 1 < cs.n) {
        bdfi_t2_push(t, (double)(-(v - 251) * 256 - cs.p[i + 1] - 108));
      }
      i += 2;
      continue;
    }
    if (v == 255) {
      if (i + 4 < cs.n) {
        bdfi_t2_push(t, (double)(int32_t)bdfi_sf32(cs, (int64_t)i + 1) / 65536);
      }
      i += 5;
      continue;
    }
    i++;
    const double *s = t->stack;
    size_t n = t->n;
    switch (v) {
    case 1:
    case 3:
    case 18:
    case 23: /* hstem, vstem, hstemhm, vstemhm */
      bdfi_t2_width(t, 0, 1);
      t->stems += (int64_t)(t->n / 2);
      break;
    case 19:
    case 20: /* hintmask, cntrmask */
      bdfi_t2_width(t, 0, 1);
      t->stems += (int64_t)(t->n / 2);
      i += (size_t)((t->stems + 7) / 8);
      break;
    case 21: /* rmoveto */
      bdfi_t2_width(t, 2, 0);
      if (t->n >= 2) {
        bdfi_t2_move(t, t->stack[t->n - 2], t->stack[t->n - 1]);
      }
      break;
    case 22: /* hmoveto */
      bdfi_t2_width(t, 1, 0);
      if (t->n >= 1) {
        bdfi_t2_move(t, t->stack[t->n - 1], 0);
      }
      break;
    case 4: /* vmoveto */
      bdfi_t2_width(t, 1, 0);
      if (t->n >= 1) {
        bdfi_t2_move(t, 0, t->stack[t->n - 1]);
      }
      break;
    case 5: /* rlineto */
      for (size_t k = 0; k + 1 < n; k += 2) {
        bdfi_t2_line(t, s[k], s[k + 1]);
      }
      break;
    case 6:
    case 7: { /* hlineto, vlineto: in turns */
      int horiz = v == 6;
      for (size_t k = 0; k < n; k++) {
        if (horiz) {
          bdfi_t2_line(t, s[k], 0);
        } else {
          bdfi_t2_line(t, 0, s[k]);
        }
        horiz = !horiz;
      }
      break;
    }
    case 8: /* rrcurveto */
      for (size_t k = 0; k + 5 < n; k += 6) {
        bdfi_t2_curve(t, s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5]);
      }
      break;
    case 24: { /* rcurveline */
      size_t k = 0;
      for (; k + 7 < n; k += 6) {
        bdfi_t2_curve(t, s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5]);
      }
      if (k + 1 < n) {
        bdfi_t2_line(t, s[k], s[k + 1]);
      }
      break;
    }
    case 25: { /* rlinecurve */
      size_t k = 0;
      for (; k + 7 < n; k += 2) {
        bdfi_t2_line(t, s[k], s[k + 1]);
      }
      if (k + 5 < n) {
        bdfi_t2_curve(t, s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5]);
      }
      break;
    }
    case 26: { /* vvcurveto */
      size_t k = 0;
      double dx1 = 0;
      if (n % 4 == 1) {
        dx1 = s[0];
        k = 1;
      }
      for (; k + 3 < n; k += 4) {
        bdfi_t2_curve(t, dx1, s[k], s[k + 1], s[k + 2], 0, s[k + 3]);
        dx1 = 0;
      }
      break;
    }
    case 27: { /* hhcurveto */
      size_t k = 0;
      double dy1 = 0;
      if (n % 4 == 1) {
        dy1 = s[0];
        k = 1;
      }
      for (; k + 3 < n; k += 4) {
        bdfi_t2_curve(t, s[k], dy1, s[k + 1], s[k + 2], s[k + 3], 0);
        dy1 = 0;
      }
      break;
    }
    case 30:
    case 31: { /* vhcurveto, hvcurveto */
      int horiz = v == 31;
      for (size_t k = 0; k + 3 < n; k += 4) {
        double last = k + 5 == n ? s[k + 4] : 0;
        if (horiz) {
          bdfi_t2_curve(t, s[k], 0, s[k + 1], s[k + 2], last, s[k + 3]);
        } else {
          bdfi_t2_curve(t, 0, s[k], s[k + 1], s[k + 2], s[k + 3], last);
        }
        horiz = !horiz;
      }
      break;
    }
    case 10:
    case 29: { /* callsubr, callgsubr */
      if (n == 0) {
        t->done = 1;
        return;
      }
      const bdfi_cff_index *subrs = v == 29 ? &t->g->global : t->local;
      int64_t at = bdfi_to_int(s[n - 1]) + bdfi_cff_bias(subrs->count);
      t->n--;
      if (at < 0 || (uint64_t)at >= subrs->count) {
        t->done = 1;
        return;
      }
      t->call_depth++;
      bdfi_t2_run(t, bdfi_cff_item(t->g, subrs, (size_t)at));
      t->call_depth--;
      continue; /* the subroutine left the stack as it is */
    }
    case 11: /* return */
      return;
    case 14: /* endchar */
      bdfi_t2_width(t, 0, 0);
      t->done = 1;
      return;
    case 12:
      if (i >= cs.n) {
        t->done = 1;
        return;
      }
      bdfi_t2_escape(t, cs.p[i]);
      i++;
      break;
    default:
      break;
    }
    t->n = 0;
  }
}

/* run: the charstring of a glyph drawn into a path. 0 when the font has no
 * such glyph (-1 when memory ran out). */
static int bdfi_cff_outline(const bdfi_cff *c, uint32_t gid, bdfi_rpath *path) {
  if (gid >= c->char_strings.count) {
    return 0;
  }
  size_t fd = 0;
  if (c->fd_select && c->fd_select[gid] < c->nlocal) {
    fd = c->fd_select[gid];
  }
  bdfi_t2 t;
  memset(&t, 0, sizeof t);
  t.g = c;
  t.local = &c->local[fd];
  t.path = path;
  bdfi_t2_run(&t, bdfi_cff_item(c, &c->char_strings, gid));
  bdfi_t2_close(&t);
  free(t.stack);
  return t.oom ? -1 : 1;
}

/* ---- A face -------------------------------------------------------------- */

/* The glyphs of a face are kept in pages of 256, made as they are asked for. */
typedef struct bdfi_glyph_page {
  bdfi_rpath *path[256];
  uint8_t state[256]; /* 0: not asked for; 1: read (NULL: no outline) */
} bdfi_glyph_page;

struct bdfi_face {
  uint8_t *owned; /* the bytes of the font, when the face keeps them */
  bdfi_span glyf, loca, head, hmtx;
  int is_cff;
  int64_t num_glyphs;
  int units_per_em;
  size_t advances; /* numberOfHMetrics, when there are metrics */
  int has_advances;
  bdfi_cmap_pair *cmap;
  size_t ncmap;
  bdfi_cff *cff;
  bdfi_glyph_page *pages[256];
  double upem, asc, dsc;
};

BDF_INTERNAL void bdfi_face_free(bdfi_face *f) {
  if (!f) {
    return;
  }
  for (int i = 0; i < 256; i++) {
    if (f->pages[i]) {
      for (int k = 0; k < 256; k++) {
        if (f->pages[i]->path[k]) {
          bdfi_rpath_clear(f->pages[i]->path[k]);
          free(f->pages[i]->path[k]);
        }
      }
      free(f->pages[i]);
    }
  }
  bdfi_cff_free(f->cff);
  free(f->cmap);
  free(f->owned);
  free(f);
}

/* The name records a rebuilt font carries over (noticeNameIDs). */
static int bdfi_sfnt_notice(uint32_t id) {
  return id == 0 || (id >= 7 && id <= 9) || (id >= 11 && id <= 14);
}

/*
 * parseNotices reads the copyright and license strings of the name table,
 * which a renderer has no use for, and refuses a font whose records are
 * more than 4 MiB of them together. Only that is kept of it here: whether
 * the reference reads the font at all.
 */
static int bdfi_sfnt_notices_fit(bdfi_span nt) {
  if (nt.n < 6) {
    return 1;
  }
  size_t count = bdfi_sf16(nt, 2);
  int64_t str_off = bdfi_sf16(nt, 4), left = 4 << 20;
  uint8_t best[15] = {0};
  for (size_t i = 0; i < count; i++) {
    int64_t rec = 6 + (int64_t)i * 12;
    if ((uint64_t)rec + 12 > nt.n) {
      break;
    }
    uint32_t pid = bdfi_sf16(nt, rec), lang = bdfi_sf16(nt, rec + 4), id = bdfi_sf16(nt, rec + 6);
    int64_t length = bdfi_sf16(nt, rec + 8), off = str_off + bdfi_sf16(nt, rec + 10);
    if ((uint64_t)(off + length) > nt.n || !bdfi_sfnt_notice(id)) {
      continue;
    }
    /* a record that would not replace the one kept is not decoded */
    uint8_t score;
    if (pid == 0 || pid == 3) {
      score = pid == 3 && lang == 0x0409 ? 3 : 2;
    } else if (pid == 1) {
      score = 1;
    } else {
      continue;
    }
    if (score <= best[id]) {
      continue;
    }
    left -= length;
    if (left < 0) {
      return 0;
    }
    /* it is kept when it decodes to a string that is not empty */
    const uint8_t *raw = nt.p + off;
    int kept = 0;
    if (pid == 1) {
      kept = length > 0;
      for (int64_t k = 0; k < length; k++) {
        kept = kept && raw[k] < 0x80;
      }
    } else {
      kept = length >= 2; /* a UTF-16 unit, whatever it is, is a character */
    }
    if (kept) {
      best[id] = score;
    }
  }
  return 1;
}

/*
 * ParseWith and newFace: a face of an sfnt font, the index-th of a
 * collection. With take, the face keeps the bytes (and frees them, also
 * when it fails); otherwise they have to outlive it. The status says why
 * there is no face: BDF_ERR_FORMAT for what the reference does not read
 * either, or that has no Unicode character map.
 */
BDF_INTERNAL bdfi_face *bdfi_face_new(const uint8_t *data, size_t len, int index, int take,
                                      bdf_status *status) {
  bdfi_face *f = calloc(1, sizeof *f);
  *status = BDF_ERR_FORMAT;
  if (!f) {
    if (take) {
      free((void *)(uintptr_t)data);
    }
    *status = BDF_ERR_NOMEM;
    return NULL;
  }
  if (take) {
    f->owned = (uint8_t *)(uintptr_t)data;
  }
  bdfi_span all = {data, len};
  bdfi_span maxp = {NULL, 0}, hhea = {NULL, 0}, cmap = {NULL, 0}, name = {NULL, 0}, cff = {NULL, 0};
  int ok = len >= 12;
  uint32_t tag = bdfi_sf32(all, 0);
  int64_t off = 0;
  if (ok && tag == 0x74746366) { /* 'ttcf' */
    ok = index >= 0 && (uint32_t)index < bdfi_sf32(all, 8);
    if (ok) {
      off = bdfi_sf32(all, 12 + 4 * (int64_t)index);
      ok = (uint64_t)off + 12 <= len;
      tag = bdfi_sf32(all, off);
    }
  } else if (index != 0) {
    ok = 0;
  }
  f->units_per_em = 1000;
  if (ok) {
    if (tag == 0x4f54544f) { /* 'OTTO' */
      f->is_cff = 1;
    } else if (tag != 0x00010000 && tag != 0x74727565) { /* 1.0, 'true' */
      ok = 0;
    }
  }
  if (ok) {
    size_t n = bdfi_sf16(all, off + 4);
    for (size_t i = 0; i < n; i++) {
      uint64_t rec = (uint64_t)off + 12 + i * 16;
      if (rec + 16 > len) {
        break;
      }
      uint64_t toff = bdfi_sf32(all, (int64_t)rec + 8), tlen = bdfi_sf32(all, (int64_t)rec + 12);
      if (toff > len) {
        continue;
      }
      if (toff + tlen > len) {
        tlen = len - toff;
      }
      /* a table that is named twice is the last of them */
      bdfi_span t = {data + toff, (size_t)tlen};
      const uint8_t *nm = data + rec;
      if (memcmp(nm, "glyf", 4) == 0) {
        f->glyf = t;
      } else if (memcmp(nm, "loca", 4) == 0) {
        f->loca = t;
      } else if (memcmp(nm, "head", 4) == 0) {
        f->head = t;
      } else if (memcmp(nm, "hmtx", 4) == 0) {
        f->hmtx = t;
      } else if (memcmp(nm, "maxp", 4) == 0) {
        maxp = t;
      } else if (memcmp(nm, "hhea", 4) == 0) {
        hhea = t;
      } else if (memcmp(nm, "cmap", 4) == 0) {
        cmap = t;
      } else if (memcmp(nm, "name", 4) == 0) {
        name = t;
      } else if (memcmp(nm, "CFF ", 4) == 0) {
        cff = t;
      }
    }
    if (cff.p && !f->glyf.p) {
      f->is_cff = 1;
    }
    if (maxp.n >= 6) {
      f->num_glyphs = bdfi_sf16(maxp, 4);
    }
    if (f->head.n >= 54 && bdfi_sf16(f->head, 18) > 0) {
      f->units_per_em = (int)bdfi_sf16(f->head, 18);
    }
    int ascent = 0, descent = 0;
    if (hhea.n >= 36) {
      ascent = bdfi_sf_i16(hhea, 4);
      descent = bdfi_sf_i16(hhea, 6);
      if (f->hmtx.p) {
        f->has_advances = 1;
        f->advances = bdfi_sf16(hhea, 34);
      }
    }
    f->upem = (double)f->units_per_em;
    f->asc = (double)ascent / f->upem;
    f->dsc = -(double)descent / f->upem;
    if (f->asc + f->dsc <= 0) {
      f->asc = 0.8;
      f->dsc = 0.2;
    }
    int found = 0;
    bdf_status st = bdfi_cmap_parse(cmap, &f->cmap, &f->ncmap, &found);
    if (st == BDF_ERR_NOMEM) {
      *status = st;
      ok = 0;
    } else {
      ok = st == BDF_OK && found && bdfi_sfnt_notices_fit(name);
    }
  }
  if (ok && f->is_cff) {
    int oom = 0;
    f->cff = bdfi_cff_parse(cff, &oom); /* without one, the glyphs have no outlines */
    if (oom) {
      *status = BDF_ERR_NOMEM;
      ok = 0;
    }
  }
  if (!ok) {
    bdfi_face_free(f);
    return NULL;
  }
  *status = BDF_OK;
  return f;
}

/* Whether the font has advance widths (len(Advances) != 0): a face of the
 * application's fonts without them has no glyphs to draw. */
BDF_INTERNAL int bdfi_face_has_metrics(const bdfi_face *f) {
  return f->has_advances && f->advances > 0;
}

BDF_INTERNAL double bdfi_face_upem(const bdfi_face *f) { return f->upem; }
BDF_INTERNAL double bdfi_face_ascent(const bdfi_face *f) { return f->asc; }
BDF_INTERNAL double bdfi_face_descent(const bdfi_face *f) { return f->dsc; }

/* fontFace.glyph: the glyph of a character, 0 when the font has none. */
BDF_INTERNAL uint16_t bdfi_face_glyph(const bdfi_face *f, uint32_t rune) {
  size_t lo = 0, hi = f->ncmap;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (f->cmap[mid].c < rune) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < f->ncmap && f->cmap[lo].c == rune ? (uint16_t)f->cmap[lo].g : 0;
}

/* fontFace.advance: the advance of a glyph in em. */
BDF_INTERNAL double bdfi_face_advance(const bdfi_face *f, uint16_t gid) {
  int adv;
  if (!f->has_advances || f->advances == 0) {
    adv = f->units_per_em / 2;
  } else {
    size_t i = gid >= f->advances ? f->advances - 1 : gid;
    adv = (int)bdfi_sf16(f->hmtx, (int64_t)i * 4);
  }
  return (double)adv / f->upem;
}

/* ---- TrueType outlines --------------------------------------------------- */

/* The transform of a component of a composite glyph: x' = a x + c y + e. */
typedef struct bdfi_affine {
  double a, b, c, d, e, f;
} bdfi_affine;

static bdfi_pt bdfi_affine_apply(const bdfi_affine *m, double x, double y) {
  bdfi_pt p = {m->a * x + m->c * y + m->e, m->b * x + m->d * y + m->f};
  return p;
}

static bdfi_affine bdfi_affine_mul(const bdfi_affine *m, const bdfi_affine *n) {
  bdfi_affine r = {m->a * n->a + m->c * n->b,        m->b * n->a + m->d * n->b,
                   m->a * n->c + m->c * n->d,        m->b * n->c + m->d * n->d,
                   m->a * n->e + m->c * n->f + m->e, m->b * n->e + m->d * n->f + m->f};
  return r;
}

/* glyfData: the entry of a glyph. 0 when it cannot be read; an entry
 * without a p is a glyph without contours. */
static int bdfi_glyf_data(const bdfi_face *f, uint32_t gid, bdfi_span *out) {
  out->p = NULL;
  out->n = 0;
  if (!f->glyf.p || !f->loca.p || f->head.n < 54 || (int64_t)gid >= f->num_glyphs) {
    return 0;
  }
  uint64_t s, e;
  if (bdfi_sf16(f->head, 50) != 0) {
    if ((uint64_t)gid * 4 + 8 > f->loca.n) {
      return 0;
    }
    s = bdfi_sf32(f->loca, (int64_t)gid * 4);
    e = bdfi_sf32(f->loca, (int64_t)gid * 4 + 4);
  } else {
    if ((uint64_t)gid * 2 + 4 > f->loca.n) {
      return 0;
    }
    s = (uint64_t)bdfi_sf16(f->loca, (int64_t)gid * 2) * 2;
    e = (uint64_t)bdfi_sf16(f->loca, (int64_t)gid * 2 + 2) * 2;
  }
  if (s == e) {
    return 1;
  }
  if (s > e || e > f->glyf.n || e - s < 10) {
    return 0;
  }
  out->p = f->glyf.p + s;
  out->n = (size_t)(e - s);
  return 1;
}

/* The points and components an outline may have, those of its components
 * included (maxOutline), and the points of one glyph (maxPoint). */
#define BDFI_MAX_OUTLINE   (1 << 18)
#define BDFI_MAX_POINT     (1 << 16)
#define BDFI_GLYF_ON_CURVE 0x01

typedef struct bdfi_contour {
  bdfi_rpath *path;
  const bdfi_affine *m;
  const double *xs, *ys;
  const uint8_t *flags;
  size_t n;
} bdfi_contour;

/* quadLoop: the n points of a contour from index from, wrapping around,
 * closed at (sx, sy). */
static void bdfi_quad_loop(const bdfi_contour *c, size_t from, double sx, double sy) {
  double cx = 0, cy = 0;
  int pending = 0;
  for (size_t k = 0; k < c->n; k++) {
    size_t i = (from + k) % c->n;
    double x = c->xs[i], y = c->ys[i];
    int on = c->flags[i] & BDFI_GLYF_ON_CURVE;
    if (k == c->n - 1 && on && x == sx && y == sy && !pending) {
      break;
    }
    if (on) {
      bdfi_pt b = bdfi_affine_apply(c->m, x, y);
      if (pending) {
        bdfi_pt a = bdfi_affine_apply(c->m, cx, cy);
        bdfi_rpath_quad_to(c->path, a.x, a.y, b.x, b.y);
        pending = 0;
      } else {
        bdfi_rpath_line_to(c->path, b.x, b.y);
      }
      continue;
    }
    if (pending) {
      /* two control points in a row: the point between them is on the curve */
      bdfi_pt a = bdfi_affine_apply(c->m, cx, cy);
      bdfi_pt b = bdfi_affine_apply(c->m, (cx + x) / 2, (cy + y) / 2);
      bdfi_rpath_quad_to(c->path, a.x, a.y, b.x, b.y);
    }
    cx = x;
    cy = y;
    pending = 1;
  }
  if (pending) {
    bdfi_pt a = bdfi_affine_apply(c->m, cx, cy), b = bdfi_affine_apply(c->m, sx, sy);
    bdfi_rpath_quad_to(c->path, a.x, a.y, b.x, b.y);
  }
}

/* contour: a contour of points on and off the curve. */
static void bdfi_glyf_contour(const bdfi_contour *c) {
  if (c->n == 0) {
    return;
  }
  /* it starts at a point on the curve, or between the last and the first
   * when every point is a control point */
  size_t first = c->n;
  for (size_t i = 0; i < c->n; i++) {
    if (c->flags[i] & BDFI_GLYF_ON_CURVE) {
      first = i;
      break;
    }
  }
  double sx, sy;
  size_t from;
  if (first == c->n) {
    sx = (c->xs[0] + c->xs[c->n - 1]) / 2;
    sy = (c->ys[0] + c->ys[c->n - 1]) / 2;
    from = 0;
  } else {
    sx = c->xs[first];
    sy = c->ys[first];
    from = first + 1;
  }
  bdfi_pt p = bdfi_affine_apply(c->m, sx, sy);
  bdfi_rpath_move_to(c->path, p.x, p.y);
  bdfi_quad_loop(c, from, sx, sy);
  bdfi_rpath_close(c->path);
}

/* The coordinates of one axis of a simple glyph (the closure coords). */
static int bdfi_glyf_coords(bdfi_span g, size_t *pos, const uint8_t *flags, size_t points,
                            uint8_t is_short, uint8_t same, double *out) {
  int64_t v = 0;
  for (size_t i = 0; i < points; i++) {
    uint8_t f = flags[i];
    if (f & is_short) {
      if (*pos >= g.n) {
        return 0;
      }
      int d = g.p[*pos];
      (*pos)++;
      v += f & same ? d : -d;
    } else if (!(f & same)) {
      if (*pos + 2 > g.n) {
        return 0;
      }
      v += bdfi_sf_i16(g, (int64_t)*pos);
      *pos += 2;
    }
    out[i] = (double)v;
  }
  return 1;
}

/* simpleGlyph. 0 when the glyph cannot be read, -1 when memory ran out. */
static int bdfi_glyf_simple(bdfi_span g, size_t contours, bdfi_rpath *path, const bdfi_affine *m,
                            int64_t *left) {
  size_t pos = 10;
  if (pos + 2 * contours + 2 > g.n) {
    return 0;
  }
  size_t points = 0;
  for (size_t i = 0; i < contours; i++) {
    size_t end = bdfi_sf16(g, (int64_t)(pos + 2 * i));
    if (i > 0 && end + 1 < points) {
      return 0;
    }
    points = end + 1; /* an end is less than BDFI_MAX_POINT: it is 16 bits */
  }
  *left -= (int64_t)points;
  if (*left < 0) {
    return 0;
  }
  size_t ends_at = pos;
  pos += 2 * contours;
  pos += 2 + bdfi_sf16(g, (int64_t)pos); /* the instructions */
  uint8_t *flags = bdfi_malloc_array(points ? points : 1, 1);
  double *xs = bdfi_malloc_array(points ? points : 1, 2 * sizeof *xs);
  if (!flags || !xs) {
    free(flags);
    free(xs);
    return -1;
  }
  double *ys = xs + points;
  int ok = 1;
  for (size_t i = 0; ok && i < points;) {
    if (pos >= g.n) {
      ok = 0;
      break;
    }
    uint8_t f = g.p[pos++];
    flags[i++] = f;
    if (f & 0x08) { /* repeated */
      if (pos >= g.n) {
        ok = 0;
        break;
      }
      for (unsigned r = g.p[pos++]; r > 0 && i < points; r--) {
        flags[i++] = f;
      }
    }
  }
  ok = ok && bdfi_glyf_coords(g, &pos, flags, points, 0x02, 0x10, xs) &&
       bdfi_glyf_coords(g, &pos, flags, points, 0x04, 0x20, ys);
  if (ok) {
    size_t start = 0;
    for (size_t i = 0; i < contours; i++) {
      size_t end = bdfi_sf16(g, (int64_t)(ends_at + 2 * i));
      bdfi_contour c = {path, m, xs + start, ys + start, flags + start, end + 1 - start};
      bdfi_glyf_contour(&c);
      start = end + 1;
    }
  }
  free(flags);
  free(xs);
  return ok;
}

/* glyf: a glyph drawn into a path; left is how many points and components
 * the outline may still have. 0 when it cannot be read, -1 without memory. */
static int bdfi_glyf_outline(const bdfi_face *f, uint32_t gid, bdfi_rpath *path, bdfi_affine m,
                             int depth, int64_t *left) {
  bdfi_span g;
  if (!bdfi_glyf_data(f, gid, &g)) {
    return 0;
  }
  if (!g.p) {
    return 1;
  }
  int32_t n = bdfi_sf_i16(g, 0);
  if (n >= 0) {
    return bdfi_glyf_simple(g, (size_t)n, path, &m, left);
  }
  if (depth > 8) {
    return 0;
  }
  size_t pos = 10;
  for (;;) {
    if (pos + 4 > g.n) {
      return 0;
    }
    uint32_t flags = bdfi_sf16(g, (int64_t)pos), comp = bdfi_sf16(g, (int64_t)pos + 2);
    pos += 4;
    double dx, dy;
    if (flags & 0x0001) { /* the arguments are words */
      if (pos + 4 > g.n) {
        return 0;
      }
      dx = (double)bdfi_sf_i16(g, (int64_t)pos);
      dy = (double)bdfi_sf_i16(g, (int64_t)pos + 2);
      pos += 4;
    } else {
      if (pos + 2 > g.n) {
        return 0;
      }
      dx = (double)(int8_t)g.p[pos];
      dy = (double)(int8_t)g.p[pos + 1];
      pos += 2;
    }
    if (!(flags & 0x0002)) {
      dx = 0; /* anchored by point numbers: rare, placed at the origin */
      dy = 0;
    }
    bdfi_affine c = {1, 0, 0, 1, dx, dy};
    if (flags & 0x0008) { /* one scale */
      if (pos + 2 > g.n) {
        return 0;
      }
      c.a = c.d = (double)bdfi_sf_i16(g, (int64_t)pos) / 16384;
      pos += 2;
    } else if (flags & 0x0040) { /* a scale for each axis */
      if (pos + 4 > g.n) {
        return 0;
      }
      c.a = (double)bdfi_sf_i16(g, (int64_t)pos) / 16384;
      c.d = (double)bdfi_sf_i16(g, (int64_t)pos + 2) / 16384;
      pos += 4;
    } else if (flags & 0x0080) { /* two by two */
      if (pos + 8 > g.n) {
        return 0;
      }
      c.a = (double)bdfi_sf_i16(g, (int64_t)pos) / 16384;
      c.b = (double)bdfi_sf_i16(g, (int64_t)pos + 2) / 16384;
      c.c = (double)bdfi_sf_i16(g, (int64_t)pos + 4) / 16384;
      c.d = (double)bdfi_sf_i16(g, (int64_t)pos + 6) / 16384;
      pos += 8;
    }
    if (--*left < 0) {
      return 0;
    }
    int r = bdfi_glyf_outline(f, comp, path, bdfi_affine_mul(&m, &c), depth + 1, left);
    if (r != 1) {
      return r;
    }
    if (!(flags & 0x0020)) { /* no more components */
      return 1;
    }
  }
}

/*
 * fontFace.outline: the outline of a glyph in font units (y up), or NULL
 * for a glyph that has none or cannot be read. The face keeps it.
 */
BDF_INTERNAL bdfi_rpath *bdfi_face_outline(bdfi_face *f, uint16_t gid, int *oom) {
  bdfi_glyph_page *page = f->pages[gid >> 8];
  if (!page) {
    page = calloc(1, sizeof *page);
    if (!page) {
      *oom = 1;
      return NULL;
    }
    f->pages[gid >> 8] = page;
  }
  if (page->state[gid & 0xff]) {
    return page->path[gid & 0xff];
  }
  bdfi_rpath *path = calloc(1, sizeof *path);
  if (!path) {
    *oom = 1;
    return NULL;
  }
  int ok = 0;
  if (f->cff) {
    ok = bdfi_cff_outline(f->cff, gid, path);
  } else if (!f->is_cff) {
    bdfi_affine identity = {1, 0, 0, 1, 0, 0};
    int64_t left = BDFI_MAX_OUTLINE;
    ok = bdfi_glyf_outline(f, gid, path, identity, 0, &left);
  }
  if (ok < 0 || path->oom) {
    bdfi_rpath_clear(path);
    free(path);
    *oom = 1;
    return NULL; /* asked for again when there is memory */
  }
  if (!ok) {
    bdfi_rpath_clear(path);
    free(path);
    path = NULL;
  }
  page->state[gid & 0xff] = 1;
  page->path[gid & 0xff] = path;
  return path;
}
