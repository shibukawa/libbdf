/*
 * What the reader of SVG documents takes from Go's strings, unicode and
 * unicode/utf8 packages (BSD license, see third_party/go-text/LICENSE): a
 * port of the functions image/svg and its XML reader call, so that a
 * document that is not what it says (bytes that are no UTF-8, the white
 * space and the capital letters of other scripts) gives the same strings
 * here as there.
 */
#include <stdlib.h>
#include <string.h>

#include "svg_internal.h"

bdfi_svg_str bdfi_svg_str_of(const char *p, size_t n) {
  bdfi_svg_str s;
  s.p = p;
  s.n = n;
  return s;
}

/* ---- unicode/utf8 --------------------------------------------------------- */

/* decodeRuneSlow in unicode/utf8/utf8.go: what is not the shortest form of a
 * character, a surrogate or past U+10FFFF is one byte that starts none. */
uint32_t bdfi_svg_rune(const char *s, size_t n, size_t i, size_t *len) {
  const uint8_t *p = (const uint8_t *)s + i;
  size_t left = n - i;
  uint8_t p0 = p[0];
  *len = 1;
  if (p0 < 0x80) {
    return p0;
  }
  size_t size;
  uint8_t lo = 0x80, hi = 0xBF; /* what the second byte may be (acceptRanges) */
  if (p0 >= 0xC2 && p0 <= 0xDF) {
    size = 2;
  } else if (p0 >= 0xE0 && p0 <= 0xEF) {
    size = 3;
    if (p0 == 0xE0) {
      lo = 0xA0;
    } else if (p0 == 0xED) {
      hi = 0x9F;
    }
  } else if (p0 >= 0xF0 && p0 <= 0xF4) {
    size = 4;
    if (p0 == 0xF0) {
      lo = 0x90;
    } else if (p0 == 0xF4) {
      hi = 0x8F;
    }
  } else {
    return 0xFFFD;
  }
  if (left < size || p[1] < lo || p[1] > hi) {
    return 0xFFFD;
  }
  if (size == 2) {
    *len = 2;
    return ((uint32_t)(p0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
  }
  if (p[2] < 0x80 || p[2] > 0xBF) {
    return 0xFFFD;
  }
  if (size == 3) {
    *len = 3;
    return ((uint32_t)(p0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
  }
  if (p[3] < 0x80 || p[3] > 0xBF) {
    return 0xFFFD;
  }
  *len = 4;
  return ((uint32_t)(p0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
         ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
}

/* DecodeLastRune in unicode/utf8/utf8.go: it looks back four bytes at most
 * for the byte the last character starts with, and the character has to end
 * where the string does. */
uint32_t bdfi_svg_last_rune(const char *s, size_t n, size_t *len) {
  const uint8_t *p = (const uint8_t *)s;
  size_t end = n;
  size_t start = end - 1;
  *len = 1;
  if (p[start] < 0x80) {
    return p[start];
  }
  size_t lim = end > 4 ? end - 4 : 0;
  while (start > lim) {
    start--;
    if ((p[start] & 0xC0) != 0x80) { /* RuneStart */
      break;
    }
  }
  size_t size;
  uint32_t r = bdfi_svg_rune(s, end, start, &size);
  if (start + size != end) {
    return 0xFFFD;
  }
  *len = size;
  return r;
}

/* AppendRune in unicode/utf8/utf8.go. */
size_t bdfi_svg_put_rune(uint32_t r, char *out) {
  if (r <= 0x7F) {
    out[0] = (char)r;
    return 1;
  }
  if (r <= 0x7FF) {
    out[0] = (char)(0xC0 | (r >> 6));
    out[1] = (char)(0x80 | (r & 0x3F));
    return 2;
  }
  if (r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF)) {
    r = 0xFFFD;
  }
  if (r <= 0xFFFF) {
    out[0] = (char)(0xE0 | (r >> 12));
    out[1] = (char)(0x80 | ((r >> 6) & 0x3F));
    out[2] = (char)(0x80 | (r & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (r >> 18));
  out[1] = (char)(0x80 | ((r >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((r >> 6) & 0x3F));
  out[3] = (char)(0x80 | (r & 0x3F));
  return 4;
}

/* ---- unicode -------------------------------------------------------------- */

/* IsSpace in unicode/graphic.go: the property White_Space. */
int bdfi_svg_is_space(uint32_t r) {
  if (r <= 0xFF) {
    return r == ' ' || (r >= '\t' && r <= '\r') || r == 0x85 || r == 0xA0;
  }
  return r == 0x1680 || (r >= 0x2000 && r <= 0x200A) || r == 0x2028 || r == 0x2029 || r == 0x202F ||
         r == 0x205F || r == 0x3000;
}

/* A range of unicode.CaseRanges with what is added to a character of it to
 * make it lower case; BDFI_SVG_LOWER_PAIRS for capital and small letters in
 * turn (UpperLower). */
#define BDFI_SVG_LOWER_PAIRS 0x110000

typedef struct bdfi_svg_case_range {
  uint32_t lo, hi;
  int32_t delta;
} bdfi_svg_case_range;

/* The table is written by gen/main.go from the unicode package of Go 1.27. */
static const bdfi_svg_case_range bdfi_svg_lower_ranges[] = {
    /* clang-format off */
    /* BEGIN GENERATED: lower */
    /* Unicode 17.0.0: 190 ranges */
    {0x00C0, 0x00D6, 32},
    {0x00D8, 0x00DE, 32},
    {0x0100, 0x012F, BDFI_SVG_LOWER_PAIRS},
    {0x0130, 0x0130, -199},
    {0x0132, 0x0137, BDFI_SVG_LOWER_PAIRS},
    {0x0139, 0x0148, BDFI_SVG_LOWER_PAIRS},
    {0x014A, 0x0177, BDFI_SVG_LOWER_PAIRS},
    {0x0178, 0x0178, -121},
    {0x0179, 0x017E, BDFI_SVG_LOWER_PAIRS},
    {0x0181, 0x0181, 210},
    {0x0182, 0x0185, BDFI_SVG_LOWER_PAIRS},
    {0x0186, 0x0186, 206},
    {0x0187, 0x0188, BDFI_SVG_LOWER_PAIRS},
    {0x0189, 0x018A, 205},
    {0x018B, 0x018C, BDFI_SVG_LOWER_PAIRS},
    {0x018E, 0x018E, 79},
    {0x018F, 0x018F, 202},
    {0x0190, 0x0190, 203},
    {0x0191, 0x0192, BDFI_SVG_LOWER_PAIRS},
    {0x0193, 0x0193, 205},
    {0x0194, 0x0194, 207},
    {0x0196, 0x0196, 211},
    {0x0197, 0x0197, 209},
    {0x0198, 0x0199, BDFI_SVG_LOWER_PAIRS},
    {0x019C, 0x019C, 211},
    {0x019D, 0x019D, 213},
    {0x019F, 0x019F, 214},
    {0x01A0, 0x01A5, BDFI_SVG_LOWER_PAIRS},
    {0x01A6, 0x01A6, 218},
    {0x01A7, 0x01A8, BDFI_SVG_LOWER_PAIRS},
    {0x01A9, 0x01A9, 218},
    {0x01AC, 0x01AD, BDFI_SVG_LOWER_PAIRS},
    {0x01AE, 0x01AE, 218},
    {0x01AF, 0x01B0, BDFI_SVG_LOWER_PAIRS},
    {0x01B1, 0x01B2, 217},
    {0x01B3, 0x01B6, BDFI_SVG_LOWER_PAIRS},
    {0x01B7, 0x01B7, 219},
    {0x01B8, 0x01B9, BDFI_SVG_LOWER_PAIRS},
    {0x01BC, 0x01BD, BDFI_SVG_LOWER_PAIRS},
    {0x01C4, 0x01C4, 2},
    {0x01C5, 0x01C5, 1},
    {0x01C7, 0x01C7, 2},
    {0x01C8, 0x01C8, 1},
    {0x01CA, 0x01CA, 2},
    {0x01CB, 0x01CB, 1},
    {0x01CD, 0x01DC, BDFI_SVG_LOWER_PAIRS},
    {0x01DE, 0x01EF, BDFI_SVG_LOWER_PAIRS},
    {0x01F1, 0x01F1, 2},
    {0x01F2, 0x01F2, 1},
    {0x01F4, 0x01F5, BDFI_SVG_LOWER_PAIRS},
    {0x01F6, 0x01F6, -97},
    {0x01F7, 0x01F7, -56},
    {0x01F8, 0x021F, BDFI_SVG_LOWER_PAIRS},
    {0x0220, 0x0220, -130},
    {0x0222, 0x0233, BDFI_SVG_LOWER_PAIRS},
    {0x023A, 0x023A, 10795},
    {0x023B, 0x023C, BDFI_SVG_LOWER_PAIRS},
    {0x023D, 0x023D, -163},
    {0x023E, 0x023E, 10792},
    {0x0241, 0x0242, BDFI_SVG_LOWER_PAIRS},
    {0x0243, 0x0243, -195},
    {0x0244, 0x0244, 69},
    {0x0245, 0x0245, 71},
    {0x0246, 0x024F, BDFI_SVG_LOWER_PAIRS},
    {0x0370, 0x0373, BDFI_SVG_LOWER_PAIRS},
    {0x0376, 0x0377, BDFI_SVG_LOWER_PAIRS},
    {0x037F, 0x037F, 116},
    {0x0386, 0x0386, 38},
    {0x0388, 0x038A, 37},
    {0x038C, 0x038C, 64},
    {0x038E, 0x038F, 63},
    {0x0391, 0x03A1, 32},
    {0x03A3, 0x03AB, 32},
    {0x03CF, 0x03CF, 8},
    {0x03D8, 0x03EF, BDFI_SVG_LOWER_PAIRS},
    {0x03F4, 0x03F4, -60},
    {0x03F7, 0x03F8, BDFI_SVG_LOWER_PAIRS},
    {0x03F9, 0x03F9, -7},
    {0x03FA, 0x03FB, BDFI_SVG_LOWER_PAIRS},
    {0x03FD, 0x03FF, -130},
    {0x0400, 0x040F, 80},
    {0x0410, 0x042F, 32},
    {0x0460, 0x0481, BDFI_SVG_LOWER_PAIRS},
    {0x048A, 0x04BF, BDFI_SVG_LOWER_PAIRS},
    {0x04C0, 0x04C0, 15},
    {0x04C1, 0x04CE, BDFI_SVG_LOWER_PAIRS},
    {0x04D0, 0x052F, BDFI_SVG_LOWER_PAIRS},
    {0x0531, 0x0556, 48},
    {0x10A0, 0x10C5, 7264},
    {0x10C7, 0x10C7, 7264},
    {0x10CD, 0x10CD, 7264},
    {0x13A0, 0x13EF, 38864},
    {0x13F0, 0x13F5, 8},
    {0x1C89, 0x1C8A, BDFI_SVG_LOWER_PAIRS},
    {0x1C90, 0x1CBA, -3008},
    {0x1CBD, 0x1CBF, -3008},
    {0x1E00, 0x1E95, BDFI_SVG_LOWER_PAIRS},
    {0x1E9E, 0x1E9E, -7615},
    {0x1EA0, 0x1EFF, BDFI_SVG_LOWER_PAIRS},
    {0x1F08, 0x1F0F, -8},
    {0x1F18, 0x1F1D, -8},
    {0x1F28, 0x1F2F, -8},
    {0x1F38, 0x1F3F, -8},
    {0x1F48, 0x1F4D, -8},
    {0x1F59, 0x1F59, -8},
    {0x1F5B, 0x1F5B, -8},
    {0x1F5D, 0x1F5D, -8},
    {0x1F5F, 0x1F5F, -8},
    {0x1F68, 0x1F6F, -8},
    {0x1F88, 0x1F8F, -8},
    {0x1F98, 0x1F9F, -8},
    {0x1FA8, 0x1FAF, -8},
    {0x1FB8, 0x1FB9, -8},
    {0x1FBA, 0x1FBB, -74},
    {0x1FBC, 0x1FBC, -9},
    {0x1FC8, 0x1FCB, -86},
    {0x1FCC, 0x1FCC, -9},
    {0x1FD8, 0x1FD9, -8},
    {0x1FDA, 0x1FDB, -100},
    {0x1FE8, 0x1FE9, -8},
    {0x1FEA, 0x1FEB, -112},
    {0x1FEC, 0x1FEC, -7},
    {0x1FF8, 0x1FF9, -128},
    {0x1FFA, 0x1FFB, -126},
    {0x1FFC, 0x1FFC, -9},
    {0x2126, 0x2126, -7517},
    {0x212A, 0x212A, -8383},
    {0x212B, 0x212B, -8262},
    {0x2132, 0x2132, 28},
    {0x2160, 0x216F, 16},
    {0x2183, 0x2184, BDFI_SVG_LOWER_PAIRS},
    {0x24B6, 0x24CF, 26},
    {0x2C00, 0x2C2F, 48},
    {0x2C60, 0x2C61, BDFI_SVG_LOWER_PAIRS},
    {0x2C62, 0x2C62, -10743},
    {0x2C63, 0x2C63, -3814},
    {0x2C64, 0x2C64, -10727},
    {0x2C67, 0x2C6C, BDFI_SVG_LOWER_PAIRS},
    {0x2C6D, 0x2C6D, -10780},
    {0x2C6E, 0x2C6E, -10749},
    {0x2C6F, 0x2C6F, -10783},
    {0x2C70, 0x2C70, -10782},
    {0x2C72, 0x2C73, BDFI_SVG_LOWER_PAIRS},
    {0x2C75, 0x2C76, BDFI_SVG_LOWER_PAIRS},
    {0x2C7E, 0x2C7F, -10815},
    {0x2C80, 0x2CE3, BDFI_SVG_LOWER_PAIRS},
    {0x2CEB, 0x2CEE, BDFI_SVG_LOWER_PAIRS},
    {0x2CF2, 0x2CF3, BDFI_SVG_LOWER_PAIRS},
    {0xA640, 0xA66D, BDFI_SVG_LOWER_PAIRS},
    {0xA680, 0xA69B, BDFI_SVG_LOWER_PAIRS},
    {0xA722, 0xA72F, BDFI_SVG_LOWER_PAIRS},
    {0xA732, 0xA76F, BDFI_SVG_LOWER_PAIRS},
    {0xA779, 0xA77C, BDFI_SVG_LOWER_PAIRS},
    {0xA77D, 0xA77D, -35332},
    {0xA77E, 0xA787, BDFI_SVG_LOWER_PAIRS},
    {0xA78B, 0xA78C, BDFI_SVG_LOWER_PAIRS},
    {0xA78D, 0xA78D, -42280},
    {0xA790, 0xA793, BDFI_SVG_LOWER_PAIRS},
    {0xA796, 0xA7A9, BDFI_SVG_LOWER_PAIRS},
    {0xA7AA, 0xA7AA, -42308},
    {0xA7AB, 0xA7AB, -42319},
    {0xA7AC, 0xA7AC, -42315},
    {0xA7AD, 0xA7AD, -42305},
    {0xA7AE, 0xA7AE, -42308},
    {0xA7B0, 0xA7B0, -42258},
    {0xA7B1, 0xA7B1, -42282},
    {0xA7B2, 0xA7B2, -42261},
    {0xA7B3, 0xA7B3, 928},
    {0xA7B4, 0xA7C3, BDFI_SVG_LOWER_PAIRS},
    {0xA7C4, 0xA7C4, -48},
    {0xA7C5, 0xA7C5, -42307},
    {0xA7C6, 0xA7C6, -35384},
    {0xA7C7, 0xA7CA, BDFI_SVG_LOWER_PAIRS},
    {0xA7CB, 0xA7CB, -42343},
    {0xA7CC, 0xA7DB, BDFI_SVG_LOWER_PAIRS},
    {0xA7DC, 0xA7DC, -42561},
    {0xA7F5, 0xA7F6, BDFI_SVG_LOWER_PAIRS},
    {0xFF21, 0xFF3A, 32},
    {0x10400, 0x10427, 40},
    {0x104B0, 0x104D3, 40},
    {0x10570, 0x1057A, 39},
    {0x1057C, 0x1058A, 39},
    {0x1058C, 0x10592, 39},
    {0x10594, 0x10595, 39},
    {0x10C80, 0x10CB2, 64},
    {0x10D50, 0x10D65, 32},
    {0x118A0, 0x118BF, 32},
    {0x16E40, 0x16E5F, 32},
    {0x16EA0, 0x16EB8, 27},
    {0x1E900, 0x1E921, 34},
    /* END GENERATED: lower */
    /* clang-format on */
};

/* ToLower in unicode/letter.go. */
uint32_t bdfi_svg_to_lower(uint32_t r) {
  if (r <= 0x7F) {
    return r >= 'A' && r <= 'Z' ? r + ('a' - 'A') : r;
  }
  size_t lo = 0, hi = sizeof bdfi_svg_lower_ranges / sizeof bdfi_svg_lower_ranges[0];
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    const bdfi_svg_case_range *cr = &bdfi_svg_lower_ranges[m];
    if (r < cr->lo) {
      hi = m;
    } else if (r > cr->hi) {
      lo = m + 1;
    } else if (cr->delta == BDFI_SVG_LOWER_PAIRS) {
      /* convertCase: the letters at even places of the range are capitals */
      return cr->lo + (((r - cr->lo) & ~(uint32_t)1) | 1);
    } else {
      return (uint32_t)((int32_t)r + cr->delta);
    }
  }
  return r;
}

/* ---- strings -------------------------------------------------------------- */

/* TrimSpace in strings/strings.go: the white space of Unicode at both ends,
 * with a byte that is no UTF-8 taken for a character that is none. */
bdfi_svg_str bdfi_svg_trim_space(bdfi_svg_str s) {
  size_t i = 0, len;
  while (i < s.n) {
    if (!bdfi_svg_is_space(bdfi_svg_rune(s.p, s.n, i, &len))) {
      break;
    }
    i += len;
  }
  /* TrimRightFunc looks at the string without what was trimmed on the left */
  const char *base = s.p + i;
  size_t end = s.n - i;
  while (end > 0) {
    if (!bdfi_svg_is_space(bdfi_svg_last_rune(base, end, &len))) {
      break;
    }
    end -= len;
  }
  return bdfi_svg_str_of(base, end);
}

/* Fields in strings/strings.go (FieldsFunc with unicode.IsSpace). */
int bdfi_svg_next_field(bdfi_svg_str s, size_t *pos, bdfi_svg_str *field) {
  size_t i = *pos, len;
  while (i < s.n && bdfi_svg_is_space(bdfi_svg_rune(s.p, s.n, i, &len))) {
    i += len;
  }
  if (i >= s.n) {
    *pos = s.n;
    return 0;
  }
  size_t start = i;
  while (i < s.n && !bdfi_svg_is_space(bdfi_svg_rune(s.p, s.n, i, &len))) {
    i += len;
  }
  *field = bdfi_svg_str_of(s.p + start, i - start);
  *pos = i;
  return 1;
}

/* ToLower in strings/strings.go, which is Map(unicode.ToLower, s) for a
 * string that is not ASCII: a byte that starts no character comes out as
 * U+FFFD, three bytes. */
size_t bdfi_svg_lower(const char *s, size_t n, char *out) {
  size_t o = 0, len;
  for (size_t i = 0; i < n; i += len) {
    uint32_t r = bdfi_svg_to_lower(bdfi_svg_rune(s, n, i, &len));
    if (out) {
      o += bdfi_svg_put_rune(r, out + o);
    } else {
      char tmp[4];
      o += bdfi_svg_put_rune(r, tmp);
    }
  }
  return o;
}

size_t bdfi_svg_lower_ascii(bdfi_svg_str s, char *out, size_t cap) {
  size_t o = 0, len;
  for (size_t i = 0; i < s.n; i += len) {
    uint32_t r = bdfi_svg_to_lower(bdfi_svg_rune(s.p, s.n, i, &len));
    if (r >= 0x80 || o >= cap) {
      return SIZE_MAX;
    }
    out[o++] = (char)r;
  }
  return o;
}

int bdfi_svg_str_cmp(bdfi_svg_str a, bdfi_svg_str b) {
  if (a.n != b.n) {
    return a.n < b.n ? -1 : 1;
  }
  return a.n == 0 ? 0 : memcmp(a.p, b.p, a.n);
}

int bdfi_svg_str_eq(bdfi_svg_str a, const char *p, size_t n) {
  return a.n == n && (n == 0 || memcmp(a.p, p, n) == 0);
}

const bdfi_svg_decl *bdfi_svg_find(const bdfi_svg_decl *d, size_t n, const char *name, size_t len) {
  bdfi_svg_str want = bdfi_svg_str_of(name, len);
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    int c = bdfi_svg_str_cmp(d[m].name, want);
    if (c == 0) {
      return &d[m];
    }
    if (c < 0) {
      lo = m + 1;
    } else {
      hi = m;
    }
  }
  return NULL;
}

/* ---- A stable sort -------------------------------------------------------- */

#define BDFI_SVG_SORT_RUN  8  /* runs of this many are sorted by insertion */
#define BDFI_SVG_SORT_ITEM 64 /* the largest item sorted */

static void bdfi_svg_sort_run(char *base, size_t n, size_t size,
                              int (*cmp)(const void *, const void *)) {
  char item[BDFI_SVG_SORT_ITEM];
  for (size_t i = 1; i < n; i++) {
    size_t j = i;
    if (cmp(base + (j - 1) * size, base + j * size) <= 0) {
      continue;
    }
    memcpy(item, base + i * size, size);
    while (j > 0 && cmp(base + (j - 1) * size, item) > 0) {
      memcpy(base + j * size, base + (j - 1) * size, size);
      j--;
    }
    memcpy(base + j * size, item, size);
  }
}

int bdfi_svg_sort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
  char *a = base;
  if (n < 2 || size == 0 || size > BDFI_SVG_SORT_ITEM) {
    return 0;
  }
  for (size_t i = 0; i < n; i += BDFI_SVG_SORT_RUN) {
    bdfi_svg_sort_run(a + i * size, n - i < BDFI_SVG_SORT_RUN ? n - i : BDFI_SVG_SORT_RUN, size,
                      cmp);
  }
  if (n <= BDFI_SVG_SORT_RUN) {
    return 0;
  }
  char *tmp = bdfi_malloc_array(n, size);
  if (!tmp) {
    return -1;
  }
  char *src = a, *dst = tmp;
  for (size_t width = BDFI_SVG_SORT_RUN; width<n; width = width> n / 2 ? n : width * 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = n - lo < width ? n : lo + width;
      size_t hi = n - lo < 2 * width ? n : lo + 2 * width;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        /* the item of the left run goes first when the two are equal */
        if (cmp(src + j * size, src + i * size) < 0) {
          memcpy(dst + k * size, src + j * size, size);
          j++;
        } else {
          memcpy(dst + k * size, src + i * size, size);
          i++;
        }
        k++;
      }
      if (i < mid) {
        memcpy(dst + k * size, src + i * size, (mid - i) * size);
      }
      if (j < hi) {
        memcpy(dst + k * size, src + j * size, (hi - j) * size);
      }
    }
    char *t = src;
    src = dst;
    dst = t;
  }
  if (src != a) {
    memcpy(a, src, n * size);
  }
  free(tmp);
  return 0;
}

/* ---- Memory of a document ------------------------------------------------- */

#define BDFI_SVG_POOL_BLOCK 16384 /* strings of up to an eighth of it are packed into blocks */

char *bdfi_svg_pool_bytes(bdfi_svg_pool *pool, size_t n) {
  if (n > BDFI_SVG_POOL_BLOCK / 8) {
    return bdfi_arena_alloc(pool->arena, n);
  }
  if (!pool->p || n > pool->left) {
    pool->p = bdfi_arena_alloc(pool->arena, BDFI_SVG_POOL_BLOCK);
    if (!pool->p) {
      pool->left = 0;
      return NULL;
    }
    pool->left = BDFI_SVG_POOL_BLOCK;
  }
  char *p = pool->p;
  pool->p += n;
  pool->left -= n;
  return p;
}

int bdfi_svg_pool_dup(bdfi_svg_pool *pool, const char *s, size_t n, bdfi_svg_str *out) {
  char *p = bdfi_svg_pool_bytes(pool, n);
  if (!p) {
    return -1;
  }
  if (n > 0) {
    memcpy(p, s, n);
  }
  *out = bdfi_svg_str_of(p, n);
  return 0;
}

void *bdfi_svg_pool_array(bdfi_svg_pool *pool, size_t n, size_t size) {
  if (size != 0 && n > SIZE_MAX / size) {
    return NULL;
  }
  return bdfi_arena_alloc(pool->arena, n * size);
}
