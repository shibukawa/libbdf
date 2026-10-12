/*
 * A raw deflate (RFC 1951) decoder for the parts and the manifest. It is
 * written for clarity and for the checks the format asks for: the output is
 * bounded by the size the manifest states, and malformed streams fail
 * rather than guess. It accepts what the reference implementation's
 * decoder (compress/flate) accepts: a single code of length one as the only
 * incomplete Huffman code, and nothing past the final block is looked at.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

#define BDFI_INF_MAXBITS 15
#define BDFI_INF_MAXLCODES 286
#define BDFI_INF_MAXDCODES 30
#define BDFI_INF_FIXLCODES 288

typedef struct bdfi_inf {
  const uint8_t *in;
  size_t in_len, in_pos;
  uint32_t bitbuf;
  int bitcnt;
  uint8_t *out;
  size_t out_len, out_cap, limit;
  const char *err; /* the first failure */
} bdfi_inf;

typedef struct bdfi_huff {
  uint16_t count[BDFI_INF_MAXBITS + 1];
  uint16_t symbol[BDFI_INF_FIXLCODES];
} bdfi_huff;

static int bdfi_inf_fail(bdfi_inf *s, const char *msg) {
  if (!s->err) {
    s->err = msg;
  }
  return -1;
}

/* bdfi_inf_bits returns the next need bits (need <= 16), or -1 at the end. */
static int bdfi_inf_bits(bdfi_inf *s, int need) {
  uint32_t val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->in_pos == s->in_len) {
      bdfi_inf_fail(s, "unexpected end of deflate stream");
      return -1;
    }
    val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = val >> need;
  s->bitcnt -= need;
  return (int)(val & ((1u << need) - 1));
}

/* bdfi_inf_put appends a byte, within the limit. */
static int bdfi_inf_put(bdfi_inf *s, uint8_t b) {
  if (s->out_len == s->limit) {
    return bdfi_inf_fail(s, "data inflates to more than its stated size");
  }
  if (s->out_len == s->out_cap) {
    size_t cap = s->out_cap ? s->out_cap * 2 : 1024;
    if (cap > s->limit) {
      cap = s->limit;
    }
    uint8_t *p = realloc(s->out, cap);
    if (!p) {
      return bdfi_inf_fail(s, "out of memory");
    }
    s->out = p;
    s->out_cap = cap;
  }
  s->out[s->out_len++] = b;
  return 0;
}

/*
 * bdfi_huff_build makes the canonical code of the given lengths. It returns
 * 0 for a complete code, a positive number for an incomplete one and a
 * negative one for an over-subscribed one.
 */
static int bdfi_huff_build(bdfi_huff *h, const uint16_t *length, int n) {
  int offs[BDFI_INF_MAXBITS + 1];
  for (int len = 0; len <= BDFI_INF_MAXBITS; len++) {
    h->count[len] = 0;
  }
  for (int sym = 0; sym < n; sym++) {
    h->count[length[sym]]++;
  }
  if (h->count[0] == n) {
    return 0; /* no codes: complete and empty, as in puff */
  }
  int left = 1;
  for (int len = 1; len <= BDFI_INF_MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) {
      return left;
    }
  }
  offs[1] = 0;
  for (int len = 1; len < BDFI_INF_MAXBITS; len++) {
    offs[len + 1] = offs[len] + h->count[len];
  }
  for (int sym = 0; sym < n; sym++) {
    if (length[sym] != 0) {
      h->symbol[offs[length[sym]]++] = (uint16_t)sym;
    }
  }
  return left;
}

/* bdfi_huff_decode reads one symbol, or -1 on a bad code or the end. */
static int bdfi_huff_decode(bdfi_inf *s, const bdfi_huff *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= BDFI_INF_MAXBITS; len++) {
    int bit = bdfi_inf_bits(s, 1);
    if (bit < 0) {
      return -1;
    }
    code |= bit;
    int count = h->count[len];
    if (code - count < first) {
      return h->symbol[index + (code - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return bdfi_inf_fail(s, "corrupt deflate stream");
}

/* bdfi_huff_usable accepts a complete code, or the one incomplete code the
 * reference accepts too: a single symbol of length one. */
static int bdfi_huff_usable(const bdfi_huff *h, int left, int n) {
  return left == 0 || (left > 0 && h->count[0] + h->count[1] == n && h->count[1] == 1);
}

static const uint16_t bdfi_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                            15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                            67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint16_t bdfi_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t bdfi_dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                             33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint16_t bdfi_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* bdfi_inf_codes decodes the data of a block with its two codes. */
static int bdfi_inf_codes(bdfi_inf *s, const bdfi_huff *lencode, const bdfi_huff *distcode) {
  for (;;) {
    int symbol = bdfi_huff_decode(s, lencode);
    if (symbol < 0) {
      return -1;
    }
    if (symbol < 256) {
      if (bdfi_inf_put(s, (uint8_t)symbol) < 0) {
        return -1;
      }
      continue;
    }
    if (symbol == 256) {
      return 0;
    }
    symbol -= 257;
    if (symbol >= 29) {
      return bdfi_inf_fail(s, "corrupt deflate stream");
    }
    int extra = bdfi_inf_bits(s, bdfi_len_extra[symbol]);
    if (extra < 0) {
      return -1;
    }
    size_t len = (size_t)bdfi_len_base[symbol] + (size_t)extra;
    symbol = bdfi_huff_decode(s, distcode);
    if (symbol < 0) {
      return -1;
    }
    if (symbol >= 30) {
      return bdfi_inf_fail(s, "corrupt deflate stream");
    }
    extra = bdfi_inf_bits(s, bdfi_dist_extra[symbol]);
    if (extra < 0) {
      return -1;
    }
    size_t dist = (size_t)bdfi_dist_base[symbol] + (size_t)extra;
    if (dist > s->out_len) {
      return bdfi_inf_fail(s, "corrupt deflate stream");
    }
    for (size_t i = 0; i < len; i++) {
      if (bdfi_inf_put(s, s->out[s->out_len - dist]) < 0) {
        return -1;
      }
    }
  }
}

static int bdfi_inf_stored(bdfi_inf *s) {
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->in_len - s->in_pos < 4) {
    return bdfi_inf_fail(s, "unexpected end of deflate stream");
  }
  unsigned len = s->in[s->in_pos] | ((unsigned)s->in[s->in_pos + 1] << 8);
  unsigned nlen = s->in[s->in_pos + 2] | ((unsigned)s->in[s->in_pos + 3] << 8);
  s->in_pos += 4;
  if (len != (~nlen & 0xffffu)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  if (s->in_len - s->in_pos < len) {
    return bdfi_inf_fail(s, "unexpected end of deflate stream");
  }
  for (unsigned i = 0; i < len; i++) {
    if (bdfi_inf_put(s, s->in[s->in_pos++]) < 0) {
      return -1;
    }
  }
  return 0;
}

static int bdfi_inf_fixed(bdfi_inf *s) {
  bdfi_huff lencode, distcode;
  uint16_t lengths[BDFI_INF_FIXLCODES];
  int sym = 0;
  for (; sym < 144; sym++) {
    lengths[sym] = 8;
  }
  for (; sym < 256; sym++) {
    lengths[sym] = 9;
  }
  for (; sym < 280; sym++) {
    lengths[sym] = 7;
  }
  for (; sym < BDFI_INF_FIXLCODES; sym++) {
    lengths[sym] = 8;
  }
  bdfi_huff_build(&lencode, lengths, BDFI_INF_FIXLCODES);
  for (sym = 0; sym < BDFI_INF_MAXDCODES; sym++) {
    lengths[sym] = 5;
  }
  bdfi_huff_build(&distcode, lengths, BDFI_INF_MAXDCODES);
  return bdfi_inf_codes(s, &lencode, &distcode);
}

static int bdfi_inf_dynamic(bdfi_inf *s) {
  static const uint16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint16_t lengths[BDFI_INF_MAXLCODES + BDFI_INF_MAXDCODES];
  bdfi_huff lencode, distcode;
  int nlen = bdfi_inf_bits(s, 5), ndist = bdfi_inf_bits(s, 5), ncode = bdfi_inf_bits(s, 4);
  if (nlen < 0 || ndist < 0 || ncode < 0) {
    return -1;
  }
  nlen += 257;
  ndist += 1;
  ncode += 4;
  if (nlen > BDFI_INF_MAXLCODES || ndist > BDFI_INF_MAXDCODES) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  int index = 0;
  for (; index < ncode; index++) {
    int v = bdfi_inf_bits(s, 3);
    if (v < 0) {
      return -1;
    }
    lengths[order[index]] = (uint16_t)v;
  }
  for (; index < 19; index++) {
    lengths[order[index]] = 0;
  }
  int left = bdfi_huff_build(&lencode, lengths, 19);
  if (!bdfi_huff_usable(&lencode, left, 19)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  index = 0;
  while (index < nlen + ndist) {
    int symbol = bdfi_huff_decode(s, &lencode);
    if (symbol < 0) {
      return -1;
    }
    if (symbol < 16) {
      lengths[index++] = (uint16_t)symbol;
      continue;
    }
    uint16_t len = 0;
    int rep;
    if (symbol == 16) {
      if (index == 0) {
        return bdfi_inf_fail(s, "corrupt deflate stream");
      }
      len = lengths[index - 1];
      rep = bdfi_inf_bits(s, 2);
      if (rep < 0) {
        return -1;
      }
      rep += 3;
    } else if (symbol == 17) {
      rep = bdfi_inf_bits(s, 3);
      if (rep < 0) {
        return -1;
      }
      rep += 3;
    } else {
      rep = bdfi_inf_bits(s, 7);
      if (rep < 0) {
        return -1;
      }
      rep += 11;
    }
    if (index + rep > nlen + ndist) {
      return bdfi_inf_fail(s, "corrupt deflate stream");
    }
    while (rep--) {
      lengths[index++] = len;
    }
  }
  if (lengths[256] == 0) {
    return bdfi_inf_fail(s, "corrupt deflate stream"); /* no end-of-block code */
  }
  left = bdfi_huff_build(&lencode, lengths, nlen);
  if (!bdfi_huff_usable(&lencode, left, nlen)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  left = bdfi_huff_build(&distcode, lengths + nlen, ndist);
  if (!bdfi_huff_usable(&distcode, left, ndist)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  return bdfi_inf_codes(s, &lencode, &distcode);
}

bdf_status bdf_inflate(const uint8_t *data, size_t len, size_t limit, uint8_t **out,
                       size_t *out_len, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || !out_len || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  bdfi_inf s;
  memset(&s, 0, sizeof s);
  s.in = data;
  s.in_len = len;
  s.limit = limit;
  int last, type;
  do {
    last = bdfi_inf_bits(&s, 1);
    type = bdfi_inf_bits(&s, 2);
    if (last < 0 || type < 0) {
      break;
    }
    int rc;
    switch (type) {
    case 0:
      rc = bdfi_inf_stored(&s);
      break;
    case 1:
      rc = bdfi_inf_fixed(&s);
      break;
    case 2:
      rc = bdfi_inf_dynamic(&s);
      break;
    default:
      rc = bdfi_inf_fail(&s, "corrupt deflate stream");
      break;
    }
    if (rc < 0) {
      break;
    }
  } while (!last);
  if (s.err) {
    free(s.out);
    bdf_status st = strcmp(s.err, "out of memory") == 0 ? BDF_ERR_NOMEM : BDF_ERR_FORMAT;
    bdfi_error_set(err, st, s.in_pos, s.err);
    return st;
  }
  if (!s.out) {
    s.out = malloc(1); /* a non-NULL result for empty data */
    if (!s.out) {
      bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
      return BDF_ERR_NOMEM;
    }
  }
  *out = s.out;
  *out_len = s.out_len;
  return BDF_OK;
}
