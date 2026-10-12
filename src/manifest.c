/*
 * The manifest (spec §4): read from JSON the way encoding/json fills the
 * reference's Manifest (unknown members ignored, null as absent, a member of
 * the wrong type an error), then checked as manifest.go's check does.
 */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

static const char *const bdfi_dc_terms[BDFI_DC_TERMS] = {
    "title",  "creator",    "subject",  "description", "publisher", "contributor",
    "date",   "type",       "format",   "identifier",  "source",    "language",
    "relation", "coverage", "rights",   "created",     "modified"};

const char *bdfi_dc_term(size_t i) { return i < BDFI_DC_TERMS ? bdfi_dc_terms[i] : NULL; }

typedef struct bdfi_mp {
  const bdfi_json *j;
  bdfi_manifest *m;
  const char *err;
  int oom;
} bdfi_mp;

static int bdfi_mp_fail(bdfi_mp *p, const char *msg) {
  if (!p->err) {
    p->err = msg;
  }
  return -1;
}

static int bdfi_mp_nomem(bdfi_mp *p) {
  p->oom = 1;
  return bdfi_mp_fail(p, "out of memory");
}

#define BDFI_WRONG_TYPE "manifest member of the wrong type"

/* bdfi_mp_get returns the member, or NULL when absent or null. */
static const bdfi_json_node *bdfi_mp_get(bdfi_mp *p, const bdfi_json_node *obj, const char *key) {
  const bdfi_json_node *n = bdfi_json_member(p->j, obj, key);
  return n && n->kind != BDFI_JSON_NULL ? n : NULL;
}

static int bdfi_mp_strdup(bdfi_mp *p, const char *s, size_t n, bdfi_str *out) {
  out->p = bdfi_arena_strdup(&p->m->arena, s, n);
  if (!out->p) {
    return bdfi_mp_nomem(p);
  }
  out->n = n;
  return 0;
}

static int bdfi_mp_str_node(bdfi_mp *p, const bdfi_json_node *n, bdfi_str *out) {
  if (n->kind != BDFI_JSON_STRING) {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  size_t len;
  const char *s = bdfi_json_text(p->j, n, 0, &len);
  return bdfi_mp_strdup(p, s, len, out);
}

static int bdfi_mp_str(bdfi_mp *p, const bdfi_json_node *obj, const char *key, bdfi_str *out) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  return n ? bdfi_mp_str_node(p, n, out) : 0;
}

static int bdfi_mp_int(bdfi_mp *p, const bdfi_json_node *obj, const char *key, int64_t *out) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  if (!n) {
    return 0;
  }
  if (bdfi_json_int64(p->j, n, out) < 0) {
    return bdfi_mp_fail(p, n->kind == BDFI_JSON_NUMBER ? "manifest number is not an integer"
                                                        : BDFI_WRONG_TYPE);
  }
  return 0;
}

static int bdfi_mp_f32_node(bdfi_mp *p, const bdfi_json_node *n, float *out) {
  double v;
  if (n->kind != BDFI_JSON_NUMBER) {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  if (bdfi_json_number(p->j, n, &v) < 0 || v > FLT_MAX || v < -FLT_MAX) {
    return bdfi_mp_fail(p, "manifest number out of range");
  }
  *out = (float)v;
  return 0;
}

static int bdfi_mp_f32(bdfi_mp *p, const bdfi_json_node *obj, const char *key, float *out,
                       int *present) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  if (!n) {
    return 0;
  }
  if (present) {
    *present = 1;
  }
  return bdfi_mp_f32_node(p, n, out);
}

static int bdfi_mp_bool(bdfi_mp *p, const bdfi_json_node *obj, const char *key, int *out) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  if (!n) {
    return 0;
  }
  if (n->kind == BDFI_JSON_TRUE) {
    *out = 1;
  } else if (n->kind == BDFI_JSON_FALSE) {
    *out = 0;
  } else {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  return 0;
}

static int bdfi_mp_hash(bdfi_mp *p, const bdfi_json_node *obj, const char *key, uint8_t out[16],
                        int *present) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  if (!n) {
    return 0;
  }
  if (n->kind != BDFI_JSON_STRING) {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  size_t len;
  const char *s = bdfi_json_text(p->j, n, 0, &len);
  if (bdf_hash_parse(s, len, out) != BDF_OK) {
    return bdfi_mp_fail(p, "bad part name");
  }
  if (present) {
    *present = 1;
  }
  return 0;
}

static int bdfi_mp_bytes(bdfi_mp *p, const bdfi_json_node *obj, const char *key, uint8_t **out,
                         size_t *len) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  if (!n) {
    return 0;
  }
  if (n->kind != BDFI_JSON_STRING) {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  size_t slen;
  const char *s = bdfi_json_text(p->j, n, 0, &slen);
  if (bdfi_base64_decode(s, slen, &p->m->arena, out, len) < 0) {
    return bdfi_mp_fail(p, "bad base64");
  }
  return 0;
}

/* bdfi_mp_array checks that a member is an array (or absent) and returns it. */
static int bdfi_mp_array(bdfi_mp *p, const bdfi_json_node *obj, const char *key,
                         const bdfi_json_node **out) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  *out = NULL;
  if (!n) {
    return 0;
  }
  if (n->kind != BDFI_JSON_ARRAY) {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  *out = n;
  return 0;
}

static int bdfi_mp_object(bdfi_mp *p, const bdfi_json_node *obj, const char *key,
                          const bdfi_json_node **out) {
  const bdfi_json_node *n = bdfi_mp_get(p, obj, key);
  *out = NULL;
  if (!n) {
    return 0;
  }
  if (n->kind != BDFI_JSON_OBJECT) {
    return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
  }
  *out = n;
  return 0;
}

static void *bdfi_mp_alloc(bdfi_mp *p, size_t n, size_t size) {
  if (size != 0 && n > SIZE_MAX / size) {
    bdfi_mp_nomem(p);
    return NULL;
  }
  void *q = bdfi_arena_alloc(&p->m->arena, n * size);
  if (!q) {
    bdfi_mp_nomem(p);
  }
  return q;
}

/* bdfi_mp_runs reads [[count, size], ...] as encoding/json fills []Run:
 * missing elements are zero, extra ones are ignored. */
static int bdfi_mp_runs(bdfi_mp *p, const bdfi_json_node *obj, const char *key, size_t *n,
                        float (**runs)[2]) {
  const bdfi_json_node *arr;
  if (bdfi_mp_array(p, obj, key, &arr) < 0 || !arr) {
    return p->err ? -1 : 0;
  }
  *runs = bdfi_mp_alloc(p, arr->count, sizeof **runs);
  if (arr->count && !*runs) {
    return -1;
  }
  size_t i = 0;
  for (const bdfi_json_node *e = bdfi_json_first(p->j, arr); e; e = bdfi_json_next(p->j, e), i++) {
    if (e->kind == BDFI_JSON_NULL) {
      continue; /* null leaves the zero run */
    }
    if (e->kind != BDFI_JSON_ARRAY) {
      return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
    }
    size_t k = 0;
    for (const bdfi_json_node *v = bdfi_json_first(p->j, e); v && k < 2;
         v = bdfi_json_next(p->j, v), k++) {
      if (v->kind == BDFI_JSON_NULL) {
        continue;
      }
      if (bdfi_mp_f32_node(p, v, &(*runs)[i][k]) < 0) {
        return -1;
      }
    }
  }
  *n = arr->count;
  return 0;
}

static int bdfi_mp_page(bdfi_mp *p, const bdfi_json_node *pg, bdfi_page *out) {
  if (bdfi_mp_f32(p, pg, "w", &out->w, NULL) < 0 || bdfi_mp_f32(p, pg, "h", &out->h, NULL) < 0) {
    return -1;
  }
  const bdfi_json_node *body;
  if (bdfi_mp_object(p, pg, "body", &body) < 0) {
    return -1;
  }
  if (body) {
    out->has_body = 1;
    if (bdfi_mp_f32(p, body, "x", &out->body[0], NULL) < 0 ||
        bdfi_mp_f32(p, body, "y", &out->body[1], NULL) < 0 ||
        bdfi_mp_f32(p, body, "w", &out->body[2], NULL) < 0 ||
        bdfi_mp_f32(p, body, "h", &out->body[3], NULL) < 0) {
      return -1;
    }
  }
  const bdfi_json_node *layers;
  if (bdfi_mp_array(p, pg, "layers", &layers) < 0) {
    return -1;
  }
  if (layers) {
    out->layers = bdfi_mp_alloc(p, layers->count, sizeof *out->layers);
    if (layers->count && !out->layers) {
      return -1;
    }
    size_t i = 0;
    for (const bdfi_json_node *l = bdfi_json_first(p->j, layers); l;
         l = bdfi_json_next(p->j, l), i++) {
      if (l->kind == BDFI_JSON_NULL) {
        continue;
      }
      if (l->kind != BDFI_JSON_OBJECT) {
        return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
      }
      if (bdfi_mp_str(p, l, "role", &out->layers[i].role) < 0 ||
          bdfi_mp_hash(p, l, "obj", out->layers[i].obj, NULL) < 0) {
        return -1;
      }
    }
    out->nlayers = layers->count;
  }
  return 0;
}

static int bdfi_mp_view(bdfi_mp *p, const bdfi_json_node *v, bdfi_view *out) {
  if (bdfi_mp_str(p, v, "id", &out->id) < 0 || bdfi_mp_str(p, v, "kind", &out->kind) < 0 ||
      bdfi_mp_str(p, v, "title", &out->title) < 0 ||
      bdfi_mp_str(p, v, "textIndex", &out->text_index) < 0 ||
      bdfi_mp_str(p, v, "direction", &out->direction) < 0) {
    return -1;
  }
  const bdfi_json_node *pages;
  if (bdfi_mp_array(p, v, "pages", &pages) < 0) {
    return -1;
  }
  if (pages) {
    out->pages = bdfi_mp_alloc(p, pages->count, sizeof *out->pages);
    if (pages->count && !out->pages) {
      return -1;
    }
    size_t i = 0;
    for (const bdfi_json_node *pg = bdfi_json_first(p->j, pages); pg;
         pg = bdfi_json_next(p->j, pg), i++) {
      if (pg->kind == BDFI_JSON_NULL) {
        return bdfi_mp_fail(p, "null page"); /* manifest.go: check */
      }
      if (pg->kind != BDFI_JSON_OBJECT) {
        return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
      }
      if (bdfi_mp_page(p, pg, &out->pages[i]) < 0) {
        return -1;
      }
    }
    out->npages = pages->count;
  }
  const bdfi_json_node *cont;
  if (bdfi_mp_object(p, v, "continuous", &cont) < 0) {
    return -1;
  }
  if (cont) {
    out->has_continuous = 1;
    if (bdfi_mp_f32(p, cont, "gap", &out->gap, NULL) < 0) {
      return -1;
    }
  }
  if (bdfi_mp_f32(p, v, "tile", &out->tile, NULL) < 0 ||
      bdfi_mp_runs(p, v, "cols", &out->ncols, &out->cols) < 0 ||
      bdfi_mp_runs(p, v, "rows", &out->nrows, &out->rows) < 0) {
    return -1;
  }
  const bdfi_json_node *freeze;
  if (bdfi_mp_object(p, v, "freeze", &freeze) < 0) {
    return -1;
  }
  if (freeze) {
    out->has_freeze = 1;
    if (bdfi_mp_int(p, freeze, "cols", &out->freeze_cols) < 0 ||
        bdfi_mp_int(p, freeze, "rows", &out->freeze_rows) < 0) {
      return -1;
    }
  }
  if (bdfi_mp_bool(p, v, "gridlines", &out->gridlines) < 0) {
    return -1;
  }
  const bdfi_json_node *tiles;
  if (bdfi_mp_object(p, v, "tiles", &tiles) < 0) {
    return -1;
  }
  if (tiles) {
    out->tiles = bdfi_mp_alloc(p, tiles->count, sizeof *out->tiles);
    if (tiles->count && !out->tiles) {
      return -1;
    }
    size_t i = 0;
    for (const bdfi_json_node *t = bdfi_json_first(p->j, tiles); t;
         t = bdfi_json_next(p->j, t), i++) {
      size_t klen;
      const char *k = bdfi_json_text(p->j, t, 1, &klen);
      if (bdfi_mp_strdup(p, k, klen, &out->tiles[i].key) < 0) {
        return -1;
      }
      if (t->kind == BDFI_JSON_NULL) {
        if (bdfi_mp_strdup(p, "", 0, &out->tiles[i].obj) < 0) {
          return -1;
        }
      } else if (bdfi_mp_str_node(p, t, &out->tiles[i].obj) < 0) {
        return -1;
      }
    }
    out->ntiles = tiles->count;
  }
  if (bdfi_mp_str(p, v, "tilesRef", &out->tiles_ref) < 0) {
    return -1;
  }
  const bdfi_json_node *play;
  if (bdfi_mp_object(p, v, "play", &play) < 0) {
    return -1;
  }
  if (play) {
    out->has_play = 1;
    if (bdfi_mp_str(p, play, "seq", &out->seq) < 0 || bdfi_mp_str(p, play, "audio", &out->audio) < 0 ||
        bdfi_mp_str(p, play, "type", &out->audio_type) < 0 ||
        bdfi_mp_str(p, play, "cues", &out->cues) < 0) {
      return -1;
    }
  }
  return 0;
}

static int bdfi_mp_parts(bdfi_mp *p, const bdfi_json_node *root) {
  const bdfi_json_node *arr;
  if (bdfi_mp_array(p, root, "parts", &arr) < 0 || !arr) {
    return p->err ? -1 : 0;
  }
  bdfi_manifest *m = p->m;
  m->parts = bdfi_mp_alloc(p, arr->count, sizeof *m->parts);
  if (arr->count && !m->parts) {
    return -1;
  }
  size_t i = 0;
  for (const bdfi_json_node *e = bdfi_json_first(p->j, arr); e; e = bdfi_json_next(p->j, e), i++) {
    bdfi_part *pt = &m->parts[i];
    if (e->kind == BDFI_JSON_NULL) {
      continue;
    }
    if (e->kind != BDFI_JSON_OBJECT) {
      return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
    }
    if (bdfi_mp_hash(p, e, "h", pt->hash, NULL) < 0 || bdfi_mp_str(p, e, "t", &pt->type) < 0 ||
        bdfi_mp_str(p, e, "enc", &pt->enc) < 0 || bdfi_mp_int(p, e, "len", &pt->len) < 0 ||
        bdfi_mp_int(p, e, "size", &pt->size) < 0 || bdfi_mp_int(p, e, "off", &pt->off) < 0 ||
        bdfi_mp_hash(p, e, "sealed", pt->sealed, &pt->has_sealed) < 0) {
      return -1;
    }
  }
  m->nparts = arr->count;
  return 0;
}

static int bdfi_mp_encryption(bdfi_mp *p, const bdfi_json_node *root) {
  const bdfi_json_node *enc;
  if (bdfi_mp_object(p, root, "encryption", &enc) < 0 || !enc) {
    return p->err ? -1 : 0;
  }
  bdfi_manifest *m = p->m;
  m->has_encryption = 1;
  if (bdfi_mp_str(p, enc, "cipher", &m->cipher) < 0) {
    return -1;
  }
  const bdfi_json_node *keys;
  if (bdfi_mp_array(p, enc, "keys", &keys) < 0) {
    return -1;
  }
  if (keys) {
    m->keys = bdfi_mp_alloc(p, keys->count, sizeof *m->keys);
    if (keys->count && !m->keys) {
      return -1;
    }
    size_t i = 0;
    for (const bdfi_json_node *k = bdfi_json_first(p->j, keys); k; k = bdfi_json_next(p->j, k), i++) {
      bdfi_key *key = &m->keys[i];
      if (k->kind == BDFI_JSON_NULL) {
        continue;
      }
      if (k->kind != BDFI_JSON_OBJECT) {
        return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
      }
      if (bdfi_mp_str(p, k, "type", &key->type) < 0 || bdfi_mp_str(p, k, "kdf", &key->kdf) < 0 ||
          bdfi_mp_int(p, k, "iter", &key->iter) < 0 ||
          bdfi_mp_bytes(p, k, "salt", &key->salt, &key->salt_len) < 0 ||
          bdfi_mp_str(p, k, "crv", &key->crv) < 0 ||
          bdfi_mp_bytes(p, k, "epk", &key->epk, &key->epk_len) < 0 ||
          bdfi_mp_bytes(p, k, "key", &key->key, &key->key_len) < 0) {
        return -1;
      }
    }
    m->nkeys = keys->count;
  }
  const bdfi_json_node *sm;
  if (bdfi_mp_object(p, enc, "manifest", &sm) < 0) {
    return -1;
  }
  if (sm && (bdfi_mp_hash(p, sm, "part", m->sealed_manifest, NULL) < 0 ||
             bdfi_mp_str(p, sm, "enc", &m->sealed_manifest_enc) < 0)) {
    return -1;
  }
  return 0;
}

static int bdfi_mp_meta(bdfi_mp *p, const bdfi_json_node *root) {
  const bdfi_json_node *meta;
  if (bdfi_mp_object(p, root, "meta", &meta) < 0 || !meta) {
    return p->err ? -1 : 0;
  }
  bdfi_manifest *m = p->m;
  if (bdfi_mp_str(p, meta, "source", &m->source) < 0 ||
      bdfi_mp_str(p, meta, "generator", &m->generator) < 0) {
    return -1;
  }
  const bdfi_json_node *dc;
  if (bdfi_mp_object(p, meta, "dc", &dc) < 0 || !dc) {
    return p->err ? -1 : 0;
  }
  for (size_t t = 0; t < BDFI_DC_TERMS; t++) {
    const bdfi_json_node *n = bdfi_mp_get(p, dc, bdfi_dc_terms[t]);
    if (!n) {
      continue;
    }
    bdfi_dc *d = &m->dc[t];
    if (n->kind == BDFI_JSON_STRING) {
      d->values = bdfi_mp_alloc(p, 1, sizeof *d->values);
      if (!d->values || bdfi_mp_str_node(p, n, &d->values[0]) < 0) {
        return -1;
      }
      d->n = 1;
    } else if (n->kind == BDFI_JSON_ARRAY) {
      d->values = bdfi_mp_alloc(p, n->count, sizeof *d->values);
      if (n->count && !d->values) {
        return -1;
      }
      size_t i = 0;
      for (const bdfi_json_node *v = bdfi_json_first(p->j, n); v; v = bdfi_json_next(p->j, v), i++) {
        if (v->kind == BDFI_JSON_NULL) {
          if (bdfi_mp_strdup(p, "", 0, &d->values[i]) < 0) {
            return -1;
          }
        } else if (bdfi_mp_str_node(p, v, &d->values[i]) < 0) {
          return bdfi_mp_fail(p, "a Dublin Core value must be a string or an array of strings");
        }
      }
      d->n = n->count;
    } else {
      return bdfi_mp_fail(p, "a Dublin Core value must be a string or an array of strings");
    }
  }
  return 0;
}

static int bdfi_mp_read(bdfi_mp *p) {
  const bdfi_json_node *root = bdfi_json_root(p->j);
  bdfi_manifest *m = p->m;
  if (!root || root->kind != BDFI_JSON_OBJECT) {
    /* encoding/json accepts null for a struct and leaves it empty; check()
     * then rejects version 0, so the outcome is the same */
    if (root && root->kind == BDFI_JSON_NULL) {
      return 0;
    }
    return bdfi_mp_fail(p, "the manifest is not a JSON object");
  }
  if (bdfi_mp_int(p, root, "bdf", &m->bdf) < 0 || bdfi_mp_int(p, root, "opset", &m->opset) < 0 ||
      bdfi_mp_str(p, root, "unit", &m->unit) < 0 || bdfi_mp_meta(p, root) < 0) {
    return -1;
  }
  const bdfi_json_node *seg;
  if (bdfi_mp_object(p, root, "segment", &seg) < 0) {
    return -1;
  }
  if (seg) {
    m->has_segment = 1;
    if (bdfi_mp_str(p, seg, "view", &m->segment_view) < 0 ||
        bdfi_mp_int(p, seg, "from", &m->segment_from) < 0 ||
        bdfi_mp_int(p, seg, "to", &m->segment_to) < 0) {
      return -1;
    }
  }
  const bdfi_json_node *views;
  if (bdfi_mp_array(p, root, "views", &views) < 0) {
    return -1;
  }
  if (views) {
    m->views = bdfi_mp_alloc(p, views->count, sizeof *m->views);
    if (views->count && !m->views) {
      return -1;
    }
    size_t i = 0;
    for (const bdfi_json_node *v = bdfi_json_first(p->j, views); v; v = bdfi_json_next(p->j, v), i++) {
      if (v->kind == BDFI_JSON_NULL) {
        return bdfi_mp_fail(p, "null view"); /* manifest.go: check */
      }
      if (v->kind != BDFI_JSON_OBJECT) {
        return bdfi_mp_fail(p, BDFI_WRONG_TYPE);
      }
      if (bdfi_mp_view(p, v, &m->views[i]) < 0) {
        return -1;
      }
    }
    m->nviews = views->count;
  }
  if (bdfi_mp_parts(p, root) < 0 || bdfi_mp_encryption(p, root) < 0) {
    return -1;
  }
  return 0;
}

/* bdfi_size_ok is check()'s size: 0 <= v <= MaxPageSize (false for NaN). */
static int bdfi_size_ok(float v) { return v >= 0 && v <= (float)BDF_MAX_PAGE_SIZE; }

static float bdfi_fabs(float v) { return v < 0 ? -v : v; }

/* bdfi_mp_check is manifest.go's check. */
static int bdfi_mp_check(bdfi_mp *p) {
  const bdfi_manifest *m = p->m;
  if (m->bdf != BDF_FORMAT_VERSION) {
    return bdfi_mp_fail(p, "unsupported manifest format version");
  }
  if (m->opset < 0 || m->opset > BDF_OPSET_VERSION) {
    return bdfi_mp_fail(p, "unsupported manifest opset");
  }
  for (size_t vi = 0; vi < m->nviews; vi++) {
    const bdfi_view *v = &m->views[vi];
    for (size_t pi = 0; pi < v->npages; pi++) {
      const bdfi_page *pg = &v->pages[pi];
      if (!bdfi_size_ok(pg->w) || !bdfi_size_ok(pg->h)) {
        return bdfi_mp_fail(p, "page size out of range");
      }
      if (pg->has_body && !(bdfi_size_ok(pg->body[2]) && bdfi_size_ok(pg->body[3]) &&
                            bdfi_size_ok(bdfi_fabs(pg->body[0])) &&
                            bdfi_size_ok(bdfi_fabs(pg->body[1])))) {
        return bdfi_mp_fail(p, "page body out of range");
      }
    }
    if (v->has_continuous && !bdfi_size_ok(bdfi_fabs(v->gap))) {
      return bdfi_mp_fail(p, "gap out of range");
    }
    if (!(v->tile <= 0 || (v->tile >= 1 && v->tile <= (float)BDF_MAX_PAGE_SIZE))) {
      return bdfi_mp_fail(p, "tile size out of range");
    }
    for (int axis = 0; axis < 2; axis++) {
      size_t n = axis ? v->nrows : v->ncols;
      float(*runs)[2] = axis ? v->rows : v->cols;
      int64_t total = 0;
      for (size_t i = 0; i < n; i++) {
        float count = runs[i][0], size = runs[i][1];
        if (!(count >= 0 && count <= (float)BDF_MAX_SHEET_ENTRIES) ||
            (float)(int64_t)count != count) {
          return bdfi_mp_fail(p, "bad number of rows or columns");
        }
        total += (int64_t)count;
        if (total > BDF_MAX_SHEET_ENTRIES) {
          return bdfi_mp_fail(p, "too many rows or columns");
        }
        if (!bdfi_size_ok(size) || (size > 0 && size < (float)BDF_MIN_ENTRY_SIZE)) {
          return bdfi_mp_fail(p, "rows or columns of a bad size");
        }
      }
    }
    if (v->has_freeze && (v->freeze_cols < 0 || v->freeze_rows < 0)) {
      return bdfi_mp_fail(p, "bad frozen panes");
    }
  }
  return 0;
}

bdf_status bdfi_manifest_parse(const uint8_t *json, size_t len, bdfi_manifest *m,
                               bdf_error *err) {
  bdfi_error_clear(err);
  memset(m, 0, sizeof *m);
  bdfi_arena_init(&m->arena);
  if (len > BDF_MAX_MANIFEST_SIZE) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "manifest too large");
    return BDF_ERR_FORMAT;
  }
  bdfi_json j;
  bdf_status st = bdfi_json_parse(json, len, &j, err);
  if (st != BDF_OK) {
    return st;
  }
  bdfi_mp p;
  memset(&p, 0, sizeof p);
  p.j = &j;
  p.m = m;
  int rc = bdfi_mp_read(&p);
  if (rc == 0) {
    rc = bdfi_mp_check(&p);
  }
  bdfi_json_free(&j);
  if (rc < 0) {
    bdfi_manifest_free(m);
    st = p.oom ? BDF_ERR_NOMEM : BDF_ERR_FORMAT;
    bdfi_error_set(err, st, 0, p.err);
    return st;
  }
  return BDF_OK;
}

void bdfi_manifest_free(bdfi_manifest *m) {
  bdfi_arena_free(&m->arena);
  memset(m, 0, sizeof *m);
}
