/*
 * A document: the container (single file or split), its manifest, and its
 * parts, read as reader.go of the reference implementation reads them.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

struct bdf_doc {
  bdfi_manifest m; /* the manifest the document is read with: once unlocked, the sealed one */
  bdf_source src;
  int split;
  uint64_t base; /* single: where the parts region starts */
  size_t *order; /* the indices of m.parts sorted by name */
  void *mem;     /* the source context bdf_doc_open_memory made */
  /* an encrypted document (spec §3.5) */
  int encrypted;       /* the stored manifest has an encryption block */
  int unlocked;
  bdfi_manifest outer; /* unlocked: the stored manifest, which names the sealed parts */
  size_t *outer_order;
  bdfi_gcm *gcm; /* unlocked: the content key */
  /* the segments put into the document (spec §3.6), which it owns */
  int owned; /* a segment of another document */
  bdf_doc **segments;
  size_t nsegments, cap_segments;
  size_t total_parts; /* the parts of m and of the segments */
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

static int bdfi_order_less(const bdfi_manifest *m, size_t a, size_t b) {
  int c = memcmp(m->parts[a].hash, m->parts[b].hash, 16);
  return c < 0 || (c == 0 && a > b);
}

static void bdfi_order_down(const bdfi_manifest *m, size_t *order, size_t at, size_t n) {
  size_t value = order[at];
  while (at < n / 2) {
    size_t child = 2 * at + 1;
    if (child + 1 < n && bdfi_order_less(m, order[child], order[child + 1])) {
      child++;
    }
    if (!bdfi_order_less(m, value, order[child])) {
      break;
    }
    order[at] = order[child];
    at = child;
  }
  order[at] = value;
}

/* Sort only the indices, without copying every 16-byte name. Heapsort
 * needs constant scratch and O(n log n) comparisons even for hostile
 * input. Equal names are ordered by descending index: the last wins. */
static size_t *bdfi_order_of(const bdfi_manifest *m, int *oom) {
  size_t n = m->nparts;
  if (n == 0) {
    return NULL;
  }
  size_t *order = bdfi_malloc_array(n, sizeof *order);
  if (!order) {
    *oom = 1;
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    order[i] = i;
  }
  for (size_t i = n / 2; i > 0; i--) {
    bdfi_order_down(m, order, i - 1, n);
  }
  for (size_t end = n - 1; end > 0; end--) {
    size_t tmp = order[end];
    order[end] = order[0];
    order[0] = tmp;
    bdfi_order_down(m, order, 0, end);
  }
  return order;
}

static const bdfi_part *bdfi_find_in(const bdfi_manifest *m, const size_t *order,
                                     const uint8_t hash[16]) {
  size_t lo = 0, hi = m->nparts;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = memcmp(m->parts[order[mid]].hash, hash, 16);
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  /* Lower bound also handles repeated names in logarithmic time. */
  return lo < m->nparts && memcmp(m->parts[order[lo]].hash, hash, 16) == 0
             ? &m->parts[order[lo]] : NULL;
}

/* bdfi_lookup finds a part in a document and then in its segments: the
 * entry, the document that holds it, and its index among all the parts. */
static const bdfi_part *bdfi_lookup(const bdf_doc *d, const uint8_t hash[16], const bdf_doc **owner,
                                    size_t *index) {
  const bdfi_part *p = bdfi_find_in(&d->m, d->order, hash);
  if (p) {
    *owner = d;
    *index = (size_t)(p - d->m.parts);
    return p;
  }
  size_t offset = d->m.nparts;
  for (size_t i = 0; i < d->nsegments; i++) {
    size_t at;
    p = bdfi_lookup(d->segments[i], hash, owner, &at);
    if (p) {
      *index = offset + at;
      return p;
    }
    offset += d->segments[i]->total_parts;
  }
  return NULL;
}

int64_t bdfi_doc_part_index(const bdf_doc *d, const uint8_t hash[16]) {
  const bdf_doc *owner;
  size_t index;
  return d && bdfi_lookup(d, hash, &owner, &index) ? (int64_t)index : -1;
}

/* ---- Opening ------------------------------------------------------------ */

void bdf_doc_free(bdf_doc *d) {
  if (!d) {
    return;
  }
  for (size_t i = 0; i < d->nsegments; i++) {
    bdf_doc_free(d->segments[i]);
  }
  free(d->segments);
  bdfi_manifest_free(&d->m);
  free(d->order);
  if (d->unlocked) {
    bdfi_manifest_free(&d->outer);
    free(d->outer_order);
  }
  if (d->gcm) {
    bdfi_wipe(d->gcm, sizeof *d->gcm);
    free(d->gcm);
  }
  free(d->mem);
  free(d);
}

static bdf_status bdfi_doc_finish(bdf_doc *d, bdf_doc **out, bdf_error *err) {
  int oom = 0;
  d->order = bdfi_order_of(&d->m, &oom);
  if (oom) {
    bdf_doc_free(d);
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  d->encrypted = d->m.has_encryption;
  d->total_parts = d->m.nparts;
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
  if (len > BDF_MAX_PART_SIZE) {
    bdfi_error_set(err, BDF_ERR_TOO_LARGE, 0, "stored part too large");
    return BDF_ERR_TOO_LARGE;
  }
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

#define BDFI_NONCE_SIZE 12
#define BDFI_TAG_SIZE   16

/* bdfi_open_sealed opens what was sealed with the content key: the nonce,
 * the ciphertext, the tag (spec §3.5). The result is the caller's. */
static bdf_status bdfi_open_sealed(const bdfi_gcm *g, uint8_t *sealed, size_t len,
                                   const uint8_t *aad, size_t aad_len, uint8_t **out,
                                   size_t *out_len, bdf_error *err) {
  if (len < BDFI_NONCE_SIZE + BDFI_TAG_SIZE) {
    return bdfi_fail(err, "sealed part too short");
  }
  size_t n = len - BDFI_NONCE_SIZE - BDFI_TAG_SIZE;
  /* Authentication completes before GCM writes anything. The destination
   * precedes the ciphertext, so the forward decryption can reuse this
   * allocation, including for an empty plaintext. Ownership transfers
   * only on success. */
  if (bdfi_gcm_open(g, sealed, aad, aad_len, sealed + BDFI_NONCE_SIZE, n,
                    sealed + BDFI_NONCE_SIZE + n, sealed) < 0) {
    return bdfi_fail(err, "sealed part fails authentication");
  }
  *out = sealed;
  *out_len = n;
  return BDF_OK;
}

/* bdfi_decode_stored decodes stored bytes of an encoding, which decode to
 * limit bytes at most; it takes the stored bytes over. */
static bdf_status bdfi_decode_stored(uint8_t *stored, size_t stored_len, const bdfi_str *enc,
                                     int64_t limit, uint8_t **out, size_t *out_len,
                                     bdf_error *err) {
  if (bdfi_str_is(enc, "") || bdfi_str_is(enc, "identity")) {
    *out = stored;
    *out_len = stored_len;
    return BDF_OK;
  }
  if (!bdfi_str_is(enc, "deflate-raw")) {
    free(stored);
    return bdfi_fail(err, "unknown encoding");
  }
  if (limit < 0) {
    free(stored);
    return bdfi_fail(err, "negative part size");
  }
  if ((uint64_t)limit > BDF_MAX_PART_SIZE) {
    free(stored);
    bdfi_error_set(err, BDF_ERR_TOO_LARGE, 0, "decoded part too large");
    return BDF_ERR_TOO_LARGE;
  }
  /* more than the address space holds is bounded by the address space (a
   * 32-bit build) */
  size_t max = (uint64_t)limit > SIZE_MAX ? SIZE_MAX : (size_t)limit;
  bdf_status st = bdf_inflate(stored, stored_len, max, out, out_len, err);
  free(stored);
  return st;
}

bdf_status bdf_doc_part(const bdf_doc *doc, const uint8_t hash[16], uint8_t **out,
                        size_t *out_len, bdf_error *err) {
  bdfi_error_clear(err);
  if (!doc || !hash || !out || !out_len) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  const bdf_doc *d;
  size_t index;
  const bdfi_part *e = bdfi_lookup(doc, hash, &d, &index);
  if (!e) {
    return bdfi_fail(err, "unknown part");
  }
  if (bdfi_str_is(&e->enc, "deflate-raw") && e->size > 0 &&
      (uint64_t)e->size > BDF_MAX_PART_SIZE) {
    bdfi_error_set(err, BDF_ERR_TOO_LARGE, 0, "decoded part too large");
    return BDF_ERR_TOO_LARGE;
  }
  uint8_t *stored = NULL;
  size_t stored_len = 0;
  bdf_status st;
  if (d->unlocked) {
    /* the part is sealed in a part of the stored manifest */
    const bdfi_part *sealed = bdfi_find_in(&d->outer, d->outer_order, e->sealed);
    if (!sealed) {
      return bdfi_fail(err, "sealed part missing");
    }
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    st = bdfi_load_stored(d, sealed, &raw, &raw_len, err);
    if (st != BDF_OK) {
      return st;
    }
    st = bdfi_open_sealed(d->gcm, raw, raw_len, e->hash, 16, &stored, &stored_len, err);
    if (st != BDF_OK) {
      free(raw);
    }
  } else {
    st = bdfi_load_stored(d, e, &stored, &stored_len, err);
  }
  if (st != BDF_OK) {
    return st;
  }
  return bdfi_decode_stored(stored, stored_len, &e->enc, e->size, out, out_len, err);
}

/* ---- Unlocking (spec §3.5) ---------------------------------------------- */

#define BDFI_KEY_SIZE       32
#define BDFI_MAX_ITERATIONS BDF_MAX_KDF_ITERATIONS
#define BDFI_MAX_ECDH_SLOTS 16

/* bdfi_unlock_with opens the sealed manifest with the content key and makes
 * it the manifest of the document. Nothing changes when it fails. */
static bdf_status bdfi_unlock_with(bdf_doc *d, const uint8_t key[BDFI_KEY_SIZE], bdf_error *err) {
  bdfi_gcm *gcm = malloc(sizeof *gcm);
  if (!gcm) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  bdfi_gcm_init(gcm, key);
  bdfi_manifest inner;
  size_t *inner_order = NULL;
  uint8_t *raw = NULL, *plain = NULL, *json = NULL;
  size_t raw_len = 0, plain_len = 0, json_len = 0;
  int have_inner = 0;
  bdf_status st;
  const bdfi_part *e = bdfi_find_in(&d->m, d->order, d->m.sealed_manifest);
  if (!e) {
    st = bdfi_fail(err, "sealed manifest part missing");
    goto fail;
  }
  if (e->len < 0 || e->len > (int64_t)BDF_MAX_MANIFEST_SIZE + BDFI_NONCE_SIZE + BDFI_TAG_SIZE) {
    st = bdfi_fail(err, "sealed manifest size out of range");
    goto fail;
  }
  st = bdfi_load_stored(d, e, &raw, &raw_len, err);
  if (st != BDF_OK) {
    goto fail;
  }
  st = bdfi_open_sealed(gcm, raw, raw_len, (const uint8_t *)"manifest", 8, &plain, &plain_len, err);
  if (st == BDF_OK) {
    raw = NULL; /* plain owns the allocation now */
  }
  free(raw);
  if (st != BDF_OK) {
    goto fail;
  }
  st = bdfi_decode_stored(plain, plain_len, &d->m.sealed_manifest_enc, BDF_MAX_MANIFEST_SIZE, &json,
                          &json_len, err);
  if (st != BDF_OK) {
    goto fail;
  }
  st = bdfi_manifest_parse(json, json_len, &inner, err);
  bdfi_wipe(json, json_len);
  bdf_free(json);
  if (st != BDF_OK) {
    goto fail;
  }
  have_inner = 1;
  if (inner.has_encryption) {
    st = bdfi_fail(err, "sealed manifest is encrypted again");
    goto fail;
  }
  for (size_t i = 0; i < inner.nparts; i++) {
    if (!bdfi_find_in(&d->m, d->order, inner.parts[i].sealed)) {
      st = bdfi_fail(err, "sealed part missing");
      goto fail;
    }
  }
  {
    int oom = 0;
    inner_order = bdfi_order_of(&inner, &oom);
    if (oom) {
      st = BDF_ERR_NOMEM;
      bdfi_error_set(err, st, 0, "out of memory");
      goto fail;
    }
  }
  d->outer = d->m;
  d->outer_order = d->order;
  d->m = inner;
  d->order = inner_order;
  d->gcm = gcm;
  d->unlocked = 1;
  d->total_parts = inner.nparts;
  return BDF_OK;
fail:
  if (have_inner) {
    bdfi_manifest_free(&inner);
  }
  bdfi_wipe(gcm, sizeof *gcm);
  free(gcm);
  return st;
}

/* bdfi_unlockable checks that a document waits for a key, and its cipher. */
static bdf_status bdfi_unlockable(const bdf_doc *d, bdf_error *err) {
  if (!d->encrypted || d->unlocked) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "the document is not locked");
    return BDF_ERR_ARG;
  }
  if (!bdfi_str_is(&d->m.cipher, "A256GCM")) {
    return bdfi_fail(err, "unknown cipher");
  }
  return BDF_OK;
}

bdf_status bdf_doc_unlock(bdf_doc *d, const char *password, size_t len, bdf_error *err) {
  bdfi_error_clear(err);
  if (!d || (!password && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  bdf_status st = bdfi_unlockable(d, err);
  if (st != BDF_OK) {
    return st;
  }
  char *pw;
  size_t pw_len;
  if (bdfi_nfkc(password ? password : "", len, &pw, &pw_len) < 0) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  uint8_t kek[BDFI_KEY_SIZE], key[BDFI_KEY_SIZE];
  int found = 0;
  int64_t spent = 0;
  st = BDF_OK;
  for (size_t i = 0; i < d->m.nkeys && !found; i++) {
    const bdfi_key *s = &d->m.keys[i];
    if (!bdfi_str_is(&s->type, "password")) {
      continue;
    }
    if (!bdfi_str_is(&s->kdf, "PBKDF2-SHA256")) {
      st = bdfi_fail(err, "unknown key derivation");
      break;
    }
    if (s->iter < 1 || s->iter > BDFI_MAX_ITERATIONS) {
      st = bdfi_fail(err, "iteration count out of range");
      break;
    }
    /* each slot is within the range, but many of them are not */
    spent += s->iter;
    if (spent > BDFI_MAX_ITERATIONS) {
      st = bdfi_fail(err, "the key slots take too many iterations");
      break;
    }
    bdfi_pbkdf2_sha256(pw, pw_len, s->salt, s->salt_len, (uint32_t)s->iter, kek);
    found = s->key_len == BDFI_KEY_SIZE + 8 && bdfi_aes_kw_unwrap(kek, s->key, s->key_len, key) == 0;
  }
  bdfi_wipe(pw, pw_len);
  free(pw);
  bdfi_wipe(kek, sizeof kek);
  if (st == BDF_OK && !found) {
    st = BDF_ERR_PASSWORD;
    bdfi_error_set(err, st, 0, "wrong password");
  }
  if (st == BDF_OK) {
    st = bdfi_unlock_with(d, key, err);
  }
  bdfi_wipe(key, sizeof key);
  return st;
}

bdf_status bdf_doc_unlock_ecdh(bdf_doc *d, const uint8_t private_key[32], bdf_error *err) {
  bdfi_error_clear(err);
  if (!d || !private_key) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  bdf_status st = bdfi_unlockable(d, err);
  if (st != BDF_OK) {
    return st;
  }
  uint8_t rpk[65];
  if (bdfi_p256_public(private_key, rpk) < 0) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "the key is not a P-256 private key");
    return BDF_ERR_ARG;
  }
  uint8_t z[32], salt[32], kek[BDFI_KEY_SIZE], key[BDFI_KEY_SIZE];
  int found = 0, tried = 0;
  for (size_t i = 0; i < d->m.nkeys && !found; i++) {
    const bdfi_key *s = &d->m.keys[i];
    if (!bdfi_str_is(&s->type, "ecdh")) {
      continue;
    }
    if (++tried > BDFI_MAX_ECDH_SLOTS) {
      st = bdfi_fail(err, "too many ecdh key slots");
      break;
    }
    if (!bdfi_str_is(&s->crv, "P-256")) {
      st = bdfi_fail(err, "unknown curve");
      break;
    }
    if (!bdfi_str_is(&s->kdf, "HKDF-SHA256")) {
      st = bdfi_fail(err, "unknown key derivation");
      break;
    }
    if (s->epk_len != 65 || bdfi_p256_ecdh(private_key, s->epk, z) != 0) {
      st = bdfi_fail(err, "bad public key in an ecdh key slot");
      break;
    }
    /* the key that wraps the content key binds both public keys */
    bdfi_sha256 h;
    bdfi_sha256_init(&h);
    bdfi_sha256_update(&h, s->epk, 65);
    bdfi_sha256_update(&h, rpk, 65);
    bdfi_sha256_final(&h, salt);
    bdfi_hkdf_sha256(z, sizeof z, salt, sizeof salt, "bdf ecdh v1", 11, kek);
    found = s->key_len == BDFI_KEY_SIZE + 8 && bdfi_aes_kw_unwrap(kek, s->key, s->key_len, key) == 0;
  }
  bdfi_wipe(z, sizeof z);
  bdfi_wipe(kek, sizeof kek);
  if (st == BDF_OK && !found) {
    st = BDF_ERR_PASSWORD;
    bdfi_error_set(err, st, 0, "the document is not sealed for this key");
  }
  if (st == BDF_OK) {
    st = bdfi_unlock_with(d, key, err);
  }
  bdfi_wipe(key, sizeof key);
  return st;
}

bdf_status bdf_p256_public_key(const uint8_t private_key[32], uint8_t out[65]) {
  if (!private_key || !out || bdfi_p256_public(private_key, out) < 0) {
    return BDF_ERR_ARG;
  }
  return BDF_OK;
}

/* ---- Segments (spec §3.6) ----------------------------------------------- */

static int64_t bdfi_view_by_id(const bdfi_manifest *m, const bdfi_str *id) {
  for (size_t i = 0; i < m->nviews; i++) {
    const bdfi_str *v = &m->views[i].id;
    if (v->n == id->n && (id->n == 0 || memcmp(v->p, id->p, id->n) == 0)) {
      return (int64_t)i;
    }
  }
  return -1;
}

bdf_status bdf_doc_add_segment(bdf_doc *d, bdf_doc *seg, bdf_error *err) {
  bdfi_error_clear(err);
  if (!d || !seg || d == seg || bdf_doc_locked(d) || d->owned || seg->owned) {
    /* a document another one owns counts among its parts: it takes no more,
     * and is not given away again */
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  if (!seg->m.has_segment) {
    /* a locked document shows no segment: unlock it first */
    bdfi_error_set(err, BDF_ERR_ARG, 0, "not a segment document");
    return BDF_ERR_ARG;
  }
  /* the same views, with the same pages: else it is of another document */
  int same = d->m.nviews == seg->m.nviews;
  for (size_t i = 0; same && i < d->m.nviews; i++) {
    const bdfi_view *a = &d->m.views[i], *b = &seg->m.views[i];
    same = a->id.n == b->id.n && (a->id.n == 0 || memcmp(a->id.p, b->id.p, a->id.n) == 0) &&
           a->npages == b->npages;
  }
  if (!same) {
    return bdfi_fail(err, "the segment is of another document");
  }
  int64_t dst = bdfi_view_by_id(&d->m, &seg->m.segment_view);
  int64_t src = bdfi_view_by_id(&seg->m, &seg->m.segment_view);
  if (dst < 0 || src < 0) {
    return bdfi_fail(err, "the segment names a view that is not there");
  }
  int64_t from = seg->m.segment_from, to = seg->m.segment_to;
  if (from < 0 || from >= to || (uint64_t)to > d->m.views[dst].npages) {
    return bdfi_fail(err, "segment of pages out of range");
  }
  if (d->nsegments == d->cap_segments) {
    size_t cap = d->cap_segments ? d->cap_segments * 2 : 4;
    bdf_doc **grown = bdfi_realloc_array(d->segments, cap, sizeof *grown);
    if (!grown) {
      bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
      return BDF_ERR_NOMEM;
    }
    d->segments = grown;
    d->cap_segments = cap;
  }
  d->segments[d->nsegments++] = seg;
  seg->owned = 1;
  d->total_parts += seg->total_parts;
  /* its pages take the place of the outlines; their layers stay the
   * segment's, which the document owns from here on */
  for (int64_t i = from; i < to; i++) {
    d->m.views[dst].pages[i] = seg->m.views[src].pages[i];
  }
  return BDF_OK;
}

/* ---- Accessors ---------------------------------------------------------- */

int bdf_doc_encrypted(const bdf_doc *d) { return d ? d->encrypted : 0; }
int bdf_doc_locked(const bdf_doc *d) { return d ? d->encrypted && !d->unlocked : 0; }
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
  if ((!key && len > 0) || !tx || !ty) {
    return 0;
  }
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

size_t bdf_doc_part_count(const bdf_doc *d) { return d ? d->total_parts : 0; }

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

/* bdfi_part_at is the part at an index among those of a document and its
 * segments, in the order bdfi_lookup numbers them. */
static const bdfi_part *bdfi_part_at(const bdf_doc *d, size_t i) {
  if (i < d->m.nparts) {
    return &d->m.parts[i];
  }
  i -= d->m.nparts;
  for (size_t k = 0; k < d->nsegments; k++) {
    if (i < d->segments[k]->total_parts) {
      return bdfi_part_at(d->segments[k], i);
    }
    i -= d->segments[k]->total_parts;
  }
  return NULL;
}

bdf_status bdf_doc_part_entry(const bdf_doc *d, size_t i, bdf_part_entry *out) {
  const bdfi_part *p = d && out ? bdfi_part_at(d, i) : NULL;
  if (!p) {
    return BDF_ERR_ARG;
  }
  bdfi_part_out(p, out);
  return BDF_OK;
}

int bdf_doc_part_find(const bdf_doc *d, const uint8_t hash[16], bdf_part_entry *out) {
  if (!d || !hash) {
    return 0;
  }
  const bdf_doc *owner;
  size_t index;
  const bdfi_part *p = bdfi_lookup(d, hash, &owner, &index);
  if (!p) {
    return 0;
  }
  if (out) {
    bdfi_part_out(p, out);
  }
  return 1;
}

/* bdfi_enc_manifest is the manifest that holds the encryption block: the
 * stored one, which an unlock puts aside. */
static const bdfi_manifest *bdfi_enc_manifest(const bdf_doc *d) {
  if (!d || !d->encrypted) {
    return NULL;
  }
  return d->unlocked ? &d->outer : &d->m;
}

const char *bdf_doc_cipher(const bdf_doc *d, size_t *len) {
  static const bdfi_str none = {NULL, 0};
  const bdfi_manifest *m = bdfi_enc_manifest(d);
  return bdfi_str_out(m ? &m->cipher : &none, len);
}

size_t bdf_doc_key_count(const bdf_doc *d) {
  const bdfi_manifest *m = bdfi_enc_manifest(d);
  return m ? m->nkeys : 0;
}

bdf_status bdf_doc_key(const bdf_doc *d, size_t i, bdf_key_slot *out) {
  const bdfi_manifest *m = bdfi_enc_manifest(d);
  if (!m || !out || i >= m->nkeys) {
    return BDF_ERR_ARG;
  }
  const bdfi_key *k = &m->keys[i];
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
  const bdfi_manifest *m = bdfi_enc_manifest(d);
  if (!m) {
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
    *enc = bdfi_str_out(&m->sealed_manifest_enc, &n);
    if (enc_len) {
      *enc_len = n;
    }
  }
  return m->sealed_manifest;
}
