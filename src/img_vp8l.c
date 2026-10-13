/*
 * VP8L, the lossless format of WebP: a port of Go's golang.org/x/image/vp8l
 * (BSD license, see third_party/go-image/LICENSE), operation for operation,
 * so that an image decodes to the bytes the Go decoder gives and the images
 * it refuses are refused. The specification is "WebP Lossless Bitstream
 * Specification"; where the Go code departs from it, this follows the Go
 * code. Two of those places decide what a damaged file decodes to:
 *
 *   - Code lengths that give one code to two symbols are not an error
 *     where all the codes have the same length: the later symbol has the
 *     code (hTree.insert).
 *   - At the end of the data a symbol is still read from the bits that are
 *     left, without the look-up table (hTree.next).
 *
 * Code lengths that leave a code without a symbol are always refused: the
 * tree would need more nodes than the 2n-1 Go gives one of n symbols. So
 * the walk of hTree.next never comes to a node that nothing was put into,
 * nor to an entry of the look-up table that no code filled. What Go would
 * do there is ported as it is.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img_webp_internal.h"

/* ---- The tables (vp8l/decode.go, vp8l/huffman.go) ------------------------- */

/* colorCacheMultiplier: the hash of the color cache (section 4.2.3). */
#define BDFI_VP8L_COLOR_CACHE_MULTIPLIER 0x1e35a7bdu

/* distanceMapTable: the short distances of a backward reference (section 4.2.2). */
static const uint8_t bdfi_vp8l_distance_map_table[120] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a, 0x38, 0x05, 0x37,
    0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b,
    0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45, 0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56,
    0x5a, 0x23, 0x2d, 0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e, 0x78, 0x01, 0x77,
    0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b,
    0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e,
    0x30, 0x73, 0x7d, 0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70};

/* codeLengthCodeOrder, repeatBits and repeatOffsets (section 5.2.2). */
static const uint8_t bdfi_vp8l_code_length_code_order[19] = {17, 18, 0, 1,  2,  3,  4,  5,  16, 6,
                                                             7,  8,  9, 10, 11, 12, 13, 14, 15};

static const uint8_t bdfi_vp8l_repeat_bits[3] = {2, 3, 7};
static const uint8_t bdfi_vp8l_repeat_offsets[3] = {3, 3, 11};

/* reverseBits: the bits of a byte, reversed. */
static const uint8_t bdfi_vp8l_reverse_bits[256] = {
    0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
    0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
    0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
    0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
    0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
    0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
    0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
    0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
    0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
    0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
    0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
    0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
    0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
    0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
    0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
    0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff};

/* The five trees of a group, and the sizes of their alphabets. */
#define BDFI_VP8L_HUFF_GREEN    0
#define BDFI_VP8L_HUFF_RED      1
#define BDFI_VP8L_HUFF_BLUE     2
#define BDFI_VP8L_HUFF_ALPHA    3
#define BDFI_VP8L_HUFF_DISTANCE 4
#define BDFI_VP8L_N_HUFF        5

#define BDFI_VP8L_N_LITERAL_CODES  256
#define BDFI_VP8L_N_LENGTH_CODES   24
#define BDFI_VP8L_N_DISTANCE_CODES 40

static const uint32_t bdfi_vp8l_alphabet_sizes[BDFI_VP8L_N_HUFF] = {
    BDFI_VP8L_N_LITERAL_CODES + BDFI_VP8L_N_LENGTH_CODES, BDFI_VP8L_N_LITERAL_CODES,
    BDFI_VP8L_N_LITERAL_CODES, BDFI_VP8L_N_LITERAL_CODES, BDFI_VP8L_N_DISTANCE_CODES};

/* The largest alphabet: the green one with a color cache of 11 bits. */
#define BDFI_VP8L_MAX_ALPHABET (256 + 24 + (1 << 11))

/* A code length is at most 15 (codeLengthsToCodes refuses more), and a code
 * of code lengths has 19 symbols. */
#define BDFI_VP8L_MAX_CODE_LENGTH     15
#define BDFI_VP8L_N_CODE_LENGTH_CODES 19

/* ---- The bit stream (vp8l/decode.go) -------------------------------------- */

/* decoder: the bytes, read from the low bit up. scratch is not Go's: the
 * code lengths of the tree being read and their codes, BDFI_VP8L_MAX_ALPHABET
 * of each, where Go makes two slices a tree. */
typedef struct bdfi_vp8l_dec {
  const uint8_t *data;
  size_t len, pos;
  uint32_t bits, nbits;
  uint32_t *scratch;
} bdfi_vp8l_dec;

/* decoder.read: the next n bits (n is 18 at most, and nbits less than n
 * where a byte is taken, so no bit is shifted out). 0 at the end of the
 * data, where Go returns io.ErrUnexpectedEOF. */
static int bdfi_vp8l_read(bdfi_vp8l_dec *d, uint32_t n, uint32_t *u) {
  while (d->nbits < n) {
    if (d->pos >= d->len) {
      return 0;
    }
    d->bits |= (uint32_t)d->data[d->pos++] << d->nbits;
    d->nbits += 8;
  }
  *u = d->bits & (((uint32_t)1 << n) - 1);
  d->bits >>= n;
  d->nbits -= n;
  return 1;
}

/* ---- Huffman trees (vp8l/huffman.go) -------------------------------------- */

/* hNode: children is the index of the first of the node's two children, 0
 * for a node that nothing was put into yet and -1 for a leaf. */
typedef struct bdfi_vp8l_node {
  uint32_t symbol;
  int32_t children;
} bdfi_vp8l_node;

#define BDFI_VP8L_LEAF_NODE (-1)

/* lutSize: the look-up table of a tree is indexed by the next 7 bits. */
#define BDFI_VP8L_LUT_SIZE 7
#define BDFI_VP8L_LUT_MASK ((1u << 7) - 1)

/* hTree. The low 8 bits of lut[x] are 1 plus the length of the code that
 * the bits x begin with, or 0 if it is longer than 7 bits; the other 24 are
 * the symbol, or the node to go on from. */
typedef struct bdfi_vp8l_tree {
  bdfi_vp8l_node *nodes;
  uint32_t len, cap; /* len(nodes) and cap(nodes) of Go: len grows by 2 up to cap */
  uint32_t lut[1 << BDFI_VP8L_LUT_SIZE];
} bdfi_vp8l_tree;

/* hGroup */
typedef struct bdfi_vp8l_group {
  bdfi_vp8l_tree t[BDFI_VP8L_N_HUFF];
} bdfi_vp8l_group;

/* hTree.insert: puts in a symbol whose code is the low code_length bits of
 * code. 0 where Go returns errInvalidHuffmanTree. */
static int bdfi_vp8l_insert(bdfi_vp8l_tree *h, uint32_t symbol, uint32_t code,
                            uint32_t code_length) {
  /* Go refuses lengths over 0xfe here, but none over 15 gets here: the
   * shifts below need that. */
  if (symbol > 0xffff || code_length > BDFI_VP8L_MAX_CODE_LENGTH) {
    return 0;
  }
  uint32_t base_code;
  if (code_length > BDFI_VP8L_LUT_SIZE) {
    base_code =
        (uint32_t)bdfi_vp8l_reverse_bits[(code >> (code_length - BDFI_VP8L_LUT_SIZE)) & 0xff] >>
        (8 - BDFI_VP8L_LUT_SIZE);
  } else {
    base_code = (uint32_t)bdfi_vp8l_reverse_bits[code & 0xff] >> (8 - code_length);
    for (uint32_t i = 0; i < ((uint32_t)1 << (BDFI_VP8L_LUT_SIZE - code_length)); i++) {
      h->lut[base_code | i << code_length] = symbol << 8 | (code_length + 1);
    }
  }

  uint32_t n = 0;
  int jump = BDFI_VP8L_LUT_SIZE;
  while (code_length > 0) {
    code_length--;
    /* n is a node that exists: 0, or a child made below */
    switch (h->nodes[n].children) {
    case BDFI_VP8L_LEAF_NODE:
      return 0;
    case 0:
      if (h->len == h->cap) {
        return 0;
      }
      /* Create two empty child nodes. len and cap are odd, so both fit. */
      h->nodes[n].children = (int32_t)h->len;
      h->len += 2;
      break;
    default:
      break;
    }
    n = (uint32_t)h->nodes[n].children + (1 & (code >> code_length));
    jump--;
    if (jump == 0 && h->lut[base_code] == 0) {
      h->lut[base_code] = n << 8;
    }
  }

  switch (h->nodes[n].children) {
  case BDFI_VP8L_LEAF_NODE:
    /* No-op: the code was given before, and the symbol is overwritten. */
    break;
  case 0:
    /* Turn the uninitialized node into a leaf. */
    h->nodes[n].children = BDFI_VP8L_LEAF_NODE;
    break;
  default:
    return 0;
  }
  h->nodes[n].symbol = symbol;
  return 1;
}

/* codeLengthsToCodes: the canonical codes of a sequence of code lengths,
 * into codes. 0 where a length is over 15. (The count of the symbols of
 * length 0 goes into the first code as in Go; it only reaches bits above the
 * code's length, which nothing uses.) */
static int bdfi_vp8l_code_lengths_to_codes(const uint32_t *code_lengths, uint32_t count,
                                           uint32_t *codes) {
  uint32_t max_code_length = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (max_code_length < code_lengths[i]) {
      max_code_length = code_lengths[i];
    }
  }
  if (count == 0 || max_code_length > BDFI_VP8L_MAX_CODE_LENGTH) {
    return 0;
  }
  uint32_t histogram[BDFI_VP8L_MAX_CODE_LENGTH + 1];
  uint32_t next_codes[BDFI_VP8L_MAX_CODE_LENGTH + 1];
  memset(histogram, 0, sizeof histogram);
  for (uint32_t i = 0; i < count; i++) {
    histogram[code_lengths[i]]++;
  }
  uint32_t curr_code = 0;
  next_codes[0] = 0;
  for (int cl = 1; cl <= BDFI_VP8L_MAX_CODE_LENGTH; cl++) {
    curr_code = (curr_code + histogram[cl - 1]) << 1;
    next_codes[cl] = curr_code;
  }
  for (uint32_t symbol = 0; symbol < count; symbol++) {
    uint32_t cl = code_lengths[symbol];
    codes[symbol] = 0;
    if (cl > 0) {
      codes[symbol] = next_codes[cl];
      next_codes[cl]++;
    }
  }
  return 1;
}

/* The nodes of a tree of n_symbols symbols: Go makes a slice of 1 node with
 * room for 2*n_symbols-1. With storage (zeroed, large enough) nothing is
 * allocated. */
static bdf_status bdfi_vp8l_make_nodes(bdfi_vp8l_tree *h, uint32_t n_symbols,
                                       bdfi_vp8l_node *storage) {
  h->cap = 2 * n_symbols - 1;
  h->len = 1;
  h->nodes = storage;
  if (!storage) {
    h->nodes = bdfi_calloc_array(h->cap, sizeof *h->nodes);
    if (!h->nodes) {
      return BDF_ERR_NOMEM;
    }
  }
  return BDF_OK;
}

/* hTree.build: a tree from the code lengths of count symbols. h is zeroed;
 * codes is room for count codes; storage as for bdfi_vp8l_make_nodes. */
static bdf_status bdfi_vp8l_build(bdfi_vp8l_tree *h, const uint32_t *code_lengths, uint32_t count,
                                  uint32_t *codes, bdfi_vp8l_node *storage) {
  /* Calculate the number of symbols. */
  uint32_t n_symbols = 0, last_symbol = 0;
  for (uint32_t symbol = 0; symbol < count; symbol++) {
    if (code_lengths[symbol] != 0) {
      n_symbols++;
      last_symbol = symbol;
    }
  }
  if (n_symbols == 0) {
    return BDF_ERR_FORMAT;
  }
  bdf_status st = bdfi_vp8l_make_nodes(h, n_symbols, storage);
  if (st != BDF_OK) {
    return st;
  }
  /* Handle the trivial case. */
  if (n_symbols == 1) {
    return bdfi_vp8l_insert(h, last_symbol, 0, 0) ? BDF_OK : BDF_ERR_FORMAT;
  }
  /* Handle the non-trivial case. */
  if (!bdfi_vp8l_code_lengths_to_codes(code_lengths, count, codes)) {
    return BDF_ERR_FORMAT;
  }
  for (uint32_t symbol = 0; symbol < count; symbol++) {
    uint32_t cl = code_lengths[symbol];
    if (cl > 0 && !bdfi_vp8l_insert(h, symbol, codes[symbol], cl)) {
      return BDF_ERR_FORMAT;
    }
  }
  return BDF_OK;
}

/* hTree.buildSimple: a tree of 1 or 2 symbols. h is zeroed. */
static bdf_status bdfi_vp8l_build_simple(bdfi_vp8l_tree *h, uint32_t n_symbols,
                                         const uint32_t symbols[2], uint32_t alphabet_size) {
  bdf_status st = bdfi_vp8l_make_nodes(h, n_symbols, NULL);
  if (st != BDF_OK) {
    return st;
  }
  for (uint32_t i = 0; i < n_symbols; i++) {
    if (symbols[i] >= alphabet_size) {
      return BDF_ERR_FORMAT;
    }
    if (!bdfi_vp8l_insert(h, symbols[i], i, n_symbols - 1)) {
      return BDF_ERR_FORMAT;
    }
  }
  return BDF_OK;
}

/* hTree.next: the next symbol of the bit stream. 0 at the end of the data,
 * where Go returns io.ErrUnexpectedEOF.
 *
 * A node index is in the tree wherever one is used: the table holds indices
 * of nodes that exist, and the children of a node exist. (A tree that build
 * accepts has no node that was never filled and no table entry of 0. If it
 * had, the walk would go on at node 0 or 1 as in Go, which exist in a tree
 * whose root is not a leaf.) */
static int bdfi_vp8l_next(const bdfi_vp8l_tree *h, bdfi_vp8l_dec *d, uint32_t *symbol) {
  uint32_t n = 0;
  int use_table = 1;
  /* Read enough bits so that we can use the look-up table. */
  if (d->nbits < BDFI_VP8L_LUT_SIZE) {
    if (d->pos < d->len) {
      d->bits |= (uint32_t)d->data[d->pos++] << d->nbits;
      d->nbits += 8;
    } else {
      /* There are no more bytes of data, but we may still be able to read
       * the next symbol out of the previously read bits. */
      use_table = 0;
    }
  }
  if (use_table) {
    /* Use the look-up table. */
    n = h->lut[d->bits & BDFI_VP8L_LUT_MASK];
    uint32_t b = n & 0xff;
    if (b != 0) {
      b--;
      d->bits >>= b;
      d->nbits -= b;
      *symbol = n >> 8;
      return 1;
    }
    /* The code is longer than 7 bits: the walk goes on from the node the
     * table names. */
    n >>= 8;
    d->bits >>= BDFI_VP8L_LUT_SIZE;
    d->nbits -= BDFI_VP8L_LUT_SIZE;
  }
  while (h->nodes[n].children != BDFI_VP8L_LEAF_NODE) {
    if (d->nbits == 0) {
      if (d->pos >= d->len) {
        return 0;
      }
      d->bits = d->data[d->pos++];
      d->nbits = 8;
    }
    n = (uint32_t)h->nodes[n].children + (1 & d->bits);
    d->bits >>= 1;
    d->nbits--;
  }
  *symbol = h->nodes[n].symbol;
  return 1;
}

/* ---- Code lengths and trees from the stream (vp8l/decode.go) -------------- */

/* decoder.decodeCodeLengths (section 5.2.2): the code lengths of a tree,
 * which are themselves coded with a tree. dst is count lengths, zeroed. */
static bdf_status bdfi_vp8l_decode_code_lengths(
    bdfi_vp8l_dec *d, uint32_t *dst, uint32_t count,
    const uint32_t code_length_code_lengths[BDFI_VP8L_N_CODE_LENGTH_CODES]) {
  bdfi_vp8l_tree h;
  bdfi_vp8l_node nodes[2 * BDFI_VP8L_N_CODE_LENGTH_CODES - 1];
  uint32_t codes[BDFI_VP8L_N_CODE_LENGTH_CODES];
  memset(&h, 0, sizeof h);
  memset(nodes, 0, sizeof nodes);
  bdf_status st =
      bdfi_vp8l_build(&h, code_length_code_lengths, BDFI_VP8L_N_CODE_LENGTH_CODES, codes, nodes);
  if (st != BDF_OK) {
    return st;
  }

  uint32_t max_symbol = count;
  uint32_t use_length;
  if (!bdfi_vp8l_read(d, 1, &use_length)) {
    return BDF_ERR_FORMAT;
  }
  if (use_length != 0) {
    uint32_t n, ms;
    if (!bdfi_vp8l_read(d, 3, &n)) {
      return BDF_ERR_FORMAT;
    }
    n = 2 + 2 * n;
    if (!bdfi_vp8l_read(d, n, &ms)) {
      return BDF_ERR_FORMAT;
    }
    max_symbol = ms + 2;
    if (max_symbol > count) {
      return BDF_ERR_FORMAT;
    }
  }

  /* The spec says that "if code 16 [meaning repeat] is used before a
   * non-zero value has been emitted, a value of 8 is repeated." */
  uint32_t prev_code_length = 8;

  for (uint32_t symbol = 0; symbol < count;) {
    if (max_symbol == 0) {
      break;
    }
    max_symbol--;
    uint32_t code_length;
    if (!bdfi_vp8l_next(&h, d, &code_length)) {
      return BDF_ERR_FORMAT;
    }
    if (code_length < 16) {
      dst[symbol] = code_length;
      symbol++;
      if (code_length != 0) {
        prev_code_length = code_length;
      }
      continue;
    }

    /* code_length is 16, 17 or 18: the code has 19 symbols */
    uint32_t repeat;
    if (!bdfi_vp8l_read(d, bdfi_vp8l_repeat_bits[code_length - 16], &repeat)) {
      return BDF_ERR_FORMAT;
    }
    repeat += bdfi_vp8l_repeat_offsets[code_length - 16];
    if (symbol + repeat > count) {
      return BDF_ERR_FORMAT;
    }
    /* A code length of 16 repeats the previous non-zero code.
     * A code length of 17 or 18 repeats zeroes. */
    uint32_t cl = 0;
    if (code_length == 16) {
      cl = prev_code_length;
    }
    for (; repeat > 0; repeat--) {
      dst[symbol] = cl;
      symbol++;
    }
  }
  return BDF_OK;
}

/* decoder.decodeHuffmanTree: reads a tree into h (zeroed), or reads and
 * drops it when h is NULL. A tree that is dropped is not built, so only what
 * reading it finds wrong is an error. */
static bdf_status bdfi_vp8l_decode_huffman_tree(bdfi_vp8l_dec *d, bdfi_vp8l_tree *h,
                                                uint32_t alphabet_size) {
  uint32_t use_simple;
  if (!bdfi_vp8l_read(d, 1, &use_simple)) {
    return BDF_ERR_FORMAT;
  }
  if (use_simple != 0) {
    uint32_t n_symbols, first_symbol_length_code;
    uint32_t symbols[2] = {0, 0};
    if (!bdfi_vp8l_read(d, 1, &n_symbols)) {
      return BDF_ERR_FORMAT;
    }
    n_symbols++;
    if (!bdfi_vp8l_read(d, 1, &first_symbol_length_code)) {
      return BDF_ERR_FORMAT;
    }
    first_symbol_length_code = 7 * first_symbol_length_code + 1;
    if (!bdfi_vp8l_read(d, first_symbol_length_code, &symbols[0])) {
      return BDF_ERR_FORMAT;
    }
    if (n_symbols == 2 && !bdfi_vp8l_read(d, 8, &symbols[1])) {
      return BDF_ERR_FORMAT;
    }
    if (!h) {
      return BDF_OK;
    }
    return bdfi_vp8l_build_simple(h, n_symbols, symbols, alphabet_size);
  }

  uint32_t n_codes;
  if (!bdfi_vp8l_read(d, 4, &n_codes)) {
    return BDF_ERR_FORMAT;
  }
  n_codes += 4; /* 19 at most: as many as there are */
  uint32_t code_length_code_lengths[BDFI_VP8L_N_CODE_LENGTH_CODES];
  memset(code_length_code_lengths, 0, sizeof code_length_code_lengths);
  for (uint32_t i = 0; i < n_codes; i++) {
    if (!bdfi_vp8l_read(d, 3, &code_length_code_lengths[bdfi_vp8l_code_length_code_order[i]])) {
      return BDF_ERR_FORMAT;
    }
  }
  uint32_t *code_lengths = d->scratch;
  uint32_t *codes = d->scratch + BDFI_VP8L_MAX_ALPHABET;
  memset(code_lengths, 0, alphabet_size * sizeof *code_lengths);
  bdf_status st =
      bdfi_vp8l_decode_code_lengths(d, code_lengths, alphabet_size, code_length_code_lengths);
  if (st != BDF_OK) {
    return st;
  }
  if (!h) {
    return BDF_OK;
  }
  return bdfi_vp8l_build(h, code_lengths, alphabet_size, codes, NULL);
}

static void bdfi_vp8l_free_groups(bdfi_vp8l_group *groups, size_t count) {
  if (!groups) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    for (int j = 0; j < BDFI_VP8L_N_HUFF; j++) {
      free(groups[i].t[j].nodes);
    }
  }
  free(groups);
}

/* nTiles in vp8l/transform.go: how many tiles of 1<<bits pixels cover size
 * pixels. */
static int32_t bdfi_vp8l_n_tiles(int32_t size, uint32_t bits) {
  return (size + ((int32_t)1 << bits) - 1) >> bits;
}

static bdf_status bdfi_vp8l_decode_pix(bdfi_vp8l_dec *d, int32_t w, int32_t h, size_t min_cap,
                                       int top_level, uint8_t **out);

/* The most groups of trees an image may name: maxHuffImageSize of Go. */
#define BDFI_VP8L_MAX_HUFF_IMAGE_SIZE 2600

/* What decodeHuffmanGroups returns. */
typedef struct bdfi_vp8l_groups {
  bdfi_vp8l_group *groups;
  size_t count;
  uint8_t *pix;  /* hPix: which group a tile uses, or NULL when there is one for the image */
  uint32_t bits; /* hBits: a tile is 1<<bits pixels wide and high; 0 without tiles */
} bdfi_vp8l_groups;

/*
 * decoder.decodeHuffmanGroups: the groups of trees the pixels are coded
 * with. Only the image itself (top_level) can have more than one: then a
 * smaller image says which group each tile uses. Go keeps only the groups
 * that are used when the largest index is 1000 or more or not below the
 * number of tiles, and rewrites the indices; the trees of the others are
 * read and dropped.
 */
static bdf_status bdfi_vp8l_decode_huffman_groups(bdfi_vp8l_dec *d, int32_t w, int32_t h,
                                                  int top_level, uint32_t cc_bits,
                                                  bdfi_vp8l_groups *out) {
  int16_t *mapping = NULL;
  int32_t max_group_index = 0;
  size_t num_groups = 1;
  bdf_status st = BDF_OK;

  if (top_level) {
    uint32_t use_meta;
    if (!bdfi_vp8l_read(d, 1, &use_meta)) {
      return BDF_ERR_FORMAT;
    }
    if (use_meta != 0) {
      if (!bdfi_vp8l_read(d, 3, &out->bits)) {
        return BDF_ERR_FORMAT;
      }
      out->bits += 2;
      int32_t tile_w = bdfi_vp8l_n_tiles(w, out->bits);
      int32_t tile_h = bdfi_vp8l_n_tiles(h, out->bits);
      int32_t num_tiles = tile_h * tile_w;
      st = bdfi_vp8l_decode_pix(d, tile_w, tile_h, 0, 0, &out->pix);
      if (st != BDF_OK) {
        return st;
      }
      size_t pix_len = 4 * (size_t)num_tiles;
      for (size_t p = 0; p < pix_len; p += 4) {
        int32_t i = (int32_t)out->pix[p] << 8 | (int32_t)out->pix[p + 1];
        if (max_group_index < i) {
          max_group_index = i;
        }
      }
      if (max_group_index >= BDFI_VP8L_MAX_HUFF_IMAGE_SIZE) {
        return BDF_ERR_FORMAT;
      }
      if (max_group_index >= 1000 || max_group_index >= num_tiles) {
        mapping = bdfi_malloc_array((size_t)max_group_index + 1, sizeof *mapping);
        if (!mapping) {
          return BDF_ERR_NOMEM;
        }
        for (int32_t i = 0; i <= max_group_index; i++) {
          mapping[i] = -1;
        }
        int16_t n = 0;
        for (size_t p = 0; p < pix_len; p += 4) {
          int32_t i = (int32_t)out->pix[p] << 8 | (int32_t)out->pix[p + 1];
          if (mapping[i] == -1) {
            mapping[i] = n;
            n++;
          }
          out->pix[p] = (uint8_t)(mapping[i] >> 8);
          out->pix[p + 1] = (uint8_t)(mapping[i] & 0xff);
        }
        num_groups = (size_t)n;
      } else {
        num_groups = (size_t)max_group_index + 1;
      }
    }
  }

  out->groups = bdfi_calloc_array(num_groups, sizeof *out->groups);
  if (!out->groups) {
    free(mapping);
    return BDF_ERR_NOMEM;
  }
  out->count = num_groups;
  for (int32_t i = 0; i <= max_group_index && st == BDF_OK; i++) {
    bdfi_vp8l_group *hg = NULL;
    if (!mapping) {
      hg = &out->groups[i];
    } else if (mapping[i] != -1) {
      hg = &out->groups[mapping[i]];
    }
    for (int j = 0; j < BDFI_VP8L_N_HUFF && st == BDF_OK; j++) {
      uint32_t alphabet_size = bdfi_vp8l_alphabet_sizes[j];
      if (j == 0 && cc_bits > 0) {
        alphabet_size += (uint32_t)1 << cc_bits;
      }
      st = bdfi_vp8l_decode_huffman_tree(d, hg ? &hg->t[j] : NULL, alphabet_size);
    }
  }
  free(mapping);
  return st;
}

/* distanceMap: the distance in pixels that a distance code stands for
 * (section 4.2.2). code is 1 or more. */
static int32_t bdfi_vp8l_distance_map(int32_t w, uint32_t code) {
  if (code > 120) {
    return (int32_t)code - 120;
  }
  int32_t dist_code = bdfi_vp8l_distance_map_table[code - 1];
  int32_t y_offset = dist_code >> 4;
  int32_t x_offset = 8 - (dist_code & 0xf);
  int32_t dist = y_offset * w + x_offset;
  if (dist >= 1) {
    return dist;
  }
  return 1;
}

/* decoder.lz77Param: a length or a distance from its symbol (section 4.2.2).
 * The symbol is below 40, so at most 18 more bits are read. */
static int bdfi_vp8l_lz77_param(bdfi_vp8l_dec *d, uint32_t symbol, uint32_t *out) {
  if (symbol < 4) {
    *out = symbol + 1;
    return 1;
  }
  uint32_t extra_bits = (symbol - 2) >> 1;
  uint32_t offset = (2 + (symbol & 1)) << extra_bits;
  uint32_t n;
  if (!bdfi_vp8l_read(d, extra_bits, &n)) {
    return 0;
  }
  *out = offset + n + 1;
  return 1;
}

/* The pixels of decodePix, once the color cache and the trees are read. pix
 * is 4*w*h bytes, R, G, B, A. */
static bdf_status bdfi_vp8l_decode_pixels(bdfi_vp8l_dec *d, int32_t w, int32_t h, uint8_t *pix,
                                          const bdfi_vp8l_groups *hg, uint32_t *cc_entries,
                                          uint32_t cc_bits) {
  uint32_t cc_shift = cc_bits != 0 ? 32 - cc_bits : 0;
  size_t cc_len = cc_bits != 0 ? (size_t)1 << cc_bits : 0;
  uint32_t h_bits = hg->bits;
  int32_t h_mask = 0, tiles_per_row = 0;
  if (h_bits != 0) {
    h_mask = ((int32_t)1 << h_bits) - 1;
    tiles_per_row = bdfi_vp8l_n_tiles(w, h_bits);
  }

  size_t len = 4 * (size_t)w * (size_t)h;
  size_t p = 0, cached_p = 0;
  int32_t x = 0, y = 0;
  const bdfi_vp8l_group *g = &hg->groups[0];
  int lookup = h_mask != 0;
  while (p < len) {
    if (lookup) {
      size_t i = 4 * ((size_t)tiles_per_row * (size_t)(y >> h_bits) + (size_t)(x >> h_bits));
      g = &hg->groups[(uint32_t)hg->pix[i] << 8 | (uint32_t)hg->pix[i + 1]];
    }

    uint32_t green;
    if (!bdfi_vp8l_next(&g->t[BDFI_VP8L_HUFF_GREEN], d, &green)) {
      return BDF_ERR_FORMAT;
    }
    if (green < BDFI_VP8L_N_LITERAL_CODES) {
      /* We have a literal pixel. */
      uint32_t red, blue, alpha;
      if (!bdfi_vp8l_next(&g->t[BDFI_VP8L_HUFF_RED], d, &red) ||
          !bdfi_vp8l_next(&g->t[BDFI_VP8L_HUFF_BLUE], d, &blue) ||
          !bdfi_vp8l_next(&g->t[BDFI_VP8L_HUFF_ALPHA], d, &alpha)) {
        return BDF_ERR_FORMAT;
      }
      pix[p + 0] = (uint8_t)red;
      pix[p + 1] = (uint8_t)green;
      pix[p + 2] = (uint8_t)blue;
      pix[p + 3] = (uint8_t)alpha;
      p += 4;

      x++;
      if (x == w) {
        x = 0;
        y++;
      }
      lookup = h_mask != 0 && (x & h_mask) == 0;

    } else if (green < BDFI_VP8L_N_LITERAL_CODES + BDFI_VP8L_N_LENGTH_CODES) {
      /* We have a LZ77 backwards reference. */
      uint32_t length, dist_sym, dist_code;
      if (!bdfi_vp8l_lz77_param(d, green - BDFI_VP8L_N_LITERAL_CODES, &length) ||
          !bdfi_vp8l_next(&g->t[BDFI_VP8L_HUFF_DISTANCE], d, &dist_sym) ||
          !bdfi_vp8l_lz77_param(d, dist_sym, &dist_code)) {
        return BDF_ERR_FORMAT;
      }
      /* the length is 4096 pixels at most, the distance is 1 or more */
      size_t dist = 4 * (size_t)bdfi_vp8l_distance_map(w, dist_code);
      size_t n = 4 * (size_t)length;
      if (len - p < n || p < dist) {
        return BDF_ERR_FORMAT;
      }
      /* Go copies byte by byte, forwards: a reference that reaches into
       * what it writes repeats it. */
      if (dist >= n) {
        memcpy(pix + p, pix + p - dist, n);
        p += n;
      } else {
        for (size_t end = p + n; p < end; p++) {
          pix[p] = pix[p - dist];
        }
      }

      x += (int32_t)length;
      while (x >= w) {
        x -= w;
        y++;
      }
      lookup = h_mask != 0;

    } else {
      /* We have a color cache lookup. A symbol this large is only in the
       * alphabet when there is a cache, and then it is below the cache's
       * size; Go would index a nil slice or find the index too large. */
      if (cc_len == 0) {
        return BDF_ERR_FORMAT;
      }
      /* First, insert previous pixels into the cache. Note that VP8L
       * assumes ARGB order, but the pixels here are in RGBA order. */
      for (; cached_p < p; cached_p += 4) {
        uint32_t argb = (uint32_t)pix[cached_p + 0] << 16 | (uint32_t)pix[cached_p + 1] << 8 |
                        (uint32_t)pix[cached_p + 2] << 0 | (uint32_t)pix[cached_p + 3] << 24;
        cc_entries[(argb * BDFI_VP8L_COLOR_CACHE_MULTIPLIER) >> cc_shift] = argb;
      }
      green -= BDFI_VP8L_N_LITERAL_CODES + BDFI_VP8L_N_LENGTH_CODES;
      if (green >= cc_len) {
        return BDF_ERR_FORMAT;
      }
      uint32_t argb = cc_entries[green];
      pix[p + 0] = (uint8_t)(argb >> 16);
      pix[p + 1] = (uint8_t)(argb >> 8);
      pix[p + 2] = (uint8_t)(argb >> 0);
      pix[p + 3] = (uint8_t)(argb >> 24);
      p += 4;

      x++;
      if (x == w) {
        x = 0;
        y++;
      }
      lookup = h_mask != 0 && (x & h_mask) == 0;
    }
  }
  return BDF_OK;
}

/*
 * decoder.decodePix (section 5.2.2): an image of w x h pixels, which is the
 * image itself (top_level) or what a transform or the choice of the trees
 * is stored as. *out is 4*w*h bytes, or min_cap if that is more, zeroed
 * beyond what is decoded. Calls itself once, not top_level, for the image
 * that chooses the trees.
 */
static bdf_status bdfi_vp8l_decode_pix(bdfi_vp8l_dec *d, int32_t w, int32_t h, size_t min_cap,
                                       int top_level, uint8_t **out) {
  *out = NULL;
  /* Decode the color cache parameters. */
  uint32_t cc_bits = 0, use_color_cache;
  if (!bdfi_vp8l_read(d, 1, &use_color_cache)) {
    return BDF_ERR_FORMAT;
  }
  if (use_color_cache != 0) {
    if (!bdfi_vp8l_read(d, 4, &cc_bits)) {
      return BDF_ERR_FORMAT;
    }
    if (cc_bits < 1 || 11 < cc_bits) {
      return BDF_ERR_FORMAT;
    }
  }

  uint32_t *cc_entries = NULL;
  uint8_t *pix = NULL;
  bdfi_vp8l_groups hg;
  memset(&hg, 0, sizeof hg);
  /* Decode the Huffman groups. */
  bdf_status st = bdfi_vp8l_decode_huffman_groups(d, w, h, top_level, cc_bits, &hg);
  if (st == BDF_OK) {
    /* Decode the pixels. */
    size_t len = 4 * (size_t)w * (size_t)h;
    pix = bdfi_calloc_array(len > min_cap ? len : min_cap, 1);
    if (cc_bits != 0) {
      cc_entries = bdfi_calloc_array((size_t)1 << cc_bits, sizeof *cc_entries);
    }
    if (!pix || (cc_bits != 0 && !cc_entries)) {
      st = BDF_ERR_NOMEM;
    } else {
      st = bdfi_vp8l_decode_pixels(d, w, h, pix, &hg, cc_entries, cc_bits);
    }
  }
  free(cc_entries);
  free(hg.pix);
  bdfi_vp8l_free_groups(hg.groups, hg.count);
  if (st != BDF_OK) {
    free(pix);
    return st;
  }
  *out = pix;
  return BDF_OK;
}

/* ---- Transforms (vp8l/transform.go, section 3) ---------------------------- */

#define BDFI_VP8L_TRANSFORM_PREDICTOR      0
#define BDFI_VP8L_TRANSFORM_CROSS_COLOR    1
#define BDFI_VP8L_TRANSFORM_SUBTRACT_GREEN 2
#define BDFI_VP8L_TRANSFORM_COLOR_INDEXING 3
#define BDFI_VP8L_N_TRANSFORM_TYPES        4

/* transform: old_width is the width of the image before the transform (the
 * color indexing one can make it narrower); bits is the size of the tiles of
 * the predictor and cross-color transforms, and 8>>bits the bits of a color
 * index; pix is the tiles, or the palette (256 colors, zeroed past those
 * the file has). */
typedef struct bdfi_vp8l_transform {
  uint32_t type;
  int32_t old_width;
  uint32_t bits;
  uint8_t *pix;
} bdfi_vp8l_transform;

/* decoder.decodeTransform: the next transform, and in *w the width of the
 * image under it. */
static bdf_status bdfi_vp8l_decode_transform(bdfi_vp8l_dec *d, int32_t *w, int32_t h,
                                             bdfi_vp8l_transform *t) {
  memset(t, 0, sizeof *t);
  t->old_width = *w;
  if (!bdfi_vp8l_read(d, 2, &t->type)) {
    return BDF_ERR_FORMAT;
  }
  switch (t->type) {
  case BDFI_VP8L_TRANSFORM_PREDICTOR:
  case BDFI_VP8L_TRANSFORM_CROSS_COLOR:
    if (!bdfi_vp8l_read(d, 3, &t->bits)) {
      return BDF_ERR_FORMAT;
    }
    t->bits += 2;
    return bdfi_vp8l_decode_pix(d, bdfi_vp8l_n_tiles(*w, t->bits), bdfi_vp8l_n_tiles(h, t->bits), 0,
                                0, &t->pix);
  case BDFI_VP8L_TRANSFORM_COLOR_INDEXING: {
    uint32_t n_colors;
    if (!bdfi_vp8l_read(d, 8, &n_colors)) {
      return BDF_ERR_FORMAT;
    }
    n_colors++;
    t->bits = 0;
    if (n_colors <= 2) {
      t->bits = 3;
    } else if (n_colors <= 4) {
      t->bits = 2;
    } else if (n_colors <= 16) {
      t->bits = 1;
    }
    *w = bdfi_vp8l_n_tiles(*w, t->bits);
    bdf_status st = bdfi_vp8l_decode_pix(d, (int32_t)n_colors, 1, 4 * 256, 0, &t->pix);
    if (st != BDF_OK) {
      return st;
    }
    for (size_t p = 4; p < 4 * (size_t)n_colors; p += 4) {
      t->pix[p + 0] = (uint8_t)(t->pix[p + 0] + t->pix[p - 4]);
      t->pix[p + 1] = (uint8_t)(t->pix[p + 1] + t->pix[p - 3]);
      t->pix[p + 2] = (uint8_t)(t->pix[p + 2] + t->pix[p - 2]);
      t->pix[p + 3] = (uint8_t)(t->pix[p + 3] + t->pix[p - 1]);
    }
    return BDF_OK;
  }
  default: /* BDFI_VP8L_TRANSFORM_SUBTRACT_GREEN: nothing to read */
    return BDF_OK;
  }
}

static int32_t bdfi_vp8l_abs(int32_t x) {
  return x < 0 ? -x : x;
}

static uint8_t bdfi_vp8l_avg2(uint8_t a, uint8_t b) {
  return (uint8_t)(((int32_t)a + b) / 2);
}

static uint8_t bdfi_vp8l_clamp_add_subtract_full(uint8_t a, uint8_t b, uint8_t c) {
  int32_t x = (int32_t)a + (int32_t)b - (int32_t)c;
  if (x < 0) {
    return 0;
  }
  if (x > 255) {
    return 255;
  }
  return (uint8_t)x;
}

static uint8_t bdfi_vp8l_clamp_add_subtract_half(uint8_t a, uint8_t b) {
  int32_t x = (int32_t)a + ((int32_t)a - (int32_t)b) / 2;
  if (x < 0) {
    return 0;
  }
  if (x > 255) {
    return 255;
  }
  return (uint8_t)x;
}

/* pix[i] += v, as Go's uint8 does it. */
static void bdfi_vp8l_add(uint8_t *pix, size_t i, uint32_t v) {
  pix[i] = (uint8_t)(pix[i] + v);
}

/* inversePredictor: adds to each pixel what its mode predicts from the
 * pixels to its left and above. A mode of 14 or 15 predicts nothing. */
static void bdfi_vp8l_inverse_predictor(const bdfi_vp8l_transform *t, uint8_t *pix, int32_t h) {
  /* The first pixel's predictor is mode 0 (opaque black). */
  bdfi_vp8l_add(pix, 3, 0xff);
  size_t p = 4;
  int32_t mask = ((int32_t)1 << t->bits) - 1;
  for (int32_t x = 1; x < t->old_width; x++) {
    /* The rest of the first row's predictor is mode 1 (L). */
    bdfi_vp8l_add(pix, p + 0, pix[p - 4]);
    bdfi_vp8l_add(pix, p + 1, pix[p - 3]);
    bdfi_vp8l_add(pix, p + 2, pix[p - 2]);
    bdfi_vp8l_add(pix, p + 3, pix[p - 1]);
    p += 4;
  }
  size_t top = 0;
  size_t tiles_per_row = (size_t)bdfi_vp8l_n_tiles(t->old_width, t->bits);
  for (int32_t y = 1; y < h; y++) {
    /* The first column's predictor is mode 2 (T). */
    bdfi_vp8l_add(pix, p + 0, pix[top + 0]);
    bdfi_vp8l_add(pix, p + 1, pix[top + 1]);
    bdfi_vp8l_add(pix, p + 2, pix[top + 2]);
    bdfi_vp8l_add(pix, p + 3, pix[top + 3]);
    p += 4;
    top += 4;

    size_t q = 4 * (size_t)(y >> t->bits) * tiles_per_row;
    uint8_t predictor_mode = t->pix[q + 1] & 0x0f;
    q += 4;
    for (int32_t x = 1; x < t->old_width; x++) {
      if ((x & mask) == 0) {
        predictor_mode = t->pix[q + 1] & 0x0f;
        q += 4;
      }
      switch (predictor_mode) {
      case 0: /* Opaque black. */
        bdfi_vp8l_add(pix, p + 3, 0xff);
        break;

      case 1: /* L. */
        bdfi_vp8l_add(pix, p + 0, pix[p - 4]);
        bdfi_vp8l_add(pix, p + 1, pix[p - 3]);
        bdfi_vp8l_add(pix, p + 2, pix[p - 2]);
        bdfi_vp8l_add(pix, p + 3, pix[p - 1]);
        break;

      case 2: /* T. */
        bdfi_vp8l_add(pix, p + 0, pix[top + 0]);
        bdfi_vp8l_add(pix, p + 1, pix[top + 1]);
        bdfi_vp8l_add(pix, p + 2, pix[top + 2]);
        bdfi_vp8l_add(pix, p + 3, pix[top + 3]);
        break;

      case 3: /* TR. */
        bdfi_vp8l_add(pix, p + 0, pix[top + 4]);
        bdfi_vp8l_add(pix, p + 1, pix[top + 5]);
        bdfi_vp8l_add(pix, p + 2, pix[top + 6]);
        bdfi_vp8l_add(pix, p + 3, pix[top + 7]);
        break;

      case 4: /* TL. */
        bdfi_vp8l_add(pix, p + 0, pix[top - 4]);
        bdfi_vp8l_add(pix, p + 1, pix[top - 3]);
        bdfi_vp8l_add(pix, p + 2, pix[top - 2]);
        bdfi_vp8l_add(pix, p + 3, pix[top - 1]);
        break;

      case 5: /* Average2(Average2(L, TR), T). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(
              pix, p + k,
              bdfi_vp8l_avg2(bdfi_vp8l_avg2(pix[p - 4 + k], pix[top + 4 + k]), pix[top + k]));
        }
        break;

      case 6: /* Average2(L, TL). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(pix, p + k, bdfi_vp8l_avg2(pix[p - 4 + k], pix[top - 4 + k]));
        }
        break;

      case 7: /* Average2(L, T). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(pix, p + k, bdfi_vp8l_avg2(pix[p - 4 + k], pix[top + k]));
        }
        break;

      case 8: /* Average2(TL, T). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(pix, p + k, bdfi_vp8l_avg2(pix[top - 4 + k], pix[top + k]));
        }
        break;

      case 9: /* Average2(T, TR). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(pix, p + k, bdfi_vp8l_avg2(pix[top + k], pix[top + 4 + k]));
        }
        break;

      case 10: /* Average2(Average2(L, TL), Average2(T, TR)). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(pix, p + k,
                        bdfi_vp8l_avg2(bdfi_vp8l_avg2(pix[p - 4 + k], pix[top - 4 + k]),
                                       bdfi_vp8l_avg2(pix[top + k], pix[top + 4 + k])));
        }
        break;

      case 11: { /* Select(L, T, TL). */
        int32_t l = 0, tt = 0;
        for (size_t k = 0; k < 4; k++) {
          int32_t c = pix[top - 4 + k];
          l += bdfi_vp8l_abs(c - (int32_t)pix[top + k]);
          tt += bdfi_vp8l_abs(c - (int32_t)pix[p - 4 + k]);
        }
        if (l < tt) {
          for (size_t k = 0; k < 4; k++) {
            bdfi_vp8l_add(pix, p + k, pix[p - 4 + k]);
          }
        } else {
          for (size_t k = 0; k < 4; k++) {
            bdfi_vp8l_add(pix, p + k, pix[top + k]);
          }
        }
        break;
      }

      case 12: /* ClampAddSubtractFull(L, T, TL). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(
              pix, p + k,
              bdfi_vp8l_clamp_add_subtract_full(pix[p - 4 + k], pix[top + k], pix[top - 4 + k]));
        }
        break;

      case 13: /* ClampAddSubtractHalf(Average2(L, T), TL). */
        for (size_t k = 0; k < 4; k++) {
          bdfi_vp8l_add(pix, p + k,
                        bdfi_vp8l_clamp_add_subtract_half(
                            bdfi_vp8l_avg2(pix[p - 4 + k], pix[top + k]), pix[top - 4 + k]));
        }
        break;

      default: /* 14 and 15: Go's switch has no case for them */
        break;
      }
      p += 4;
      top += 4;
    }
  }
}

/* int32(int8(v)) of Go. */
static int32_t bdfi_vp8l_i8(uint8_t v) {
  return v > 0x7f ? (int32_t)v - 0x100 : (int32_t)v;
}

/* uint8(uint32(a*b) >> 5) of inverseCrossColor: the product is taken as
 * unsigned before it is shifted. */
static uint32_t bdfi_vp8l_color_delta(int32_t a, int32_t b) {
  return ((uint32_t)(a * b) >> 5) & 0xff;
}

/* inverseCrossColor */
static void bdfi_vp8l_inverse_cross_color(const bdfi_vp8l_transform *t, uint8_t *pix, int32_t h) {
  int32_t green_to_red = 0, green_to_blue = 0, red_to_blue = 0;
  size_t p = 0;
  int32_t mask = ((int32_t)1 << t->bits) - 1;
  size_t tiles_per_row = (size_t)bdfi_vp8l_n_tiles(t->old_width, t->bits);
  for (int32_t y = 0; y < h; y++) {
    size_t q = 4 * (size_t)(y >> t->bits) * tiles_per_row;
    for (int32_t x = 0; x < t->old_width; x++) {
      if ((x & mask) == 0) {
        red_to_blue = bdfi_vp8l_i8(t->pix[q + 0]);
        green_to_blue = bdfi_vp8l_i8(t->pix[q + 1]);
        green_to_red = bdfi_vp8l_i8(t->pix[q + 2]);
        q += 4;
      }
      uint8_t red = pix[p + 0];
      uint8_t green = pix[p + 1];
      uint8_t blue = pix[p + 2];
      red = (uint8_t)(red + bdfi_vp8l_color_delta(green_to_red, bdfi_vp8l_i8(green)));
      blue = (uint8_t)(blue + bdfi_vp8l_color_delta(green_to_blue, bdfi_vp8l_i8(green)));
      blue = (uint8_t)(blue + bdfi_vp8l_color_delta(red_to_blue, bdfi_vp8l_i8(red)));
      pix[p + 0] = red;
      pix[p + 2] = blue;
      p += 4;
    }
  }
}

/* inverseSubtractGreen */
static void bdfi_vp8l_inverse_subtract_green(uint8_t *pix, size_t len) {
  for (size_t p = 0; p < len; p += 4) {
    uint8_t green = pix[p + 1];
    pix[p + 0] = (uint8_t)(pix[p + 0] + green);
    pix[p + 2] = (uint8_t)(pix[p + 2] + green);
  }
}

/* inverseColorIndexing: the colors of the indices, which are the green
 * values. With 16 colors or fewer a pixel of pix holds the indices of
 * several pixels, and the image goes into dst, which is as wide as the
 * image was before the transform; else it stays in pix (len bytes) and dst
 * is not used. */
static void bdfi_vp8l_inverse_color_indexing(const bdfi_vp8l_transform *t, uint8_t *pix, size_t len,
                                             uint8_t *dst, int32_t h) {
  if (t->bits == 0) {
    for (size_t p = 0; p < len; p += 4) {
      size_t i = 4 * (size_t)pix[p + 1];
      pix[p + 0] = t->pix[i + 0];
      pix[p + 1] = t->pix[i + 1];
      pix[p + 2] = t->pix[i + 2];
      pix[p + 3] = t->pix[i + 3];
    }
    return;
  }

  uint32_t v_mask = 0, bits_per_pixel = (uint32_t)8 >> t->bits;
  int32_t x_mask = 0;
  switch (t->bits) {
  case 1:
    v_mask = 0x0f;
    x_mask = 0x01;
    break;
  case 2:
    v_mask = 0x03;
    x_mask = 0x03;
    break;
  default: /* 3 */
    v_mask = 0x01;
    x_mask = 0x07;
    break;
  }

  size_t di = 0, p = 0;
  uint32_t v = 0;
  for (int32_t y = 0; y < h; y++) {
    for (int32_t x = 0; x < t->old_width; x++) {
      if ((x & x_mask) == 0) {
        v = pix[p + 1];
        p += 4;
      }

      size_t i = 4 * (size_t)(v & v_mask);
      dst[di + 0] = t->pix[i + 0];
      dst[di + 1] = t->pix[i + 1];
      dst[di + 2] = t->pix[i + 2];
      dst[di + 3] = t->pix[i + 3];
      di += 4;

      v >>= bits_per_pixel;
    }
  }
}

/* ---- The entry points ----------------------------------------------------- */

/* decodeHeader: the magic number, the size, a hint that is not used and the
 * version, 40 bits that Go reads one field after the other. With fewer than
 * 5 bytes one of the reads fails, whatever the bytes are. */
BDF_INTERNAL int bdfi_vp8l_header(const uint8_t *data, size_t len, int32_t *w, int32_t *h) {
  *w = 0;
  *h = 0;
  if (len < BDFI_VP8L_HEADER_SIZE) {
    return 0;
  }
  if (data[0] != 0x2f) {
    return 0;
  }
  uint32_t bits = (uint32_t)data[1] | (uint32_t)data[2] << 8 | (uint32_t)data[3] << 16 |
                  (uint32_t)data[4] << 24;
  uint32_t version = bits >> 29;
  if (version != 0) {
    return 0;
  }
  *w = (int32_t)(bits & 0x3fff) + 1;
  *h = (int32_t)(bits >> 14 & 0x3fff) + 1;
  return 1;
}

/* Decode, after decodeHeader: the transforms, the pixels, and the inverse
 * transforms in reverse order. */
static bdf_status bdfi_vp8l_decode_image(bdfi_vp8l_dec *d, int32_t w, int32_t h,
                                         bdfi_vp8l_transform *transforms, uint8_t **out) {
  int n_transforms = 0;
  int seen[BDFI_VP8L_N_TRANSFORM_TYPES] = {0, 0, 0, 0};
  int32_t original_w = w;
  /* Decode the transforms. */
  for (;;) {
    uint32_t more;
    if (!bdfi_vp8l_read(d, 1, &more)) {
      return BDF_ERR_FORMAT;
    }
    if (more == 0) {
      break;
    }
    bdfi_vp8l_transform t;
    bdf_status st = bdfi_vp8l_decode_transform(d, &w, h, &t);
    if (st != BDF_OK) {
      return st;
    }
    /* Go reads a transform before it finds that it had one of the type. */
    if (seen[t.type]) {
      free(t.pix);
      return BDF_ERR_FORMAT;
    }
    seen[t.type] = 1;
    transforms[n_transforms] = t;
    n_transforms++;
  }
  /* Decode the transformed pixels. */
  uint8_t *pix = NULL;
  bdf_status st = bdfi_vp8l_decode_pix(d, w, h, 0, 1, &pix);
  if (st != BDF_OK) {
    return st;
  }
  /* Apply the inverse transformations. The image is as wide under each as
   * the transform's old_width says, but for the color indexing one. */
  size_t len = 4 * (size_t)w * (size_t)h;
  for (int i = n_transforms - 1; i >= 0; i--) {
    const bdfi_vp8l_transform *t = &transforms[i];
    switch (t->type) {
    case BDFI_VP8L_TRANSFORM_PREDICTOR:
      bdfi_vp8l_inverse_predictor(t, pix, h);
      break;
    case BDFI_VP8L_TRANSFORM_CROSS_COLOR:
      bdfi_vp8l_inverse_cross_color(t, pix, h);
      break;
    case BDFI_VP8L_TRANSFORM_SUBTRACT_GREEN:
      bdfi_vp8l_inverse_subtract_green(pix, len);
      break;
    default: /* BDFI_VP8L_TRANSFORM_COLOR_INDEXING */
      if (t->bits == 0) {
        bdfi_vp8l_inverse_color_indexing(t, pix, len, NULL, h);
      } else {
        len = 4 * (size_t)t->old_width * (size_t)h;
        uint8_t *dst = bdfi_calloc_array(len, 1);
        if (!dst) {
          free(pix);
          return BDF_ERR_NOMEM;
        }
        bdfi_vp8l_inverse_color_indexing(t, pix, 0, dst, h);
        free(pix);
        pix = dst;
      }
      break;
    }
  }
  /* pix is 4*original_w*h bytes now: only the color indexing transform
   * changes the width, and it gives the old one back. */
  (void)original_w;
  *out = pix;
  return BDF_OK;
}

BDF_INTERNAL bdf_status bdfi_vp8l_decode(const uint8_t *data, size_t len, int32_t w, int32_t h,
                                         uint8_t **pix) {
  *pix = NULL;
  if (w <= 0 || h <= 0) {
    return BDF_ERR_FORMAT;
  }
  if ((int64_t)w * h > BDFI_MAX_DECODE_PIXELS) {
    return BDF_ERR_TOO_LARGE;
  }
  bdfi_vp8l_dec d;
  bdfi_vp8l_transform transforms[BDFI_VP8L_N_TRANSFORM_TYPES];
  memset(&d, 0, sizeof d);
  memset(transforms, 0, sizeof transforms);
  d.data = data;
  d.len = len;
  d.scratch = bdfi_malloc_array(2 * BDFI_VP8L_MAX_ALPHABET, sizeof *d.scratch);
  if (!d.scratch) {
    return BDF_ERR_NOMEM;
  }
  bdf_status st = bdfi_vp8l_decode_image(&d, w, h, transforms, pix);
  for (int i = 0; i < BDFI_VP8L_N_TRANSFORM_TYPES; i++) {
    free(transforms[i].pix);
  }
  free(d.scratch);
  return st;
}
