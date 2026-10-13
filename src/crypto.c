/*
 * The cryptography of encrypted documents (spec §3.5): SHA-256, HMAC,
 * PBKDF2 and HKDF for the keys, AES-256 with key unwrap (RFC 3394) and GCM
 * for the content, and ECDH on P-256 for the key slots of one reader.
 *
 * It is written to be read and to be checked against test vectors and
 * against the reference implementation, which uses Go's standard library.
 * The scalar multiplication on P-256 is constant time (complete formulas,
 * one double and one add for every bit, selected without branches). AES
 * uses a table for its S-box and GHASH a table of sixteen multiples of the
 * key, as the readers of other document formats do: on a machine that runs
 * hostile code beside it, their timing may tell of the key.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

void bdfi_wipe(void *p, size_t n) {
  volatile uint8_t *b = p;
  while (n--) {
    *b++ = 0;
  }
}

/* bdfi_equal compares two byte strings in time that does not depend on
 * where they differ: 1 when equal. */
static int bdfi_equal(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

static uint32_t bdfi_load32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void bdfi_store32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void bdfi_store64be(uint8_t *p, uint64_t v) {
  bdfi_store32be(p, (uint32_t)(v >> 32));
  bdfi_store32be(p + 4, (uint32_t)v);
}

/* ---- SHA-256 (FIPS 180-4) ----------------------------------------------- */

static const uint32_t bdfi_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define BDFI_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void bdfi_sha256_compress(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = bdfi_load32be(block + 4 * i);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = BDFI_ROTR(w[i - 15], 7) ^ BDFI_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = BDFI_ROTR(w[i - 2], 17) ^ BDFI_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = BDFI_ROTR(e, 6) ^ BDFI_ROTR(e, 11) ^ BDFI_ROTR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + s1 + ch + bdfi_sha256_k[i] + w[i];
    uint32_t s0 = BDFI_ROTR(a, 2) ^ BDFI_ROTR(a, 13) ^ BDFI_ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

void bdfi_sha256_init(bdfi_sha256 *s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv);
  s->len = 0;
  s->n = 0;
}

void bdfi_sha256_update(bdfi_sha256 *s, const void *data, size_t len) {
  const uint8_t *p = data;
  if (len == 0) {
    return; /* and data may be NULL, which memcpy is not to be given */
  }
  s->len += len;
  if (s->n > 0) {
    size_t take = 64 - s->n < len ? 64 - s->n : len;
    memcpy(s->buf + s->n, p, take);
    s->n += take;
    p += take;
    len -= take;
    if (s->n < 64) {
      return;
    }
    bdfi_sha256_compress(s->h, s->buf);
    s->n = 0;
  }
  while (len >= 64) {
    bdfi_sha256_compress(s->h, p);
    p += 64;
    len -= 64;
  }
  if (len > 0) {
    memcpy(s->buf, p, len);
    s->n = len;
  }
}

void bdfi_sha256_final(bdfi_sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8;
  s->buf[s->n++] = 0x80;
  if (s->n > 56) {
    memset(s->buf + s->n, 0, 64 - s->n);
    bdfi_sha256_compress(s->h, s->buf);
    s->n = 0;
  }
  memset(s->buf + s->n, 0, 56 - s->n);
  bdfi_store64be(s->buf + 56, bits);
  bdfi_sha256_compress(s->h, s->buf);
  for (int i = 0; i < 8; i++) {
    bdfi_store32be(out + 4 * i, s->h[i]);
  }
  bdfi_wipe(s, sizeof *s);
}

void bdfi_sha256_of(const void *data, size_t len, uint8_t out[32]) {
  bdfi_sha256 s;
  bdfi_sha256_init(&s);
  bdfi_sha256_update(&s, data, len);
  bdfi_sha256_final(&s, out);
}

/* ---- HMAC-SHA-256 (RFC 2104) -------------------------------------------- */

/* The states of the hash after the two pads of a key: an HMAC with the key
 * starts from them. */
typedef struct bdfi_hmac {
  bdfi_sha256 inner, outer;
} bdfi_hmac;

static void bdfi_hmac_init(bdfi_hmac *m, const uint8_t *key, size_t key_len) {
  uint8_t k[64] = {0}, pad[64];
  if (key_len > 64) {
    bdfi_sha256_of(key, key_len, k);
  } else if (key_len > 0) {
    memcpy(k, key, key_len);
  }
  for (int i = 0; i < 64; i++) {
    pad[i] = k[i] ^ 0x36;
  }
  bdfi_sha256_init(&m->inner);
  bdfi_sha256_update(&m->inner, pad, 64);
  for (int i = 0; i < 64; i++) {
    pad[i] = k[i] ^ 0x5c;
  }
  bdfi_sha256_init(&m->outer);
  bdfi_sha256_update(&m->outer, pad, 64);
  bdfi_wipe(k, sizeof k);
  bdfi_wipe(pad, sizeof pad);
}

/* bdfi_hmac_of is the HMAC of two pieces of data, one after the other. */
static void bdfi_hmac_of(const bdfi_hmac *m, const void *a, size_t a_len, const void *b, size_t b_len,
                         uint8_t out[32]) {
  bdfi_sha256 s = m->inner;
  uint8_t digest[32];
  bdfi_sha256_update(&s, a, a_len);
  bdfi_sha256_update(&s, b, b_len);
  bdfi_sha256_final(&s, digest);
  s = m->outer;
  bdfi_sha256_update(&s, digest, 32);
  bdfi_sha256_final(&s, out);
  bdfi_wipe(digest, sizeof digest);
}

void bdfi_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
                      uint8_t out[32]) {
  bdfi_hmac m;
  bdfi_hmac_init(&m, key, key_len);
  bdfi_hmac_of(&m, data, len, NULL, 0, out);
  bdfi_wipe(&m, sizeof m);
}

/* ---- PBKDF2-HMAC-SHA-256 (RFC 8018), a key of 32 bytes ------------------- */

/* bdfi_hmac_block is the HMAC of 32 bytes from the two pad states: one
 * compression each, which is all an iteration of PBKDF2 takes. */
static void bdfi_hmac_block(const uint32_t inner[8], const uint32_t outer[8], uint8_t u[32]) {
  uint8_t block[64];
  uint32_t h[8];
  /* the 32 bytes follow a pad of 64: 96 bytes, 768 bits */
  memcpy(block, u, 32);
  block[32] = 0x80;
  memset(block + 33, 0, 29);
  block[62] = 0x03;
  block[63] = 0x00;
  memcpy(h, inner, sizeof h);
  bdfi_sha256_compress(h, block);
  for (int i = 0; i < 8; i++) {
    bdfi_store32be(block + 4 * i, h[i]);
  }
  memcpy(h, outer, sizeof h);
  bdfi_sha256_compress(h, block);
  for (int i = 0; i < 8; i++) {
    bdfi_store32be(u + 4 * i, h[i]);
  }
  bdfi_wipe(block, sizeof block);
  bdfi_wipe(h, sizeof h);
}

void bdfi_pbkdf2_sha256(const void *password, size_t password_len, const uint8_t *salt,
                        size_t salt_len, uint32_t iterations, uint8_t out[32]) {
  bdfi_hmac m;
  bdfi_hmac_init(&m, password, password_len);
  static const uint8_t one[4] = {0, 0, 0, 1};
  uint8_t u[32];
  bdfi_hmac_of(&m, salt, salt_len, one, 4, u);
  memcpy(out, u, 32);
  for (uint32_t i = 1; i < iterations; i++) {
    bdfi_hmac_block(m.inner.h, m.outer.h, u);
    for (int k = 0; k < 32; k++) {
      out[k] ^= u[k];
    }
  }
  bdfi_wipe(&m, sizeof m);
  bdfi_wipe(u, sizeof u);
}

/* ---- HKDF-SHA-256 (RFC 5869), a key of 32 bytes ------------------------- */

void bdfi_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                      const void *info, size_t info_len, uint8_t out[32]) {
  uint8_t prk[32];
  static const uint8_t one = 1;
  bdfi_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
  bdfi_hmac m;
  bdfi_hmac_init(&m, prk, 32);
  bdfi_hmac_of(&m, info, info_len, &one, 1, out);
  bdfi_wipe(&m, sizeof m);
  bdfi_wipe(prk, sizeof prk);
}

/* ---- AES-256 (FIPS 197) ------------------------------------------------- */

static uint8_t bdfi_rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

static uint8_t bdfi_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

void bdfi_aes256_init(bdfi_aes *a, const uint8_t key[32]) {
  /* the S-box, from its definition: the inverse in GF(2^8) and an affine map */
  uint8_t p = 1, q = 1;
  do {
    p = (uint8_t)(p ^ bdfi_xtime(p)); /* p × 3 */
    q ^= (uint8_t)(q << 1);           /* q ÷ 3 */
    q ^= (uint8_t)(q << 2);
    q ^= (uint8_t)(q << 4);
    if (q & 0x80) {
      q ^= 0x09;
    }
    uint8_t x = (uint8_t)(q ^ bdfi_rotl8(q, 1) ^ bdfi_rotl8(q, 2) ^ bdfi_rotl8(q, 3) ^ bdfi_rotl8(q, 4));
    a->sbox[p] = (uint8_t)(x ^ 0x63);
  } while (p != 1);
  a->sbox[0] = 0x63;
  for (int i = 0; i < 256; i++) {
    a->inv[a->sbox[i]] = (uint8_t)i;
  }
  /* the key schedule: 15 round keys */
  memcpy(a->rk, key, 32);
  uint8_t rcon = 1;
  for (int i = 32; i < 240; i += 4) {
    uint8_t t[4];
    memcpy(t, a->rk + i - 4, 4);
    if (i % 32 == 0) {
      uint8_t t0 = t[0];
      t[0] = (uint8_t)(a->sbox[t[1]] ^ rcon);
      t[1] = a->sbox[t[2]];
      t[2] = a->sbox[t[3]];
      t[3] = a->sbox[t0];
      rcon = bdfi_xtime(rcon);
    } else if (i % 32 == 16) {
      for (int k = 0; k < 4; k++) {
        t[k] = a->sbox[t[k]];
      }
    }
    for (int k = 0; k < 4; k++) {
      a->rk[i + k] = (uint8_t)(a->rk[i - 32 + k] ^ t[k]);
    }
  }
}

static void bdfi_aes_add_key(uint8_t s[16], const uint8_t *rk) {
  for (int i = 0; i < 16; i++) {
    s[i] ^= rk[i];
  }
}

void bdfi_aes256_encrypt(const bdfi_aes *a, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  memcpy(s, in, 16);
  bdfi_aes_add_key(s, a->rk);
  for (int round = 1; round <= 14; round++) {
    /* SubBytes and ShiftRows: row r of the state moves r columns to the left */
    for (int c = 0; c < 4; c++) {
      for (int r = 0; r < 4; r++) {
        t[4 * c + r] = a->sbox[s[4 * ((c + r) & 3) + r]];
      }
    }
    if (round < 14) {
      for (int c = 0; c < 4; c++) {
        uint8_t *col = t + 4 * c;
        uint8_t all = (uint8_t)(col[0] ^ col[1] ^ col[2] ^ col[3]);
        uint8_t c0 = col[0];
        col[0] ^= (uint8_t)(all ^ bdfi_xtime((uint8_t)(col[0] ^ col[1])));
        col[1] ^= (uint8_t)(all ^ bdfi_xtime((uint8_t)(col[1] ^ col[2])));
        col[2] ^= (uint8_t)(all ^ bdfi_xtime((uint8_t)(col[2] ^ col[3])));
        col[3] ^= (uint8_t)(all ^ bdfi_xtime((uint8_t)(col[3] ^ c0)));
      }
    }
    bdfi_aes_add_key(t, a->rk + 16 * round);
    memcpy(s, t, 16);
  }
  memcpy(out, s, 16);
  bdfi_wipe(s, sizeof s);
  bdfi_wipe(t, sizeof t);
}

/* bdfi_gmul multiplies in GF(2^8). */
static uint8_t bdfi_gmul(uint8_t x, uint8_t y) {
  uint8_t r = 0;
  for (int i = 0; i < 8; i++) {
    if (y & 1) {
      r ^= x;
    }
    x = bdfi_xtime(x);
    y >>= 1;
  }
  return r;
}

void bdfi_aes256_decrypt(const bdfi_aes *a, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  memcpy(s, in, 16);
  bdfi_aes_add_key(s, a->rk + 16 * 14);
  for (int round = 13; round >= 0; round--) {
    /* InvShiftRows and InvSubBytes: row r moves r columns to the right */
    for (int c = 0; c < 4; c++) {
      for (int r = 0; r < 4; r++) {
        t[4 * ((c + r) & 3) + r] = a->inv[s[4 * c + r]];
      }
    }
    bdfi_aes_add_key(t, a->rk + 16 * round);
    if (round > 0) {
      for (int c = 0; c < 4; c++) {
        uint8_t *col = t + 4 * c;
        uint8_t c0 = col[0], c1 = col[1], c2 = col[2], c3 = col[3];
        col[0] = (uint8_t)(bdfi_gmul(c0, 14) ^ bdfi_gmul(c1, 11) ^ bdfi_gmul(c2, 13) ^ bdfi_gmul(c3, 9));
        col[1] = (uint8_t)(bdfi_gmul(c0, 9) ^ bdfi_gmul(c1, 14) ^ bdfi_gmul(c2, 11) ^ bdfi_gmul(c3, 13));
        col[2] = (uint8_t)(bdfi_gmul(c0, 13) ^ bdfi_gmul(c1, 9) ^ bdfi_gmul(c2, 14) ^ bdfi_gmul(c3, 11));
        col[3] = (uint8_t)(bdfi_gmul(c0, 11) ^ bdfi_gmul(c1, 13) ^ bdfi_gmul(c2, 9) ^ bdfi_gmul(c3, 14));
      }
    }
    memcpy(s, t, 16);
  }
  memcpy(out, s, 16);
  bdfi_wipe(s, sizeof s);
  bdfi_wipe(t, sizeof t);
}

/* ---- AES key unwrap (RFC 3394) ------------------------------------------ */

int bdfi_aes_kw_unwrap(const uint8_t kek[32], const uint8_t *wrapped, size_t len, uint8_t *out) {
  static const uint8_t iv[8] = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};
  if (len % 8 != 0 || len < 24 || len > 8 + BDFI_KW_MAX_KEY) {
    return -1;
  }
  bdfi_aes aes;
  bdfi_aes256_init(&aes, kek);
  size_t n = len / 8 - 1;
  uint8_t a[8], r[BDFI_KW_MAX_KEY], blk[16];
  memcpy(a, wrapped, 8);
  memcpy(r, wrapped + 8, 8 * n);
  for (int j = 5; j >= 0; j--) {
    for (size_t i = n; i >= 1; i--) {
      uint64_t t = (uint64_t)n * (uint64_t)j + i;
      memcpy(blk, a, 8);
      for (int k = 0; k < 8; k++) {
        blk[7 - k] ^= (uint8_t)(t >> (8 * k));
      }
      memcpy(blk + 8, r + 8 * (i - 1), 8);
      bdfi_aes256_decrypt(&aes, blk, blk);
      memcpy(a, blk, 8);
      memcpy(r + 8 * (i - 1), blk + 8, 8);
    }
  }
  int ok = bdfi_equal(a, iv, 8);
  if (ok) {
    memcpy(out, r, 8 * n);
  }
  bdfi_wipe(&aes, sizeof aes);
  bdfi_wipe(r, sizeof r);
  bdfi_wipe(blk, sizeof blk);
  bdfi_wipe(a, sizeof a);
  return ok ? 0 : -1;
}

/* ---- AES-256-GCM (NIST SP 800-38D), nonces of 12 bytes ------------------ */

/*
 * An element of GF(2^128) as GCM writes it: the first bit of the first byte
 * is the coefficient of x^0. hi holds the first eight bytes, big-endian, so
 * multiplying by x is a shift to the right, and what falls off the end of lo
 * comes back as the reduction polynomial at the top of hi.
 */
static void bdfi_gcm_times_x(uint64_t *hi, uint64_t *lo) {
  uint64_t carry = *lo & 1;
  *lo = (*hi << 63) | (*lo >> 1);
  *hi = (*hi >> 1) ^ (carry ? 0xE100000000000000ULL : 0);
}

void bdfi_gcm_init(bdfi_gcm *g, const uint8_t key[32]) {
  uint8_t h[16] = {0};
  bdfi_aes256_init(&g->aes, key);
  bdfi_aes256_encrypt(&g->aes, h, h);
  /* the multiples of the hash key by the sixteen polynomials of a nibble:
   * the top bit of a nibble is its lowest power */
  uint64_t hi = ((uint64_t)bdfi_load32be(h) << 32) | bdfi_load32be(h + 4);
  uint64_t lo = ((uint64_t)bdfi_load32be(h + 8) << 32) | bdfi_load32be(h + 12);
  memset(g->mh, 0, sizeof g->mh);
  memset(g->ml, 0, sizeof g->ml);
  for (int bit = 8; bit > 0; bit >>= 1) {
    g->mh[bit] = hi;
    g->ml[bit] = lo;
    bdfi_gcm_times_x(&hi, &lo);
  }
  for (int i = 2; i < 16; i <<= 1) {
    for (int j = 1; j < i; j++) {
      g->mh[i + j] = g->mh[i] ^ g->mh[j];
      g->ml[i + j] = g->ml[i] ^ g->ml[j];
    }
  }
  /* what the four bits that fall off in a multiplication by x^4 come back as */
  for (int r = 0; r < 16; r++) {
    uint64_t rh = 0, rl = (uint64_t)r;
    for (int k = 0; k < 4; k++) {
      bdfi_gcm_times_x(&rh, &rl);
    }
    g->red[r] = rh;
  }
  bdfi_wipe(h, sizeof h);
}

/* bdfi_ghash_block folds a block into the hash: y = (y ⊕ block) × H. */
static void bdfi_ghash_block(const bdfi_gcm *g, uint8_t y[16], const uint8_t *block, size_t len) {
  uint8_t x[16];
  memcpy(x, y, 16);
  for (size_t i = 0; i < len; i++) {
    x[i] ^= block[i];
  }
  uint64_t zh = 0, zl = 0;
  /* Horner's rule in x^4, from the highest powers: the low nibble of the
   * last byte first */
  for (int i = 15; i >= 0; i--) {
    for (int half = 0; half < 2; half++) {
      unsigned nibble = half == 0 ? (x[i] & 15u) : (unsigned)(x[i] >> 4);
      unsigned rem = (unsigned)(zl & 15);
      zl = (zh << 60) | (zl >> 4);
      zh = (zh >> 4) ^ g->red[rem];
      zh ^= g->mh[nibble];
      zl ^= g->ml[nibble];
    }
  }
  bdfi_store64be(y, zh);
  bdfi_store64be(y + 8, zl);
}

static void bdfi_ghash(const bdfi_gcm *g, uint8_t y[16], const uint8_t *data, size_t len) {
  while (len >= 16) {
    bdfi_ghash_block(g, y, data, 16);
    data += 16;
    len -= 16;
  }
  if (len > 0) {
    bdfi_ghash_block(g, y, data, len); /* padded with zeros */
  }
}

int bdfi_gcm_open(const bdfi_gcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                  const uint8_t *ct, size_t ct_len, const uint8_t tag[16], uint8_t *out) {
  uint8_t counter[16], y[16] = {0}, lens[16], s[16];
  memcpy(counter, nonce, 12);
  counter[12] = counter[13] = counter[14] = 0;
  counter[15] = 1;
  /* the tag first: nothing is decrypted that is not authentic */
  bdfi_ghash(g, y, aad, aad_len);
  bdfi_ghash(g, y, ct, ct_len);
  bdfi_store64be(lens, (uint64_t)aad_len * 8);
  bdfi_store64be(lens + 8, (uint64_t)ct_len * 8);
  bdfi_ghash_block(g, y, lens, 16);
  bdfi_aes256_encrypt(&g->aes, counter, s);
  for (int i = 0; i < 16; i++) {
    s[i] ^= y[i];
  }
  if (!bdfi_equal(s, tag, 16)) {
    return -1;
  }
  uint32_t n = 1;
  for (size_t off = 0; off < ct_len; off += 16) {
    n++;
    bdfi_store32be(counter + 12, n);
    bdfi_aes256_encrypt(&g->aes, counter, s);
    size_t take = ct_len - off < 16 ? ct_len - off : 16;
    for (size_t i = 0; i < take; i++) {
      out[off + i] = (uint8_t)(ct[off + i] ^ s[i]);
    }
  }
  bdfi_wipe(s, sizeof s);
  return 0;
}

/* ---- P-256 (FIPS 186, SEC 2: secp256r1) --------------------------------- */

/* A number of 256 bits, the least significant word first. */
typedef struct bdfi_fe {
  uint32_t v[8];
} bdfi_fe;

/* p = 2^256 − 2^224 + 2^192 + 2^96 − 1 */
static const bdfi_fe bdfi_p256_p = {{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000,
                                     0x00000000, 0x00000001, 0xFFFFFFFF}};
/* the order of the group */
static const bdfi_fe bdfi_p256_n = {{0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD, 0xFFFFFFFF,
                                     0xFFFFFFFF, 0x00000000, 0xFFFFFFFF}};
static const bdfi_fe bdfi_p256_b = {{0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0, 0x769886BC,
                                     0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8}};
static const bdfi_fe bdfi_p256_gx = {{0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81, 0x63A440F2,
                                      0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2}};
static const bdfi_fe bdfi_p256_gy = {{0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357, 0x7C0F9E16,
                                      0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2}};

static void bdfi_fe_from_bytes(bdfi_fe *r, const uint8_t in[32]) {
  for (int i = 0; i < 8; i++) {
    r->v[i] = bdfi_load32be(in + 4 * (7 - i));
  }
}

static void bdfi_fe_to_bytes(const bdfi_fe *a, uint8_t out[32]) {
  for (int i = 0; i < 8; i++) {
    bdfi_store32be(out + 4 * (7 - i), a->v[i]);
  }
}

/* bdfi_fe_sub_raw is a − b, and the borrow out of the top. */
static uint32_t bdfi_fe_sub_raw(bdfi_fe *r, const bdfi_fe *a, const bdfi_fe *b) {
  uint64_t borrow = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t t = (uint64_t)a->v[i] - b->v[i] - borrow;
    r->v[i] = (uint32_t)t;
    borrow = (t >> 32) & 1;
  }
  return (uint32_t)borrow;
}

static uint32_t bdfi_fe_add_raw(bdfi_fe *r, const bdfi_fe *a, const bdfi_fe *b) {
  uint64_t carry = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t t = (uint64_t)a->v[i] + b->v[i] + carry;
    r->v[i] = (uint32_t)t;
    carry = t >> 32;
  }
  return (uint32_t)carry;
}

/* bdfi_fe_select sets r to a when pick is 1 and to b when it is 0, without
 * a branch. */
static void bdfi_fe_select(bdfi_fe *r, uint32_t pick, const bdfi_fe *a, const bdfi_fe *b) {
  uint32_t mask = (uint32_t)0 - pick;
  for (int i = 0; i < 8; i++) {
    r->v[i] = (a->v[i] & mask) | (b->v[i] & ~mask);
  }
}

/* bdfi_fe_less reports a < b (1 or 0), without a branch on the numbers. */
static uint32_t bdfi_fe_less(const bdfi_fe *a, const bdfi_fe *b) {
  bdfi_fe t;
  return bdfi_fe_sub_raw(&t, a, b);
}

static uint32_t bdfi_fe_is_zero(const bdfi_fe *a) {
  uint32_t any = 0;
  for (int i = 0; i < 8; i++) {
    any |= a->v[i];
  }
  return any == 0;
}

/* Arithmetic modulo p, on numbers below p. */

static void bdfi_fp_add(bdfi_fe *r, const bdfi_fe *a, const bdfi_fe *b) {
  bdfi_fe sum, less;
  uint32_t carry = bdfi_fe_add_raw(&sum, a, b);
  uint32_t borrow = bdfi_fe_sub_raw(&less, &sum, &bdfi_p256_p);
  /* the sum is taken less p when it carried or is not below p */
  bdfi_fe_select(r, carry | (1 - borrow), &less, &sum);
}

static void bdfi_fp_sub(bdfi_fe *r, const bdfi_fe *a, const bdfi_fe *b) {
  bdfi_fe diff, more;
  uint32_t borrow = bdfi_fe_sub_raw(&diff, a, b);
  bdfi_fe_add_raw(&more, &diff, &bdfi_p256_p);
  bdfi_fe_select(r, borrow, &more, &diff);
}

/* bdfi_fp_mul is the Montgomery product a × b ÷ 2^256 mod p. With p ≡ −1
 * modulo 2^32 the multiple of p that clears the lowest word is that word. */
static void bdfi_fp_mul(bdfi_fe *r, const bdfi_fe *a, const bdfi_fe *b) {
  uint32_t t[10] = {0};
  for (int i = 0; i < 8; i++) {
    uint64_t carry = 0;
    for (int j = 0; j < 8; j++) {
      uint64_t x = (uint64_t)a->v[j] * b->v[i] + t[j] + carry;
      t[j] = (uint32_t)x;
      carry = x >> 32;
    }
    uint64_t x = (uint64_t)t[8] + carry;
    t[8] = (uint32_t)x;
    t[9] = (uint32_t)(x >> 32);
    uint32_t m = t[0];
    x = (uint64_t)m * bdfi_p256_p.v[0] + t[0];
    carry = x >> 32;
    for (int j = 1; j < 8; j++) {
      x = (uint64_t)m * bdfi_p256_p.v[j] + t[j] + carry;
      t[j - 1] = (uint32_t)x;
      carry = x >> 32;
    }
    x = (uint64_t)t[8] + carry;
    t[7] = (uint32_t)x;
    t[8] = t[9] + (uint32_t)(x >> 32);
  }
  bdfi_fe res, less;
  memcpy(res.v, t, sizeof res.v);
  uint32_t borrow = bdfi_fe_sub_raw(&less, &res, &bdfi_p256_p);
  bdfi_fe_select(r, t[8] | (1 - borrow), &less, &res);
}

/* The constants of the curve in Montgomery form, made once for an operation. */
typedef struct bdfi_p256 {
  bdfi_fe one; /* 2^256 mod p */
  bdfi_fe r2;  /* 2^512 mod p: a × r2 is a in Montgomery form */
  bdfi_fe b;
} bdfi_p256;

static void bdfi_p256_setup(bdfi_p256 *c) {
  bdfi_fe zero = {{0}};
  bdfi_fe_sub_raw(&c->one, &zero, &bdfi_p256_p); /* 2^256 − p */
  c->r2 = c->one;
  for (int i = 0; i < 256; i++) {
    bdfi_fp_add(&c->r2, &c->r2, &c->r2);
  }
  bdfi_fp_mul(&c->b, &bdfi_p256_b, &c->r2);
}

/* bdfi_fp_inv is 1 ÷ a, in Montgomery form: a^(p−2). */
static void bdfi_fp_inv(const bdfi_p256 *c, bdfi_fe *r, const bdfi_fe *a) {
  bdfi_fe e = bdfi_p256_p, acc = c->one;
  e.v[0] -= 2; /* p − 2; the lowest word of p is 0xFFFFFFFF */
  for (int i = 255; i >= 0; i--) {
    bdfi_fp_mul(&acc, &acc, &acc);
    if ((e.v[i / 32] >> (i % 32)) & 1) {
      bdfi_fp_mul(&acc, &acc, a);
    }
  }
  *r = acc;
}

/* A point in projective coordinates (x = X/Z, y = Y/Z), in Montgomery form;
 * the point at infinity is (0 : 1 : 0). */
typedef struct bdfi_point {
  bdfi_fe x, y, z;
} bdfi_point;

/* The complete formulas for curves with a = −3 of Renes, Costello and
 * Batina (https://eprint.iacr.org/2015/1060, §A.2): they hold for every
 * pair of points, so there are no cases to tell apart. */
static void bdfi_point_add(const bdfi_p256 *c, bdfi_point *q, const bdfi_point *p1,
                           const bdfi_point *p2) {
  bdfi_fe t0, t1, t2, t3, t4, x3, y3, z3;
  bdfi_fp_mul(&t0, &p1->x, &p2->x);
  bdfi_fp_mul(&t1, &p1->y, &p2->y);
  bdfi_fp_mul(&t2, &p1->z, &p2->z);
  bdfi_fp_add(&t3, &p1->x, &p1->y);
  bdfi_fp_add(&t4, &p2->x, &p2->y);
  bdfi_fp_mul(&t3, &t3, &t4);
  bdfi_fp_add(&t4, &t0, &t1);
  bdfi_fp_sub(&t3, &t3, &t4);
  bdfi_fp_add(&t4, &p1->y, &p1->z);
  bdfi_fp_add(&x3, &p2->y, &p2->z);
  bdfi_fp_mul(&t4, &t4, &x3);
  bdfi_fp_add(&x3, &t1, &t2);
  bdfi_fp_sub(&t4, &t4, &x3);
  bdfi_fp_add(&x3, &p1->x, &p1->z);
  bdfi_fp_add(&y3, &p2->x, &p2->z);
  bdfi_fp_mul(&x3, &x3, &y3);
  bdfi_fp_add(&y3, &t0, &t2);
  bdfi_fp_sub(&y3, &x3, &y3);
  bdfi_fp_mul(&z3, &c->b, &t2);
  bdfi_fp_sub(&x3, &y3, &z3);
  bdfi_fp_add(&z3, &x3, &x3);
  bdfi_fp_add(&x3, &x3, &z3);
  bdfi_fp_sub(&z3, &t1, &x3);
  bdfi_fp_add(&x3, &t1, &x3);
  bdfi_fp_mul(&y3, &c->b, &y3);
  bdfi_fp_add(&t1, &t2, &t2);
  bdfi_fp_add(&t2, &t1, &t2);
  bdfi_fp_sub(&y3, &y3, &t2);
  bdfi_fp_sub(&y3, &y3, &t0);
  bdfi_fp_add(&t1, &y3, &y3);
  bdfi_fp_add(&y3, &t1, &y3);
  bdfi_fp_add(&t1, &t0, &t0);
  bdfi_fp_add(&t0, &t1, &t0);
  bdfi_fp_sub(&t0, &t0, &t2);
  bdfi_fp_mul(&t1, &t4, &y3);
  bdfi_fp_mul(&t2, &t0, &y3);
  bdfi_fp_mul(&y3, &x3, &z3);
  bdfi_fp_add(&y3, &y3, &t2);
  bdfi_fp_mul(&x3, &t3, &x3);
  bdfi_fp_sub(&x3, &x3, &t1);
  bdfi_fp_mul(&z3, &t4, &z3);
  bdfi_fp_mul(&t1, &t3, &t0);
  bdfi_fp_add(&z3, &z3, &t1);
  q->x = x3;
  q->y = y3;
  q->z = z3;
}

static void bdfi_point_double(const bdfi_p256 *c, bdfi_point *q, const bdfi_point *p) {
  bdfi_fe t0, t1, t2, t3, x3, y3, z3;
  bdfi_fp_mul(&t0, &p->x, &p->x);
  bdfi_fp_mul(&t1, &p->y, &p->y);
  bdfi_fp_mul(&t2, &p->z, &p->z);
  bdfi_fp_mul(&t3, &p->x, &p->y);
  bdfi_fp_add(&t3, &t3, &t3);
  bdfi_fp_mul(&z3, &p->x, &p->z);
  bdfi_fp_add(&z3, &z3, &z3);
  bdfi_fp_mul(&y3, &c->b, &t2);
  bdfi_fp_sub(&y3, &y3, &z3);
  bdfi_fp_add(&x3, &y3, &y3);
  bdfi_fp_add(&y3, &x3, &y3);
  bdfi_fp_sub(&x3, &t1, &y3);
  bdfi_fp_add(&y3, &t1, &y3);
  bdfi_fp_mul(&y3, &x3, &y3);
  bdfi_fp_mul(&x3, &x3, &t3);
  bdfi_fp_add(&t3, &t2, &t2);
  bdfi_fp_add(&t2, &t2, &t3);
  bdfi_fp_mul(&z3, &c->b, &z3);
  bdfi_fp_sub(&z3, &z3, &t2);
  bdfi_fp_sub(&z3, &z3, &t0);
  bdfi_fp_add(&t3, &z3, &z3);
  bdfi_fp_add(&z3, &z3, &t3);
  bdfi_fp_add(&t3, &t0, &t0);
  bdfi_fp_add(&t0, &t3, &t0);
  bdfi_fp_sub(&t0, &t0, &t2);
  bdfi_fp_mul(&t0, &t0, &z3);
  bdfi_fp_add(&y3, &y3, &t0);
  bdfi_fp_mul(&t0, &p->y, &p->z);
  bdfi_fp_add(&t0, &t0, &t0);
  bdfi_fp_mul(&z3, &t0, &z3);
  bdfi_fp_sub(&x3, &x3, &z3);
  bdfi_fp_mul(&z3, &t0, &t1);
  bdfi_fp_add(&z3, &z3, &z3);
  bdfi_fp_add(&z3, &z3, &z3);
  q->x = x3;
  q->y = y3;
  q->z = z3;
}

/* bdfi_point_mul is k × p: for every bit of k a doubling and an addition,
 * of which one is kept, whatever the bit. */
static void bdfi_point_mul(const bdfi_p256 *c, bdfi_point *r, const uint8_t k[32],
                           const bdfi_point *p) {
  bdfi_point acc, sum;
  memset(&acc, 0, sizeof acc);
  acc.y = c->one;
  for (int i = 0; i < 256; i++) {
    uint32_t bit = (uint32_t)(k[i / 8] >> (7 - i % 8)) & 1;
    bdfi_point_double(c, &acc, &acc);
    bdfi_point_add(c, &sum, &acc, p);
    bdfi_fe_select(&acc.x, bit, &sum.x, &acc.x);
    bdfi_fe_select(&acc.y, bit, &sum.y, &acc.y);
    bdfi_fe_select(&acc.z, bit, &sum.z, &acc.z);
  }
  *r = acc;
  bdfi_wipe(&acc, sizeof acc);
  bdfi_wipe(&sum, sizeof sum);
}

/* bdfi_point_affine writes x and y (NULL: not wanted) of a point as 32
 * bytes each: 0, or -1 for the point at infinity. */
static int bdfi_point_affine(const bdfi_p256 *c, const bdfi_point *p, uint8_t x[32], uint8_t y[32]) {
  if (bdfi_fe_is_zero(&p->z)) {
    return -1;
  }
  bdfi_fe zinv, t, one = {{1}};
  bdfi_fp_inv(c, &zinv, &p->z);
  bdfi_fp_mul(&t, &p->x, &zinv);
  bdfi_fp_mul(&t, &t, &one); /* out of Montgomery form */
  bdfi_fe_to_bytes(&t, x);
  if (y) {
    bdfi_fp_mul(&t, &p->y, &zinv);
    bdfi_fp_mul(&t, &t, &one);
    bdfi_fe_to_bytes(&t, y);
  }
  bdfi_wipe(&zinv, sizeof zinv);
  bdfi_wipe(&t, sizeof t);
  return 0;
}

/* bdfi_scalar_valid reports a private key in [1, n − 1]. */
static int bdfi_scalar_valid(const uint8_t k[32]) {
  bdfi_fe s;
  bdfi_fe_from_bytes(&s, k);
  int ok = !bdfi_fe_is_zero(&s) && bdfi_fe_less(&s, &bdfi_p256_n);
  bdfi_wipe(&s, sizeof s);
  return ok;
}

/* bdfi_point_decode reads an uncompressed point (0x04, x, y) and checks that
 * it is on the curve: 0, or -1. */
static int bdfi_point_decode(const bdfi_p256 *c, bdfi_point *p, const uint8_t in[65]) {
  if (in[0] != 0x04) {
    return -1;
  }
  bdfi_fe x, y;
  bdfi_fe_from_bytes(&x, in + 1);
  bdfi_fe_from_bytes(&y, in + 33);
  if (!bdfi_fe_less(&x, &bdfi_p256_p) || !bdfi_fe_less(&y, &bdfi_p256_p)) {
    return -1;
  }
  bdfi_fp_mul(&p->x, &x, &c->r2);
  bdfi_fp_mul(&p->y, &y, &c->r2);
  p->z = c->one;
  /* y² = x³ − 3x + b */
  bdfi_fe lhs, rhs, t;
  bdfi_fp_mul(&lhs, &p->y, &p->y);
  bdfi_fp_mul(&rhs, &p->x, &p->x);
  bdfi_fp_mul(&rhs, &rhs, &p->x);
  bdfi_fp_add(&t, &p->x, &p->x);
  bdfi_fp_add(&t, &t, &p->x);
  bdfi_fp_sub(&rhs, &rhs, &t);
  bdfi_fp_add(&rhs, &rhs, &c->b);
  bdfi_fp_sub(&t, &lhs, &rhs);
  return bdfi_fe_is_zero(&t) ? 0 : -1;
}

int bdfi_p256_public(const uint8_t private_key[32], uint8_t out[65]) {
  if (!bdfi_scalar_valid(private_key)) {
    return -1;
  }
  bdfi_p256 c;
  bdfi_point g, q;
  bdfi_p256_setup(&c);
  bdfi_fp_mul(&g.x, &bdfi_p256_gx, &c.r2);
  bdfi_fp_mul(&g.y, &bdfi_p256_gy, &c.r2);
  g.z = c.one;
  bdfi_point_mul(&c, &q, private_key, &g);
  out[0] = 0x04;
  int rc = bdfi_point_affine(&c, &q, out + 1, out + 33);
  bdfi_wipe(&q, sizeof q);
  return rc;
}

int bdfi_p256_ecdh(const uint8_t private_key[32], const uint8_t peer[65], uint8_t out[32]) {
  if (!bdfi_scalar_valid(private_key)) {
    return -1;
  }
  bdfi_p256 c;
  bdfi_point p, q;
  bdfi_p256_setup(&c);
  if (bdfi_point_decode(&c, &p, peer) < 0) {
    return -2;
  }
  bdfi_point_mul(&c, &q, private_key, &p);
  int rc = bdfi_point_affine(&c, &q, out, NULL);
  bdfi_wipe(&q, sizeof q);
  return rc;
}
