/*
 * A search of the runs of a text index that takes characters for what they
 * are to a reader: a port of TextSearch of the reference's TypeScript reader
 * (packages/core/src/search.ts). The text of a run is normalised as a string
 * (form KC, so that a letter and the marks after it are the letter they
 * compose), and then a character at a time (lower case, katakana as
 * hiragana, the minus sign as a hyphen; the tables are that reader's, asked
 * about every character of normalised text). The normalised text keeps where
 * each of its characters came from, so that a hit is located in the runs.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"
#include "unicode_fold.h"

/* A character becomes at most 18 others, and the arrays below hold 16 bytes
 * for each: the text a search takes is bounded. */
#define BDFI_MAX_SEARCH_TEXT ((size_t)1 << 26)
#define BDFI_NO_RUN          UINT32_MAX
#define BDFI_NFKC_MOST       18

/* MAX_PIECE of the reference: the most characters of a piece of a run that
 * is normalised together. A letter has a few marks; text made to have
 * thousands in a row would be normalised again for each of them. Past this a
 * piece ends where it is. */
#define BDFI_MAX_PIECE 32

/* ---- What a character is to a search ------------------------------------ */

/* bdfi_table_find looks a character up: 1 and what it becomes, in *one when
 * that is one character, else in *out and *n. */
static int bdfi_table_find(const uint32_t (*ranges)[3], size_t nranges,
                           const uint32_t (*strings)[3], size_t nstrings, const uint32_t *pool,
                           uint32_t cp, uint32_t *one, const uint32_t **out, size_t *n) {
  size_t lo = 0, hi = nranges;
  while (lo < hi) { /* the last range that starts at or before cp */
    size_t mid = lo + (hi - lo) / 2;
    if (ranges[mid][0] <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo > 0) {
    const uint32_t *r = ranges[lo - 1];
    uint32_t last = r[1] & 0xFFFFFF, step = r[1] >> 24;
    if (cp <= last && (cp - r[0]) % step == 0) {
      *one = r[2] + (cp - r[0]);
      *out = one;
      *n = 1;
      return 1;
    }
  }
  lo = 0;
  hi = nstrings;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (strings[mid][0] < cp) {
      lo = mid + 1;
    } else if (strings[mid][0] > cp) {
      hi = mid;
    } else {
      *out = pool + strings[mid][1];
      *n = strings[mid][2];
      return 1;
    }
  }
  return 0;
}

/* bdfi_fold is a character of normalised text as a search has it: *out and
 * its length (one holds it when it is one character). */
static size_t bdfi_fold(uint32_t cp, int case_sensitive, uint32_t *one, const uint32_t **out) {
  size_t n = 1;
  *out = one;
  if (cp < 0x80) {
    *one = !case_sensitive && cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
    return 1;
  }
  if (!case_sensitive &&
      bdfi_table_find(bdfi_lower_ranges, sizeof bdfi_lower_ranges / sizeof bdfi_lower_ranges[0],
                      bdfi_lower_strings, BDFI_LOWER_STRINGS, bdfi_lower_pool, cp, one, out, &n)) {
    return n;
  }
  if (bdfi_table_find(bdfi_fold_ranges, sizeof bdfi_fold_ranges / sizeof bdfi_fold_ranges[0],
                      bdfi_fold_strings, BDFI_FOLD_STRINGS, bdfi_fold_pool, cp, one, out, &n)) {
    return n;
  }
  *one = cp;
  return 1;
}

/* The white space of a JavaScript regular expression (\s). */
static int bdfi_is_space(uint32_t c) {
  return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
         c == 0x3000 || c == 0xFEFF;
}

/* A growing array of code points. */
typedef struct bdfi_u32s {
  uint32_t *v;
  size_t n, cap;
} bdfi_u32s;

static int bdfi_u32s_push(bdfi_u32s *b, uint32_t c) {
  if (b->n == b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 64;
    uint32_t *v = bdfi_realloc_array(b->v, cap, sizeof *v);
    if (!v) {
      return -1;
    }
    b->v = v;
    b->cap = cap;
  }
  b->v[b->n++] = c;
  return 0;
}

/* bdfi_fold_text normalises UTF-8 text into code points (normalizeQuery):
 * form KC of the text as a whole, and then each of its characters folded. */
static int bdfi_fold_text(const char *s, size_t len, int case_sensitive, bdfi_u32s *out) {
  bdfi_u32s src = {0};
  bdfi_points norm = {0};
  int rc = -1;
  size_t used;
  for (size_t i = 0; i < len; i += used) {
    if (bdfi_u32s_push(&src, bdfi_utf8_next(s, len, i, &used)) < 0) {
      goto done;
    }
  }
  if (bdfi_nfkc_points(&norm, src.v, src.n) < 0) {
    goto done;
  }
  for (size_t i = 0; i < norm.n; i++) {
    uint32_t one;
    const uint32_t *to;
    size_t n = bdfi_fold(norm.c[i], case_sensitive, &one, &to);
    for (size_t k = 0; k < n; k++) {
      if (bdfi_u32s_push(out, to[k]) < 0) {
        goto done;
      }
    }
  }
  rc = 0;
done:
  free(src.v);
  free(norm.c);
  free(norm.cc);
  return rc;
}

/* A growing string. */
typedef struct bdfi_chars {
  char *v;
  size_t n, cap;
} bdfi_chars;

static int bdfi_chars_add(bdfi_chars *b, const char *s, size_t len) {
  if (len > b->cap - b->n) {
    size_t cap = b->cap ? b->cap : 64;
    while (len > cap - b->n) {
      if (cap > SIZE_MAX / 2) {
        return -1;
      }
      cap *= 2;
    }
    char *v = bdfi_realloc_array(b->v, cap, 1);
    if (!v) {
      return -1;
    }
    b->v = v;
    b->cap = cap;
  }
  if (len > 0) {
    memcpy(b->v + b->n, s, len);
  }
  b->n += len;
  return 0;
}

static int bdfi_chars_put(bdfi_chars *b, uint32_t c) {
  char buf[4];
  return bdfi_chars_add(b, buf, bdfi_utf8_put(c, buf));
}

bdf_status bdf_text_search_normalize(const char *s, size_t len, int case_sensitive, char **out,
                                size_t *out_len, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || !out_len || (!s && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  bdfi_u32s cps = {0};
  bdfi_chars text = {0};
  int bad = bdfi_fold_text(s ? s : "", len, case_sensitive, &cps);
  for (size_t i = 0; !bad && i < cps.n; i++) {
    bad = bdfi_chars_put(&text, cps.v[i]);
  }
  bad = bad || bdfi_chars_add(&text, "", 1) < 0;
  free(cps.v);
  if (bad) {
    free(text.v);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  *out = text.v;
  *out_len = text.n - 1;
  return BDF_OK;
}

/* ---- The searchable text ------------------------------------------------ */

struct bdf_text_search {
  const bdf_text_index *index;
  int case_sensitive;
  /* the normalised text, a code point each, with for each the run it came
   * from (BDFI_NO_RUN: a separator between runs) and where the piece of the
   * run's text it was normalised from starts and ends */
  uint32_t *norm, *run_of, *off_of, *end_of;
  size_t n, cap;
};

void bdf_text_search_free(bdf_text_search *s) {
  if (!s) {
    return;
  }
  free(s->norm);
  free(s->run_of);
  free(s->off_of);
  free(s->end_of);
  free(s);
}

static int bdfi_search_push(bdf_text_search *s, uint32_t c, uint32_t run, uint32_t off,
                            uint32_t end) {
  if (s->n == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 256;
    uint32_t *a = bdfi_realloc_array(s->norm, cap, sizeof *a);
    if (a) {
      s->norm = a;
    }
    uint32_t *b = bdfi_realloc_array(s->run_of, cap, sizeof *b);
    if (b) {
      s->run_of = b;
    }
    uint32_t *c3 = bdfi_realloc_array(s->off_of, cap, sizeof *c3);
    if (c3) {
      s->off_of = c3;
    }
    uint32_t *d = bdfi_realloc_array(s->end_of, cap, sizeof *d);
    if (d) {
      s->end_of = d;
    }
    if (!a || !b || !c3 || !d) {
      return -1;
    }
    s->cap = cap;
  }
  s->norm[s->n] = c;
  s->run_of[s->n] = run;
  s->off_of[s->n] = off;
  s->end_of[s->n] = end;
  s->n++;
  return 0;
}

/* The characters a piece of a run normalises to, folded and added to the
 * text with where the piece is. */
static bdf_status bdfi_search_piece(bdf_text_search *s, const uint32_t *norm, size_t n, uint32_t run,
                                    size_t start, size_t end) {
  for (size_t i = 0; i < n; i++) {
    uint32_t one;
    const uint32_t *to;
    size_t m = bdfi_fold(norm[i], s->case_sensitive, &one, &to);
    if (m > BDFI_MAX_SEARCH_TEXT - s->n) {
      return BDF_ERR_FORMAT;
    }
    for (size_t k = 0; k < m; k++) {
      if (bdfi_search_push(s, to[k], run, (uint32_t)start, (uint32_t)end) < 0) {
        return BDF_ERR_NOMEM;
      }
    }
  }
  return BDF_OK;
}

/*
 * pieces of the reference: the text of a run in the pieces that form KC
 * normalises on their own, each added to the searchable text. A character
 * is a piece, unless it changes with those before it: its normal form starts
 * with a mark, or the piece with it is not the piece's normal form and its
 * own one after the other (the letters of a Hangul syllable). The pieces
 * normalised one after another are the text normalised, but for a piece cut
 * at BDFI_MAX_PIECE characters.
 */
static bdf_status bdfi_search_run(bdf_text_search *s, uint32_t ri, const char *text, size_t len,
                                  bdfi_points *one, bdfi_points *more) {
  uint32_t src[BDFI_MAX_PIECE + 1];
  uint32_t norm[(BDFI_MAX_PIECE + 1) * BDFI_NFKC_MOST], alone[BDFI_NFKC_MOST];
  size_t nsrc = 0, nnorm = 0, start = 0, off = 0, used;
  for (; off < len; off += used) {
    uint32_t c = bdfi_utf8_next(text, len, off, &used);
    /* (ASCII is as it is, and changes nothing before it) */
    int ascii = c < 0x80;
    /* most other characters are as they are too, and change what is before
     * them only when they compose with the last of it: the answer of the
     * comparison below, without normalising */
    int alone_is_c = ascii || bdfi_nfkc_alone(c);
    size_t nalone = 1;
    alone[0] = c;
    if (!alone_is_c) {
      if (bdfi_nfkc_points(one, &c, 1) < 0) {
        return BDF_ERR_NOMEM;
      }
      if (one->n == 0 || one->n > BDFI_NFKC_MOST) {
        return BDF_ERR_FORMAT; /* (no character becomes none, or that many) */
      }
      nalone = one->n;
      memcpy(alone, one->c, nalone * sizeof *alone);
    }
    if (nsrc > 0) {
      if (!ascii && nsrc < BDFI_MAX_PIECE &&
          !(alone_is_c && !bdfi_composes(norm[nnorm - 1], c))) {
        src[nsrc] = c;
        if (bdfi_nfkc_points(more, src, nsrc + 1) < 0) {
          return BDF_ERR_NOMEM;
        }
        if (more->n == 0 || more->n > sizeof norm / sizeof norm[0]) {
          return BDF_ERR_FORMAT;
        }
        if (bdfi_is_mark(alone[0]) || more->n != nnorm + nalone ||
            memcmp(more->c, norm, nnorm * sizeof *norm) != 0 ||
            memcmp(more->c + nnorm, alone, nalone * sizeof *alone) != 0) {
          nsrc++;
          nnorm = more->n;
          memcpy(norm, more->c, nnorm * sizeof *norm);
          continue;
        }
      }
      bdf_status st = bdfi_search_piece(s, norm, nnorm, ri, start, off);
      if (st != BDF_OK) {
        return st;
      }
    }
    start = off;
    src[0] = c;
    nsrc = 1;
    nnorm = nalone;
    memcpy(norm, alone, nalone * sizeof *norm);
  }
  return nsrc > 0 ? bdfi_search_piece(s, norm, nnorm, ri, start, off) : BDF_OK;
}

bdf_status bdf_text_search_new(const bdf_text_index *index, int case_sensitive, bdf_text_search **out,
                          bdf_error *err) {
  bdfi_error_clear(err);
  if (!index || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  size_t runs = bdf_text_index_count(index);
  bdf_text_search *s = calloc(1, sizeof *s);
  if (!s) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  s->index = index;
  s->case_sensitive = case_sensitive ? 1 : 0;
  bdf_status st = BDF_OK;
  bdfi_points one = {0}, more = {0};
  for (size_t ri = 0; ri < runs && st == BDF_OK; ri++) {
    bdf_index_run run;
    if (bdf_text_index_run(index, ri, &run) != BDF_OK || ri >= BDFI_NO_RUN ||
        run.text_len > UINT32_MAX) {
      st = BDF_ERR_FORMAT;
      break;
    }
    if (ri > 0 && run.sep != BDF_SEP_NONE) {
      /* a paragraph break is a character no query holds; a space is a space */
      if (bdfi_search_push(s, run.sep == BDF_SEP_BREAK ? 0 : ' ', BDFI_NO_RUN, 0, 0) < 0) {
        st = BDF_ERR_NOMEM;
        break;
      }
    }
    st = bdfi_search_run(s, (uint32_t)ri, run.text, run.text_len, &one, &more);
  }
  free(one.c);
  free(one.cc);
  free(more.c);
  free(more.cc);
  if (st != BDF_OK) {
    bdf_text_search_free(s);
    bdfi_error_set(err, st, 0, st == BDF_ERR_NOMEM ? "out of memory" : "text too long to search");
    return st;
  }
  *out = s;
  return BDF_OK;
}

/* bdfi_search_slice writes code points [from, to) of the normalised text as
 * UTF-8, a paragraph break as mark. */
static int bdfi_search_slice(const bdf_text_search *s, size_t from, size_t to, const char *brk,
                             int newlines_too, bdfi_chars *out) {
  for (size_t i = from; i < to; i++) {
    uint32_t c = s->norm[i];
    if (c == 0 || (newlines_too && c == '\n')) {
      if (bdfi_chars_add(out, brk, strlen(brk)) < 0) {
        return -1;
      }
    } else if (bdfi_chars_put(out, c) < 0) {
      return -1;
    }
  }
  return 0;
}

bdf_status bdf_text_search_plain(const bdf_text_search *s, char **out, size_t *out_len, bdf_error *err) {
  bdfi_error_clear(err);
  if (!s || !out || !out_len) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  bdfi_chars text = {0};
  if (bdfi_search_slice(s, 0, s->n, "\n", 0, &text) < 0 || bdfi_chars_add(&text, "", 1) < 0) {
    free(text.v);
    *out = NULL;
    *out_len = 0;
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  *out = text.v;
  *out_len = text.n - 1;
  return BDF_OK;
}

/* ---- Hits ---------------------------------------------------------------- */

typedef struct bdfi_hit {
  char *text, *context;
  size_t text_len, context_len;
  bdf_text_hit_segment *segments;
  size_t nsegments;
} bdfi_hit;

struct bdf_text_hits {
  bdfi_hit *hits;
  size_t n, cap;
};

void bdf_text_hits_free(bdf_text_hits *h) {
  if (!h) {
    return;
  }
  for (size_t i = 0; i < h->n; i++) {
    free(h->hits[i].text);
    free(h->hits[i].context);
    free(h->hits[i].segments);
  }
  free(h->hits);
  free(h);
}

size_t bdf_text_hits_count(const bdf_text_hits *h) { return h ? h->n : 0; }

bdf_status bdf_text_hits_at(const bdf_text_hits *h, size_t i, bdf_text_hit *out) {
  if (!h || !out || i >= h->n) {
    return BDF_ERR_ARG;
  }
  const bdfi_hit *hit = &h->hits[i];
  out->text = hit->text;
  out->text_len = hit->text_len;
  out->context = hit->context;
  out->context_len = hit->context_len;
  out->nsegments = hit->nsegments;
  return BDF_OK;
}

bdf_status bdf_text_hits_segment(const bdf_text_hits *h, size_t i, size_t k,
                                  bdf_text_hit_segment *out) {
  if (!h || !out || i >= h->n || k >= h->hits[i].nsegments) {
    return BDF_ERR_ARG;
  }
  *out = h->hits[i].segments[k];
  return BDF_OK;
}

/* bdfi_search_locate makes the hit of code points [start, end) of the
 * normalised text: 1, 0 when it holds nothing of a run (separators alone),
 * -1 when out of memory. */
static int bdfi_search_locate(const bdf_text_search *s, size_t start, size_t end, size_t context,
                              bdfi_hit *hit) {
  memset(hit, 0, sizeof *hit);
  size_t cap = 0;
  bdfi_chars text = {0}, ctx = {0};
  for (size_t i = start; i < end; i++) {
    uint32_t ri = s->run_of[i];
    if (ri == BDFI_NO_RUN) {
      continue;
    }
    bdf_index_run run;
    if (bdf_text_index_run(s->index, ri, &run) != BDF_OK) {
      continue;
    }
    size_t off = s->off_of[i], piece_end = s->end_of[i];
    bdf_text_hit_segment *last = hit->nsegments ? &hit->segments[hit->nsegments - 1] : NULL;
    if (last && last->run == ri) {
      if (piece_end > last->end) {
        last->end = piece_end;
      }
      continue;
    }
    if (hit->nsegments == cap) {
      cap = cap ? cap * 2 : 4;
      bdf_text_hit_segment *grown = bdfi_realloc_array(hit->segments, cap, sizeof *grown);
      if (!grown) {
        goto oom;
      }
      hit->segments = grown;
    }
    bdf_text_hit_segment *seg = &hit->segments[hit->nsegments++];
    seg->run = ri;
    seg->a = run.a;
    seg->b = run.b;
    seg->ordinal = run.ordinal;
    seg->start = off;
    seg->end = piece_end;
  }
  if (hit->nsegments == 0) {
    free(hit->segments);
    hit->segments = NULL;
    return 0;
  }
  /* the text as the document has it, a space between runs a space parts */
  for (size_t k = 0; k < hit->nsegments; k++) {
    const bdf_text_hit_segment *seg = &hit->segments[k];
    bdf_index_run run;
    if (bdf_text_index_run(s->index, seg->run, &run) != BDF_OK) {
      continue;
    }
    if ((text.n > 0 && run.sep == BDF_SEP_SPACE && bdfi_chars_add(&text, " ", 1) < 0) ||
        bdfi_chars_add(&text, run.text + seg->start, seg->end - seg->start) < 0) {
      goto oom;
    }
  }
  /* some of the searchable text around the hit, the breaks made visible */
  size_t from = start > context ? start - context : 0;
  size_t to = context > s->n - end ? s->n : end + context;
  if (bdfi_search_slice(s, from, to, " \xE2\x8F\x8E ", 1, &ctx) < 0 ||
      bdfi_chars_add(&text, "", 1) < 0 || bdfi_chars_add(&ctx, "", 1) < 0) {
    goto oom;
  }
  hit->text = text.v;
  hit->text_len = text.n - 1;
  hit->context = ctx.v;
  hit->context_len = ctx.n - 1;
  return 1;
oom:
  free(hit->segments);
  free(text.v);
  free(ctx.v);
  memset(hit, 0, sizeof *hit);
  return -1;
}

bdf_status bdf_text_search_find(const bdf_text_search *s, const char *query, size_t len, size_t limit,
                           size_t context, bdf_text_hits **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!s || !out || (!query && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdf_text_hits *hits = calloc(1, sizeof *hits);
  bdfi_u32s folded = {0};
  if (!hits || bdfi_fold_text(query ? query : "", len, s->case_sensitive, &folded) < 0) {
    goto oom;
  }
  /* white space of any kind and length is one space */
  size_t m = 0;
  for (size_t i = 0; i < folded.n; i++) {
    if (!bdfi_is_space(folded.v[i])) {
      folded.v[m++] = folded.v[i];
    } else if (i == 0 || !bdfi_is_space(folded.v[i - 1])) {
      folded.v[m++] = ' ';
    }
  }
  const uint32_t *q = folded.v;
  for (size_t from = 0; m > 0 && hits->n < limit && m <= s->n && from <= s->n - m;) {
    size_t at = from;
    while (at <= s->n - m &&
           (s->norm[at] != q[0] || memcmp(s->norm + at, q, m * sizeof *q) != 0)) {
      at++;
    }
    if (at > s->n - m) {
      break;
    }
    from = at + 1;
    bdfi_hit hit;
    int rc = bdfi_search_locate(s, at, at + m, context, &hit);
    if (rc < 0) {
      goto oom;
    }
    if (rc == 0) {
      continue;
    }
    if (hits->n == hits->cap) {
      size_t cap = hits->cap ? hits->cap * 2 : 16;
      bdfi_hit *grown = bdfi_realloc_array(hits->hits, cap, sizeof *grown);
      if (!grown) {
        free(hit.text);
        free(hit.context);
        free(hit.segments);
        goto oom;
      }
      hits->hits = grown;
      hits->cap = cap;
    }
    hits->hits[hits->n++] = hit;
  }
  free(folded.v);
  *out = hits;
  return BDF_OK;
oom:
  free(folded.v);
  bdf_text_hits_free(hits);
  bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
  return BDF_ERR_NOMEM;
}
