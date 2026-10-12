/*
 * Unicode normalisation form C, for passwords (spec §3.5: a password is
 * normalised to NFC before a key is derived from it). The tables are those
 * of the reference implementation's library (unicode_nfc.h is generated
 * from it); the algorithm is that of UAX #15.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"
#include "unicode_nfc.h"

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

typedef struct bdfi_cps {
  uint32_t *c;
  uint8_t *cc; /* the combining class of each */
  size_t n, cap;
} bdfi_cps;

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

int bdfi_nfc(const char *s, size_t n, char **out, size_t *out_len) {
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
    size_t m;
    const uint32_t *seq = bdfi_nfd_of(c, &m);
    if (seq) {
      for (size_t k = 0; k < m; k++) {
        if (bdfi_cps_push(&d, seq[k]) < 0) {
          goto done;
        }
      }
    } else if (c >= BDFI_HANGUL_S && c < BDFI_HANGUL_S + BDFI_HANGUL_SCOUNT) {
      uint32_t si = c - BDFI_HANGUL_S;
      uint32_t t = si % BDFI_HANGUL_TCOUNT;
      if (bdfi_cps_push(&d, BDFI_HANGUL_L + si / BDFI_HANGUL_NCOUNT) < 0 ||
          bdfi_cps_push(&d, BDFI_HANGUL_V + (si % BDFI_HANGUL_NCOUNT) / BDFI_HANGUL_TCOUNT) < 0 ||
          (t != 0 && bdfi_cps_push(&d, BDFI_HANGUL_T + t) < 0)) {
        goto done;
      }
    } else if (bdfi_cps_push(&d, c) < 0) {
      goto done;
    }
  }
  if (bdfi_reorder(&d) < 0) {
    goto done;
  }

  /* compose, in place: a character composes with the last starter unless a
   * character between them blocks it (one of class 0, or of its class or a
   * higher one) */
  size_t k = 0;
  size_t starter = SIZE_MAX;
  uint8_t last_cc = 0;
  for (size_t i = 0; i < d.n; i++) {
    uint32_t c = d.c[i];
    uint8_t cc = d.cc[i];
    if (starter != SIZE_MAX && (k == starter + 1 || (last_cc != 0 && last_cc < cc))) {
      uint32_t composite = bdfi_compose_pair(d.c[starter], c);
      if (composite) {
        d.c[starter] = composite;
        continue;
      }
    }
    if (cc == 0) {
      starter = k;
    }
    last_cc = cc;
    d.c[k++] = c;
  }

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
