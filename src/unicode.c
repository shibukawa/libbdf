/*
 * Unicode normalisation form KC: for passwords (spec §3.5: a password is
 * normalised to NFKC before a key is derived from it), for the family names
 * of fonts and for a search. The tables are those of the reference
 * implementation's library (unicode_nfc.h and unicode_text.h are generated
 * from it); the algorithm is that of UAX #15.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"
#include "unicode_nfc.h"
#include "unicode_text.h"

/* Bytes that are not UTF-8 pass through as they are, each as a character of
 * its own that combines with nothing. */
#define BDFI_RAW_BYTE 0x110000u

enum {
  BDFI_HANGUL_S = 0xAC00,
  BDFI_HANGUL_L = 0x1100,
  BDFI_HANGUL_V = 0x1161,
  BDFI_HANGUL_T = 0x11A7,
  BDFI_HANGUL_LCOUNT = 19,
  BDFI_HANGUL_VCOUNT = 21,
  BDFI_HANGUL_TCOUNT = 28,
  BDFI_HANGUL_NCOUNT = 588,   /* VCOUNT × TCOUNT */
  BDFI_HANGUL_SCOUNT = 11172 /* LCOUNT × NCOUNT */
};

static uint8_t bdfi_ccc(uint32_t c) {
  if (c < 0x300 || c >= BDFI_RAW_BYTE) {
    return 0;
  }
  size_t lo = 0, hi = sizeof bdfi_ccc_ranges / sizeof bdfi_ccc_ranges[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < bdfi_ccc_ranges[mid][0]) {
      hi = mid;
    } else if (c > bdfi_ccc_ranges[mid][1]) {
      lo = mid + 1;
    } else {
      return (uint8_t)bdfi_ccc_ranges[mid][2];
    }
  }
  return 0;
}

/* bdfi_nfd_of returns the full canonical decomposition of a character that
 * has one in the table, and its length; NULL for the others. */
static const uint32_t *bdfi_nfd_of(uint32_t c, size_t *n) {
  if (c < 0xC0) {
    return NULL;
  }
  size_t lo = 0, hi = sizeof bdfi_nfd_index / sizeof bdfi_nfd_index[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < bdfi_nfd_index[mid][0]) {
      hi = mid;
    } else if (c > bdfi_nfd_index[mid][0]) {
      lo = mid + 1;
    } else {
      *n = bdfi_nfd_index[mid][2];
      return &bdfi_nfd_pool[bdfi_nfd_index[mid][1]];
    }
  }
  return NULL;
}

/* bdfi_nfkd_of is bdfi_nfd_of for form KD: the full compatibility
 * decomposition of a character whose is not the canonical one. */
static const uint32_t *bdfi_nfkd_of(uint32_t c, size_t *n) {
  if (c < 0xA0) {
    return NULL;
  }
  size_t lo = 0, hi = sizeof bdfi_nfkd_index / sizeof bdfi_nfkd_index[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < bdfi_nfkd_index[mid][0]) {
      hi = mid;
    } else if (c > bdfi_nfkd_index[mid][0]) {
      lo = mid + 1;
    } else {
      *n = bdfi_nfkd_index[mid][2];
      return &bdfi_nfkd_pool[bdfi_nfkd_index[mid][1]];
    }
  }
  return NULL;
}

/* bdfi_compose_pair returns what two characters compose to, or 0. */
static uint32_t bdfi_compose_pair(uint32_t a, uint32_t b) {
  if (a >= BDFI_HANGUL_L && a < BDFI_HANGUL_L + BDFI_HANGUL_LCOUNT && b >= BDFI_HANGUL_V &&
      b < BDFI_HANGUL_V + BDFI_HANGUL_VCOUNT) {
    return BDFI_HANGUL_S +
           ((a - BDFI_HANGUL_L) * BDFI_HANGUL_VCOUNT + (b - BDFI_HANGUL_V)) * BDFI_HANGUL_TCOUNT;
  }
  if (a >= BDFI_HANGUL_S && a < BDFI_HANGUL_S + BDFI_HANGUL_SCOUNT &&
      (a - BDFI_HANGUL_S) % BDFI_HANGUL_TCOUNT == 0 && b > BDFI_HANGUL_T &&
      b < BDFI_HANGUL_T + BDFI_HANGUL_TCOUNT) {
    return a + (b - BDFI_HANGUL_T);
  }
  size_t lo = 0, hi = sizeof bdfi_nfc_pairs / sizeof bdfi_nfc_pairs[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const uint32_t *p = bdfi_nfc_pairs[mid];
    if (a < p[0] || (a == p[0] && b < p[1])) {
      hi = mid;
    } else if (a > p[0] || b > p[1]) {
      lo = mid + 1;
    } else {
      return p[2];
    }
  }
  return 0;
}

/* bdfi_utf8_decode reads one character at s: its length and value, or 0 for
 * bytes that are not UTF-8. */
static size_t bdfi_utf8_decode(const uint8_t *s, size_t n, uint32_t *out) {
  uint8_t c = s[0];
  if (c < 0x80) {
    *out = c;
    return 1;
  }
  size_t len;
  uint8_t lo = 0x80, hi = 0xBF;
  uint32_t v;
  if (c >= 0xC2 && c <= 0xDF) {
    len = 2;
    v = c & 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) {
    len = 3;
    v = c & 0x0F;
    if (c == 0xE0) {
      lo = 0xA0;
    } else if (c == 0xED) {
      hi = 0x9F;
    }
  } else if (c >= 0xF0 && c <= 0xF4) {
    len = 4;
    v = c & 0x07;
    if (c == 0xF0) {
      lo = 0x90;
    } else if (c == 0xF4) {
      hi = 0x8F;
    }
  } else {
    return 0;
  }
  if (n < len || s[1] < lo || s[1] > hi) {
    return 0;
  }
  v = (v << 6) | (s[1] & 0x3F);
  for (size_t i = 2; i < len; i++) {
    if (s[i] < 0x80 || s[i] > 0xBF) {
      return 0;
    }
    v = (v << 6) | (s[i] & 0x3F);
  }
  *out = v;
  return len;
}

static size_t bdfi_utf8_encode(uint32_t c, uint8_t *out) {
  if (c >= BDFI_RAW_BYTE) {
    out[0] = (uint8_t)(c - BDFI_RAW_BYTE);
    return 1;
  }
  if (c < 0x80) {
    out[0] = (uint8_t)c;
    return 1;
  }
  if (c < 0x800) {
    out[0] = (uint8_t)(0xC0 | (c >> 6));
    out[1] = (uint8_t)(0x80 | (c & 0x3F));
    return 2;
  }
  if (c < 0x10000) {
    out[0] = (uint8_t)(0xE0 | (c >> 12));
    out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 | (c & 0x3F));
    return 3;
  }
  out[0] = (uint8_t)(0xF0 | (c >> 18));
  out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
  out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
  out[3] = (uint8_t)(0x80 | (c & 0x3F));
  return 4;
}

uint32_t bdfi_utf8_next(const char *s, size_t n, size_t i, size_t *len) {
  const uint8_t *b = (const uint8_t *)s;
  uint8_t c = b[i];
  if (c < 0x80) {
    *len = 1;
    return c;
  }
  size_t need = 0;
  uint8_t lo = 0x80, hi = 0xBF;
  uint32_t v = 0;
  if (c >= 0xC2 && c <= 0xDF) {
    need = 1;
    v = c & 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) {
    need = 2;
    v = c & 0x0F;
    lo = c == 0xE0 ? 0xA0 : 0x80;
    hi = c == 0xED ? 0x9F : 0xBF;
  } else if (c >= 0xF0 && c <= 0xF4) {
    need = 3;
    v = c & 0x07;
    lo = c == 0xF0 ? 0x90 : 0x80;
    hi = c == 0xF4 ? 0x8F : 0xBF;
  }
  size_t k = 1, got = 0;
  while (got < need && i + k < n && b[i + k] >= lo && b[i + k] <= hi) {
    v = (v << 6) | (b[i + k] & 0x3F);
    k++;
    got++;
    lo = 0x80;
    hi = 0xBF;
  }
  *len = k;
  return need > 0 && got == need ? v : 0xFFFD;
}

size_t bdfi_utf8_put(uint32_t c, char *out) { return bdfi_utf8_encode(c, (uint8_t *)out); }

typedef bdfi_points bdfi_cps;

static int bdfi_cps_push(bdfi_cps *b, uint32_t c) {
  if (b->n == b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 64;
    uint32_t *nc = bdfi_realloc_array(b->c, cap, sizeof *nc);
    if (!nc) {
      return -1;
    }
    b->c = nc;
    uint8_t *ncc = bdfi_realloc_array(b->cc, cap, 1);
    if (!ncc) {
      return -1;
    }
    b->cc = ncc;
    b->cap = cap;
  }
  b->c[b->n] = c;
  b->cc[b->n] = bdfi_ccc(c);
  b->n++;
  return 0;
}

/* bdfi_reorder puts every run of combining marks in the order of their
 * classes, keeping the order of marks of one class (the canonical ordering).
 * A counting sort: a run of any length takes time in proportion to it. */
static int bdfi_reorder(bdfi_cps *b) {
  uint32_t *tmp = NULL;
  size_t tmp_cap = 0;
  for (size_t i = 0; i < b->n;) {
    if (b->cc[i] == 0) {
      i++;
      continue;
    }
    size_t j = i;
    int sorted = 1;
    while (j < b->n && b->cc[j] != 0) {
      if (j > i && b->cc[j] < b->cc[j - 1]) {
        sorted = 0;
      }
      j++;
    }
    size_t len = j - i;
    if (!sorted) {
      if (len > tmp_cap) {
        uint32_t *grown = bdfi_realloc_array(tmp, len, sizeof *grown);
        if (!grown) {
          free(tmp);
          return -1;
        }
        tmp = grown;
        tmp_cap = len;
      }
      size_t count[257] = {0};
      for (size_t k = i; k < j; k++) {
        count[b->cc[k] + 1]++;
      }
      for (size_t k = 1; k < 257; k++) {
        count[k] += count[k - 1];
      }
      for (size_t k = i; k < j; k++) {
        tmp[count[b->cc[k]]++] = b->c[k];
      }
      for (size_t k = 0; k < len; k++) {
        b->c[i + k] = tmp[k];
        b->cc[i + k] = bdfi_ccc(tmp[k]);
      }
    }
    i = j;
  }
  free(tmp);
  return 0;
}

/* The decomposition of a character in form KD (the compatibility one, or
 * the canonical one where that is all it has), added to d. */
static int bdfi_decompose(bdfi_cps *d, uint32_t c) {
  size_t m = 0;
  const uint32_t *seq = bdfi_nfkd_of(c, &m);
  if (!seq) {
    seq = bdfi_nfd_of(c, &m);
  }
  if (seq) {
    for (size_t k = 0; k < m; k++) {
      if (bdfi_cps_push(d, seq[k]) < 0) {
        return -1;
      }
    }
    return 0;
  }
  if (c >= BDFI_HANGUL_S && c < BDFI_HANGUL_S + BDFI_HANGUL_SCOUNT) {
    uint32_t si = c - BDFI_HANGUL_S;
    uint32_t t = si % BDFI_HANGUL_TCOUNT;
    if (bdfi_cps_push(d, BDFI_HANGUL_L + si / BDFI_HANGUL_NCOUNT) < 0 ||
        bdfi_cps_push(d, BDFI_HANGUL_V + (si % BDFI_HANGUL_NCOUNT) / BDFI_HANGUL_TCOUNT) < 0 ||
        (t != 0 && bdfi_cps_push(d, BDFI_HANGUL_T + t) < 0)) {
      return -1;
    }
    return 0;
  }
  return bdfi_cps_push(d, c);
}

/* Composes decomposed characters in their canonical order, in place, and
 * returns how many are left: a character composes with the last starter
 * unless a character between them blocks it (one of class 0, or of its
 * class or a higher one). */
static size_t bdfi_compose(bdfi_cps *d) {
  size_t k = 0;
  size_t starter = SIZE_MAX;
  uint8_t last_cc = 0;
  for (size_t i = 0; i < d->n; i++) {
    uint32_t c = d->c[i];
    uint8_t cc = d->cc[i];
    if (starter != SIZE_MAX && (k == starter + 1 || (last_cc != 0 && last_cc < cc))) {
      uint32_t composite = bdfi_compose_pair(d->c[starter], c);
      if (composite) {
        d->c[starter] = composite;
        continue;
      }
    }
    if (cc == 0) {
      starter = k;
    }
    last_cc = cc;
    d->c[k++] = c;
  }
  return k;
}

int bdfi_nfkc_points(bdfi_points *w, const uint32_t *in, size_t n) {
  w->n = 0;
  for (size_t i = 0; i < n; i++) {
    if (bdfi_decompose(w, in[i]) < 0) {
      return -1;
    }
  }
  if (bdfi_reorder(w) < 0) {
    return -1;
  }
  w->n = bdfi_compose(w);
  return 0;
}

int bdfi_is_mark(uint32_t c) {
  size_t lo = 0, hi = sizeof bdfi_mark_ranges / sizeof bdfi_mark_ranges[0];
  if (c < bdfi_mark_ranges[0][0]) {
    return 0;
  }
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < bdfi_mark_ranges[mid][0]) {
      hi = mid;
    } else if (c > bdfi_mark_ranges[mid][1]) {
      lo = mid + 1;
    } else {
      return 1;
    }
  }
  return 0;
}

int bdfi_nfkc_alone(uint32_t c) {
  size_t n;
  return bdfi_ccc(c) == 0 && !(c >= BDFI_HANGUL_S && c < BDFI_HANGUL_S + BDFI_HANGUL_SCOUNT) &&
         !bdfi_nfkd_of(c, &n) && !bdfi_nfd_of(c, &n) && !bdfi_is_mark(c);
}

int bdfi_composes(uint32_t a, uint32_t b) { return bdfi_compose_pair(a, b) != 0; }

/* Form KC: the compatibility decompositions are taken, and what they give
 * composes as in form C. */
int bdfi_nfkc(const char *s, size_t n, char **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0;
  const uint8_t *b = (const uint8_t *)s;
  size_t ascii = 0;
  while (ascii < n && b[ascii] < 0x80) {
    ascii++;
  }
  if (ascii == n) {
    /* ASCII is in every normalisation form */
    char *copy = malloc(n + 1);
    if (!copy) {
      return -1;
    }
    if (n > 0) {
      memcpy(copy, s, n);
    }
    copy[n] = '\0';
    *out = copy;
    *out_len = n;
    return 0;
  }

  /* decompose */
  bdfi_cps d = {0};
  int rc = -1;
  for (size_t i = 0; i < n;) {
    /* A character that the text ends in the middle of: the reference's
     * library takes what is left as it is, whatever follows the first byte
     * (it normalises streams, where the rest may still come). */
    if ((b[i] >= 0xE0 && b[i] < 0xF0 && n - i < 3) || (b[i] >= 0xF0 && b[i] < 0xF8 && n - i < 4)) {
      for (; i < n; i++) {
        if (bdfi_cps_push(&d, BDFI_RAW_BYTE + b[i]) < 0) {
          goto done;
        }
      }
      break;
    }
    uint32_t c;
    size_t len = bdfi_utf8_decode(b + i, n - i, &c);
    if (len == 0) {
      c = BDFI_RAW_BYTE + b[i];
      len = 1;
    }
    i += len;
    if (bdfi_decompose(&d, c) < 0) {
      goto done;
    }
  }
  if (bdfi_reorder(&d) < 0) {
    goto done;
  }
  size_t k = bdfi_compose(&d);

  /* encode */
  {
    size_t bytes = k > SIZE_MAX / 4 - 1 ? 0 : k * 4 + 1;
    uint8_t *o = bytes ? malloc(bytes) : NULL;
    if (!o) {
      goto done;
    }
    size_t w = 0;
    for (size_t i = 0; i < k; i++) {
      w += bdfi_utf8_encode(d.c[i], o + w);
    }
    o[w] = '\0';
    *out = (char *)o;
    *out_len = w;
    rc = 0;
  }
done:
  free(d.c);
  free(d.cc);
  return rc;
}

/* unicode.ToLower of the reference's language. */
uint32_t bdfi_to_lower(uint32_t c) {
  if (c < 0x80) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
  }
  size_t lo = 0, hi = sizeof bdfi_lower_pairs / sizeof bdfi_lower_pairs[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < bdfi_lower_pairs[mid][0]) {
      hi = mid;
    } else if (c > bdfi_lower_pairs[mid][0]) {
      lo = mid + 1;
    } else {
      return bdfi_lower_pairs[mid][1];
    }
  }
  return c;
}

/* The direction of a character: 1 for right to left (the bidirectional
 * classes R and AL), 0 for left to right (L, EN and AN), -1 for the others,
 * which take the direction around them. */
int bdfi_bidi_direction(uint32_t c) {
  size_t lo = 0, hi = sizeof bdfi_bidi_ranges / sizeof bdfi_bidi_ranges[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c < bdfi_bidi_ranges[mid][0]) {
      hi = mid;
    } else if (c > bdfi_bidi_ranges[mid][1]) {
      lo = mid + 1;
    } else {
      return (int)bdfi_bidi_ranges[mid][2];
    }
  }
  return -1;
}

/* bdfi_utf8_rune decodes as the reference's language does: a byte that
 * starts no character is U+FFFD, one byte long. */
uint32_t bdfi_utf8_rune(const char *s, size_t n, size_t *len) {
  uint32_t c;
  *len = bdfi_utf8_decode((const uint8_t *)s, n, &c);
  if (*len == 0) {
    *len = 1;
    return 0xFFFD;
  }
  return c;
}

/* Unicode's White_Space, as unicode.IsSpace has it. */
int bdfi_go_is_space(uint32_t c) {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
         c == 0x3000;
}

/*
 * fontdb.Normalize: a family name folded for comparison: form KC (full-width
 * letters become ASCII), lower case, without spaces, hyphens and
 * underscores. The key is the caller's to free; -1 when out of memory.
 */
int bdfi_family_key(const char *name, size_t n, char **out, size_t *out_len) {
  char *kc;
  size_t kn;
  *out = NULL;
  *out_len = 0;
  if (bdfi_nfkc(name, n, &kc, &kn) < 0) {
    return -1;
  }
  /* a character is at most three bytes longer in lower case, and a byte
   * that is no character becomes the three of U+FFFD */
  char *key = bdfi_malloc_array(kn + 1, 3);
  if (!key) {
    free(kc);
    return -1;
  }
  size_t w = 0;
  for (size_t i = 0; i < kn;) {
    size_t len;
    uint32_t c = bdfi_to_lower(bdfi_utf8_rune(kc + i, kn - i, &len));
    i += len;
    if (c == ' ' || c == '-' || c == '_' || c == 0x3000) {
      continue;
    }
    w += bdfi_utf8_encode(c, (uint8_t *)key + w);
  }
  key[w] = '\0';
  free(kc);
  *out = key;
  *out_len = w;
  return 0;
}
