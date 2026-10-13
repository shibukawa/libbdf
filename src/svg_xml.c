/*
 * The XML reader of the SVG reader: a port of xmltree.Open of the reference
 * implementation (internal/xmltree/open.go) and of the pull reader it opens,
 * xmlro.Reader of github.com/shibukawa/tinygodriver (encoding/xmlro, by the
 * author of libbdf, who gives this port under libbdf's license), as
 * svg.Parse configures it: lenient (an end tag that does not match ends the
 * elements it leaves open, attributes without values or quotes), with the
 * entities and the void elements of HTML, a DOCTYPE read for the entities
 * it declares, and documents in the encodings they declare. The tokens, the
 * point where a damaged document stops and the bytes of names and values
 * are those of the Go code.
 *
 * What differs is how it gets there. The Go reader reads a converted
 * document through a buffer that grows; here every document is in memory,
 * which gives the same tokens (the buffer is never too small: see
 * bdfi_xml_open). The searches the Go code repeats from every '&' are
 * remembered instead, so that a document of nothing but '&' takes time in
 * proportion to its size. The namespaces and the names of invented end tags
 * are not kept: svg.Parse asks for neither.
 *
 * The encodings read are UTF-8, UTF-16 and windows-1252 (which is what the
 * labels of ISO-8859-1 and US-ASCII mean to golang.org/x/net/html/charset);
 * the decoders are ports of those of golang.org/x/text (BSD license, see
 * third_party/go-text/LICENSE). A
 * document that declares another encoding is read while it is ASCII, which
 * is the same bytes there, and refused when it is not.
 */
#include <stdlib.h>
#include <string.h>

#include "svg_internal.h"

#define BDFI_XML_NONE_AT SIZE_MAX /* of a search: not found */

/* maxEntityName in entity.go: the longest name a reference is looked up by. */
#define BDFI_XML_MAX_ENTITY_NAME 64

/* ---- Bytes ---------------------------------------------------------------- */

static int bdfi_xml_buf_reserve(bdfi_xml_buf *b, size_t extra) {
  if (extra > SIZE_MAX - b->n) {
    return -1;
  }
  size_t need = b->n + extra;
  if (need <= b->cap && b->p) {
    return 0;
  }
  size_t cap = b->cap ? b->cap : 256;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  uint8_t *p = bdfi_realloc_array(b->p, cap, 1);
  if (!p) {
    return -1;
  }
  b->p = p;
  b->cap = cap;
  return 0;
}

int bdfi_xml_buf_append(bdfi_xml_buf *b, const void *p, size_t n) {
  if (bdfi_xml_buf_reserve(b, n) != 0) {
    return -1;
  }
  if (n > 0) {
    memcpy(b->p + b->n, p, n);
    b->n += n;
  }
  return 0;
}

static int bdfi_xml_buf_byte(bdfi_xml_buf *b, uint8_t c) {
  return bdfi_xml_buf_append(b, &c, 1);
}

static int bdfi_xml_buf_rune(bdfi_xml_buf *b, uint32_t r) {
  char enc[4];
  return bdfi_xml_buf_append(b, enc, bdfi_svg_put_rune(r, enc));
}

void bdfi_xml_buf_free(bdfi_xml_buf *b) {
  free(b->p);
  b->p = NULL;
  b->n = b->cap = 0;
}

/* bytes.Index of s in b[from:to]: where it is, or BDFI_XML_NONE_AT. */
static size_t bdfi_xml_index(const uint8_t *b, size_t from, size_t to, const char *s, size_t len) {
  while (from <= to && to - from >= len) {
    const uint8_t *p = memchr(b + from, s[0], to - from - len + 1);
    if (!p) {
      break;
    }
    if (memcmp(p, s, len) == 0) {
      return (size_t)(p - b);
    }
    from = (size_t)(p - b) + 1;
  }
  return BDFI_XML_NONE_AT;
}

static size_t bdfi_xml_index_byte(const uint8_t *b, size_t from, size_t to, uint8_t c) {
  const uint8_t *p = from < to ? memchr(b + from, c, to - from) : NULL;
  return p ? (size_t)(p - b) : BDFI_XML_NONE_AT;
}

static int bdfi_xml_is_space(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static uint8_t bdfi_xml_fold(uint8_t c) {
  return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c;
}

/* ---- The encodings (golang.org/x/text/encoding) --------------------------- */

/* What a label of an encoding names. */
enum {
  BDFI_XML_ENC_OTHER, /* an encoding the reference reads and this library does not */
  BDFI_XML_ENC_ASCII, /* one of those in which ASCII is ASCII: read when it holds nothing else */
  BDFI_XML_ENC_ASCII_NO_ESC, /* the same but for the escape character (ISO-2022-JP) */
  BDFI_XML_ENC_UTF8,
  BDFI_XML_ENC_UTF16BE,
  BDFI_XML_ENC_UTF16LE,
  BDFI_XML_ENC_WINDOWS1252,
  BDFI_XML_ENC_UNKNOWN /* no label of an encoding */
};

typedef struct bdfi_xml_label {
  char label[20];
  uint8_t kind;
} bdfi_xml_label;

/* nameMap of golang.org/x/text/encoding/htmlindex, which
 * charset.NewReaderLabel looks a label up in. The table is written by
 * gen/main.go, which asks the decoder of every encoding that is not read
 * here what it makes of ASCII. */
static const bdfi_xml_label bdfi_xml_labels[] = {
    /* clang-format off */
    /* BEGIN GENERATED: labels */
    /* 228 labels */
    {"866", BDFI_XML_ENC_ASCII},
    {"ansi_x3.4-1968", BDFI_XML_ENC_WINDOWS1252},
    {"arabic", BDFI_XML_ENC_ASCII},
    {"ascii", BDFI_XML_ENC_WINDOWS1252},
    {"asmo-708", BDFI_XML_ENC_ASCII},
    {"big5", BDFI_XML_ENC_ASCII},
    {"big5-hkscs", BDFI_XML_ENC_ASCII},
    {"chinese", BDFI_XML_ENC_ASCII},
    {"cn-big5", BDFI_XML_ENC_ASCII},
    {"cp1250", BDFI_XML_ENC_ASCII},
    {"cp1251", BDFI_XML_ENC_ASCII},
    {"cp1252", BDFI_XML_ENC_WINDOWS1252},
    {"cp1253", BDFI_XML_ENC_ASCII},
    {"cp1254", BDFI_XML_ENC_ASCII},
    {"cp1255", BDFI_XML_ENC_ASCII},
    {"cp1256", BDFI_XML_ENC_ASCII},
    {"cp1257", BDFI_XML_ENC_ASCII},
    {"cp1258", BDFI_XML_ENC_ASCII},
    {"cp819", BDFI_XML_ENC_WINDOWS1252},
    {"cp866", BDFI_XML_ENC_ASCII},
    {"csbig5", BDFI_XML_ENC_ASCII},
    {"cseuckr", BDFI_XML_ENC_ASCII},
    {"cseucpkdfmtjapanese", BDFI_XML_ENC_ASCII},
    {"csgb2312", BDFI_XML_ENC_ASCII},
    {"csibm866", BDFI_XML_ENC_ASCII},
    {"csiso2022jp", BDFI_XML_ENC_ASCII_NO_ESC},
    {"csiso2022kr", BDFI_XML_ENC_OTHER},
    {"csiso58gb231280", BDFI_XML_ENC_ASCII},
    {"csiso88596e", BDFI_XML_ENC_ASCII},
    {"csiso88596i", BDFI_XML_ENC_ASCII},
    {"csiso88598e", BDFI_XML_ENC_ASCII},
    {"csiso88598i", BDFI_XML_ENC_ASCII},
    {"csisolatin1", BDFI_XML_ENC_WINDOWS1252},
    {"csisolatin2", BDFI_XML_ENC_ASCII},
    {"csisolatin3", BDFI_XML_ENC_ASCII},
    {"csisolatin4", BDFI_XML_ENC_ASCII},
    {"csisolatin5", BDFI_XML_ENC_ASCII},
    {"csisolatin6", BDFI_XML_ENC_ASCII},
    {"csisolatin9", BDFI_XML_ENC_ASCII},
    {"csisolatinarabic", BDFI_XML_ENC_ASCII},
    {"csisolatincyrillic", BDFI_XML_ENC_ASCII},
    {"csisolatingreek", BDFI_XML_ENC_ASCII},
    {"csisolatinhebrew", BDFI_XML_ENC_ASCII},
    {"cskoi8r", BDFI_XML_ENC_ASCII},
    {"csksc56011987", BDFI_XML_ENC_ASCII},
    {"csmacintosh", BDFI_XML_ENC_ASCII},
    {"csshiftjis", BDFI_XML_ENC_ASCII},
    {"csunicode", BDFI_XML_ENC_UTF16LE},
    {"cyrillic", BDFI_XML_ENC_ASCII},
    {"dos-874", BDFI_XML_ENC_ASCII},
    {"ecma-114", BDFI_XML_ENC_ASCII},
    {"ecma-118", BDFI_XML_ENC_ASCII},
    {"elot_928", BDFI_XML_ENC_ASCII},
    {"euc-jp", BDFI_XML_ENC_ASCII},
    {"euc-kr", BDFI_XML_ENC_ASCII},
    {"gb18030", BDFI_XML_ENC_ASCII},
    {"gb2312", BDFI_XML_ENC_ASCII},
    {"gb_2312", BDFI_XML_ENC_ASCII},
    {"gb_2312-80", BDFI_XML_ENC_ASCII},
    {"gbk", BDFI_XML_ENC_ASCII},
    {"greek", BDFI_XML_ENC_ASCII},
    {"greek8", BDFI_XML_ENC_ASCII},
    {"hebrew", BDFI_XML_ENC_ASCII},
    {"hz-gb-2312", BDFI_XML_ENC_OTHER},
    {"ibm819", BDFI_XML_ENC_WINDOWS1252},
    {"ibm866", BDFI_XML_ENC_ASCII},
    {"iso-10646-ucs-2", BDFI_XML_ENC_UTF16LE},
    {"iso-2022-cn", BDFI_XML_ENC_OTHER},
    {"iso-2022-cn-ext", BDFI_XML_ENC_OTHER},
    {"iso-2022-jp", BDFI_XML_ENC_ASCII_NO_ESC},
    {"iso-2022-kr", BDFI_XML_ENC_OTHER},
    {"iso-8859-1", BDFI_XML_ENC_WINDOWS1252},
    {"iso-8859-10", BDFI_XML_ENC_ASCII},
    {"iso-8859-11", BDFI_XML_ENC_ASCII},
    {"iso-8859-13", BDFI_XML_ENC_ASCII},
    {"iso-8859-14", BDFI_XML_ENC_ASCII},
    {"iso-8859-15", BDFI_XML_ENC_ASCII},
    {"iso-8859-16", BDFI_XML_ENC_ASCII},
    {"iso-8859-2", BDFI_XML_ENC_ASCII},
    {"iso-8859-3", BDFI_XML_ENC_ASCII},
    {"iso-8859-4", BDFI_XML_ENC_ASCII},
    {"iso-8859-5", BDFI_XML_ENC_ASCII},
    {"iso-8859-6", BDFI_XML_ENC_ASCII},
    {"iso-8859-6-e", BDFI_XML_ENC_ASCII},
    {"iso-8859-6-i", BDFI_XML_ENC_ASCII},
    {"iso-8859-7", BDFI_XML_ENC_ASCII},
    {"iso-8859-8", BDFI_XML_ENC_ASCII},
    {"iso-8859-8-e", BDFI_XML_ENC_ASCII},
    {"iso-8859-8-i", BDFI_XML_ENC_ASCII},
    {"iso-8859-9", BDFI_XML_ENC_ASCII},
    {"iso-ir-100", BDFI_XML_ENC_WINDOWS1252},
    {"iso-ir-101", BDFI_XML_ENC_ASCII},
    {"iso-ir-109", BDFI_XML_ENC_ASCII},
    {"iso-ir-110", BDFI_XML_ENC_ASCII},
    {"iso-ir-126", BDFI_XML_ENC_ASCII},
    {"iso-ir-127", BDFI_XML_ENC_ASCII},
    {"iso-ir-138", BDFI_XML_ENC_ASCII},
    {"iso-ir-144", BDFI_XML_ENC_ASCII},
    {"iso-ir-148", BDFI_XML_ENC_ASCII},
    {"iso-ir-149", BDFI_XML_ENC_ASCII},
    {"iso-ir-157", BDFI_XML_ENC_ASCII},
    {"iso-ir-58", BDFI_XML_ENC_ASCII},
    {"iso8859-1", BDFI_XML_ENC_WINDOWS1252},
    {"iso8859-10", BDFI_XML_ENC_ASCII},
    {"iso8859-11", BDFI_XML_ENC_ASCII},
    {"iso8859-13", BDFI_XML_ENC_ASCII},
    {"iso8859-14", BDFI_XML_ENC_ASCII},
    {"iso8859-15", BDFI_XML_ENC_ASCII},
    {"iso8859-2", BDFI_XML_ENC_ASCII},
    {"iso8859-3", BDFI_XML_ENC_ASCII},
    {"iso8859-4", BDFI_XML_ENC_ASCII},
    {"iso8859-5", BDFI_XML_ENC_ASCII},
    {"iso8859-6", BDFI_XML_ENC_ASCII},
    {"iso8859-7", BDFI_XML_ENC_ASCII},
    {"iso8859-8", BDFI_XML_ENC_ASCII},
    {"iso8859-9", BDFI_XML_ENC_ASCII},
    {"iso88591", BDFI_XML_ENC_WINDOWS1252},
    {"iso885910", BDFI_XML_ENC_ASCII},
    {"iso885911", BDFI_XML_ENC_ASCII},
    {"iso885913", BDFI_XML_ENC_ASCII},
    {"iso885914", BDFI_XML_ENC_ASCII},
    {"iso885915", BDFI_XML_ENC_ASCII},
    {"iso88592", BDFI_XML_ENC_ASCII},
    {"iso88593", BDFI_XML_ENC_ASCII},
    {"iso88594", BDFI_XML_ENC_ASCII},
    {"iso88595", BDFI_XML_ENC_ASCII},
    {"iso88596", BDFI_XML_ENC_ASCII},
    {"iso88597", BDFI_XML_ENC_ASCII},
    {"iso88598", BDFI_XML_ENC_ASCII},
    {"iso88599", BDFI_XML_ENC_ASCII},
    {"iso_8859-1", BDFI_XML_ENC_WINDOWS1252},
    {"iso_8859-15", BDFI_XML_ENC_ASCII},
    {"iso_8859-1:1987", BDFI_XML_ENC_WINDOWS1252},
    {"iso_8859-2", BDFI_XML_ENC_ASCII},
    {"iso_8859-2:1987", BDFI_XML_ENC_ASCII},
    {"iso_8859-3", BDFI_XML_ENC_ASCII},
    {"iso_8859-3:1988", BDFI_XML_ENC_ASCII},
    {"iso_8859-4", BDFI_XML_ENC_ASCII},
    {"iso_8859-4:1988", BDFI_XML_ENC_ASCII},
    {"iso_8859-5", BDFI_XML_ENC_ASCII},
    {"iso_8859-5:1988", BDFI_XML_ENC_ASCII},
    {"iso_8859-6", BDFI_XML_ENC_ASCII},
    {"iso_8859-6:1987", BDFI_XML_ENC_ASCII},
    {"iso_8859-7", BDFI_XML_ENC_ASCII},
    {"iso_8859-7:1987", BDFI_XML_ENC_ASCII},
    {"iso_8859-8", BDFI_XML_ENC_ASCII},
    {"iso_8859-8:1988", BDFI_XML_ENC_ASCII},
    {"iso_8859-9", BDFI_XML_ENC_ASCII},
    {"iso_8859-9:1989", BDFI_XML_ENC_ASCII},
    {"koi", BDFI_XML_ENC_ASCII},
    {"koi8", BDFI_XML_ENC_ASCII},
    {"koi8-r", BDFI_XML_ENC_ASCII},
    {"koi8-ru", BDFI_XML_ENC_ASCII},
    {"koi8-u", BDFI_XML_ENC_ASCII},
    {"koi8_r", BDFI_XML_ENC_ASCII},
    {"korean", BDFI_XML_ENC_ASCII},
    {"ks_c_5601-1987", BDFI_XML_ENC_ASCII},
    {"ks_c_5601-1989", BDFI_XML_ENC_ASCII},
    {"ksc5601", BDFI_XML_ENC_ASCII},
    {"ksc_5601", BDFI_XML_ENC_ASCII},
    {"l1", BDFI_XML_ENC_WINDOWS1252},
    {"l2", BDFI_XML_ENC_ASCII},
    {"l3", BDFI_XML_ENC_ASCII},
    {"l4", BDFI_XML_ENC_ASCII},
    {"l5", BDFI_XML_ENC_ASCII},
    {"l6", BDFI_XML_ENC_ASCII},
    {"l9", BDFI_XML_ENC_ASCII},
    {"latin1", BDFI_XML_ENC_WINDOWS1252},
    {"latin2", BDFI_XML_ENC_ASCII},
    {"latin3", BDFI_XML_ENC_ASCII},
    {"latin4", BDFI_XML_ENC_ASCII},
    {"latin5", BDFI_XML_ENC_ASCII},
    {"latin6", BDFI_XML_ENC_ASCII},
    {"logical", BDFI_XML_ENC_ASCII},
    {"mac", BDFI_XML_ENC_ASCII},
    {"macintosh", BDFI_XML_ENC_ASCII},
    {"ms932", BDFI_XML_ENC_ASCII},
    {"ms_kanji", BDFI_XML_ENC_ASCII},
    {"replacement", BDFI_XML_ENC_OTHER},
    {"shift-jis", BDFI_XML_ENC_ASCII},
    {"shift_jis", BDFI_XML_ENC_ASCII},
    {"sjis", BDFI_XML_ENC_ASCII},
    {"sun_eu_greek", BDFI_XML_ENC_ASCII},
    {"tis-620", BDFI_XML_ENC_ASCII},
    {"ucs-2", BDFI_XML_ENC_UTF16LE},
    {"unicode", BDFI_XML_ENC_UTF16LE},
    {"unicode-1-1-utf-8", BDFI_XML_ENC_UTF8},
    {"unicode11utf8", BDFI_XML_ENC_UTF8},
    {"unicode20utf8", BDFI_XML_ENC_UTF8},
    {"unicodefeff", BDFI_XML_ENC_UTF16LE},
    {"unicodefffe", BDFI_XML_ENC_UTF16BE},
    {"us-ascii", BDFI_XML_ENC_WINDOWS1252},
    {"utf-16", BDFI_XML_ENC_UTF16LE},
    {"utf-16be", BDFI_XML_ENC_UTF16BE},
    {"utf-16le", BDFI_XML_ENC_UTF16LE},
    {"utf-8", BDFI_XML_ENC_UTF8},
    {"utf8", BDFI_XML_ENC_UTF8},
    {"visual", BDFI_XML_ENC_ASCII},
    {"windows-1250", BDFI_XML_ENC_ASCII},
    {"windows-1251", BDFI_XML_ENC_ASCII},
    {"windows-1252", BDFI_XML_ENC_WINDOWS1252},
    {"windows-1253", BDFI_XML_ENC_ASCII},
    {"windows-1254", BDFI_XML_ENC_ASCII},
    {"windows-1255", BDFI_XML_ENC_ASCII},
    {"windows-1256", BDFI_XML_ENC_ASCII},
    {"windows-1257", BDFI_XML_ENC_ASCII},
    {"windows-1258", BDFI_XML_ENC_ASCII},
    {"windows-31j", BDFI_XML_ENC_ASCII},
    {"windows-874", BDFI_XML_ENC_ASCII},
    {"windows-949", BDFI_XML_ENC_ASCII},
    {"x-cp1250", BDFI_XML_ENC_ASCII},
    {"x-cp1251", BDFI_XML_ENC_ASCII},
    {"x-cp1252", BDFI_XML_ENC_WINDOWS1252},
    {"x-cp1253", BDFI_XML_ENC_ASCII},
    {"x-cp1254", BDFI_XML_ENC_ASCII},
    {"x-cp1255", BDFI_XML_ENC_ASCII},
    {"x-cp1256", BDFI_XML_ENC_ASCII},
    {"x-cp1257", BDFI_XML_ENC_ASCII},
    {"x-cp1258", BDFI_XML_ENC_ASCII},
    {"x-euc-jp", BDFI_XML_ENC_ASCII},
    {"x-gbk", BDFI_XML_ENC_ASCII},
    {"x-mac-cyrillic", BDFI_XML_ENC_ASCII},
    {"x-mac-roman", BDFI_XML_ENC_ASCII},
    {"x-mac-ukrainian", BDFI_XML_ENC_ASCII},
    {"x-sjis", BDFI_XML_ENC_ASCII},
    {"x-unicode20utf8", BDFI_XML_ENC_UTF8},
    {"x-user-defined", BDFI_XML_ENC_ASCII},
    {"x-x-big5", BDFI_XML_ENC_ASCII},
    /* END GENERATED: labels */
    /* clang-format on */
};

/* The characters of the bytes 0x80 to 0x9F in windows-1252, as
 * charmap.Windows1252 decodes them; from 0xA0 a byte is its character. The
 * table is written by gen/main.go. */
static const uint16_t bdfi_xml_cp1252[32] = {
    /* clang-format off */
    /* BEGIN GENERATED: cp1252 */
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
    /* END GENERATED: cp1252 */
    /* clang-format on */
};

/* htmlindex.Get: the label without the white space around it, in lower
 * case, is looked up. */
static int bdfi_xml_label_kind(bdfi_svg_str label) {
  char word[20];
  size_t n = bdfi_svg_lower_ascii(bdfi_svg_trim_space(label), word, sizeof word - 1);
  if (n == SIZE_MAX) {
    return BDFI_XML_ENC_UNKNOWN;
  }
  size_t lo = 0, hi = sizeof bdfi_xml_labels / sizeof bdfi_xml_labels[0];
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    const bdfi_xml_label *l = &bdfi_xml_labels[m];
    size_t ln = strlen(l->label);
    int c = memcmp(word, l->label, n < ln ? n : ln);
    if (c == 0 && n == ln) {
      return l->kind;
    }
    if (c < 0 || (c == 0 && n < ln)) {
      hi = m;
    } else {
      lo = m + 1;
    }
  }
  return BDFI_XML_ENC_UNKNOWN;
}

/* utf8.Valid. */
static int bdfi_xml_utf8_valid(const uint8_t *b, size_t n) {
  size_t i = 0, len;
  while (i < n) {
    if (b[i] < 0x80) {
      i++;
      continue;
    }
    if (bdfi_svg_rune((const char *)b, n, i, &len) == 0xFFFD && len == 1) {
      return 0;
    }
    i += len;
  }
  return 1;
}

/* bytes.ToValidUTF8 with U+FFFD: every run of bytes that are no UTF-8 is
 * one U+FFFD. */
static int bdfi_xml_to_valid_utf8(const uint8_t *b, size_t n, bdfi_xml_buf *out) {
  size_t i = 0, len;
  int invalid = 0; /* the previous byte was from an invalid sequence */
  if (bdfi_xml_buf_reserve(out, n) != 0) {
    return -1;
  }
  while (i < n) {
    if (b[i] < 0x80) {
      size_t j = i + 1;
      while (j < n && b[j] < 0x80) {
        j++;
      }
      if (bdfi_xml_buf_append(out, b + i, j - i) != 0) {
        return -1;
      }
      i = j;
      invalid = 0;
      continue;
    }
    (void)bdfi_svg_rune((const char *)b, n, i, &len);
    if (len == 1) {
      i++;
      if (!invalid) {
        invalid = 1;
        if (bdfi_xml_buf_rune(out, 0xFFFD) != 0) {
          return -1;
        }
      }
      continue;
    }
    invalid = 0;
    if (bdfi_xml_buf_append(out, b + i, len) != 0) {
      return -1;
    }
    i += len;
  }
  return 0;
}

/* The decoder of unicode.UTF8 (utf8Decoder.Transform in
 * encoding/unicode/unicode.go) on a whole document: what is no UTF-8 is
 * U+FFFD by its maximal subpart, the way of the W3C, not one for a run. */
static int bdfi_xml_decode_utf8(const uint8_t *src, size_t n, bdfi_xml_buf *out) {
  size_t i = 0, copied = 0;
  if (bdfi_xml_buf_reserve(out, n) != 0) {
    return -1;
  }
  while (i < n) {
    uint8_t c = src[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    size_t size;
    uint8_t lo = 0x80, hi = 0xBF; /* utf8internal.AcceptRanges */
    int bad = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      size = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
      size = 3;
      if (c == 0xE0) {
        lo = 0xA0;
      } else if (c == 0xED) {
        hi = 0x9F;
      }
    } else if (c >= 0xF0 && c <= 0xF4) {
      size = 4;
      if (c == 0xF0) {
        lo = 0x90;
      } else if (c == 0xF4) {
        hi = 0x8F;
      }
    } else {
      size = 1; /* no byte a character starts with */
      bad = 1;
    }
    if (!bad && i + size > n) {
      /* the maximal subpart of a sequence the document ends in */
      if (i + 1 >= n || src[i + 1] < lo || src[i + 1] > hi) {
        size = 1;
      } else if (i + 2 >= n || src[i + 2] < 0x80 || src[i + 2] > 0xBF) {
        size = 2;
      } else {
        size = 3;
      }
      bad = 1;
    }
    if (!bad) {
      if (src[i + 1] < lo || src[i + 1] > hi) {
        size = 1;
        bad = 1;
      } else if (size > 2 && (src[i + 2] < 0x80 || src[i + 2] > 0xBF)) {
        size = 2;
        bad = 1;
      } else if (size > 3 && (src[i + 3] < 0x80 || src[i + 3] > 0xBF)) {
        size = 3;
        bad = 1;
      }
    }
    if (bad) {
      if (bdfi_xml_buf_append(out, src + copied, i - copied) != 0 ||
          bdfi_xml_buf_rune(out, 0xFFFD) != 0) {
        return -1;
      }
      i += size;
      copied = i;
    } else {
      i += size;
    }
  }
  return bdfi_xml_buf_append(out, src + copied, n - copied);
}

/* The decoder of unicode.UTF16(e, unicode.IgnoreBOM) (utf16Decoder.Transform)
 * on a whole document. A surrogate that is not the first of a pair is
 * U+FFFD, and so are two second ones in a row, together; so is a byte left
 * over at the end. */
static int bdfi_xml_decode_utf16(const uint8_t *src, size_t n, int big_endian, bdfi_xml_buf *out) {
  size_t i = 0;
  if (bdfi_xml_buf_reserve(out, n + n / 2 + 3) != 0) {
    return -1;
  }
  while (i < n) {
    uint32_t r;
    size_t size;
    if (i + 1 < n) {
      r = big_endian ? ((uint32_t)src[i] << 8) | src[i + 1] : ((uint32_t)src[i + 1] << 8) | src[i];
      size = 2;
      if (r >= 0xD800 && r <= 0xDFFF) {
        if (i + 3 < n) {
          uint32_t x = big_endian ? ((uint32_t)src[i + 2] << 8) | src[i + 3]
                                  : ((uint32_t)src[i + 3] << 8) | src[i + 2];
          if (x >= 0xDC00 && x <= 0xDFFF) {
            /* utf16.DecodeRune: a pair, or U+FFFD for the two */
            r = r < 0xDC00 ? (((r - 0xD800) << 10) | (x - 0xDC00)) + 0x10000 : 0xFFFD;
            size = 4;
          }
        }
        /* a surrogate that is left has no UTF-8: bdfi_svg_put_rune makes
         * it U+FFFD, as utf8.RuneLen says */
      }
    } else {
      r = 0xFFFD; /* a single trailing byte */
      size = 1;
    }
    if (bdfi_xml_buf_rune(out, r) != 0) {
      return -1;
    }
    i += size;
  }
  return 0;
}

/* The decoder of charmap.Windows1252. */
static int bdfi_xml_decode_cp1252(const uint8_t *src, size_t n, bdfi_xml_buf *out) {
  if (bdfi_xml_buf_reserve(out, n) != 0) {
    return -1;
  }
  for (size_t i = 0; i < n; i++) {
    uint8_t c = src[i];
    int err = c < 0x80   ? bdfi_xml_buf_byte(out, c)
              : c < 0xA0 ? bdfi_xml_buf_rune(out, bdfi_xml_cp1252[c - 0x80])
                         : bdfi_xml_buf_rune(out, c);
    if (err != 0) {
      return -1;
    }
  }
  return 0;
}

/* xmltree.UTF8 for a document with a UTF-16 byte order mark: utf16.Decode
 * of its units, as UTF-8. A byte left over at the end is dropped, and a
 * surrogate without its pair is U+FFFD. */
static int bdfi_xml_utf16_document(const uint8_t *b, size_t n, bdfi_xml_buf *out) {
  int big_endian = b[0] == 0xFE;
  if (bdfi_xml_buf_reserve(out, n + n / 2) != 0) {
    return -1;
  }
  for (size_t i = 2; i + 1 < n; i += 2) {
    uint32_t r = big_endian ? ((uint32_t)b[i] << 8) | b[i + 1] : ((uint32_t)b[i + 1] << 8) | b[i];
    if (r >= 0xD800 && r < 0xDC00 && i + 3 < n) {
      uint32_t x =
          big_endian ? ((uint32_t)b[i + 2] << 8) | b[i + 3] : ((uint32_t)b[i + 3] << 8) | b[i + 2];
      if (x >= 0xDC00 && x <= 0xDFFF) {
        r = (((r - 0xD800) << 10) | (x - 0xDC00)) + 0x10000;
        i += 2;
      }
    }
    if (bdfi_xml_buf_rune(out, r) != 0) {
      return -1;
    }
  }
  return 0;
}

/* ---- What xmltree.Open does to a document first (open.go) ----------------- */

/* DeclaredEncoding(b) is "" or "utf-8": the encoding of the XML declaration
 * a document starts with, as open.go finds it (not as the reader does). */
static int bdfi_xml_declares_utf8(const uint8_t *b, size_t n) {
  if (n > 1024) {
    n = 1024;
  }
  while (n > 0 && bdfi_xml_is_space(b[0])) {
    b++;
    n--;
  }
  if (n > 256) {
    n = 256;
  }
  if (n < 5 || memcmp(b, "<?xml", 5) != 0) {
    return 1;
  }
  size_t end = bdfi_xml_index(b, 0, n, "?>", 2);
  if (end == BDFI_XML_NONE_AT) {
    return 1;
  }
  size_t i = bdfi_xml_index(b, 0, end, "encoding", 8);
  if (i == BDFI_XML_NONE_AT) {
    return 1;
  }
  i += 8;
  while (i < end && (bdfi_xml_is_space(b[i]) || b[i] == '=')) {
    i++;
  }
  if (i >= end || (b[i] != '"' && b[i] != '\'')) {
    return 1;
  }
  size_t close = bdfi_xml_index_byte(b, i + 1, end, b[i]);
  if (close == BDFI_XML_NONE_AT) {
    close = end;
  }
  bdfi_svg_str v = bdfi_svg_trim_space(bdfi_svg_str_of((const char *)b + i + 1, close - i - 1));
  char word[5];
  return v.n == 0 ||
         (bdfi_svg_lower_ascii(v, word, sizeof word) == 5 && memcmp(word, "utf-8", 5) == 0);
}

/* An entity "<!ENTITY" declares, with the size of its value as written. */
typedef struct bdfi_xml_entity_size {
  bdfi_svg_str name;
  size_t size;
} bdfi_xml_entity_size;

static int bdfi_xml_entity_size_cmp(const void *a, const void *b) {
  return bdfi_svg_str_cmp(((const bdfi_xml_entity_size *)a)->name,
                          ((const bdfi_xml_entity_size *)b)->name);
}

/* isEntityNameByte in entity.go, and the names of declaredEntity. */
static int bdfi_xml_entity_name_byte(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.' || c == ':' || c >= 0x80;
}

/*
 * BoundEntities in open.go: reports in *dropped whether the entities of a
 * document stand for more than BDFI_XML_MAX_ENTITY_TEXT, all their
 * references together, by the count of the reference: every "<!ENTITY"
 * counts, wherever it is, with the largest value of its name, and every
 * "&name;". It returns 0, or -1 when out of memory.
 */
static int bdfi_xml_bound_entities(const uint8_t *data, size_t n, int *dropped) {
  bdfi_xml_entity_size *sizes = NULL;
  size_t count = 0, cap = 0, longest = 0;
  /* the next quote of each kind, so that declarations without an end do
   * not each look through the rest of the document */
  size_t quote_from[2] = {0, 0}, quote_at[2] = {0, 0};
  int quote_known[2] = {0, 0};
  *dropped = 0;
  for (size_t at = bdfi_xml_index(data, 0, n, "<!ENTITY", 8); at != BDFI_XML_NONE_AT;
       at = bdfi_xml_index(data, at + 8, n, "<!ENTITY", 8)) {
    /* declaredEntity: white space, a name, white space, a quoted value */
    size_t i = at + 8;
    if (i >= n || !bdfi_xml_is_space(data[i])) {
      continue;
    }
    while (i < n && bdfi_xml_is_space(data[i])) {
      i++;
    }
    size_t start = i;
    while (i < n && bdfi_xml_entity_name_byte(data[i])) {
      i++;
    }
    size_t name_len = i - start;
    while (i < n && bdfi_xml_is_space(data[i])) {
      i++;
    }
    if (name_len == 0 || i >= n || (data[i] != '"' && data[i] != '\'')) {
      continue;
    }
    int q = data[i] == '"';
    if (!quote_known[q] || i + 1 < quote_from[q] || i + 1 > quote_at[q]) {
      quote_from[q] = i + 1;
      quote_at[q] = bdfi_xml_index_byte(data, i + 1, n, data[i]);
      quote_known[q] = 1;
    }
    if (quote_at[q] == BDFI_XML_NONE_AT) {
      continue;
    }
    if (count == cap) {
      size_t ncap = cap ? cap * 2 : 16;
      bdfi_xml_entity_size *p = ncap > cap ? bdfi_realloc_array(sizes, ncap, sizeof *sizes) : NULL;
      if (!p) {
        free(sizes);
        return -1;
      }
      sizes = p;
      cap = ncap;
    }
    sizes[count].name = bdfi_svg_str_of((const char *)data + start, name_len);
    sizes[count].size = quote_at[q] - (i + 1);
    count++;
    if (name_len > longest) {
      longest = name_len;
    }
  }
  if (count == 0) {
    free(sizes);
    return 0;
  }
  /* sizes[name] = max(sizes[name], size) */
  if (bdfi_svg_sort(sizes, count, sizeof *sizes, bdfi_xml_entity_size_cmp) != 0) {
    free(sizes);
    return -1;
  }
  size_t names = 0;
  for (size_t i = 0; i < count; i++) {
    if (names > 0 && bdfi_svg_str_cmp(sizes[names - 1].name, sizes[i].name) == 0) {
      if (sizes[i].size > sizes[names - 1].size) {
        sizes[names - 1].size = sizes[i].size;
      }
    } else {
      sizes[names++] = sizes[i];
    }
  }
  uint64_t total = 0;
  size_t semi = 0; /* the next ';' at or after what was looked at */
  int semi_known = 0;
  for (size_t amp = bdfi_xml_index_byte(data, 0, n, '&'); amp != BDFI_XML_NONE_AT;
       amp = bdfi_xml_index_byte(data, amp + 1, n, '&')) {
    size_t rest = amp + 1;
    if (!semi_known || (semi != BDFI_XML_NONE_AT && semi < rest)) {
      semi = bdfi_xml_index_byte(data, rest, n, ';');
      semi_known = 1;
    }
    if (semi == BDFI_XML_NONE_AT) {
      break; /* no reference ends after here */
    }
    /* j := bytes.IndexByte(rest[:min(len(rest), longest+1)], ';'); j > 0 */
    size_t j = semi - rest;
    if (j == 0 || j > longest) {
      continue;
    }
    bdfi_svg_str name = bdfi_svg_str_of((const char *)data + rest, j);
    size_t lo = 0, hi = names;
    while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      int c = bdfi_svg_str_cmp(sizes[m].name, name);
      if (c == 0) {
        total += sizes[m].size;
        break;
      }
      if (c < 0) {
        lo = m + 1;
      } else {
        hi = m;
      }
    }
    if (total > BDFI_XML_MAX_ENTITY_TEXT) {
      *dropped = 1;
      break;
    }
  }
  free(sizes);
  return 0;
}

/* The declarations of a document whose entities stand for too much are
 * unmade: a declaration in lower case is one the reader passes over. */
static void bdfi_xml_drop_entities(uint8_t *data, size_t n) {
  for (size_t at = bdfi_xml_index(data, 0, n, "<!ENTITY", 8); at != BDFI_XML_NONE_AT;
       at = bdfi_xml_index(data, at + 8, n, "<!ENTITY", 8)) {
    memcpy(data + at, "<!entity", 8);
  }
}

/* undeclare in open.go: the XML declarations in the rest of a document that
 * was converted become processing instructions of another name. Comments
 * and CDATA sections are passed over. */
static void bdfi_xml_undeclare(uint8_t *b, size_t n) {
  if (bdfi_xml_index(b, 0, n, "<?xml", 5) == BDFI_XML_NONE_AT) {
    return;
  }
  int no_comment_end = 0, no_cdata_end = 0;
  size_t i = 0;
  for (;;) {
    i = bdfi_xml_index_byte(b, i, n, '<');
    if (i == BDFI_XML_NONE_AT) {
      return;
    }
    size_t left = n - i;
    if (left >= 4 && memcmp(b + i, "<!--", 4) == 0) {
      size_t end = no_comment_end ? BDFI_XML_NONE_AT : bdfi_xml_index(b, i + 4, n, "-->", 3);
      no_comment_end = end == BDFI_XML_NONE_AT;
      i = no_comment_end ? i + 4 : end + 3;
    } else if (left >= 9 && memcmp(b + i, "<![CDATA[", 9) == 0) {
      size_t end = no_cdata_end ? BDFI_XML_NONE_AT : bdfi_xml_index(b, i + 9, n, "]]>", 3);
      no_cdata_end = end == BDFI_XML_NONE_AT;
      i = no_cdata_end ? i + 9 : end + 3;
    } else if (left > 5 && memcmp(b + i, "<?xml", 5) == 0 && bdfi_xml_is_space(b[i + 5])) {
      b[i + 4] = 'L';
      i += 5;
    } else {
      i++;
    }
  }
}

/* Takes the bytes of a buffer for the reader's document. */
static void bdfi_xml_adopt(bdfi_xml_reader *r, bdfi_xml_buf *b) {
  free(r->owned);
  r->owned = b->p;
  r->buf = b->p;
  r->r = 0;
  r->w = b->n;
  b->p = NULL;
  b->n = b->cap = 0;
}

/* Bounds the entities of the reader's document, in a copy of it when it is
 * the caller's. */
static int bdfi_xml_bound(bdfi_xml_reader *r) {
  int dropped;
  if (bdfi_xml_bound_entities(r->buf + r->r, r->w - r->r, &dropped) != 0) {
    return -1;
  }
  if (!dropped) {
    return 0;
  }
  if (!r->owned) {
    bdfi_xml_buf copy = {NULL, 0, 0};
    if (bdfi_xml_buf_reserve(&copy, r->w - r->r) != 0 ||
        bdfi_xml_buf_append(&copy, r->buf + r->r, r->w - r->r) != 0) {
      bdfi_xml_buf_free(&copy);
      return -1;
    }
    bdfi_xml_adopt(r, &copy);
  }
  bdfi_xml_drop_entities(r->owned + (r->buf - r->owned) + r->r, r->w - r->r);
  return 0;
}

/*
 * xmltree.Open. A document with a UTF-16 byte order mark is decoded, and a
 * UTF-8 one dropped; a document that is not UTF-8 and declares no other
 * encoding is made valid; its entities are bounded.
 *
 * Options.MaxBufferBytes, four times the document and a megabyte, bounds
 * the text a token's entities are replaced into. It also bounds the buffer
 * the reference reads a converted document through, which is why that
 * buffer needs no port: what an encoding read here converts to is less than
 * four times the document.
 */
int bdfi_xml_open(bdfi_xml_reader *r, const uint8_t *data, size_t len, size_t max_depth) {
  memset(r, 0, sizeof *r);
  bdfi_arena_init(&r->ent_arena);
  r->max_depth = max_depth;
  r->buf = len > 0 ? data : (const uint8_t *)"";
  r->w = len;
  r->marked =
      len >= 2 && ((data[0] == 0xFE && data[1] == 0xFF) || (data[0] == 0xFF && data[1] == 0xFE));
  bdfi_xml_buf conv = {NULL, 0, 0};
  if (r->marked) {
    if (bdfi_xml_utf16_document(data, len, &conv) != 0) {
      bdfi_xml_buf_free(&conv);
      return BDFI_XML_NOMEM;
    }
    bdfi_xml_adopt(r, &conv);
  } else if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    r->r = 3;
  }
  /* a document in another encoding is converted first */
  if (!bdfi_xml_utf8_valid(r->buf + r->r, r->w - r->r) &&
      bdfi_xml_declares_utf8(r->buf + r->r, r->w - r->r)) {
    if (bdfi_xml_to_valid_utf8(r->buf + r->r, r->w - r->r, &conv) != 0) {
      bdfi_xml_buf_free(&conv);
      return BDFI_XML_NOMEM;
    }
    bdfi_xml_adopt(r, &conv);
  }
  if (bdfi_xml_bound(r) != 0) {
    return BDFI_XML_NOMEM;
  }
  /* the reader starts at the document: what was dropped is not in it */
  r->buf += r->r;
  r->w -= r->r;
  r->r = 0;
  uint64_t max = 4 * (uint64_t)r->w + BDFI_XML_MAX_ENTITY_TEXT;
  r->max_buffer = max > INT32_MAX ? (size_t)INT32_MAX : (size_t)max;
  return BDFI_XML_OK;
}

void bdfi_xml_close(bdfi_xml_reader *r) {
  free(r->owned);
  free(r->stack);
  free(r->attrs);
  free(r->ents);
  bdfi_xml_buf_free(&r->alt);
  bdfi_xml_buf_free(&r->label);
  bdfi_arena_free(&r->ent_arena);
  memset(r, 0, sizeof *r);
}

/* ---- References (value.go, entity.go, htmlentity) ------------------------- */

typedef struct bdfi_xml_html_entity {
  char name[9];
  char value[4];
} bdfi_xml_html_entity;

/* The named character references of HTML 4 (htmlentity.Lookup, which is
 * encoding/xml.HTMLEntity), by name. The table is written by gen/main.go. */
static const bdfi_xml_html_entity bdfi_xml_html_entities[] = {
    /* clang-format off */
    /* BEGIN GENERATED: entities */
    {"AElig", "\xc3\x86"},
    {"Aacute", "\xc3\x81"},
    {"Acirc", "\xc3\x82"},
    {"Agrave", "\xc3\x80"},
    {"Alpha", "\xce\x91"},
    {"Aring", "\xc3\x85"},
    {"Atilde", "\xc3\x83"},
    {"Auml", "\xc3\x84"},
    {"Beta", "\xce\x92"},
    {"Ccedil", "\xc3\x87"},
    {"Chi", "\xce\xa7"},
    {"Dagger", "\xe2\x80\xa1"},
    {"Delta", "\xce\x94"},
    {"ETH", "\xc3\x90"},
    {"Eacute", "\xc3\x89"},
    {"Ecirc", "\xc3\x8a"},
    {"Egrave", "\xc3\x88"},
    {"Epsilon", "\xce\x95"},
    {"Eta", "\xce\x97"},
    {"Euml", "\xc3\x8b"},
    {"Gamma", "\xce\x93"},
    {"Iacute", "\xc3\x8d"},
    {"Icirc", "\xc3\x8e"},
    {"Igrave", "\xc3\x8c"},
    {"Iota", "\xce\x99"},
    {"Iuml", "\xc3\x8f"},
    {"Kappa", "\xce\x9a"},
    {"Lambda", "\xce\x9b"},
    {"Mu", "\xce\x9c"},
    {"Ntilde", "\xc3\x91"},
    {"Nu", "\xce\x9d"},
    {"OElig", "\xc5\x92"},
    {"Oacute", "\xc3\x93"},
    {"Ocirc", "\xc3\x94"},
    {"Ograve", "\xc3\x92"},
    {"Omega", "\xce\xa9"},
    {"Omicron", "\xce\x9f"},
    {"Oslash", "\xc3\x98"},
    {"Otilde", "\xc3\x95"},
    {"Ouml", "\xc3\x96"},
    {"Phi", "\xce\xa6"},
    {"Pi", "\xce\xa0"},
    {"Prime", "\xe2\x80\xb3"},
    {"Psi", "\xce\xa8"},
    {"Rho", "\xce\xa1"},
    {"Scaron", "\xc5\xa0"},
    {"Sigma", "\xce\xa3"},
    {"THORN", "\xc3\x9e"},
    {"Tau", "\xce\xa4"},
    {"Theta", "\xce\x98"},
    {"Uacute", "\xc3\x9a"},
    {"Ucirc", "\xc3\x9b"},
    {"Ugrave", "\xc3\x99"},
    {"Upsilon", "\xce\xa5"},
    {"Uuml", "\xc3\x9c"},
    {"Xi", "\xce\x9e"},
    {"Yacute", "\xc3\x9d"},
    {"Yuml", "\xc5\xb8"},
    {"Zeta", "\xce\x96"},
    {"aacute", "\xc3\xa1"},
    {"acirc", "\xc3\xa2"},
    {"acute", "\xc2\xb4"},
    {"aelig", "\xc3\xa6"},
    {"agrave", "\xc3\xa0"},
    {"alefsym", "\xe2\x84\xb5"},
    {"alpha", "\xce\xb1"},
    {"amp", "&"},
    {"and", "\xe2\x88\xa7"},
    {"ang", "\xe2\x88\xa0"},
    {"aring", "\xc3\xa5"},
    {"asymp", "\xe2\x89\x88"},
    {"atilde", "\xc3\xa3"},
    {"auml", "\xc3\xa4"},
    {"bdquo", "\xe2\x80\x9e"},
    {"beta", "\xce\xb2"},
    {"brvbar", "\xc2\xa6"},
    {"bull", "\xe2\x80\xa2"},
    {"cap", "\xe2\x88\xa9"},
    {"ccedil", "\xc3\xa7"},
    {"cedil", "\xc2\xb8"},
    {"cent", "\xc2\xa2"},
    {"chi", "\xcf\x87"},
    {"circ", "\xcb\x86"},
    {"clubs", "\xe2\x99\xa3"},
    {"cong", "\xe2\x89\x85"},
    {"copy", "\xc2\xa9"},
    {"crarr", "\xe2\x86\xb5"},
    {"cup", "\xe2\x88\xaa"},
    {"curren", "\xc2\xa4"},
    {"dArr", "\xe2\x87\x93"},
    {"dagger", "\xe2\x80\xa0"},
    {"darr", "\xe2\x86\x93"},
    {"deg", "\xc2\xb0"},
    {"delta", "\xce\xb4"},
    {"diams", "\xe2\x99\xa6"},
    {"divide", "\xc3\xb7"},
    {"eacute", "\xc3\xa9"},
    {"ecirc", "\xc3\xaa"},
    {"egrave", "\xc3\xa8"},
    {"empty", "\xe2\x88\x85"},
    {"emsp", "\xe2\x80\x83"},
    {"ensp", "\xe2\x80\x82"},
    {"epsilon", "\xce\xb5"},
    {"equiv", "\xe2\x89\xa1"},
    {"eta", "\xce\xb7"},
    {"eth", "\xc3\xb0"},
    {"euml", "\xc3\xab"},
    {"euro", "\xe2\x82\xac"},
    {"exist", "\xe2\x88\x83"},
    {"fnof", "\xc6\x92"},
    {"forall", "\xe2\x88\x80"},
    {"frac12", "\xc2\xbd"},
    {"frac14", "\xc2\xbc"},
    {"frac34", "\xc2\xbe"},
    {"frasl", "\xe2\x81\x84"},
    {"gamma", "\xce\xb3"},
    {"ge", "\xe2\x89\xa5"},
    {"gt", ">"},
    {"hArr", "\xe2\x87\x94"},
    {"harr", "\xe2\x86\x94"},
    {"hearts", "\xe2\x99\xa5"},
    {"hellip", "\xe2\x80\xa6"},
    {"iacute", "\xc3\xad"},
    {"icirc", "\xc3\xae"},
    {"iexcl", "\xc2\xa1"},
    {"igrave", "\xc3\xac"},
    {"image", "\xe2\x84\x91"},
    {"infin", "\xe2\x88\x9e"},
    {"int", "\xe2\x88\xab"},
    {"iota", "\xce\xb9"},
    {"iquest", "\xc2\xbf"},
    {"isin", "\xe2\x88\x88"},
    {"iuml", "\xc3\xaf"},
    {"kappa", "\xce\xba"},
    {"lArr", "\xe2\x87\x90"},
    {"lambda", "\xce\xbb"},
    {"lang", "\xe2\x8c\xa9"},
    {"laquo", "\xc2\xab"},
    {"larr", "\xe2\x86\x90"},
    {"lceil", "\xe2\x8c\x88"},
    {"ldquo", "\xe2\x80\x9c"},
    {"le", "\xe2\x89\xa4"},
    {"lfloor", "\xe2\x8c\x8a"},
    {"lowast", "\xe2\x88\x97"},
    {"loz", "\xe2\x97\x8a"},
    {"lrm", "\xe2\x80\x8e"},
    {"lsaquo", "\xe2\x80\xb9"},
    {"lsquo", "\xe2\x80\x98"},
    {"lt", "<"},
    {"macr", "\xc2\xaf"},
    {"mdash", "\xe2\x80\x94"},
    {"micro", "\xc2\xb5"},
    {"middot", "\xc2\xb7"},
    {"minus", "\xe2\x88\x92"},
    {"mu", "\xce\xbc"},
    {"nabla", "\xe2\x88\x87"},
    {"nbsp", "\xc2\xa0"},
    {"ndash", "\xe2\x80\x93"},
    {"ne", "\xe2\x89\xa0"},
    {"ni", "\xe2\x88\x8b"},
    {"not", "\xc2\xac"},
    {"notin", "\xe2\x88\x89"},
    {"nsub", "\xe2\x8a\x84"},
    {"ntilde", "\xc3\xb1"},
    {"nu", "\xce\xbd"},
    {"oacute", "\xc3\xb3"},
    {"ocirc", "\xc3\xb4"},
    {"oelig", "\xc5\x93"},
    {"ograve", "\xc3\xb2"},
    {"oline", "\xe2\x80\xbe"},
    {"omega", "\xcf\x89"},
    {"omicron", "\xce\xbf"},
    {"oplus", "\xe2\x8a\x95"},
    {"or", "\xe2\x88\xa8"},
    {"ordf", "\xc2\xaa"},
    {"ordm", "\xc2\xba"},
    {"oslash", "\xc3\xb8"},
    {"otilde", "\xc3\xb5"},
    {"otimes", "\xe2\x8a\x97"},
    {"ouml", "\xc3\xb6"},
    {"para", "\xc2\xb6"},
    {"part", "\xe2\x88\x82"},
    {"permil", "\xe2\x80\xb0"},
    {"perp", "\xe2\x8a\xa5"},
    {"phi", "\xcf\x86"},
    {"pi", "\xcf\x80"},
    {"piv", "\xcf\x96"},
    {"plusmn", "\xc2\xb1"},
    {"pound", "\xc2\xa3"},
    {"prime", "\xe2\x80\xb2"},
    {"prod", "\xe2\x88\x8f"},
    {"prop", "\xe2\x88\x9d"},
    {"psi", "\xcf\x88"},
    {"quot", "\x22"},
    {"rArr", "\xe2\x87\x92"},
    {"radic", "\xe2\x88\x9a"},
    {"rang", "\xe2\x8c\xaa"},
    {"raquo", "\xc2\xbb"},
    {"rarr", "\xe2\x86\x92"},
    {"rceil", "\xe2\x8c\x89"},
    {"rdquo", "\xe2\x80\x9d"},
    {"real", "\xe2\x84\x9c"},
    {"reg", "\xc2\xae"},
    {"rfloor", "\xe2\x8c\x8b"},
    {"rho", "\xcf\x81"},
    {"rlm", "\xe2\x80\x8f"},
    {"rsaquo", "\xe2\x80\xba"},
    {"rsquo", "\xe2\x80\x99"},
    {"sbquo", "\xe2\x80\x9a"},
    {"scaron", "\xc5\xa1"},
    {"sdot", "\xe2\x8b\x85"},
    {"sect", "\xc2\xa7"},
    {"shy", "\xc2\xad"},
    {"sigma", "\xcf\x83"},
    {"sigmaf", "\xcf\x82"},
    {"sim", "\xe2\x88\xbc"},
    {"spades", "\xe2\x99\xa0"},
    {"sub", "\xe2\x8a\x82"},
    {"sube", "\xe2\x8a\x86"},
    {"sum", "\xe2\x88\x91"},
    {"sup", "\xe2\x8a\x83"},
    {"sup1", "\xc2\xb9"},
    {"sup2", "\xc2\xb2"},
    {"sup3", "\xc2\xb3"},
    {"supe", "\xe2\x8a\x87"},
    {"szlig", "\xc3\x9f"},
    {"tau", "\xcf\x84"},
    {"there4", "\xe2\x88\xb4"},
    {"theta", "\xce\xb8"},
    {"thetasym", "\xcf\x91"},
    {"thinsp", "\xe2\x80\x89"},
    {"thorn", "\xc3\xbe"},
    {"tilde", "\xcb\x9c"},
    {"times", "\xc3\x97"},
    {"trade", "\xe2\x84\xa2"},
    {"uArr", "\xe2\x87\x91"},
    {"uacute", "\xc3\xba"},
    {"uarr", "\xe2\x86\x91"},
    {"ucirc", "\xc3\xbb"},
    {"ugrave", "\xc3\xb9"},
    {"uml", "\xc2\xa8"},
    {"upsih", "\xcf\x92"},
    {"upsilon", "\xcf\x85"},
    {"uuml", "\xc3\xbc"},
    {"weierp", "\xe2\x84\x98"},
    {"xi", "\xce\xbe"},
    {"yacute", "\xc3\xbd"},
    {"yen", "\xc2\xa5"},
    {"yuml", "\xc3\xbf"},
    {"zeta", "\xce\xb6"},
    {"zwj", "\xe2\x80\x8d"},
    {"zwnj", "\xe2\x80\x8c"},
    /* END GENERATED: entities */
    /* clang-format on */
};

/* strconv.ParseUint(s, base, 32) of a character reference: 1 and the
 * number, or 0 for what is none or does not fit in 32 bits. */
static int bdfi_xml_parse_uint(const uint8_t *s, size_t n, uint32_t base, uint32_t *out) {
  uint64_t v = 0;
  if (n == 0) {
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    uint32_t d;
    uint8_t c = s[i];
    if (c >= '0' && c <= '9') {
      d = (uint32_t)(c - '0');
    } else if (bdfi_xml_fold(c) >= 'a' && bdfi_xml_fold(c) <= 'z') {
      d = (uint32_t)(bdfi_xml_fold(c) - 'a') + 10;
    } else {
      return 0;
    }
    if (d >= base) {
      return 0;
    }
    v = v * base + d;
    if (v > 0xFFFFFFFFu) {
      return 0;
    }
  }
  *out = (uint32_t)v;
  return 1;
}

/* appendReference in value.go: 1 when ref (what is between '&' and ';') is
 * one of the five predefined entities or a character reference, appended;
 * 0 when it is not, and -1 when out of memory. */
static int bdfi_xml_append_reference(bdfi_xml_buf *dst, const uint8_t *ref, size_t n) {
  uint8_t c = 0;
  if (n == 2 && memcmp(ref, "lt", 2) == 0) {
    c = '<';
  } else if (n == 2 && memcmp(ref, "gt", 2) == 0) {
    c = '>';
  } else if (n == 3 && memcmp(ref, "amp", 3) == 0) {
    c = '&';
  } else if (n == 4 && memcmp(ref, "apos", 4) == 0) {
    c = '\'';
  } else if (n == 4 && memcmp(ref, "quot", 4) == 0) {
    c = '"';
  }
  if (c != 0) {
    return bdfi_xml_buf_byte(dst, c) != 0 ? -1 : 1;
  }
  if (n < 2 || ref[0] != '#') {
    return 0;
  }
  /* a hexadecimal reference takes a lower-case x only */
  uint32_t v;
  int ok = ref[1] == 'x' ? bdfi_xml_parse_uint(ref + 2, n - 2, 16, &v)
                         : bdfi_xml_parse_uint(ref + 1, n - 1, 10, &v);
  if (!ok || v > 0x10FFFF) {
    return 0;
  }
  /* a surrogate becomes U+FFFD */
  return bdfi_xml_buf_rune(dst, v) != 0 ? -1 : 1;
}

/* Unescape in value.go. The Go code looks for the next '&', '\r' and ';'
 * from every reference; here the last answers are kept. */
int bdfi_xml_unescape(bdfi_xml_buf *dst, bdfi_svg_str str) {
  const uint8_t *s = (const uint8_t *)str.p;
  size_t n = str.n, pos = 0;
  size_t amp = bdfi_xml_index_byte(s, 0, n, '&');
  size_t cr = bdfi_xml_index_byte(s, 0, n, '\r');
  size_t semi = 0;
  int semi_known = 0;
  for (;;) {
    if (amp != BDFI_XML_NONE_AT && amp < pos) {
      amp = bdfi_xml_index_byte(s, pos, n, '&');
    }
    if (cr != BDFI_XML_NONE_AT && cr < pos) {
      cr = bdfi_xml_index_byte(s, pos, n, '\r');
    }
    if (amp == BDFI_XML_NONE_AT && cr == BDFI_XML_NONE_AT) {
      return bdfi_xml_buf_append(dst, s + pos, n - pos);
    }
    if (amp == BDFI_XML_NONE_AT || (cr != BDFI_XML_NONE_AT && cr < amp)) {
      /* a line end: "\r\n" and a lone "\r" are "\n" */
      if (bdfi_xml_buf_append(dst, s + pos, cr - pos) != 0 || bdfi_xml_buf_byte(dst, '\n') != 0) {
        return -1;
      }
      pos = cr + 1;
      if (pos < n && s[pos] == '\n') {
        pos++;
      }
      continue;
    }
    if (bdfi_xml_buf_append(dst, s + pos, amp - pos) != 0) {
      return -1;
    }
    pos = amp;
    if (!semi_known || (semi != BDFI_XML_NONE_AT && semi < pos)) {
      semi = bdfi_xml_index_byte(s, pos, n, ';');
      semi_known = 1;
    }
    int ok = 0;
    if (semi != BDFI_XML_NONE_AT && semi - pos <= BDFI_XML_MAX_ENTITY_NAME) {
      ok = bdfi_xml_append_reference(dst, s + pos + 1, semi - pos - 1);
      if (ok < 0) {
        return -1;
      }
    }
    if (ok) {
      pos = semi + 1;
    } else {
      /* not a reference: the '&' stands, and the scan resumes right after
       * it */
      if (bdfi_xml_buf_byte(dst, '&') != 0) {
        return -1;
      }
      pos++;
    }
  }
}

/* Value.AppendTo: the decoded content, which is the bytes themselves
 * without a '&' or a '\r'. */
static int bdfi_xml_append_value(bdfi_xml_buf *dst, bdfi_svg_str v) {
  if (v.n == 0 || (!memchr(v.p, '&', v.n) && !memchr(v.p, '\r', v.n))) {
    return bdfi_xml_buf_append(dst, v.p, v.n);
  }
  return bdfi_xml_unescape(dst, v);
}

/* isPredefined in entity.go. */
static int bdfi_xml_is_predefined(const uint8_t *name, size_t n) {
  return (n == 2 && (memcmp(name, "lt", 2) == 0 || memcmp(name, "gt", 2) == 0)) ||
         (n == 3 && memcmp(name, "amp", 3) == 0) ||
         (n == 4 && (memcmp(name, "apos", 4) == 0 || memcmp(name, "quot", 4) == 0));
}

/* Reader.lookupEntity: the DOCTYPE's declarations, the first of which wins,
 * and then htmlentity.Lookup. *declared says which it was. */
static int bdfi_xml_lookup_entity(const bdfi_xml_reader *r, const uint8_t *name, size_t n,
                                  bdfi_svg_str *value, int *declared) {
  for (size_t i = 0; i < r->nents; i++) {
    if (bdfi_svg_str_eq(r->ents[i].name, (const char *)name, n)) {
      *value = r->ents[i].value;
      *declared = 1;
      return 1;
    }
  }
  *declared = 0;
  size_t lo = 0, hi = sizeof bdfi_xml_html_entities / sizeof bdfi_xml_html_entities[0];
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    const bdfi_xml_html_entity *e = &bdfi_xml_html_entities[m];
    size_t en = strlen(e->name);
    int c = memcmp(e->name, name, en < n ? en : n);
    if (c == 0 && en == n) {
      *value = bdfi_svg_str_of(e->value, strlen(e->value));
      return 1;
    }
    if (c < 0 || (c == 0 && en < n)) {
      lo = m + 1;
    } else {
      hi = m;
    }
  }
  return 0;
}

/*
 * Reader.expand in entity.go: appends src to the reader's alt with every
 * reference a table resolves replaced by its value, and reports whether any
 * was. The five predefined references and character references are left for
 * bdfi_xml_unescape, and a '&' or '\r' in a value is written as a
 * reference, so that decoding the result yields the value itself.
 */
static int bdfi_xml_expand(bdfi_xml_reader *r, const uint8_t *src, size_t n, int *changed) {
  bdfi_xml_buf *dst = &r->alt;
  size_t i = 0;
  *changed = 0;
  for (;;) {
    size_t a = bdfi_xml_index_byte(src, i, n, '&');
    if (a == BDFI_XML_NONE_AT) {
      break;
    }
    if (bdfi_xml_buf_append(dst, src + i, a - i) != 0) {
      return BDFI_XML_NOMEM;
    }
    /* the name runs from the '&' to a ';' within the bound */
    size_t j = 1;
    while (a + j < n && j <= BDFI_XML_MAX_ENTITY_NAME && bdfi_xml_entity_name_byte(src[a + j])) {
      j++;
    }
    bdfi_svg_str value;
    int declared;
    if (j == 1 || a + j >= n || src[a + j] != ';' || bdfi_xml_is_predefined(src + a + 1, j - 1)) {
      if (bdfi_xml_buf_byte(dst, '&') != 0) {
        return BDFI_XML_NOMEM;
      }
      i = a + 1;
      continue;
    }
    if (bdfi_xml_lookup_entity(r, src + a + 1, j - 1, &value, &declared)) {
      /* appendEscaped */
      size_t from = 0;
      for (size_t k = 0; k < value.n; k++) {
        if (value.p[k] != '&' && value.p[k] != '\r') {
          continue;
        }
        if (bdfi_xml_buf_append(dst, value.p + from, k - from) != 0 ||
            bdfi_xml_buf_append(dst, value.p[k] == '&' ? "&amp;" : "&#13;", 5) != 0) {
          return BDFI_XML_NOMEM;
        }
        from = k + 1;
      }
      if (bdfi_xml_buf_append(dst, value.p + from, value.n - from) != 0) {
        return BDFI_XML_NOMEM;
      }
      *changed = 1;
      if (dst->n > r->max_buffer) {
        return BDFI_XML_STOP; /* ErrTooLarge */
      }
      /* Not in the reference: the megabyte its BoundEntities counts is a
       * bound here whatever the encodings of a document hide from the
       * count. */
      if (declared) {
        r->ent_text += value.n;
        if (r->ent_text > BDFI_XML_MAX_ENTITY_TEXT) {
          return BDFI_XML_ENTITIES;
        }
      }
    } else if (bdfi_xml_buf_append(dst, src + a, j + 1) != 0) {
      return BDFI_XML_NOMEM;
    }
    i = a + j + 1;
  }
  return bdfi_xml_buf_append(dst, src + i, n - i) != 0 ? BDFI_XML_NOMEM : BDFI_XML_OK;
}

/* skipDeclaration in doctype.go: the length of the markup declaration at
 * the start of b, up to and including its '>', with quoted literals passed
 * over; or all of b when it has no end. */
static size_t bdfi_xml_skip_declaration(const uint8_t *b, size_t n) {
  uint8_t quote = 0;
  for (size_t i = 1; i < n; i++) {
    uint8_t c = b[i];
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i + 1;
    }
  }
  return n;
}

/* Reader.declareEntities in doctype.go: reads the general entities an
 * internal subset declares in the internal form, <!ENTITY name "value">.
 * The character references and predefined entities of a value are decoded
 * now; a reference to another entity in it is kept as written. */
static int bdfi_xml_declare_entities(bdfi_xml_reader *r, const uint8_t *sub, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t *rest = sub + i;
    size_t left = n - i;
    if (left >= 4 && memcmp(rest, "<!--", 4) == 0) {
      size_t end = bdfi_xml_index(rest, 4, left, "-->", 3);
      if (end == BDFI_XML_NONE_AT) {
        return 0;
      }
      i += end + 3;
    } else if (left >= 2 && memcmp(rest, "<?", 2) == 0) {
      size_t end = bdfi_xml_index(rest, 2, left, "?>", 2);
      if (end == BDFI_XML_NONE_AT) {
        return 0;
      }
      i += end + 2;
    } else if (left > 8 && memcmp(rest, "<!ENTITY", 8) == 0 && bdfi_xml_is_space(rest[8])) {
      size_t j = 8;
      while (j < left && bdfi_xml_is_space(rest[j])) {
        j++;
      }
      if (j < left && rest[j] != '%') {
        size_t ns = j;
        while (j < left && bdfi_xml_entity_name_byte(rest[j])) {
          j++;
        }
        size_t name_len = j - ns;
        while (j < left && bdfi_xml_is_space(rest[j])) {
          j++;
        }
        if (name_len > 0 && j < left && (rest[j] == '"' || rest[j] == '\'')) {
          size_t end = bdfi_xml_index_byte(rest, j + 1, left, rest[j]);
          /* a reference is looked up by a name of at most
           * BDFI_XML_MAX_ENTITY_NAME bytes: a longer one is never found */
          if (end != BDFI_XML_NONE_AT && name_len <= BDFI_XML_MAX_ENTITY_NAME) {
            bdfi_xml_buf value = {NULL, 0, 0};
            if (r->nents == r->ents_cap) {
              size_t cap = r->ents_cap ? r->ents_cap * 2 : 8;
              bdfi_xml_entity *p =
                  cap > r->ents_cap ? bdfi_realloc_array(r->ents, cap, sizeof *p) : NULL;
              if (!p) {
                return -1;
              }
              r->ents = p;
              r->ents_cap = cap;
            }
            if (bdfi_xml_unescape(&value,
                                  bdfi_svg_str_of((const char *)rest + j + 1, end - j - 1)) != 0) {
              bdfi_xml_buf_free(&value);
              return -1;
            }
            bdfi_xml_entity *e = &r->ents[r->nents];
            e->name.p = bdfi_arena_strdup(&r->ent_arena, (const char *)rest + ns, name_len);
            e->name.n = name_len;
            e->value.p = bdfi_arena_strdup(&r->ent_arena, (const char *)value.p, value.n);
            e->value.n = value.n;
            bdfi_xml_buf_free(&value);
            if (!e->name.p || !e->value.p) {
              return -1;
            }
            r->nents++;
          }
        }
      }
      i += bdfi_xml_skip_declaration(rest, left);
    } else if (rest[0] == '<') {
      i += bdfi_xml_skip_declaration(rest, left);
    } else {
      i++;
    }
  }
  return 0;
}

/* ---- Tokens (reader.go, lenient.go, doctype.go, charset.go) --------------- */

/* nameByte: the bytes a name may contain: everything but white space, the
 * tag delimiters and the attribute punctuation. */
static int bdfi_xml_name_byte(uint8_t c) {
  switch (c) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
  case '<':
  case '>':
  case '/':
  case '=':
  case '"':
  case '\'':
  case '?':
  case '&':
    return 0;
  default:
    return 1;
  }
}

/* unquotedByte: the bytes an unquoted attribute value may contain. */
static int bdfi_xml_unquoted_byte(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == ':' || c == '-';
}

/* Reader.scanName: a name from buf[from], with the FNV-1a hash that end
 * tags are matched by: where it ends, or BDFI_XML_NONE_AT when the document
 * ends in it. */
static size_t bdfi_xml_scan_name(const bdfi_xml_reader *r, size_t from, uint32_t *hash) {
  uint32_t h = 2166136261u;
  size_t i = from;
  for (;;) {
    if (i >= r->w) {
      return BDFI_XML_NONE_AT;
    }
    uint8_t c = r->buf[i];
    if (!bdfi_xml_name_byte(c)) {
      *hash = h;
      return i;
    }
    h = (h ^ c) * 16777619u;
    i++;
  }
}

static int bdfi_xml_has_prefix(const bdfi_xml_reader *r, size_t at, const char *s, size_t len) {
  return r->w - at >= len && memcmp(r->buf + at, s, len) == 0;
}

static int bdfi_xml_add_attr(bdfi_xml_reader *r, size_t name_off, size_t name_len, size_t val_off,
                             size_t val_len) {
  if (r->nattrs == r->attrs_cap) {
    size_t cap = r->attrs_cap ? r->attrs_cap * 2 : 8;
    bdfi_xml_attr *p = cap > r->attrs_cap ? bdfi_realloc_array(r->attrs, cap, sizeof *p) : NULL;
    if (!p) {
      return -1;
    }
    r->attrs = p;
    r->attrs_cap = cap;
  }
  bdfi_xml_attr *a = &r->attrs[r->nattrs++];
  a->name_off = name_off;
  a->name_len = name_len;
  a->val_off = val_off;
  a->val_len = val_len;
  a->in_alt = 0;
  return 0;
}

/* htmlentity.AutoClose: the elements HTML ends at their start tag, whatever
 * their case and their prefix. */
static int bdfi_xml_auto_close(const uint8_t *name, size_t n) {
  static const char *const voids[13] = {"area", "base",  "basefont", "br",   "col",  "frame", "hr",
                                        "img",  "input", "isindex",  "link", "meta", "param"};
  const uint8_t *colon = memchr(name, ':', n);
  if (colon && colon > name && colon < name + n - 1) {
    n -= (size_t)(colon - name) + 1;
    name = colon + 1;
  }
  if (n < 2 || n > 8) {
    return 0;
  }
  for (size_t v = 0; v < 13; v++) {
    if (strlen(voids[v]) != n) {
      continue;
    }
    size_t i = 0;
    while (i < n && bdfi_xml_fold(name[i]) == (uint8_t)voids[v][i]) {
      i++;
    }
    if (i == n) {
      return 1;
    }
  }
  return 0;
}

/* Reader.peekNotEndTag in lenient.go: compares what follows a start tag (at
 * `at`) with "</", the element's own name under ASCII case folding, optional
 * space and '>': 1 at the first byte that differs; 0 when the document ends
 * first or the end tag is complete. */
static int bdfi_xml_peek_not_end_tag(const bdfi_xml_reader *r, size_t at, size_t name_len) {
  for (size_t j = 0;; j++) {
    if (at + j >= r->w) {
      return 0;
    }
    uint8_t c = r->buf[at + j];
    if (j == 0) {
      if (c != '<') {
        return 1;
      }
    } else if (j == 1) {
      if (c != '/') {
        return 1;
      }
    } else if (j < 2 + name_len) {
      if (bdfi_xml_fold(c) != bdfi_xml_fold(r->buf[r->name_off + j - 2])) {
        return 1;
      }
    } else if (c == '>') {
      return 0;
    } else if (!bdfi_xml_is_space(c)) {
      return 1;
    }
  }
}

/* Reader.expandAttrs in entity.go: the values of the start tag just scanned
 * that a table changes are rewritten into alt. */
static int bdfi_xml_expand_attrs(bdfi_xml_reader *r) {
  for (size_t i = 0; i < r->nattrs; i++) {
    bdfi_xml_attr *a = &r->attrs[i];
    if (a->val_len == 0 || !memchr(r->buf + a->val_off, '&', a->val_len)) {
      continue;
    }
    size_t start = r->alt.n;
    int changed;
    int st = bdfi_xml_expand(r, r->buf + a->val_off, a->val_len, &changed);
    if (st != BDFI_XML_OK) {
      return st;
    }
    if (!changed) {
      r->alt.n = start;
      continue;
    }
    a->in_alt = 1;
    a->val_off = start;
    a->val_len = r->alt.n - start;
  }
  return BDFI_XML_OK;
}

/* Reader.scanStartTag, with startTagFeatures. */
static int bdfi_xml_scan_start_tag(bdfi_xml_reader *r, int *kind) {
  const uint8_t *b = r->buf;
  size_t w = r->w;
  uint32_t h = 0;
  size_t end = bdfi_xml_scan_name(r, r->r + 1, &h);
  if (end == BDFI_XML_NONE_AT || end == r->r + 1) {
    return BDFI_XML_STOP; /* the document ends, or: expected element name */
  }
  size_t name_len = end - (r->r + 1);
  size_t i = end;
  int self_close = 0;
  /* one pass over the attributes, recording where each name and value sits */
  for (;;) {
    if (i >= w) {
      return BDFI_XML_STOP;
    }
    uint8_t c = b[i];
    if (bdfi_xml_is_space(c)) {
      i++;
      continue;
    }
    if (c == '>') {
      break;
    }
    if (c == '/') {
      if (i + 1 >= w || b[i + 1] != '>') {
        return BDFI_XML_STOP; /* unexpected '/' in tag */
      }
      self_close = 1;
      i++;
      break;
    }
    /* attribute name, then optional space, '=', optional space, a quote */
    size_t ns = i;
    while (i < w && bdfi_xml_name_byte(b[i])) {
      i++;
    }
    if (i >= w || i == ns) {
      return BDFI_XML_STOP; /* unexpected byte in tag */
    }
    size_t ne = i;
    while (i < w && bdfi_xml_is_space(b[i])) {
      i++;
    }
    if (i >= w) {
      return BDFI_XML_STOP;
    }
    if (b[i] != '=') {
      /* the name is the value, as encoding/xml has it */
      if (bdfi_xml_add_attr(r, ns, ne - ns, ns, ne - ns) != 0) {
        return BDFI_XML_NOMEM;
      }
      continue;
    }
    i++;
    while (i < w && bdfi_xml_is_space(b[i])) {
      i++;
    }
    if (i >= w) {
      return BDFI_XML_STOP;
    }
    c = b[i];
    if (c != '"' && c != '\'') {
      /* scanUnquotedValue */
      size_t ve = i;
      while (ve < w && bdfi_xml_unquoted_byte(b[ve])) {
        ve++;
      }
      if (ve >= w) {
        return BDFI_XML_STOP;
      }
      if (bdfi_xml_add_attr(r, ns, ne - ns, i, ve - i) != 0) {
        return BDFI_XML_NOMEM;
      }
      i = ve;
      continue;
    }
    i++;
    size_t ve = bdfi_xml_index_byte(b, i, w, c);
    if (ve == BDFI_XML_NONE_AT) {
      return BDFI_XML_STOP;
    }
    if (bdfi_xml_add_attr(r, ns, ne - ns, i, ve - i) != 0) {
      return BDFI_XML_NOMEM;
    }
    i = ve + 1;
  }
  if (r->depth >= r->max_depth) {
    return BDFI_XML_STOP; /* ErrTooDeep */
  }
  r->name_off = r->r + 1;
  r->name_len = name_len;
  if (!self_close && bdfi_xml_auto_close(b + r->name_off, name_len)) {
    /* a void element ends here unless its own end tag follows */
    self_close = bdfi_xml_peek_not_end_tag(r, i + 1, name_len);
  }
  int st = bdfi_xml_expand_attrs(r);
  if (st != BDFI_XML_OK) {
    return st;
  }
  if (r->depth == r->stack_cap) {
    size_t cap = r->stack_cap ? r->stack_cap * 2 : 32;
    uint32_t *p = cap > r->stack_cap ? bdfi_realloc_array(r->stack, cap, sizeof *p) : NULL;
    if (!p) {
      return BDFI_XML_NOMEM;
    }
    r->stack = p;
    r->stack_cap = cap;
  }
  r->r = i + 1;
  r->stack[r->depth++] = h;
  r->pending_end = self_close;
  *kind = BDFI_XML_START;
  return BDFI_XML_OK;
}

/* Reader.unwind in lenient.go: an end tag that does not match the open
 * element ends the innermost open element, a call at a time, until the
 * element it names is on top and the tag ends it, or no element is left and
 * the tag is an error. */
static int bdfi_xml_unwind(bdfi_xml_reader *r, int *kind) {
  if (r->depth == 0) {
    r->unwinding = 0;
    return BDFI_XML_STOP; /* unexpected end element */
  }
  if (r->stack[r->depth - 1] == r->close_hash) {
    r->unwinding = 0;
    r->r = r->close_advance;
  }
  r->depth--;
  *kind = BDFI_XML_END;
  return BDFI_XML_OK;
}

/* Reader.scanEndTag. The names of the open elements are compared by their
 * hashes, as there. */
static int bdfi_xml_scan_end_tag(bdfi_xml_reader *r, int *kind) {
  uint32_t h = 0;
  size_t end = bdfi_xml_scan_name(r, r->r + 2, &h);
  if (end == BDFI_XML_NONE_AT || end == r->r + 2) {
    return BDFI_XML_STOP;
  }
  size_t i = end;
  for (;;) {
    if (i >= r->w) {
      return BDFI_XML_STOP;
    }
    uint8_t c = r->buf[i];
    if (c == '>') {
      break;
    }
    if (!bdfi_xml_is_space(c)) {
      return BDFI_XML_STOP; /* expected '>' after end tag name */
    }
    i++;
  }
  if (r->depth == 0) {
    return BDFI_XML_STOP; /* end tag with no open element */
  }
  if (r->stack[r->depth - 1] != h) {
    /* mismatchedEndTag: the input does not move until the tag is consumed */
    r->close_hash = h;
    r->close_advance = i + 1;
    r->unwinding = 1;
    return bdfi_xml_unwind(r, kind);
  }
  r->r = i + 1;
  r->depth--;
  *kind = BDFI_XML_END;
  return BDFI_XML_OK;
}

/* Reader.declAttr: a pseudo-attribute of the XML declaration, whose body is
 * b: 1 and its value, or 0. */
static int bdfi_xml_decl_attr(const uint8_t *b, size_t n, const char *name, size_t name_len,
                              bdfi_svg_str *value) {
  size_t i = 0;
  for (;;) {
    while (i < n && bdfi_xml_is_space(b[i])) {
      i++;
    }
    if (i >= n) {
      return 0;
    }
    size_t ns = i;
    while (i < n && b[i] != '=' && !bdfi_xml_is_space(b[i])) {
      i++;
    }
    size_t ne = i;
    while (i < n && bdfi_xml_is_space(b[i])) {
      i++;
    }
    if (i >= n || b[i] != '=') {
      return 0;
    }
    i++;
    while (i < n && bdfi_xml_is_space(b[i])) {
      i++;
    }
    if (i >= n || (b[i] != '"' && b[i] != '\'')) {
      return 0;
    }
    size_t end = bdfi_xml_index_byte(b, i + 1, n, b[i]);
    if (end == BDFI_XML_NONE_AT) {
      return 0;
    }
    if (ne - ns == name_len && memcmp(b + ns, name, name_len) == 0) {
      *value = bdfi_svg_str_of((const char *)b + i + 1, end - i - 1);
      return 1;
    }
    i = end + 1;
  }
}

/* Reader.scanProcInst. The declaration, a processing instruction of the
 * name xml wherever it is, may name an encoding to switch to before the
 * next token. */
static int bdfi_xml_scan_proc_inst(bdfi_xml_reader *r, int *kind) {
  uint32_t h = 0;
  size_t end = bdfi_xml_scan_name(r, r->r + 2, &h);
  if (end == BDFI_XML_NONE_AT || end == r->r + 2) {
    return BDFI_XML_STOP;
  }
  size_t close = bdfi_xml_index(r->buf, end, r->w, "?>", 2);
  if (close == BDFI_XML_NONE_AT) {
    return BDFI_XML_STOP;
  }
  int is_decl = end - (r->r + 2) == 3 && memcmp(r->buf + r->r + 2, "xml", 3) == 0;
  bdfi_svg_str enc;
  if (is_decl && bdfi_xml_decl_attr(r->buf + end, close - end, "encoding", 8, &enc)) {
    /* Value.EqualFold("utf-8"), of the value as written */
    int utf8 = enc.n == 5;
    for (size_t i = 0; utf8 && i < 5; i++) {
      utf8 = bdfi_xml_fold((uint8_t)enc.p[i]) == (uint8_t)"utf-8"[i];
    }
    if (!utf8) {
      /* Value.String: the label with its references decoded */
      r->label.n = 0;
      if (bdfi_xml_append_value(&r->label, enc) != 0) {
        return BDFI_XML_NOMEM;
      }
      r->charset = 1;
    }
  }
  r->r = close + 2;
  *kind = BDFI_XML_PROCINST;
  return BDFI_XML_OK;
}

/* Reader.scanDoctype in doctype.go: the closing '>' of the DOCTYPE at r, and
 * the bounds of its internal subset, the bytes between '[' and ']'. Quoted
 * literals, and inside the subset comments and processing instructions, are
 * passed over. */
static int bdfi_xml_scan_doctype(const bdfi_xml_reader *r, size_t *end, size_t *sub_start,
                                 size_t *sub_end) {
  const uint8_t *b = r->buf;
  size_t i = r->r + 9;
  uint8_t quote = 0;
  int in_subset = 0;
  *sub_start = *sub_end = BDFI_XML_NONE_AT;
  for (;;) {
    if (i >= r->w) {
      return BDFI_XML_STOP;
    }
    uint8_t c = b[i];
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      }
      i++;
    } else if (c == '"' || c == '\'') {
      quote = c;
      i++;
    } else if (c == '[') {
      if (!in_subset && *sub_start == BDFI_XML_NONE_AT) {
        in_subset = 1;
        *sub_start = i + 1;
      }
      i++;
    } else if (c == ']') {
      if (in_subset) {
        in_subset = 0;
        *sub_end = i;
      }
      i++;
    } else if (c == '<' && in_subset && bdfi_xml_has_prefix(r, i, "<!--", 4)) {
      size_t j = bdfi_xml_index(b, i + 4, r->w, "-->", 3);
      if (j == BDFI_XML_NONE_AT) {
        return BDFI_XML_STOP;
      }
      i = j + 3;
    } else if (c == '<' && in_subset && bdfi_xml_has_prefix(r, i, "<?", 2)) {
      size_t j = bdfi_xml_index(b, i + 2, r->w, "?>", 2);
      if (j == BDFI_XML_NONE_AT) {
        return BDFI_XML_STOP;
      }
      i = j + 2;
    } else if (c == '>' && !in_subset) {
      *end = i;
      return BDFI_XML_OK;
    } else {
      i++;
    }
  }
}

/* Reader.scanBang: a comment, a CDATA section or a DOCTYPE. */
static int bdfi_xml_scan_bang(bdfi_xml_reader *r, int *kind) {
  if (bdfi_xml_has_prefix(r, r->r, "<!--", 4)) {
    size_t end = bdfi_xml_index(r->buf, r->r + 4, r->w, "-->", 3);
    if (end == BDFI_XML_NONE_AT) {
      return BDFI_XML_STOP;
    }
    r->text_off = r->r + 4;
    r->text_len = end - r->text_off;
    r->r = end + 3;
    *kind = BDFI_XML_COMMENT;
    return BDFI_XML_OK;
  }
  if (bdfi_xml_has_prefix(r, r->r, "<![CDATA[", 9)) {
    size_t end = bdfi_xml_index(r->buf, r->r + 9, r->w, "]]>", 3);
    if (end == BDFI_XML_NONE_AT) {
      return BDFI_XML_STOP;
    }
    r->text_off = r->r + 9;
    r->text_len = end - r->text_off;
    r->r = end + 3;
    *kind = BDFI_XML_CDATA;
    return BDFI_XML_OK;
  }
  if (bdfi_xml_has_prefix(r, r->r, "<!DOCTYPE", 9)) {
    size_t end = 0, sub_start, sub_end;
    if (bdfi_xml_scan_doctype(r, &end, &sub_start, &sub_end) != BDFI_XML_OK) {
      return BDFI_XML_STOP;
    }
    if (sub_start != BDFI_XML_NONE_AT && sub_end != BDFI_XML_NONE_AT && sub_end > sub_start &&
        bdfi_xml_declare_entities(r, r->buf + sub_start, sub_end - sub_start) != 0) {
      return BDFI_XML_NOMEM;
    }
    r->text_off = r->r + 9;
    r->text_len = end - r->text_off;
    r->r = end + 1;
    *kind = BDFI_XML_DIRECTIVE;
    return BDFI_XML_OK;
  }
  return BDFI_XML_STOP; /* unexpected '<!' */
}

/* Reader.scanText, with expandText: character data up to the next '<'. */
static int bdfi_xml_scan_text(bdfi_xml_reader *r, int *kind) {
  size_t end = bdfi_xml_index_byte(r->buf, r->r + 1, r->w, '<');
  if (end == BDFI_XML_NONE_AT) {
    end = r->w;
  }
  r->text_off = r->r;
  r->text_len = end - r->r;
  r->r = end;
  if (memchr(r->buf + r->text_off, '&', r->text_len)) {
    int changed;
    int st = bdfi_xml_expand(r, r->buf + r->text_off, r->text_len, &changed);
    if (st != BDFI_XML_OK) {
      return st;
    }
    if (changed) {
      r->text_in_alt = 1;
    } else {
      r->alt.n = 0;
    }
  }
  *kind = BDFI_XML_TEXT;
  return BDFI_XML_OK;
}

/*
 * Reader.switchCharset in charset.go with the CharsetReader xmltree.Open
 * gives it: the rest of the document, after its declaration, is converted
 * whole from the encoding the declaration named (not when a byte order mark
 * said more than the declaration), made valid, bounded as the document was,
 * and read on. A second declaration that asks for it is an error.
 */
static int bdfi_xml_switch_charset(bdfi_xml_reader *r) {
  if (r->switched) {
    return BDFI_XML_STOP; /* errDeclarations */
  }
  r->switched = 1;
  const uint8_t *rest = r->buf + r->r;
  size_t n = r->w - r->r;
  bdfi_xml_buf conv = {NULL, 0, 0};
  int err;
  if (r->marked) {
    err = bdfi_xml_buf_reserve(&conv, n) != 0 || bdfi_xml_buf_append(&conv, rest, n) != 0;
  } else {
    int kind = bdfi_xml_label_kind(bdfi_svg_str_of((const char *)r->label.p, r->label.n));
    switch (kind) {
    case BDFI_XML_ENC_UTF8:
      err = bdfi_xml_decode_utf8(rest, n, &conv);
      break;
    case BDFI_XML_ENC_UTF16BE:
      err = bdfi_xml_decode_utf16(rest, n, 1, &conv);
      break;
    case BDFI_XML_ENC_UTF16LE:
      err = bdfi_xml_decode_utf16(rest, n, 0, &conv);
      break;
    case BDFI_XML_ENC_WINDOWS1252:
      err = bdfi_xml_decode_cp1252(rest, n, &conv);
      break;
    case BDFI_XML_ENC_ASCII:
    case BDFI_XML_ENC_ASCII_NO_ESC:
      /* The decoder of the encoding is not here, and is not needed for a
       * rest of the document that is ASCII: it leaves that as it is. */
      for (size_t i = 0; i < n; i++) {
        if (rest[i] >= 0x80 || (rest[i] == 0x1B && kind == BDFI_XML_ENC_ASCII_NO_ESC)) {
          return BDFI_XML_ENCODING;
        }
      }
      err = bdfi_xml_buf_reserve(&conv, n) != 0 || bdfi_xml_buf_append(&conv, rest, n) != 0;
      break;
    case BDFI_XML_ENC_OTHER:
      return BDFI_XML_ENCODING;
    default:
      return BDFI_XML_STOP; /* charset.NewReaderLabel: unsupported charset */
    }
  }
  if (err) {
    bdfi_xml_buf_free(&conv);
    return BDFI_XML_NOMEM;
  }
  if (!bdfi_xml_utf8_valid(conv.p, conv.n)) {
    bdfi_xml_buf valid = {NULL, 0, 0};
    if (bdfi_xml_to_valid_utf8(conv.p, conv.n, &valid) != 0) {
      bdfi_xml_buf_free(&valid);
      bdfi_xml_buf_free(&conv);
      return BDFI_XML_NOMEM;
    }
    bdfi_xml_buf_free(&conv);
    conv = valid;
  }
  bdfi_xml_adopt(r, &conv);
  if (bdfi_xml_bound(r) != 0) {
    return BDFI_XML_NOMEM;
  }
  bdfi_xml_undeclare(r->owned, r->w);
  return BDFI_XML_OK;
}

/* Reader.next. */
static int bdfi_xml_scan(bdfi_xml_reader *r, int *kind) {
  if (r->pending_end) {
    r->pending_end = 0;
    r->depth--;
    *kind = BDFI_XML_END;
    return BDFI_XML_OK;
  }
  /* beforeToken */
  if (r->unwinding) {
    return bdfi_xml_unwind(r, kind);
  }
  if (r->charset) {
    r->charset = 0;
    int st = bdfi_xml_switch_charset(r);
    if (st != BDFI_XML_OK) {
      return st;
    }
  }
  r->alt.n = 0;
  r->text_in_alt = 0;
  r->name_len = r->text_len = r->attr_pos = r->nattrs = 0;
  if (r->r >= r->w) {
    if (r->depth > 0) {
      return BDFI_XML_STOP; /* ErrTruncated */
    }
    *kind = BDFI_XML_EOF;
    return BDFI_XML_OK;
  }
  /* A UTF-8 byte order mark at the start is not text. (A UTF-16 one cannot
   * be there: bdfi_xml_open decoded such a document, and made valid UTF-8
   * of every other that does not start with a declaration.) */
  if (!r->switched && r->r == 0 && r->w > 2 && r->buf[0] == 0xEF && r->buf[1] == 0xBB &&
      r->buf[2] == 0xBF) {
    r->r = 3;
    if (r->r >= r->w) {
      *kind = BDFI_XML_EOF;
      return BDFI_XML_OK;
    }
  }
  if (r->buf[r->r] != '<') {
    return bdfi_xml_scan_text(r, kind);
  }
  if (r->r + 1 >= r->w) {
    return BDFI_XML_STOP;
  }
  switch (r->buf[r->r + 1]) {
  case '/':
    return bdfi_xml_scan_end_tag(r, kind);
  case '?':
    return bdfi_xml_scan_proc_inst(r, kind);
  case '!':
    return bdfi_xml_scan_bang(r, kind);
  default:
    return bdfi_xml_scan_start_tag(r, kind);
  }
}

/* Reader.Next: after an error every later call returns the same error. */
int bdfi_xml_next(bdfi_xml_reader *r, int *kind) {
  *kind = BDFI_XML_NONE;
  if (r->err != BDFI_XML_OK) {
    return r->err;
  }
  int k = BDFI_XML_NONE;
  int st = bdfi_xml_scan(r, &k);
  if (st != BDFI_XML_OK) {
    r->err = st;
    r->kind = BDFI_XML_NONE;
    return st;
  }
  r->kind = k;
  *kind = k;
  return BDFI_XML_OK;
}

bdfi_svg_str bdfi_xml_local(bdfi_svg_str name) {
  const char *colon = name.n > 0 ? memchr(name.p, ':', name.n) : NULL;
  if (colon && colon > name.p && colon < name.p + name.n - 1) {
    return bdfi_svg_str_of(colon + 1, name.n - (size_t)(colon - name.p) - 1);
  }
  return name;
}

bdfi_svg_str bdfi_xml_local_name(const bdfi_xml_reader *r) {
  return bdfi_xml_local(bdfi_svg_str_of((const char *)r->buf + r->name_off, r->name_len));
}

int bdfi_xml_next_attr(bdfi_xml_reader *r, bdfi_svg_str *name, bdfi_svg_str *value) {
  if (r->attr_pos >= r->nattrs) {
    return 0;
  }
  const bdfi_xml_attr *a = &r->attrs[r->attr_pos++];
  *name = bdfi_svg_str_of((const char *)r->buf + a->name_off, a->name_len);
  *value = bdfi_svg_str_of((const char *)(a->in_alt ? r->alt.p : r->buf) + a->val_off, a->val_len);
  return 1;
}

bdfi_svg_str bdfi_xml_text(const bdfi_xml_reader *r) {
  if (r->text_in_alt) {
    return bdfi_svg_str_of((const char *)r->alt.p, r->alt.n);
  }
  return bdfi_svg_str_of((const char *)r->buf + r->text_off, r->text_len);
}

/* AppendText in open.go: the entities of text replaced, and the line ends
 * of text and of a CDATA section made "\n". */
int bdfi_xml_append_text(bdfi_xml_buf *dst, const bdfi_xml_reader *r) {
  bdfi_svg_str t = bdfi_xml_text(r);
  if (r->kind == BDFI_XML_TEXT) {
    return bdfi_xml_append_value(dst, t);
  }
  size_t from = 0;
  for (size_t i = 0; i < t.n; i++) {
    if (t.p[i] != '\r') {
      continue;
    }
    if (bdfi_xml_buf_append(dst, t.p + from, i - from) != 0 || bdfi_xml_buf_byte(dst, '\n') != 0) {
      return -1;
    }
    if (i + 1 < t.n && t.p[i + 1] == '\n') {
      i++;
    }
    from = i + 1;
  }
  return bdfi_xml_buf_append(dst, t.p + from, t.n - from);
}
