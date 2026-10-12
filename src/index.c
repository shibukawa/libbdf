/*
 * The index parts (text index, cue index) and the text of a document's
 * views: ports of textindex.go, cues.go and searchtext.go of the reference
 * implementation.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

/* ---- Text index part (spec §7.9) ---------------------------------------- */

typedef struct bdfi_irun {
  uint32_t a, b, ordinal;
  uint8_t sep;
  const char *text;
  size_t len;
} bdfi_irun;

struct bdf_text_index {
  bdfi_arena arena; /* the strings */
  bdfi_irun *runs;
  size_t n, cap;
};

static bdf_text_index *bdfi_index_new(void) {
  bdf_text_index *idx = calloc(1, sizeof *idx);
  if (idx) {
    bdfi_arena_init(&idx->arena);
  }
  return idx;
}

void bdf_text_index_free(bdf_text_index *idx) {
  if (!idx) {
    return;
  }
  bdfi_arena_free(&idx->arena);
  free(idx->runs);
  free(idx);
}

/* bdfi_index_push appends a run: 0, or -1 when out of memory. */
static int bdfi_index_push(bdf_text_index *idx, const bdfi_irun *run) {
  if (idx->n == idx->cap) {
    size_t cap = idx->cap ? idx->cap * 2 : 64;
    bdfi_irun *runs = bdfi_realloc_array(idx->runs, cap, sizeof *runs);
    if (!runs) {
      return -1;
    }
    idx->runs = runs;
    idx->cap = cap;
  }
  idx->runs[idx->n++] = *run;
  return 0;
}

bdf_status bdf_text_index_decode(const uint8_t *data, size_t len, bdf_text_index **out,
                                 bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdfi_reader rd, *r = &rd;
  bdfi_rd_init(r, data, len);
  const uint8_t *magic = bdfi_rd_bytes(r, 4);
  if (!magic || memcmp(magic, "BTXT", 4) != 0) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "not a text index part");
    return BDF_ERR_FORMAT;
  }
  if (bdfi_rd_u16(r) > 1) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "unsupported text index version");
    return BDF_ERR_FORMAT;
  }
  bdf_text_index *idx = bdfi_index_new();
  if (!idx) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  /* a run is three numbers, a separator and a string: 5 bytes or more */
  size_t n = bdfi_rd_count(r, 5, "bad run count");
  int oom = 0;
  idx->runs = bdfi_alloc_items(n, sizeof *idx->runs, &oom);
  if (oom) {
    goto nomem;
  }
  idx->cap = n;
  for (size_t i = 0; i < n && !r->failed; i++) {
    bdfi_irun *run = &idx->runs[i];
    run->a = (uint32_t)bdfi_rd_varuint(r);
    run->b = (uint32_t)bdfi_rd_varuint(r);
    run->ordinal = (uint32_t)bdfi_rd_varuint(r);
    run->sep = bdfi_rd_u8(r);
    size_t slen = bdfi_rd_count(r, 1, "bad string length");
    const uint8_t *s = bdfi_rd_bytes(r, slen);
    if (r->failed) {
      break;
    }
    char *copy = bdfi_arena_strdup(&idx->arena, (const char *)s, slen);
    if (!copy) {
      goto nomem;
    }
    run->text = copy;
    run->len = slen;
    idx->n = i + 1;
  }
  if (r->failed) {
    bdf_text_index_free(idx);
    bdfi_error_from_reader(err, r);
    return BDF_ERR_FORMAT;
  }
  *out = idx;
  return BDF_OK;
nomem:
  bdf_text_index_free(idx);
  bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
  return BDF_ERR_NOMEM;
}

size_t bdf_text_index_count(const bdf_text_index *idx) { return idx ? idx->n : 0; }

bdf_status bdf_text_index_run(const bdf_text_index *idx, size_t i, bdf_index_run *out) {
  if (!idx || !out || i >= idx->n) {
    return BDF_ERR_ARG;
  }
  const bdfi_irun *r = &idx->runs[i];
  out->a = r->a;
  out->b = r->b;
  out->ordinal = r->ordinal;
  out->sep = r->sep;
  out->text = r->text ? r->text : "";
  out->text_len = r->len;
  return BDF_OK;
}

/*
 * bdfi_plain_len is the length of the runs [from, to) of an index joined as
 * PlainText joins them: nothing before the first, then a space or a line
 * break where a run's separator says so. *ok is cleared when it overflows.
 */
static size_t bdfi_plain_len(const bdfi_irun *runs, const size_t *order, size_t from, size_t to,
                             int *ok) {
  size_t total = 0;
  for (size_t i = from; i < to; i++) {
    const bdfi_irun *r = &runs[order ? order[i] : i];
    size_t add = r->len + (i > from && (r->sep == BDF_SEP_SPACE || r->sep == BDF_SEP_BREAK) ? 1 : 0);
    if (add > SIZE_MAX - 1 - total) {
      *ok = 0;
      return 0;
    }
    total += add;
  }
  return total;
}

static void bdfi_plain_fill(const bdfi_irun *runs, const size_t *order, size_t from, size_t to,
                            char *out) {
  size_t k = 0;
  for (size_t i = from; i < to; i++) {
    const bdfi_irun *r = &runs[order ? order[i] : i];
    if (i > from) {
      if (r->sep == BDF_SEP_SPACE) {
        out[k++] = ' ';
      } else if (r->sep == BDF_SEP_BREAK) {
        out[k++] = '\n';
      }
    }
    if (r->len > 0) {
      memcpy(out + k, r->text, r->len);
      k += r->len;
    }
  }
  out[k] = '\0';
}

bdf_status bdf_text_index_plain(const bdf_text_index *idx, char **out, size_t *out_len,
                                bdf_error *err) {
  bdfi_error_clear(err);
  if (!idx || !out || !out_len) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  int ok = 1;
  size_t total = bdfi_plain_len(idx->runs, NULL, 0, idx->n, &ok);
  char *buf = ok ? malloc(total + 1) : NULL;
  if (!buf) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  bdfi_plain_fill(idx->runs, NULL, 0, idx->n, buf);
  *out = buf;
  *out_len = total;
  return BDF_OK;
}

/* ---- Cue index part (spec §4.4) ----------------------------------------- */

struct bdf_cues {
  bdf_cue_system *systems;
  size_t nsystems;
  bdf_cue *cues;
  size_t ncues;
};

void bdf_cues_free(bdf_cues *c) {
  if (!c) {
    return;
  }
  free(c->systems);
  free(c->cues);
  free(c);
}

bdf_status bdf_cues_decode(const uint8_t *data, size_t len, bdf_cues **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdfi_reader rd, *r = &rd;
  bdfi_rd_init(r, data, len);
  const uint8_t *magic = bdfi_rd_bytes(r, 4);
  if (!magic || memcmp(magic, "BCUE", 4) != 0) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "not a cue index part");
    return BDF_ERR_FORMAT;
  }
  if (bdfi_rd_u16(r) > 1) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "unsupported cue index version");
    return BDF_ERR_FORMAT;
  }
  bdf_cues *c = calloc(1, sizeof *c);
  if (!c) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  int oom = 0;
  /* a system is a page number and four f32: 17 bytes or more */
  size_t n = bdfi_rd_count(r, 17, "bad system count");
  c->systems = bdfi_alloc_items(n, sizeof *c->systems, &oom);
  if (oom) {
    goto nomem;
  }
  for (size_t i = 0; i < n && !r->failed; i++) {
    bdf_cue_system *s = &c->systems[i];
    s->page = (uint32_t)bdfi_rd_varuint(r);
    s->x = bdfi_rd_f32(r);
    s->y = bdfi_rd_f32(r);
    s->w = bdfi_rd_f32(r);
    s->h = bdfi_rd_f32(r);
    c->nsystems = i + 1;
  }
  /* a cue is two numbers and an f32: 6 bytes or more */
  n = bdfi_rd_count(r, 6, "bad cue count");
  c->cues = bdfi_alloc_items(n, sizeof *c->cues, &oom);
  if (oom) {
    goto nomem;
  }
  uint32_t t = 0;
  for (size_t i = 0; i < n && !r->failed; i++) {
    bdf_cue *q = &c->cues[i];
    t += (uint32_t)bdfi_rd_varuint(r);
    q->tick = t;
    q->system = (uint32_t)bdfi_rd_varuint(r);
    q->x = bdfi_rd_f32(r);
    if (q->system >= c->nsystems && !r->failed) {
      bdf_cues_free(c);
      bdfi_error_set(err, BDF_ERR_FORMAT, 0, "cue refers to a missing system");
      return BDF_ERR_FORMAT;
    }
    c->ncues = i + 1;
  }
  if (r->failed) {
    bdf_cues_free(c);
    bdfi_error_from_reader(err, r);
    return BDF_ERR_FORMAT;
  }
  *out = c;
  return BDF_OK;
nomem:
  bdf_cues_free(c);
  bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
  return BDF_ERR_NOMEM;
}

size_t bdf_cues_system_count(const bdf_cues *c) { return c ? c->nsystems : 0; }

bdf_status bdf_cues_system(const bdf_cues *c, size_t i, bdf_cue_system *out) {
  if (!c || !out || i >= c->nsystems) {
    return BDF_ERR_ARG;
  }
  *out = c->systems[i];
  return BDF_OK;
}

size_t bdf_cues_count(const bdf_cues *c) { return c ? c->ncues : 0; }

bdf_status bdf_cues_cue(const bdf_cues *c, size_t i, bdf_cue *out) {
  if (!c || !out || i >= c->ncues) {
    return BDF_ERR_ARG;
  }
  *out = c->cues[i];
  return BDF_OK;
}

/* ---- The objects of a document, decoded once ---------------------------- */

enum { BDFI_OBJ_UNTRIED = 0, BDFI_OBJ_DECODED = 1, BDFI_OBJ_INVALID = 2 };

/* A cache by the index of the part in the manifest: no table to collide in. */
typedef struct bdfi_objcache {
  const bdf_doc *doc;
  bdf_object **objs;
  uint8_t *state;
  size_t n;
  int oom;
} bdfi_objcache;

static void bdfi_objcache_free(void *p) {
  bdfi_objcache *c = p;
  if (!c) {
    return;
  }
  for (size_t i = 0; i < c->n; i++) {
    bdf_object_free(c->objs[i]);
  }
  free(c->objs);
  free(c->state);
  free(c);
}

static bdfi_objcache *bdfi_objcache_new(const bdf_doc *doc) {
  bdfi_objcache *c = calloc(1, sizeof *c);
  if (!c) {
    return NULL;
  }
  c->doc = doc;
  c->n = bdf_doc_part_count(doc);
  if (c->n > 0) {
    c->objs = bdfi_calloc_array(c->n, sizeof *c->objs);
    c->state = calloc(c->n, 1);
    if (!c->objs || !c->state) {
      bdfi_objcache_free(c);
      return NULL;
    }
  }
  return c;
}

/* bdfi_objcache_get is a bdf_resolve_fn: the decoded object of a part, or
 * NULL for one that is missing or is no object. */
static const bdf_object *bdfi_objcache_get(void *ctx, const uint8_t hash[16]) {
  bdfi_objcache *c = ctx;
  int64_t i = bdfi_doc_part_index(c->doc, hash);
  if (i < 0) {
    return NULL;
  }
  if (c->state[i] == BDFI_OBJ_UNTRIED) {
    uint8_t *data;
    size_t len;
    bdf_error err;
    bdf_status st = bdf_doc_part(c->doc, hash, &data, &len, &err);
    if (st == BDF_OK) {
      st = bdf_object_decode(data, len, &c->objs[i], &err);
      bdf_free(data);
    }
    if (st == BDF_ERR_NOMEM) {
      c->oom = 1; /* not the document's fault: not remembered as invalid */
      return NULL;
    }
    c->state[i] = st == BDF_OK ? BDFI_OBJ_DECODED : BDFI_OBJ_INVALID;
  }
  return c->objs[i];
}

bdf_status bdf_doc_object(const bdf_doc *doc, const uint8_t hash[16], bdf_object **out,
                          bdf_error *err) {
  bdfi_error_clear(err);
  if (!doc || !hash || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  uint8_t *data;
  size_t len;
  bdf_status st = bdf_doc_part(doc, hash, &data, &len, err);
  if (st != BDF_OK) {
    return st;
  }
  st = bdf_object_decode(data, len, out, err);
  bdf_free(data);
  return st;
}

bdf_status bdf_doc_text(const bdf_doc *doc, const uint8_t hash[16], bdf_text **out,
                        bdf_error *err) {
  bdfi_error_clear(err);
  if (!doc || !hash || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdfi_objcache *cache = bdfi_objcache_new(doc);
  if (!cache) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  const bdf_object *obj = bdfi_objcache_get(cache, hash);
  bdf_status st;
  if (!obj) {
    st = cache->oom ? BDF_ERR_NOMEM : BDF_ERR_FORMAT;
    bdfi_error_set(err, st, 0, cache->oom ? "out of memory" : "object missing or invalid");
    bdfi_objcache_free(cache);
    return st;
  }
  bdfi_use_limits limits;
  bdfi_use_limits_init(&limits);
  bdf_text *text = NULL;
  st = bdfi_text_extract(obj, bdfi_objcache_get, cache, &limits, 1, &text, err);
  bdfi_use_limits_free(&limits);
  if (st == BDF_OK && cache->oom) {
    /* a child that could not be read for lack of memory was skipped */
    bdf_text_free(text);
    st = BDF_ERR_NOMEM;
    bdfi_error_set(err, st, 0, "out of memory");
  }
  if (st != BDF_OK) {
    bdfi_objcache_free(cache);
    return st;
  }
  bdfi_text_adopt(text, cache, bdfi_objcache_free);
  *out = text;
  return BDF_OK;
}

/* ---- The text of a view (textindex.go: indexRuns) ----------------------- */

/* Strings by the address they were found at: a run that an object draws a
 * thousand times is kept once. */
typedef struct bdfi_intern {
  const char **keys;
  const char **vals;
  size_t n, cap; /* cap is a power of two */
} bdfi_intern;

static size_t bdfi_ptr_hash(const char *p) {
  uint64_t x = (uint64_t)(uintptr_t)p;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return (size_t)x;
}

/* bdfi_intern_get returns the copy of a string in the arena, made the first
 * time its address is seen; NULL when out of memory. */
static const char *bdfi_intern_get(bdfi_intern *m, bdfi_arena *arena, const char *s, size_t len) {
  if (m->n * 2 >= m->cap) {
    size_t cap = m->cap ? m->cap * 2 : 64;
    const char **keys = bdfi_calloc_array(cap, sizeof *keys);
    const char **vals = bdfi_calloc_array(cap, sizeof *vals);
    if (!keys || !vals) {
      free(keys);
      free(vals);
      return NULL;
    }
    for (size_t i = 0; i < m->cap; i++) {
      if (m->keys[i]) {
        size_t j = bdfi_ptr_hash(m->keys[i]) & (cap - 1);
        while (keys[j]) {
          j = (j + 1) & (cap - 1);
        }
        keys[j] = m->keys[i];
        vals[j] = m->vals[i];
      }
    }
    free(m->keys);
    free(m->vals);
    m->keys = keys;
    m->vals = vals;
    m->cap = cap;
  }
  size_t j = bdfi_ptr_hash(s) & (m->cap - 1);
  while (m->keys[j]) {
    if (m->keys[j] == s) {
      return m->vals[j];
    }
    j = (j + 1) & (m->cap - 1);
  }
  const char *copy = bdfi_arena_strdup(arena, s, len);
  if (!copy) {
    return NULL;
  }
  m->keys[j] = s;
  m->vals[j] = copy;
  m->n++;
  return copy;
}

typedef struct bdfi_view_walk {
  const bdf_doc *doc;
  bdfi_objcache *cache;
  bdfi_use_limits limits; /* of the whole view */
  bdfi_intern intern;
  bdf_text_index *idx;
  int sheet;
  float tile;
} bdfi_view_walk;

/* bdfi_view_add appends the runs of one top-level object. */
static bdf_status bdfi_view_add(bdfi_view_walk *w, uint32_t a, uint32_t b, const uint8_t hash[16],
                                bdf_error *err) {
  const bdf_object *obj = bdfi_objcache_get(w->cache, hash);
  if (!obj) {
    bdf_status st = w->cache->oom ? BDF_ERR_NOMEM : BDF_ERR_FORMAT;
    bdfi_error_set(err, st, 0, w->cache->oom ? "out of memory" : "object missing or invalid");
    return st;
  }
  bdf_text *text = NULL;
  bdf_status st = bdfi_text_extract(obj, bdfi_objcache_get, w->cache, &w->limits, 0, &text, err);
  if (st != BDF_OK) {
    return st;
  }
  if (w->cache->oom) {
    bdf_text_free(text);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  size_t kept = 0, n = bdf_text_run_count(text);
  for (size_t i = 0; i < n; i++) {
    bdf_text_run tr;
    if (bdf_text_run_at(text, i, &tr) != BDF_OK) {
      continue;
    }
    /* in a sheet, a tile keeps the runs whose anchor lies in it: tiles
     * repeat what straddles them (spec §4.1) */
    if (w->sheet && !(tr.x >= 0 && tr.x < w->tile && tr.y >= 0 && tr.y < w->tile)) {
      continue;
    }
    bdfi_irun run;
    run.a = a;
    run.b = b;
    run.ordinal = tr.ordinal;
    run.sep = kept == 0 ? BDF_SEP_BREAK : tr.sep;
    run.len = tr.text_len;
    run.text = bdfi_intern_get(&w->intern, &w->idx->arena, tr.text, tr.text_len);
    kept++;
    if (!run.text || bdfi_index_push(w->idx, &run) < 0) {
      bdf_text_free(text);
      bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
      return BDF_ERR_NOMEM;
    }
  }
  bdf_text_free(text);
  return BDF_OK;
}

typedef struct bdfi_tile_at {
  int64_t x, y;
  size_t index;
} bdfi_tile_at;

static int bdfi_tile_at_cmp(const void *a, const void *b) {
  const bdfi_tile_at *p = a, *q = b;
  if (p->y != q->y) {
    return p->y < q->y ? -1 : 1;
  }
  if (p->x != q->x) {
    return p->x < q->x ? -1 : 1;
  }
  return 0;
}

/* bdfi_tile_number reads a decimal number as the reference writes it back
 * (no sign, no leading zero): the count of bytes read, or 0. */
static size_t bdfi_tile_number(const char *s, size_t n, int64_t *out) {
  size_t i = 0;
  int64_t v = 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    int d = s[i] - '0';
    if (v > (INT64_MAX - d) / 10) {
      return 0;
    }
    v = v * 10 + d;
    i++;
  }
  if (i == 0 || (i > 1 && s[0] == '0')) {
    return 0;
  }
  *out = v;
  return i;
}

static bdf_status bdfi_view_extract(const bdf_doc *doc, size_t view, const bdf_view *v,
                                    bdf_text_index **out, bdf_error *err) {
  bdfi_view_walk w;
  memset(&w, 0, sizeof w);
  w.doc = doc;
  w.sheet = v->kind_len == 5 && memcmp(v->kind, "sheet", 5) == 0;
  w.tile = v->tile;
  bdfi_use_limits_init(&w.limits);
  w.cache = bdfi_objcache_new(doc);
  w.idx = bdfi_index_new();
  bdfi_tile_at *tiles = NULL;
  bdf_status st = BDF_OK;
  if (!w.cache || !w.idx) {
    st = BDF_ERR_NOMEM;
    bdfi_error_set(err, st, 0, "out of memory");
    goto done;
  }
  if (w.sheet) {
    size_t n = v->ntiles;
    tiles = bdfi_malloc_array(n, sizeof *tiles);
    if (!tiles) {
      st = BDF_ERR_NOMEM;
      bdfi_error_set(err, st, 0, "out of memory");
      goto done;
    }
    for (size_t i = 0; i < n; i++) {
      bdf_tile t;
      if (bdf_doc_tile(doc, view, i, &t) != BDF_OK) {
        st = BDF_ERR_ARG;
        bdfi_error_set(err, st, 0, "bad argument");
        goto done;
      }
      size_t kx = bdfi_tile_number(t.key, t.key_len, &tiles[i].x);
      size_t ky = 0;
      if (kx > 0 && kx < t.key_len && t.key[kx] == ',') {
        ky = bdfi_tile_number(t.key + kx + 1, t.key_len - kx - 1, &tiles[i].y);
      }
      if (kx == 0 || ky == 0 || kx + 1 + ky != t.key_len) {
        st = BDF_ERR_FORMAT;
        bdfi_error_set(err, st, 0, "bad tile key");
        goto done;
      }
      tiles[i].index = i;
    }
    qsort(tiles, n, sizeof *tiles, bdfi_tile_at_cmp);
    for (size_t i = 0; i < n; i++) {
      bdf_tile t;
      uint8_t hash[16];
      if (bdf_doc_tile(doc, view, tiles[i].index, &t) != BDF_OK ||
          bdf_hash_parse(t.obj, t.obj_len, hash) != BDF_OK) {
        st = BDF_ERR_FORMAT;
        bdfi_error_set(err, st, 0, "bad part name");
        goto done;
      }
      st = bdfi_view_add(&w, (uint32_t)tiles[i].x, (uint32_t)tiles[i].y, hash, err);
      if (st != BDF_OK) {
        goto done;
      }
    }
  } else {
    for (size_t pi = 0; pi < v->npages; pi++) {
      bdf_page page;
      if (bdf_doc_page(doc, view, pi, &page) != BDF_OK) {
        break;
      }
      for (size_t li = 0; li < page.nlayers; li++) {
        bdf_layer layer;
        if (bdf_doc_layer(doc, view, pi, li, &layer) != BDF_OK) {
          break;
        }
        st = bdfi_view_add(&w, (uint32_t)pi, (uint32_t)li, layer.obj, err);
        if (st != BDF_OK) {
          goto done;
        }
      }
    }
  }
done:
  free(tiles);
  free(w.intern.keys);
  free(w.intern.vals);
  bdfi_use_limits_free(&w.limits);
  bdfi_objcache_free(w.cache);
  if (st != BDF_OK) {
    bdf_text_index_free(w.idx);
    return st;
  }
  *out = w.idx;
  return BDF_OK;
}

bdf_status bdf_doc_view_text(const bdf_doc *doc, size_t view, int extract, bdf_text_index **out,
                             bdf_error *err) {
  bdfi_error_clear(err);
  bdf_view v;
  if (!doc || !out || bdf_doc_view(doc, view, &v) != BDF_OK) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  uint8_t hash[16];
  if (!extract && v.text_index_len > 0 &&
      bdf_hash_parse(v.text_index, v.text_index_len, hash) == BDF_OK &&
      bdf_doc_part_find(doc, hash, NULL)) {
    uint8_t *data;
    size_t len;
    bdf_status st = bdf_doc_part(doc, hash, &data, &len, err);
    if (st != BDF_OK) {
      return st;
    }
    st = bdf_text_index_decode(data, len, out, err);
    bdf_free(data);
    return st;
  }
  return bdfi_view_extract(doc, view, &v, out, err);
}

/* ---- The text of a document for a search index (searchtext.go) ---------- */

typedef struct bdfi_spage {
  int64_t page;
  const char *text;
  size_t len;
} bdfi_spage;

typedef struct bdfi_sview {
  const char *id, *kind, *title;
  size_t id_len, kind_len, title_len;
  bdfi_spage *pages;
  size_t npages;
} bdfi_sview;

struct bdf_search_text {
  bdfi_arena arena;
  bdfi_sview *views;
  size_t nviews;
};

void bdf_search_text_free(bdf_search_text *st) {
  if (!st) {
    return;
  }
  for (size_t i = 0; i < st->nviews; i++) {
    free(st->views[i].pages);
  }
  free(st->views);
  bdfi_arena_free(&st->arena);
  free(st);
}

/* bdfi_space_at is the length of the white space (unicode.IsSpace) that
 * starts at s, or 0. */
static size_t bdfi_space_at(const uint8_t *s, size_t n) {
  if (n == 0) {
    return 0;
  }
  uint8_t c = s[0];
  if (c == ' ' || (c >= '\t' && c <= '\r')) {
    return 1;
  }
  if (c == 0xC2 && n >= 2 && (s[1] == 0x85 || s[1] == 0xA0)) {
    return 2; /* U+0085, U+00A0 */
  }
  if (n >= 3) {
    if (c == 0xE1 && s[1] == 0x9A && s[2] == 0x80) {
      return 3; /* U+1680 */
    }
    if (c == 0xE2 && s[1] == 0x80 &&
        ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 || s[2] == 0xA9 || s[2] == 0xAF)) {
      return 3; /* U+2000–U+200A, U+2028, U+2029, U+202F */
    }
    if (c == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) {
      return 3; /* U+205F */
    }
    if (c == 0xE3 && s[1] == 0x80 && s[2] == 0x80) {
      return 3; /* U+3000 */
    }
  }
  return 0;
}

/* bdfi_trim_space is strings.TrimSpace on [*from, *to) of s. */
static void bdfi_trim_space(const char *s, size_t *from, size_t *to) {
  const uint8_t *b = (const uint8_t *)s;
  size_t a = *from, z = *to;
  for (;;) {
    size_t k = bdfi_space_at(b + a, z - a);
    if (k == 0) {
      break;
    }
    a += k;
  }
  for (;;) {
    size_t k = 0;
    /* the white space that ends at z is 1 to 3 bytes long */
    for (size_t len = 1; len <= 3 && len <= z - a; len++) {
      if (bdfi_space_at(b + z - len, len) == len) {
        k = len;
        break;
      }
    }
    if (k == 0) {
      break;
    }
    z -= k;
  }
  *from = a;
  *to = z;
}

typedef struct bdfi_page_ref {
  uint32_t page;
  size_t index;
} bdfi_page_ref;

static int bdfi_page_ref_cmp(const void *a, const void *b) {
  const bdfi_page_ref *p = a, *q = b;
  if (p->page != q->page) {
    return p->page < q->page ? -1 : 1;
  }
  return p->index < q->index ? -1 : p->index > q->index;
}

/* bdfi_search_add_page joins runs order[from, to) of an index, trims the
 * text, and appends it to the pages of a view unless nothing is left. */
static int bdfi_search_add_page(bdf_search_text *st, bdfi_sview *sv, size_t *cap,
                                const bdf_text_index *idx, const size_t *order, size_t from,
                                size_t to, int64_t page) {
  int ok = 1;
  size_t total = bdfi_plain_len(idx->runs, order, from, to, &ok);
  char *buf = ok ? bdfi_arena_alloc(&st->arena, total + 1) : NULL;
  if (!buf) {
    return -1;
  }
  bdfi_plain_fill(idx->runs, order, from, to, buf);
  size_t a = 0, z = total;
  bdfi_trim_space(buf, &a, &z);
  if (a == z) {
    return 0;
  }
  buf[z] = '\0';
  if (sv->npages == *cap) {
    size_t grown = *cap ? *cap * 2 : 8;
    bdfi_spage *pages = bdfi_realloc_array(sv->pages, grown, sizeof *pages);
    if (!pages) {
      return -1;
    }
    sv->pages = pages;
    *cap = grown;
  }
  bdfi_spage *pg = &sv->pages[sv->npages++];
  pg->page = page;
  pg->text = buf + a;
  pg->len = z - a;
  return 0;
}

bdf_status bdf_doc_search_text(const bdf_doc *doc, bdf_search_text **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!doc || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdf_search_text *st = calloc(1, sizeof *st);
  if (!st) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  bdfi_arena_init(&st->arena);
  size_t nviews = bdf_doc_view_count(doc);
  st->views = bdfi_calloc_array(nviews, sizeof *st->views);
  bdf_text_index *idx = NULL;
  bdfi_page_ref *refs = NULL;
  size_t *order = NULL;
  bdf_status status = BDF_ERR_NOMEM;
  if (!st->views) {
    goto fail;
  }
  int has_flow = 0;
  for (size_t i = 0; i < nviews; i++) {
    bdf_view v;
    if (bdf_doc_view(doc, i, &v) != BDF_OK) {
      continue;
    }
    has_flow = has_flow || (v.kind_len == 4 && memcmp(v.kind, "flow", 4) == 0);
  }
  for (size_t i = 0; i < nviews; i++) {
    bdf_view v;
    if (bdf_doc_view(doc, i, &v) != BDF_OK) {
      continue;
    }
    if (has_flow && v.kind_len == 6 && memcmp(v.kind, "scroll", 6) == 0) {
      continue; /* the same text as the flow view, laid out the other way */
    }
    status = bdf_doc_view_text(doc, i, 0, &idx, err);
    if (status != BDF_OK) {
      goto fail_set;
    }
    status = BDF_ERR_NOMEM;
    bdfi_sview *sv = &st->views[st->nviews];
    sv->id = bdfi_arena_strdup(&st->arena, v.id, v.id_len);
    sv->kind = bdfi_arena_strdup(&st->arena, v.kind, v.kind_len);
    sv->title = bdfi_arena_strdup(&st->arena, v.title, v.title_len);
    if (!sv->id || !sv->kind || !sv->title) {
      goto fail;
    }
    sv->id_len = v.id_len;
    sv->kind_len = v.kind_len;
    sv->title_len = v.title_len;
    st->nviews++;
    size_t cap = 0;
    if (v.kind_len == 5 && memcmp(v.kind, "sheet", 5) == 0) {
      /* a sheet is one text */
      if (bdfi_search_add_page(st, sv, &cap, idx, NULL, 0, idx->n, 0) < 0) {
        goto fail;
      }
    } else if (idx->n > 0) {
      /* the runs of a page are in order, those of its layers one after another */
      refs = bdfi_malloc_array(idx->n, sizeof *refs);
      order = bdfi_malloc_array(idx->n, sizeof *order);
      if (!refs || !order) {
        goto fail;
      }
      for (size_t k = 0; k < idx->n; k++) {
        refs[k].page = idx->runs[k].a;
        refs[k].index = k;
      }
      qsort(refs, idx->n, sizeof *refs, bdfi_page_ref_cmp);
      for (size_t k = 0; k < idx->n; k++) {
        order[k] = refs[k].index;
      }
      for (size_t from = 0; from < idx->n;) {
        size_t to = from + 1;
        while (to < idx->n && refs[to].page == refs[from].page) {
          to++;
        }
        if (bdfi_search_add_page(st, sv, &cap, idx, order, from, to, (int64_t)refs[from].page + 1) < 0) {
          goto fail;
        }
        from = to;
      }
      free(refs);
      free(order);
      refs = NULL;
      order = NULL;
    }
    bdf_text_index_free(idx);
    idx = NULL;
  }
  *out = st;
  return BDF_OK;
fail:
  bdfi_error_set(err, status, 0, "out of memory");
fail_set:
  free(refs);
  free(order);
  bdf_text_index_free(idx);
  bdf_search_text_free(st);
  return status;
}

size_t bdf_search_text_view_count(const bdf_search_text *st) { return st ? st->nviews : 0; }

bdf_status bdf_search_text_view(const bdf_search_text *st, size_t i, bdf_search_view *out) {
  if (!st || !out || i >= st->nviews) {
    return BDF_ERR_ARG;
  }
  const bdfi_sview *v = &st->views[i];
  out->id = v->id;
  out->id_len = v->id_len;
  out->kind = v->kind;
  out->kind_len = v->kind_len;
  out->title = v->title;
  out->title_len = v->title_len;
  out->npages = v->npages;
  return BDF_OK;
}

bdf_status bdf_search_text_page(const bdf_search_text *st, size_t view, size_t i,
                                bdf_search_page *out) {
  if (!st || !out || view >= st->nviews || i >= st->views[view].npages) {
    return BDF_ERR_ARG;
  }
  const bdfi_spage *p = &st->views[view].pages[i];
  out->page = p->page;
  out->text = p->text;
  out->text_len = p->len;
  return BDF_OK;
}
