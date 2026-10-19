/*
 * A raw deflate (RFC 1951) decoder for the parts and the manifest. It is
 * written for clarity and for the checks the format asks for: the output is
 * bounded by the size the manifest states, and malformed streams fail
 * rather than guess. It accepts what the reference implementation's
 * decoder (compress/flate) accepts: a single code of length one as the only
 * incomplete Huffman code, and nothing past the final block is looked at.
 *
 * A symbol is found by the bits of its code through a table of the short
 * codes, which most are, and a bit at a time for the rest. The bits are
 * taken ahead of need, but only from the stretch of input at hand, and the
 * whole bytes among them are given back wherever the position matters:
 * the input is then where a reader that takes a bit at a time leaves it.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

#define BDFI_INF_MAXBITS 15
#define BDFI_INF_MAXLCODES 286
#define BDFI_INF_MAXDCODES 30
#define BDFI_INF_FIXLCODES 288
/* The codes of at most this many bits are found in one step. */
#define BDFI_INF_FAST_BITS 10
#define BDFI_INF_FAST_SIZE (1 << BDFI_INF_FAST_BITS)

typedef struct bdfi_huff {
  uint16_t count[BDFI_INF_MAXBITS + 1];
  uint16_t symbol[BDFI_INF_FIXLCODES];
  /* the symbol (above the low four bits) and the length (in them) of each
   * code of at most BDFI_INF_FAST_BITS bits, by the bits the stream gives
   * first; 0 where the bits start a longer code, or none */
  uint16_t fast[BDFI_INF_FAST_SIZE];
  /* for the longer codes: the first code of each length, and where the
   * symbols of that length start */
  uint16_t first[BDFI_INF_MAXBITS + 1], index[BDFI_INF_MAXBITS + 1];
} bdfi_huff;

typedef struct bdfi_inf {
  const uint8_t *in;
  size_t in_len, in_pos;
  uint64_t bitbuf;
  int bitcnt;
  bdfi_pull *pull; /* where more input comes from, for a stream in stretches */
  uint8_t *out;
  size_t out_len, out_cap, limit;
  size_t out_mask; /* SIZE_MAX for contiguous output, window size - 1 for a sink */
  int (*emit)(void *, const uint8_t *, size_t);
  void *emit_ctx;
  const char *err; /* the first failure */
  /* the codes of a block, here rather than on the stack of each block */
  bdfi_huff lencode, distcode;
  uint16_t lengths[BDFI_INF_MAXLCODES + BDFI_INF_MAXDCODES];
} bdfi_inf;

static int bdfi_inf_fail(bdfi_inf *s, const char *msg) {
  if (!s->err) {
    s->err = msg;
  }
  return -1;
}

/* bdfi_inf_more asks a stream in stretches for its next one, of at most
 * want bytes, once the one at hand is used up. It returns 0 at the end. */
static int bdfi_inf_more(bdfi_inf *s, size_t want) {
  bdfi_pull *in = s->pull;
  if (!in) {
    return 0;
  }
  in->pos = s->in_pos;
  if (!in->more(in, want)) {
    return 0;
  }
  s->in = in->p;
  s->in_len = in->n;
  s->in_pos = in->pos;
  return s->in_pos < s->in_len;
}

/* bdfi_inf_fill takes into the bit buffer what bytes of the stretch at hand
 * fit in it. It asks for no other stretch: a stretch is asked for only when
 * a bit of it is needed. */
static void bdfi_inf_fill(bdfi_inf *s) {
  if (s->bitcnt > 32) {
    return; /* enough for a code and what follows it */
  }
  if (s->in_len - s->in_pos >= 4) {
    const uint8_t *p = s->in + s->in_pos;
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    s->bitbuf |= (uint64_t)v << s->bitcnt;
    s->bitcnt += 32;
    s->in_pos += 4;
    return;
  }
  while (s->bitcnt <= 56 && s->in_pos < s->in_len) {
    s->bitbuf |= (uint64_t)s->in[s->in_pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
}

/*
 * bdfi_inf_unread gives back the whole bytes in the bit buffer. They were
 * taken ahead from the stretch at hand (a bit taken as it is needed leaves
 * fewer than eight behind, and a stretch is not left while whole bytes of
 * it are in the buffer), so the input is then where it would be had every
 * bit been taken as it was needed.
 */
static void bdfi_inf_unread(bdfi_inf *s) {
  int back = s->bitcnt >> 3;
  s->in_pos -= (size_t)back;
  s->bitcnt -= 8 * back;
  s->bitbuf &= ((uint64_t)1 << s->bitcnt) - 1;
}

/* bdfi_inf_bits returns the next need bits (need <= 16), or -1 at the end.
 * It takes no byte before a bit of it is needed: what follows the stream
 * stays unread. */
static int bdfi_inf_bits(bdfi_inf *s, int need) {
  uint64_t val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->in_pos == s->in_len && !bdfi_inf_more(s, BDFI_PULL_BUFFER)) {
      bdfi_inf_fail(s, "unexpected end of deflate stream");
      return -1;
    }
    val |= (uint64_t)s->in[s->in_pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = val >> need;
  s->bitcnt -= need;
  return (int)(val & (((uint64_t)1 << need) - 1));
}

static int bdfi_inf_reserve(bdfi_inf *s, size_t n) {
  if (n > s->limit - s->out_len) {
    return bdfi_inf_fail(s, "data inflates to more than its stated size");
  }
  if (!s->emit && s->out_len + n > s->out_cap) {
    size_t need = s->out_len + n;
    size_t cap = s->out_cap ? s->out_cap : 1024;
    while (cap < need && cap <= s->limit / 2) {
      cap *= 2;
    }
    if (cap < need || cap > s->limit) {
      cap = s->limit;
    }
    if (cap > (size_t)PTRDIFF_MAX) {
      return bdfi_inf_fail(s, "out of memory");
    }
    uint8_t *p = realloc(s->out, cap);
    if (!p) {
      return bdfi_inf_fail(s, "out of memory");
    }
    s->out = p;
    s->out_cap = cap;
  }
  return 0;
}

/* bdfi_inf_flush hands a sink its window when it has just been filled. */
static int bdfi_inf_flush(bdfi_inf *s) {
  if (s->emit && (s->out_len & s->out_mask) == 0 &&
      s->emit(s->emit_ctx, s->out, s->out_cap) < 0) {
    return bdfi_inf_fail(s, "invalid inflated data");
  }
  return 0;
}

/* Append to the output or the sliding window, after reserving space. */
static int bdfi_inf_write(bdfi_inf *s, uint8_t b) {
  s->out[s->out_len++ & s->out_mask] = b;
  return bdfi_inf_flush(s);
}

static int bdfi_inf_put(bdfi_inf *s, uint8_t b) {
  return bdfi_inf_reserve(s, 1) < 0 ? -1 : bdfi_inf_write(s, b);
}

static int bdfi_inf_bytes(bdfi_inf *s, const uint8_t *p, size_t n) {
  if (bdfi_inf_reserve(s, n) < 0) {
    return -1;
  }
  while (n) {
    size_t at = s->out_len & s->out_mask;
    size_t take = s->out_cap - at;
    if (take > n) {
      take = n;
    }
    memcpy(s->out + at, p, take);
    s->out_len += take;
    p += take;
    n -= take;
    if (bdfi_inf_flush(s) < 0) {
      return -1;
    }
  }
  return 0;
}

/*
 * bdfi_inf_copy appends len bytes from dist bytes back (dist <= out_len,
 * and at most the window of a sink): the bytes of a run that goes on past
 * where it started are the run again, as a copy a byte at a time makes
 * them. A sink is handed its window at the same points as by such a copy.
 */
static int bdfi_inf_copy(bdfi_inf *s, size_t dist, size_t len) {
  if (bdfi_inf_reserve(s, len) < 0) {
    return -1;
  }
  while (len > 0) {
    size_t at = s->out_len & s->out_mask;
    size_t from = (s->out_len - dist) & s->out_mask;
    size_t run = len;
    if (s->emit) {
      /* to the end of the window, for either end of the copy */
      if (run > s->out_cap - at) {
        run = s->out_cap - at;
      }
      if (run > s->out_cap - from) {
        run = s->out_cap - from;
      }
    }
    uint8_t *d = s->out + at;
    const uint8_t *p = s->out + from;
    if (dist >= run) {
      /* the source is behind the destination or, in a window, wholly
       * before it: in the order of a copy a byte at a time */
      memmove(d, p, run);
    } else if (dist == 1) {
      memset(d, p[0], run);
    } else {
      /* the pattern of dist bytes repeats */
      size_t left = run;
      while (left > 0) {
        size_t k = left < dist ? left : dist;
        memcpy(d, p, k);
        d += k;
        left -= k;
      }
    }
    s->out_len += run;
    len -= run;
    if (bdfi_inf_flush(s) < 0) {
      return -1;
    }
  }
  return 0;
}

/*
 * bdfi_huff_build makes the canonical code of the given lengths. It returns
 * 0 for a complete code, a positive number for an incomplete one and a
 * negative one for an over-subscribed one, whose table is not made.
 */
static int bdfi_huff_build(bdfi_huff *h, const uint16_t *length, int n) {
  int offs[BDFI_INF_MAXBITS + 1];
  for (int len = 0; len <= BDFI_INF_MAXBITS; len++) {
    h->count[len] = 0;
  }
  for (int sym = 0; sym < n; sym++) {
    h->count[length[sym]]++;
  }
  memset(h->fast, 0, sizeof h->fast);
  memset(h->first, 0, sizeof h->first);
  memset(h->index, 0, sizeof h->index);
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
  /* the first code of each length: those of a length follow each other in
   * the order of their symbols, after the codes of the lengths before */
  uint32_t next[BDFI_INF_MAXBITS + 1];
  uint32_t code = 0;
  next[1] = 0;
  for (int len = 2; len <= BDFI_INF_MAXBITS; len++) {
    code = (code + h->count[len - 1]) << 1;
    next[len] = code;
  }
  for (int len = 1; len <= BDFI_INF_MAXBITS; len++) {
    h->first[len] = (uint16_t)(next[len] < 0xffff ? next[len] : 0xffff);
    h->index[len] = (uint16_t)offs[len];
  }
  for (int sym = 0; sym < n; sym++) {
    int len = length[sym];
    if (len == 0) {
      continue;
    }
    h->symbol[offs[len]++] = (uint16_t)sym;
    uint32_t c = next[len]++;
    if (len > BDFI_INF_FAST_BITS) {
      continue;
    }
    /* the stream gives the first bit of a code first, and the table is
     * looked up by the bits in that order */
    uint32_t rev = 0;
    for (int i = 0; i < len; i++) {
      rev = (rev << 1) | ((c >> i) & 1);
    }
    for (uint32_t i = rev; i < BDFI_INF_FAST_SIZE; i += 1u << len) {
      h->fast[i] = (uint16_t)(sym << 4 | len);
    }
  }
  return left;
}

/* bdfi_huff_decode reads one symbol, or -1 on a bad code or the end. */
static int bdfi_huff_decode(bdfi_inf *s, const bdfi_huff *h) {
  bdfi_inf_fill(s);
  uint16_t e = h->fast[s->bitbuf & (BDFI_INF_FAST_SIZE - 1)];
  int len = e & 15;
  if (len != 0 && len <= s->bitcnt) {
    s->bitbuf >>= len;
    s->bitcnt -= len;
    return e >> 4;
  }
  if (len == 0 && s->bitcnt >= BDFI_INF_MAXBITS) {
    /* a longer code, with every bit it can take at hand: its first bits
     * are the ones looked up, in the order the stream gives them */
    uint32_t code = 0;
    for (int i = 0; i < BDFI_INF_FAST_BITS; i++) {
      code = (code << 1) | ((s->bitbuf >> i) & 1);
    }
    for (len = BDFI_INF_FAST_BITS + 1; len <= BDFI_INF_MAXBITS; len++) {
      code = (code << 1) | ((s->bitbuf >> (len - 1)) & 1);
      int first = h->first[len];
      if ((int)code - h->count[len] < first) {
        s->bitbuf >>= len;
        s->bitcnt -= len;
        return h->symbol[h->index[len] + ((int)code - first)];
      }
    }
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  /* a stretch that ends within the code: a bit at a time */
  int code = 0, first = 0, index = 0;
  for (len = 1; len <= BDFI_INF_MAXBITS; len++) {
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
      if (!s->emit && s->out_len < s->out_cap && s->out_len < s->limit) {
        s->out[s->out_len++] = (uint8_t)symbol; /* room for it, within the limit */
      } else if (bdfi_inf_put(s, (uint8_t)symbol) < 0) {
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
    if (bdfi_inf_copy(s, dist, len) < 0) {
      return -1;
    }
  }
}

/*
 * bdfi_inf_stored_pulled reads a stored block of a stream in stretches, and
 * asks for the stretches as the reference's decoder (compress/flate behind
 * a bufio.Reader) asks its reader: the four bytes of the lengths through
 * the buffer, and the bytes in runs that end where its window of 32768
 * bytes ends, a run of a buffer or more with nothing buffered being read
 * past the buffer. How much of the input has been taken when the stream
 * ends is what a reader of PNG decides by.
 */
static int bdfi_inf_stored_pulled(bdfi_inf *s) {
  uint8_t head[4];
  for (int i = 0; i < 4; i++) {
    if (s->in_pos == s->in_len && !bdfi_inf_more(s, BDFI_PULL_BUFFER)) {
      return bdfi_inf_fail(s, "unexpected end of deflate stream");
    }
    head[i] = s->in[s->in_pos++];
  }
  unsigned len = head[0] | ((unsigned)head[1] << 8);
  unsigned nlen = head[2] | ((unsigned)head[3] << 8);
  if (len != (~nlen & 0xffffu)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  while (len > 0) {
    size_t run = 32768 - (s->out_len & 32767); /* copyData: to the end of the window */
    if (run > len) {
      run = len;
    }
    len -= (unsigned)run;
    while (run > 0) {
      if (s->in_pos == s->in_len &&
          !bdfi_inf_more(s, run >= BDFI_PULL_BUFFER ? run : BDFI_PULL_BUFFER)) {
        return bdfi_inf_fail(s, "unexpected end of deflate stream");
      }
      size_t take = s->in_len - s->in_pos;
      if (take > run) {
        take = run;
      }
      if (bdfi_inf_bytes(s, s->in + s->in_pos, take) < 0) {
        return -1;
      }
      s->in_pos += take;
      run -= take;
    }
  }
  return 0;
}

static int bdfi_inf_stored(bdfi_inf *s) {
  /* the block starts at a byte: the bits left of the one before it are
   * dropped, and the bytes taken ahead are read as the block's */
  bdfi_inf_unread(s);
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->pull) {
    return bdfi_inf_stored_pulled(s);
  }
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
  int rc = bdfi_inf_bytes(s, s->in + s->in_pos, len);
  s->in_pos += len;
  return rc;
}

static int bdfi_inf_fixed(bdfi_inf *s) {
  uint16_t *lengths = s->lengths;
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
  bdfi_huff_build(&s->lencode, lengths, BDFI_INF_FIXLCODES);
  for (sym = 0; sym < BDFI_INF_MAXDCODES; sym++) {
    lengths[sym] = 5;
  }
  bdfi_huff_build(&s->distcode, lengths, BDFI_INF_MAXDCODES);
  return bdfi_inf_codes(s, &s->lencode, &s->distcode);
}

static int bdfi_inf_dynamic(bdfi_inf *s) {
  static const uint16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint16_t *lengths = s->lengths;
  bdfi_huff *lencode = &s->lencode, *distcode = &s->distcode;
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
  int left = bdfi_huff_build(lencode, lengths, 19);
  if (!bdfi_huff_usable(lencode, left, 19)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  index = 0;
  while (index < nlen + ndist) {
    int symbol = bdfi_huff_decode(s, lencode);
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
  left = bdfi_huff_build(lencode, lengths, nlen);
  if (!bdfi_huff_usable(lencode, left, nlen)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  left = bdfi_huff_build(distcode, lengths + nlen, ndist);
  if (!bdfi_huff_usable(distcode, left, ndist)) {
    return bdfi_inf_fail(s, "corrupt deflate stream");
  }
  return bdfi_inf_codes(s, lencode, distcode);
}

/* bdfi_inf_blocks inflates the blocks of a stream, to the last one, and
 * leaves the input where the stream ends. */
static void bdfi_inf_blocks(bdfi_inf *s) {
  int last, type;
  do {
    last = bdfi_inf_bits(s, 1);
    type = bdfi_inf_bits(s, 2);
    if (last < 0 || type < 0) {
      break;
    }
    int rc;
    switch (type) {
    case 0:
      rc = bdfi_inf_stored(s);
      break;
    case 1:
      rc = bdfi_inf_fixed(s);
      break;
    case 2:
      rc = bdfi_inf_dynamic(s);
      break;
    default:
      rc = bdfi_inf_fail(s, "corrupt deflate stream");
      break;
    }
    if (rc < 0) {
      break;
    }
  } while (!last);
  bdfi_inf_unread(s);
}

BDF_INTERNAL bdf_status bdfi_inflate_sink(bdfi_pull *in, size_t limit,
                                          int (*emit)(void *, const uint8_t *, size_t),
                                          void *ctx, size_t *out_len) {
  *out_len = 0;
  bdfi_inf *s = calloc(1, sizeof *s);
  if (!s) {
    return BDF_ERR_NOMEM;
  }
  s->pull = in;
  s->in = in->p;
  s->in_len = in->n;
  s->in_pos = in->pos;
  s->limit = limit;
  s->emit = emit;
  s->emit_ctx = ctx;
  s->out_cap = 256;
  while (s->out_cap < limit && s->out_cap < 32768) {
    s->out_cap *= 2;
  }
  s->out_mask = s->out_cap - 1;
  s->out = malloc(s->out_cap);
  if (!s->out) {
    free(s);
    return BDF_ERR_NOMEM;
  }
  bdfi_inf_blocks(s);
  in->pos = s->in_pos; /* in is the stretch the stream ended in */
  if (!s->err && (s->out_len & s->out_mask) &&
      emit(ctx, s->out, s->out_len & s->out_mask) < 0) {
    bdfi_inf_fail(s, "invalid inflated data");
  }
  free(s->out);
  bdf_status st = BDF_OK;
  if (s->err) {
    st = strcmp(s->err, "out of memory") == 0 ? BDF_ERR_NOMEM : BDF_ERR_FORMAT;
  } else {
    *out_len = s->out_len;
  }
  free(s);
  return st;
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
  bdfi_inf *s = calloc(1, sizeof *s);
  if (!s) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  s->in = data;
  s->in_len = len;
  s->limit = limit;
  s->out_mask = SIZE_MAX;
  bdfi_inf_blocks(s);
  if (s->err) {
    free(s->out);
    bdf_status st = strcmp(s->err, "out of memory") == 0 ? BDF_ERR_NOMEM : BDF_ERR_FORMAT;
    bdfi_error_set(err, st, s->in_pos, s->err);
    free(s);
    return st;
  }
  if (!s->out) {
    s->out = malloc(1); /* a non-NULL result for empty data */
    if (!s->out) {
      free(s);
      bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
      return BDF_ERR_NOMEM;
    }
  }
  *out = s->out;
  *out_len = s->out_len;
  free(s);
  return BDF_OK;
}
