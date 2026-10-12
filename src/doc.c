/*
 * A document: the container (single file or split), its manifest, and its
 * parts, read as reader.go of the reference implementation reads them.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

struct bdf_doc {
  bdfi_manifest m;
  bdf_source src;
  int split;
  uint64_t base;   /* single: where the parts region starts */
  size_t *order;   /* part indices sorted by hash */
  void *mem;       /* the source context bdf_doc_open_memory made */
};

/* ---- A source over memory ---------------------------------------------- */

typedef struct bdfi_memsrc {
  const uint8_t *data;
  size_t len;
} bdfi_memsrc;

static int bdfi_mem_read_at(void *ctx, uint64_t off, uint8_t *buf, size_t len) {
  const bdfi_memsrc *m = ctx;
  if (off > m->len || len > m->len - off) {
    return -1;
  }
  if (len > 0) {
    memcpy(buf, m->data + (size_t)off, len);
  }
  return 0;
}

/* ---- Parts by hash ------------------------------------------------------ */

static int bdfi_cmp_hash(const void *a, const void *b) { return memcmp(a, b, 16); }

/* bdfi_build_order sorts the part indices by hash, keeping the last of equal
 * hashes first, as the reference's map (last entry wins). */
static int bdfi_build_order(bdf_doc *d) {
  size_t n = d->m.nparts;
  if (n == 0) {
    return 0;
  }
  /* sort copies of (hash, index) so that the comparison needs no state */
  typedef struct {
    uint8_t hash[16];
    size_t index;
  } rec;
  rec *recs = malloc(n * sizeof *recs);
  d->order = malloc(n * sizeof *d->order);
  if (!recs || !d->order) {
    free(recs);
    return -1;
  }
  for (size_t i = 0; i < n; i++) {
    memcpy(recs[i].hash, d->m.parts[i].hash, 16);
    recs[i].index = i;
  }
  /* a stable sort by hash: insertion for small n, else qsort then fix ties
   * by index so that the last entry of a hash comes first */
  qsort(recs, n, sizeof *recs, bdfi_cmp_hash);
  for (size_t i = 0; i < n;) {
    size_t j = i + 1;
    while (j < n && memcmp(recs[j].hash, recs[i].hash, 16) == 0) {
      j++;
    }
    /* order the run i..j by descending index */
    for (size_t a = i; a < j; a++) {
      for (size_t b = a + 1; b < j; b++) {
        if (recs[b].index > recs[a].index) {
          rec t = recs[a];
          recs[a] = recs[b];
          recs[b] = t;
        }
      }
    }
    i = j;
  }
  for (size_t i = 0; i < n; i++) {
    d->order[i] = recs[i].index;
  }
  free(recs);
  return 0;
}

static const bdfi_part *bdfi_find_part(const bdf_doc *d, const uint8_t hash[16]) {
  size_t lo = 0, hi = d->m.nparts;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = memcmp(d->m.parts[d->order[mid]].hash, hash, 16);
    if (c < 0) {
      lo = mid + 1;
    } else if (c > 0) {
      hi = mid;
    } else {
      /* the first of equal hashes is the last entry of the manifest */
      while (mid > 0 && memcmp(d->m.parts[d->order[mid - 1]].hash, hash, 16) == 0) {
        mid--;
      }
      return &d->m.parts[d->order[mid]];
    }
  }
  return NULL;
}

int64_t bdfi_doc_part_index(const bdf_doc *d, const uint8_t hash[16]) {
  const bdfi_part *p = d ? bdfi_find_part(d, hash) : NULL;
  return p ? (int64_t)(p - d->m.parts) : -1;
}

/* ---- Opening ------------------------------------------------------------ */

void bdf_doc_free(bdf_doc *d) {
  if (!d) {
    return;
  }
  bdfi_manifest_free(&d->m);
  free(d->order);
  free(d->mem);
  free(d);
}

static bdf_status bdfi_doc_finish(bdf_doc *d, bdf_doc **out, bdf_error *err) {
  if (bdfi_build_order(d) < 0) {
    bdf_doc_free(d);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  *out = d;
  return BDF_OK;
}

bdf_status bdf_doc_open(const bdf_source *src, bdf_doc **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!src || !src->read_at || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdf_doc *d = calloc(1, sizeof *d);
  if (!d) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  d->src = *src;
  uint8_t hdr[BDF_HEADER_SIZE];
  if (src->size < BDF_HEADER_SIZE || src->read_at(src->ctx, 0, hdr, sizeof hdr) < 0) {
    bdf_doc_free(d);
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "short header");
    return BDF_ERR_FORMAT;
  }
  bdf_header h;
  bdf_status st = bdf_header_parse(hdr, sizeof hdr, src->size, &h, err);
  if (st != BDF_OK) {
    bdf_doc_free(d);
    return st;
  }
  /* the manifest: at most 256 MiB and within the file, both checked above */
  size_t mlen = (size_t)h.manifest_len;
  uint8_t *mj = malloc(mlen ? mlen : 1);
  if (!mj) {
    bdf_doc_free(d);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  if (src->read_at(src->ctx, h.manifest_off, mj, mlen) < 0) {
    free(mj);
    bdf_doc_free(d);
    bdfi_error_set(err, BDF_ERR_FORMAT, (size_t)h.manifest_off, "cannot read the manifest");
    return BDF_ERR_FORMAT;
  }
  if (h.manifest_enc == 1) {
    uint8_t *plain;
    size_t plain_len;
    st = bdf_inflate(mj, mlen, BDF_MAX_MANIFEST_SIZE, &plain, &plain_len, err);
    free(mj);
    if (st != BDF_OK) {
      bdf_doc_free(d);
      return st;
    }
    mj = plain;
    mlen = plain_len;
  }
  st = bdfi_manifest_parse(mj, mlen, &d->m, err);
  free(mj);
  if (st != BDF_OK) {
    bdf_doc_free(d);
    return st;
  }
  if (((h.flags & BDF_FLAG_ENCRYPTED) != 0) != (d->m.has_encryption != 0)) {
    bdf_doc_free(d);
    bdfi_error_set(err, BDF_ERR_FORMAT, 6, "encryption flag does not match the manifest");
    return BDF_ERR_FORMAT;
  }
  d->base = h.manifest_off + h.manifest_len;
  return bdfi_doc_finish(d, out, err);
}

bdf_status bdf_doc_open_memory(const uint8_t *data, size_t len, bdf_doc **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  bdfi_memsrc *mem = malloc(sizeof *mem);
  if (!mem) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  mem->data = data;
  mem->len = len;
  bdf_source src;
  memset(&src, 0, sizeof src);
  src.ctx = mem;
  src.read_at = bdfi_mem_read_at;
  src.size = len;
  bdf_status st = bdf_doc_open(&src, out, err);
  if (st != BDF_OK) {
    free(mem);
    return st;
  }
  (*out)->mem = mem;
  return BDF_OK;
}

bdf_status bdf_doc_open_split(const uint8_t *manifest, size_t len, const bdf_source *src,
                              bdf_doc **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || (!manifest && len > 0) || !src || !src->read_part) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  bdf_doc *d = calloc(1, sizeof *d);
  if (!d) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  d->src = *src;
  d->split = 1;
  bdf_status st = bdfi_manifest_parse(manifest, len, &d->m, err);
  if (st != BDF_OK) {
    bdf_doc_free(d);
    return st;
  }
  return bdfi_doc_finish(d, out, err);
}

/* ---- Parts -------------------------------------------------------------- */

static bdf_status bdfi_doc_fail(bdf_error *err, const char *msg) {
  bdfi_error_set(err, BDF_ERR_FORMAT, 0, msg);
  return BDF_ERR_FORMAT;
}
#define bdfi_fail bdfi_doc_fail

/* bdfi_load_stored fetches the stored bytes of a part as the reference's
 * load functions do: within the file for the single form, exactly the
 * stated length for the split form. */
static bdf_status bdfi_load_stored(const bdf_doc *d, const bdfi_part *e, uint8_t **out,
                                   size_t *out_len, bdf_error *err) {
  if (e->len < 0) {
    return bdfi_fail(err, d->split ? "negative stored size" : "part out of range");
  }
  if ((uint64_t)e->len > SIZE_MAX) {
    /* a 32-bit build cannot hold it, whatever the file says */
    return bdfi_fail(err, "part out of range");
  }
  size_t len = (size_t)e->len;
  if (d->split) {
    char name[33];
    bdf_hash_format(e->hash, name);
    size_t actual = 0;
    if (d->src.read_part(d->src.ctx, name, NULL, 0, &actual) < 0) {
      return bdfi_fail(err, "cannot read the part");
    }
    if (actual > len) {
      return bdfi_fail(err, "stored data exceeds its size limit");
    }
    if (actual != len) {
      return bdfi_fail(err, "part is not its stated length");
    }
  } else {
    uint64_t avail = d->src.size - d->base;
    if (e->off < 0 || (uint64_t)e->off > avail || (uint64_t)len > avail - (uint64_t)e->off) {
      return bdfi_fail(err, "part out of range");
    }
  }
  uint8_t *buf = malloc(len ? len : 1);
  if (!buf) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  int rc;
  if (d->split) {
    char name[33];
    size_t got = 0;
    bdf_hash_format(e->hash, name);
    rc = d->src.read_part(d->src.ctx, name, buf, len, &got);
    if (rc == 0 && got != len) {
      rc = -1;
    }
  } else {
    rc = d->src.read_at(d->src.ctx, d->base + (uint64_t)e->off, buf, len);
  }
  if (rc < 0) {
    free(buf);
    return bdfi_fail(err, "cannot read the part");
  }
  *out = buf;
  *out_len = len;
  return BDF_OK;
}

static int bdfi_str_is(const bdfi_str *s, const char *lit) {
  return s->p ? strcmp(s->p, lit) == 0 : lit[0] == '\0';
}

bdf_status bdf_doc_part(const bdf_doc *d, const uint8_t hash[16], uint8_t **out, size_t *out_len,
                        bdf_error *err) {
  bdfi_error_clear(err);
  if (!d || !hash || !out || !out_len) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  const bdfi_part *e = bdfi_find_part(d, hash);
  if (!e) {
    return bdfi_fail(err, "unknown part");
  }
  uint8_t *stored = NULL;
  size_t stored_len = 0;
  bdf_status st = bdfi_load_stored(d, e, &stored, &stored_len, err);
  if (st != BDF_OK) {
    return st;
  }
  if (bdfi_str_is(&e->enc, "") || bdfi_str_is(&e->enc, "identity")) {
    *out = stored;
    *out_len = stored_len;
    return BDF_OK;
  }
  if (!bdfi_str_is(&e->enc, "deflate-raw")) {
    free(stored);
    return bdfi_fail(err, "unknown encoding");
  }
  if (e->size < 0) {
    free(stored);
    return bdfi_fail(err, "negative part size");
  }
  /* the limit is the stated size; more than the address space holds is
   * bounded by the address space (a 32-bit build) */
  size_t limit = (uint64_t)e->size > SIZE_MAX ? SIZE_MAX : (size_t)e->size;
  st = bdf_inflate(stored, stored_len, limit, out, out_len, err);
  free(stored);
  return st;
}

/* ---- Accessors ---------------------------------------------------------- */

int bdf_doc_encrypted(const bdf_doc *d) { return d ? d->m.has_encryption : 0; }
int bdf_doc_locked(const bdf_doc *d) { return d ? d->m.has_encryption : 0; }
int bdf_doc_split(const bdf_doc *d) { return d ? d->split : 0; }
int64_t bdf_doc_opset(const bdf_doc *d) { return d ? d->m.opset : 0; }

static const char *bdfi_str_out(const bdfi_str *s, size_t *len) {
  if (len) {
    *len = s->n;
  }
  return s->p ? s->p : "";
}

const char *bdf_doc_unit(const bdf_doc *d, size_t *len) {
  static const bdfi_str none = {NULL, 0};
  return bdfi_str_out(d ? &d->m.unit : &none, len);
}

void bdf_doc_meta(const bdf_doc *d, bdf_meta *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof *out);
  if (!d) {
    out->source = out->generator = "";
    return;
  }
  out->source = bdfi_str_out(&d->m.source, &out->source_len);
  out->generator = bdfi_str_out(&d->m.generator, &out->generator_len);
}

static int bdfi_dc_index(const char *element) {
  if (!element) {
    return -1;
  }
  for (int i = 0; i < BDFI_DC_TERMS; i++) {
    if (strcmp(bdfi_dc_term((size_t)i), element) == 0) {
      return i;
    }
  }
  return -1;
}

size_t bdf_doc_dc_count(const bdf_doc *d, const char *element) {
  int i = bdfi_dc_index(element);
  return d && i >= 0 ? d->m.dc[i].n : 0;
}

const char *bdf_doc_dc(const bdf_doc *d, const char *element, size_t i, size_t *len) {
  int t = bdfi_dc_index(element);
  if (!d || t < 0 || i >= d->m.dc[t].n) {
    if (len) {
      *len = 0;
    }
    return NULL;
  }
  return bdfi_str_out(&d->m.dc[t].values[i], len);
}

int bdf_doc_segment(const bdf_doc *d, bdf_segment *out) {
  if (!d || !out || !d->m.has_segment) {
    return 0;
  }
  out->view = bdfi_str_out(&d->m.segment_view, &out->view_len);
  out->from = d->m.segment_from;
  out->to = d->m.segment_to;
  return 1;
}

size_t bdf_doc_view_count(const bdf_doc *d) { return d ? d->m.nviews : 0; }

static bdf_view_kind bdfi_view_kind(const bdfi_str *kind) {
  if (bdfi_str_is(kind, "fixed")) {
    return BDF_VIEW_FIXED;
  }
  if (bdfi_str_is(kind, "flow")) {
    return BDF_VIEW_FLOW;
  }
  if (bdfi_str_is(kind, "sheet")) {
    return BDF_VIEW_SHEET;
  }
  if (bdfi_str_is(kind, "scroll")) {
    return BDF_VIEW_SCROLL;
  }
  return BDF_VIEW_UNKNOWN;
}

bdf_status bdf_doc_view(const bdf_doc *d, size_t i, bdf_view *out) {
  if (!d || !out || i >= d->m.nviews) {
    return BDF_ERR_ARG;
  }
  const bdfi_view *v = &d->m.views[i];
  memset(out, 0, sizeof *out);
  out->id = bdfi_str_out(&v->id, &out->id_len);
  out->kind = bdfi_str_out(&v->kind, &out->kind_len);
  out->kind_code = bdfi_view_kind(&v->kind);
  out->title = bdfi_str_out(&v->title, &out->title_len);
  out->direction = bdfi_str_out(&v->direction, &out->direction_len);
  out->rtl = bdfi_str_is(&v->direction, "rtl");
  out->text_index = bdfi_str_out(&v->text_index, &out->text_index_len);
  out->npages = v->npages;
  out->has_continuous = v->has_continuous;
  out->gap = v->gap;
  out->tile_stated = v->tile;
  out->tile = v->tile > 0 ? v->tile : (float)BDF_DEFAULT_TILE;
  out->ncols = v->ncols;
  out->nrows = v->nrows;
  out->has_freeze = v->has_freeze;
  out->freeze_cols = v->freeze_cols;
  out->freeze_rows = v->freeze_rows;
  out->gridlines = v->gridlines;
  out->ntiles = v->ntiles;
  out->tiles_ref = bdfi_str_out(&v->tiles_ref, &out->tiles_ref_len);
  out->has_play = v->has_play;
  out->seq = bdfi_str_out(&v->seq, &out->seq_len);
  out->audio = bdfi_str_out(&v->audio, &out->audio_len);
  out->audio_type = bdfi_str_out(&v->audio_type, &out->audio_type_len);
  out->cues = bdfi_str_out(&v->cues, &out->cues_len);
  return BDF_OK;
}

int64_t bdf_doc_view_find(const bdf_doc *d, const char *id, size_t len) {
  if (!d || (!id && len > 0)) {
    return -1;
  }
  for (size_t i = 0; i < d->m.nviews; i++) {
    const bdfi_str *vid = &d->m.views[i].id;
    if (vid->n == len && (len == 0 || memcmp(vid->p, id, len) == 0)) {
      return (int64_t)i;
    }
  }
  return -1;
}

bdf_status bdf_doc_view_run(const bdf_doc *d, size_t view, int rows, size_t i, float out[2]) {
  if (!d || !out || view >= d->m.nviews) {
    return BDF_ERR_ARG;
  }
  const bdfi_view *v = &d->m.views[view];
  size_t n = rows ? v->nrows : v->ncols;
  if (i >= n) {
    return BDF_ERR_ARG;
  }
  const float *run = rows ? v->rows[i] : v->cols[i];
  out[0] = run[0];
  out[1] = run[1];
  return BDF_OK;
}

bdf_status bdf_doc_page(const bdf_doc *d, size_t view, size_t page, bdf_page *out) {
  if (!d || !out || view >= d->m.nviews || page >= d->m.views[view].npages) {
    return BDF_ERR_ARG;
  }
  const bdfi_page *p = &d->m.views[view].pages[page];
  out->w = p->w;
  out->h = p->h;
  out->has_body = p->has_body;
  memcpy(out->body, p->body, sizeof out->body);
  out->nlayers = p->nlayers;
  return BDF_OK;
}

bdf_status bdf_doc_layer(const bdf_doc *d, size_t view, size_t page, size_t layer,
                         bdf_layer *out) {
  if (!d || !out || view >= d->m.nviews || page >= d->m.views[view].npages ||
      layer >= d->m.views[view].pages[page].nlayers) {
    return BDF_ERR_ARG;
  }
  const bdfi_layer *l = &d->m.views[view].pages[page].layers[layer];
  out->role = bdfi_str_out(&l->role, &out->role_len);
  out->obj = l->obj;
  return BDF_OK;
}

bdf_status bdf_doc_tile(const bdf_doc *d, size_t view, size_t i, bdf_tile *out) {
  if (!d || !out || view >= d->m.nviews || i >= d->m.views[view].ntiles) {
    return BDF_ERR_ARG;
  }
  const bdfi_tile *t = &d->m.views[view].tiles[i];
  out->key = bdfi_str_out(&t->key, &out->key_len);
  out->obj = bdfi_str_out(&t->obj, &out->obj_len);
  return BDF_OK;
}

int bdf_tile_key_parse(const char *key, size_t len, uint32_t *tx, uint32_t *ty) {
  uint64_t v[2] = {0, 0};
  size_t i = 0;
  for (int k = 0; k < 2; k++) {
    size_t start = i;
    while (i < len && key[i] >= '0' && key[i] <= '9') {
      if (v[k] > (UINT32_MAX - (uint64_t)(key[i] - '0')) / 10) {
        return 0;
      }
      v[k] = v[k] * 10 + (uint64_t)(key[i] - '0');
      i++;
    }
    if (i == start) {
      return 0;
    }
    if (k == 0) {
      if (i >= len || key[i] != ',') {
        return 0;
      }
      i++;
    }
  }
  if (i != len) {
    return 0;
  }
  *tx = (uint32_t)v[0];
  *ty = (uint32_t)v[1];
  return 1;
}

size_t bdf_doc_part_count(const bdf_doc *d) { return d ? d->m.nparts : 0; }

static void bdfi_part_out(const bdfi_part *p, bdf_part_entry *out) {
  out->hash = p->hash;
  out->type = bdfi_str_out(&p->type, &out->type_len);
  out->enc_name = bdfi_str_out(&p->enc, &out->enc_name_len);
  if (bdfi_str_is(&p->enc, "") || bdfi_str_is(&p->enc, "identity")) {
    out->enc = BDF_ENC_IDENTITY;
  } else if (bdfi_str_is(&p->enc, "deflate-raw")) {
    out->enc = BDF_ENC_DEFLATE_RAW;
  } else {
    out->enc = BDF_ENC_UNKNOWN;
  }
  out->len = p->len;
  out->size = p->size;
  out->off = p->off;
  out->sealed = p->has_sealed ? p->sealed : NULL;
}

bdf_status bdf_doc_part_entry(const bdf_doc *d, size_t i, bdf_part_entry *out) {
  if (!d || !out || i >= d->m.nparts) {
    return BDF_ERR_ARG;
  }
  bdfi_part_out(&d->m.parts[i], out);
  return BDF_OK;
}

int bdf_doc_part_find(const bdf_doc *d, const uint8_t hash[16], bdf_part_entry *out) {
  if (!d || !hash) {
    return 0;
  }
  const bdfi_part *p = bdfi_find_part(d, hash);
  if (!p) {
    return 0;
  }
  if (out) {
    bdfi_part_out(p, out);
  }
  return 1;
}

const char *bdf_doc_cipher(const bdf_doc *d, size_t *len) {
  static const bdfi_str none = {NULL, 0};
  return bdfi_str_out(d && d->m.has_encryption ? &d->m.cipher : &none, len);
}

size_t bdf_doc_key_count(const bdf_doc *d) { return d ? d->m.nkeys : 0; }

bdf_status bdf_doc_key(const bdf_doc *d, size_t i, bdf_key_slot *out) {
  if (!d || !out || i >= d->m.nkeys) {
    return BDF_ERR_ARG;
  }
  const bdfi_key *k = &d->m.keys[i];
  out->type = bdfi_str_out(&k->type, &out->type_len);
  out->kdf = bdfi_str_out(&k->kdf, &out->kdf_len);
  out->crv = bdfi_str_out(&k->crv, &out->crv_len);
  out->iter = k->iter;
  out->salt = k->salt;
  out->salt_len = k->salt_len;
  out->epk = k->epk;
  out->epk_len = k->epk_len;
  out->key = k->key;
  out->key_len = k->key_len;
  return BDF_OK;
}

const uint8_t *bdf_doc_sealed_manifest(const bdf_doc *d, const char **enc, size_t *enc_len) {
  if (!d || !d->m.has_encryption) {
    if (enc) {
      *enc = "";
    }
    if (enc_len) {
      *enc_len = 0;
    }
    return NULL;
  }
  if (enc) {
    size_t n;
    *enc = bdfi_str_out(&d->m.sealed_manifest_enc, &n);
    if (enc_len) {
      *enc_len = n;
    }
  }
  return d->m.sealed_manifest;
}
