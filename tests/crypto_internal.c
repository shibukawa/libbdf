/* Tests of the primitives behind encrypted documents (spec §3.5) and of form
 * C, against the answers of Go's standard library and x/text
 * (crypto_vectors.h, written by go/genvectors). */
#include "testutil.h"

#include "bdf_internal.h"
#include "crypto_vectors.h"

static void test_sha256(void) {
  const struct {
    const uint8_t *in;
    size_t n;
    const uint8_t *want;
  } cases[] = {
      {cv_sha_in0, 0, cv_sha_out0},
      {cv_sha_in1, sizeof cv_sha_in1, cv_sha_out1},
      {cv_sha_in2, sizeof cv_sha_in2, cv_sha_out2},
      {cv_sha_in3, sizeof cv_sha_in3, cv_sha_out3},
      {cv_sha_in4, sizeof cv_sha_in4, cv_sha_out4},
      {cv_sha_in5, sizeof cv_sha_in5, cv_sha_out5},
      {cv_sha_in6, sizeof cv_sha_in6, cv_sha_out6},
  };
  uint8_t out[32];
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    bdfi_sha256_of(cases[i].in, cases[i].n, out);
    CHECK(memcmp(out, cases[i].want, 32) == 0);
    /* the same a byte at a time */
    bdfi_sha256 s;
    bdfi_sha256_init(&s);
    for (size_t k = 0; k < cases[i].n; k++) {
      bdfi_sha256_update(&s, cases[i].in + k, 1);
    }
    bdfi_sha256_final(&s, out);
    CHECK(memcmp(out, cases[i].want, 32) == 0);
  }
  /* a million bytes in pieces of every length up to 200 */
  static uint8_t as[200];
  memset(as, 'a', sizeof as);
  bdfi_sha256 s;
  bdfi_sha256_init(&s);
  size_t left = 1000000, piece = 0;
  while (left > 0) {
    size_t n = piece % 201;
    piece++;
    if (n > left) {
      n = left;
    }
    bdfi_sha256_update(&s, as, n);
    left -= n;
  }
  bdfi_sha256_final(&s, out);
  CHECK(memcmp(out, cv_sha_million, 32) == 0);
}

static void test_hmac(void) {
  uint8_t out[32];
  bdfi_hmac_sha256(cv_hmac_key0, sizeof cv_hmac_key0, cv_hmac_in0, sizeof cv_hmac_in0, out);
  CHECK(memcmp(out, cv_hmac_out0, 32) == 0);
  bdfi_hmac_sha256(cv_hmac_key1, sizeof cv_hmac_key1, cv_hmac_in1, sizeof cv_hmac_in1, out);
  CHECK(memcmp(out, cv_hmac_out1, 32) == 0);
  bdfi_hmac_sha256(cv_hmac_key2, sizeof cv_hmac_key2, cv_hmac_in2, sizeof cv_hmac_in2, out);
  CHECK(memcmp(out, cv_hmac_out2, 32) == 0);
  bdfi_hmac_sha256(cv_hmac_key3, sizeof cv_hmac_key3, cv_hmac_in3, 0, out);
  CHECK(memcmp(out, cv_hmac_out3, 32) == 0);
}

#define PBKDF2_CASE(i)                                                                             \
  do {                                                                                             \
    bdfi_pbkdf2_sha256(cv_pbkdf2_password##i, cv_pbkdf2_password##i##_len, cv_pbkdf2_salt##i,      \
                       cv_pbkdf2_salt##i##_len, cv_pbkdf2_iter##i, out);                           \
    CHECK(memcmp(out, cv_pbkdf2_out##i, 32) == 0);                                                 \
  } while (0)

#define HKDF_CASE(i)                                                                               \
  do {                                                                                             \
    bdfi_hkdf_sha256(cv_hkdf_ikm##i, cv_hkdf_ikm##i##_len, cv_hkdf_salt##i, cv_hkdf_salt##i##_len, \
                     cv_hkdf_info##i, cv_hkdf_info##i##_len, out);                                 \
    CHECK(memcmp(out, cv_hkdf_out##i, 32) == 0);                                                   \
  } while (0)

static void test_kdf(void) {
  uint8_t out[32];
  PBKDF2_CASE(0);
  PBKDF2_CASE(1);
  PBKDF2_CASE(2);
  PBKDF2_CASE(3);
  PBKDF2_CASE(4);
  PBKDF2_CASE(5);
  PBKDF2_CASE(6);
  HKDF_CASE(0);
  HKDF_CASE(1);
  HKDF_CASE(2);
}

static void test_aes(void) {
  bdfi_aes a;
  uint8_t out[16], back[16];
  bdfi_aes256_init(&a, cv_aes_key);
  bdfi_aes256_encrypt(&a, cv_aes_plain, out);
  CHECK(memcmp(out, cv_aes_cipher, 16) == 0);
  bdfi_aes256_decrypt(&a, out, back);
  CHECK(memcmp(back, cv_aes_plain, 16) == 0);
  /* in place */
  memcpy(out, cv_aes_plain, 16);
  bdfi_aes256_encrypt(&a, out, out);
  CHECK(memcmp(out, cv_aes_cipher, 16) == 0);
  bdfi_aes256_decrypt(&a, out, out);
  CHECK(memcmp(out, cv_aes_plain, 16) == 0);
}

static void test_key_wrap(void) {
  uint8_t out[BDFI_KW_MAX_KEY];
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, cv_kw_wrapped, sizeof cv_kw_wrapped, out) == 0);
  CHECK(memcmp(out, cv_kw_key, sizeof cv_kw_key) == 0);
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, cv_kw_wrapped16, sizeof cv_kw_wrapped16, out) == 0);
  CHECK(memcmp(out, cv_kw_key16, sizeof cv_kw_key16) == 0);
  /* any changed byte fails the check value */
  for (size_t i = 0; i < sizeof cv_kw_wrapped; i++) {
    uint8_t bad[sizeof cv_kw_wrapped];
    memcpy(bad, cv_kw_wrapped, sizeof bad);
    bad[i] ^= 1;
    CHECK(bdfi_aes_kw_unwrap(cv_aes_key, bad, sizeof bad, out) != 0);
  }
  uint8_t other[32];
  memcpy(other, cv_aes_key, 32);
  other[31] ^= 1;
  CHECK(bdfi_aes_kw_unwrap(other, cv_kw_wrapped, sizeof cv_kw_wrapped, out) != 0);
  /* lengths that are none of a wrapped key */
  static const uint8_t long_key[BDFI_KW_MAX_KEY + 16] = {0};
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, cv_kw_wrapped, 0, out) != 0);
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, cv_kw_wrapped, 8, out) != 0);
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, cv_kw_wrapped, 16, out) != 0);
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, cv_kw_wrapped, 39, out) != 0);
  CHECK(bdfi_aes_kw_unwrap(cv_aes_key, long_key, sizeof long_key, out) != 0);
}

static void test_gcm(void) {
  bdfi_gcm *g = malloc(sizeof *g);
  uint8_t out[1000], plain[1000], zero[1000];
  memset(zero, 0, sizeof zero);
  bdfi_gcm_init(g, cv_gcm_key);
  size_t aad_off = 0;
  for (size_t i = 0; i < CV_GCM_CASES; i++) {
    size_t n = cv_gcm_lens[i], an = cv_gcm_aad_lens[i];
    const uint8_t *ct = cv_gcm_sealed + cv_gcm_offs[i];
    const uint8_t *aad = cv_gcm_aad + aad_off;
    for (size_t k = 0; k < n; k++) {
      plain[k] = (uint8_t)(i * 7 + k);
    }
    memset(out, 0, sizeof out);
    CHECK(bdfi_gcm_open(g, cv_gcm_nonce, aad, an, ct, n, ct + n, out) == 0);
    CHECK(memcmp(out, plain, n) == 0);
    /* a changed tag, ciphertext, nonce or additional data: refused, and
     * nothing written */
    uint8_t bad[1016];
    memcpy(bad, ct, n + 16);
    memset(out, 0, sizeof out);
    for (size_t at = 0; at < n + 16; at += 1 + n / 20) {
      bad[at] ^= 0x80;
      CHECK(bdfi_gcm_open(g, cv_gcm_nonce, aad, an, bad, n, bad + n, out) != 0);
      bad[at] ^= 0x80;
    }
    uint8_t nonce[12];
    memcpy(nonce, cv_gcm_nonce, 12);
    nonce[11] ^= 1;
    CHECK(bdfi_gcm_open(g, nonce, aad, an, ct, n, ct + n, out) != 0);
    if (an > 0) {
      uint8_t other[64];
      memcpy(other, aad, an);
      other[an - 1] ^= 1;
      CHECK(bdfi_gcm_open(g, cv_gcm_nonce, other, an, ct, n, ct + n, out) != 0);
      CHECK(bdfi_gcm_open(g, cv_gcm_nonce, aad, an - 1, ct, n, ct + n, out) != 0);
    }
    CHECK(memcmp(out, zero, sizeof out) == 0);
    aad_off += an;
  }
  free(g);
}

static void test_p256(void) {
  /* the base point */
  static const uint8_t gx[32] = {0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6,
                                 0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb,
                                 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96};
  uint8_t pub[65], z[32], z2[32];
  for (size_t i = 0; i < CV_P256_KEYS; i++) {
    const uint8_t *priv = cv_p256_private + 32 * i;
    size_t next = (i + 1) % CV_P256_KEYS;
    CHECK(bdfi_p256_public(priv, pub) == 0);
    CHECK(memcmp(pub, cv_p256_public + 65 * i, 65) == 0);
    /* both sides come to the same secret */
    CHECK(bdfi_p256_ecdh(priv, cv_p256_public + 65 * next, z) == 0);
    CHECK(memcmp(z, cv_p256_shared + 32 * i, 32) == 0);
    CHECK(bdfi_p256_ecdh(cv_p256_private + 32 * next, pub, z2) == 0);
    CHECK(memcmp(z, z2, 32) == 0);
  }
  CHECK(bdfi_p256_public(cv_p256_private, pub) == 0 && pub[0] == 4 &&
        memcmp(pub + 1, gx, 32) == 0);
  /* private keys out of range: zero, the order, all ones */
  uint8_t k[32] = {0};
  CHECK(bdfi_p256_public(k, pub) == -1);
  CHECK(bdfi_p256_ecdh(k, cv_p256_public, z) == -1);
  CHECK(bdfi_p256_public(cv_p256_order, pub) == -1);
  memset(k, 0xff, sizeof k);
  CHECK(bdfi_p256_public(k, pub) == -1);
  /* peers that are no points of the curve */
  const uint8_t *priv = cv_p256_private + 32 * 2;
  memcpy(pub, cv_p256_public, 65);
  pub[64] ^= 1; /* off the curve */
  CHECK(bdfi_p256_ecdh(priv, pub, z) == -2);
  memcpy(pub, cv_p256_public, 65);
  pub[0] = 2; /* a compressed point */
  CHECK(bdfi_p256_ecdh(priv, pub, z) == -2);
  pub[0] = 0;
  CHECK(bdfi_p256_ecdh(priv, pub, z) == -2);
  memset(pub, 0, sizeof pub); /* the point at infinity, as some write it */
  pub[0] = 4;
  CHECK(bdfi_p256_ecdh(priv, pub, z) == -2);
  memset(pub + 1, 0xff, 64); /* coordinates that are no field elements */
  CHECK(bdfi_p256_ecdh(priv, pub, z) == -2);
}

static void test_nfc(void) {
  size_t in = 0, want = 0;
  for (size_t i = 0; i < CV_NFC_CASES; i++) {
    char *out = NULL;
    size_t n = 0;
    CHECK(bdfi_nfc((const char *)cv_nfc_in + in, cv_nfc_in_lens[i], &out, &n) == 0);
    if (n != cv_nfc_want_lens[i] || memcmp(out, cv_nfc_want + want, n) != 0) {
      failures++;
      fprintf(stderr, "form C of case %zu is %zu bytes, want %zu\n", i, n, cv_nfc_want_lens[i]);
    }
    CHECK(out && out[n] == 0);
    free(out);
    in += cv_nfc_in_lens[i];
    want += cv_nfc_want_lens[i];
  }
  /* a long run of marks is put in order whole (x/text breaks it with a
   * joiner after 30, which is not form C) */
  char run[1 + 2 * 200];
  run[0] = 'a';
  for (int i = 0; i < 200; i++) {
    /* U+0301 (above, class 230) and U+0323 (below, class 220) in turn */
    run[1 + 2 * i] = (char)0xCC;
    run[2 + 2 * i] = i % 2 == 0 ? (char)0x81 : (char)0xA3;
  }
  char *out = NULL;
  size_t n = 0;
  CHECK(bdfi_nfc(run, sizeof run, &out, &n) == 0);
  /* a + the first dot below is U+1EA1; then 99 dots below, then 100 acutes */
  CHECK(n == 3 + 2 * 199);
  CHECK(n > 3 && memcmp(out, "\xE1\xBA\xA1", 3) == 0);
  for (size_t i = 0; i < 199 && 4 + 2 * i < n; i++) {
    CHECK((uint8_t)out[3 + 2 * i] == 0xCC && (uint8_t)out[4 + 2 * i] == (i < 99 ? 0xA3 : 0x81));
  }
  free(out);
}

static void test_wipe(void) {
  uint8_t secret[64];
  memset(secret, 0xAA, sizeof secret);
  bdfi_wipe(secret, sizeof secret);
  for (size_t i = 0; i < sizeof secret; i++) {
    CHECK(secret[i] == 0);
  }
  bdfi_wipe(NULL, 0);
}

int main(void) {
  test_sha256();
  test_hmac();
  test_kdf();
  test_aes();
  test_key_wrap();
  test_gcm();
  test_p256();
  test_nfc();
  test_wipe();
  return finish();
}
