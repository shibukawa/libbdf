/*
 * The JSON reader of the manifest. See bdf_internal.h for what it accepts.
 */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

typedef struct bdfi_jp {
  const uint8_t *s;
  size_t n, pos;
  bdfi_json *j;
  int depth;
  const char *err;
  size_t err_off;
  int oom;
} bdfi_jp;

static int bdfi_jp_fail(bdfi_jp *p, const char *msg) {
  if (!p->err) {
    p->err = msg;
    p->err_off = p->pos;
  }
  return -1;
}

static uint32_t bdfi_jp_node(bdfi_jp *p, uint32_t kind) {
  bdfi_json *j = p->j;
  if (j->n == j->cap) {
    if (j->cap >= UINT32_MAX / 2) {
      p->oom = 1;
      return UINT32_MAX;
    }
    uint32_t cap = j->cap ? j->cap * 2 : 64;
    bdfi_json_node *nodes = realloc(j->nodes, (size_t)cap * sizeof *nodes);
    if (!nodes) {
      p->oom = 1;
      return UINT32_MAX;
    }
    j->nodes = nodes;
    j->cap = cap;
  }
  bdfi_json_node *nd = &j->nodes[j->n];
  memset(nd, 0, sizeof *nd);
  nd->kind = kind;
  return j->n++;
}

static int bdfi_jp_put(bdfi_jp *p, const uint8_t *b, size_t n) {
  bdfi_json *j = p->j;
  if (n > UINT32_MAX - j->buf_n) {
    p->oom = 1;
    return -1;
  }
  if (j->buf_n + n > j->buf_cap) {
    uint32_t cap = j->buf_cap ? j->buf_cap : 256;
    while (cap < j->buf_n + n) {
      if (cap > UINT32_MAX / 2) {
        cap = UINT32_MAX;
        break;
      }
      cap *= 2;
    }
    uint8_t *buf = realloc(j->buf, cap);
    if (!buf) {
      p->oom = 1;
      return -1;
    }
    j->buf = buf;
    j->buf_cap = cap;
  }
  if (n > 0) {
    memcpy(j->buf + j->buf_n, b, n);
  }
  j->buf_n += (uint32_t)n;
  return 0;
}

static void bdfi_jp_ws(bdfi_jp *p) {
  while (p->pos < p->n) {
    uint8_t c = p->s[p->pos];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      break;
    }
    p->pos++;
  }
}

static int bdfi_jp_hex(uint8_t c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* bdfi_jp_hex4 reads \uXXXX at pos (pos at the first hex digit). */
static int bdfi_jp_hex4(bdfi_jp *p, size_t at, uint32_t *out) {
  if (at + 4 > p->n) {
    return -1;
  }
  uint32_t v = 0;
  for (size_t i = 0; i < 4; i++) {
    int h = bdfi_jp_hex(p->s[at + i]);
    if (h < 0) {
      return -1;
    }
    v = (v << 4) | (uint32_t)h;
  }
  *out = v;
  return 0;
}

static int bdfi_jp_put_rune(bdfi_jp *p, uint32_t cp) {
  uint8_t b[4];
  size_t n;
  if (cp < 0x80) {
    b[0] = (uint8_t)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (uint8_t)(0xC0 | (cp >> 6));
    b[1] = (uint8_t)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = (uint8_t)(0xE0 | (cp >> 12));
    b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (uint8_t)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    b[0] = (uint8_t)(0xF0 | (cp >> 18));
    b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (uint8_t)(0x80 | (cp & 0x3F));
    n = 4;
  }
  return bdfi_jp_put(p, b, n);
}

/* bdfi_utf8_len returns the length of a valid UTF-8 sequence at s, or 0. */
static size_t bdfi_utf8_len(const uint8_t *s, size_t n) {
  uint8_t c = s[0];
  size_t len;
  uint8_t lo = 0x80, hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) {
    len = 2;
  } else if (c >= 0xE0 && c <= 0xEF) {
    len = 3;
    if (c == 0xE0) {
      lo = 0xA0;
    } else if (c == 0xED) {
      hi = 0x9F;
    }
  } else if (c >= 0xF0 && c <= 0xF4) {
    len = 4;
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
  for (size_t i = 2; i < len; i++) {
    if (s[i] < 0x80 || s[i] > 0xBF) {
      return 0;
    }
  }
  return len;
}

/* bdfi_jp_string decodes the string at pos (its opening quote) into the
 * buffer and reports where it landed. */
static int bdfi_jp_string(bdfi_jp *p, uint32_t *off, uint32_t *len) {
  p->pos++; /* the quote */
  *off = p->j->buf_n;
  for (;;) {
    if (p->pos >= p->n) {
      return bdfi_jp_fail(p, "unterminated string");
    }
    uint8_t c = p->s[p->pos];
    if (c == '"') {
      p->pos++;
      *len = p->j->buf_n - *off;
      /* a NUL after every string, so that the text can be used as a C
       * string as well; it is not part of the length */
      uint8_t nul = 0;
      return bdfi_jp_put(p, &nul, 1);
    }
    if (c < 0x20) {
      return bdfi_jp_fail(p, "control character in string");
    }
    if (c == '\\') {
      if (p->pos + 1 >= p->n) {
        return bdfi_jp_fail(p, "unterminated string");
      }
      uint8_t e = p->s[p->pos + 1];
      p->pos += 2;
      uint8_t out;
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out = e;
        break;
      case 'b':
        out = 0x08;
        break;
      case 'f':
        out = 0x0C;
        break;
      case 'n':
        out = '\n';
        break;
      case 'r':
        out = '\r';
        break;
      case 't':
        out = '\t';
        break;
      case 'u': {
        uint32_t cp;
        if (bdfi_jp_hex4(p, p->pos, &cp) < 0) {
          return bdfi_jp_fail(p, "bad unicode escape");
        }
        p->pos += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t lo;
          if (p->pos + 6 <= p->n && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u' &&
              bdfi_jp_hex4(p, p->pos + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p->pos += 6;
          } else {
            cp = 0xFFFD; /* a lone surrogate, as encoding/json decodes it */
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        if (bdfi_jp_put_rune(p, cp) < 0) {
          return -1;
        }
        continue;
      }
      default:
        return bdfi_jp_fail(p, "bad escape");
      }
      if (bdfi_jp_put(p, &out, 1) < 0) {
        return -1;
      }
      continue;
    }
    if (c < 0x80) {
      if (bdfi_jp_put(p, &c, 1) < 0) {
        return -1;
      }
      p->pos++;
      continue;
    }
    size_t len8 = bdfi_utf8_len(p->s + p->pos, p->n - p->pos);
    if (len8 == 0) {
      /* invalid UTF-8 becomes U+FFFD, one byte at a time, as in encoding/json */
      if (bdfi_jp_put_rune(p, 0xFFFD) < 0) {
        return -1;
      }
      p->pos++;
      continue;
    }
    if (bdfi_jp_put(p, p->s + p->pos, len8) < 0) {
      return -1;
    }
    p->pos += len8;
  }
}

static int bdfi_jp_digit(bdfi_jp *p) { return p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9'; }

static int bdfi_jp_number(bdfi_jp *p, uint32_t *idx) {
  size_t start = p->pos;
  if (p->s[p->pos] == '-') {
    p->pos++;
  }
  if (p->pos < p->n && p->s[p->pos] == '0') {
    p->pos++;
  } else if (bdfi_jp_digit(p)) {
    while (bdfi_jp_digit(p)) {
      p->pos++;
    }
  } else {
    return bdfi_jp_fail(p, "bad number");
  }
  if (p->pos < p->n && p->s[p->pos] == '.') {
    p->pos++;
    if (!bdfi_jp_digit(p)) {
      return bdfi_jp_fail(p, "bad number");
    }
    while (bdfi_jp_digit(p)) {
      p->pos++;
    }
  }
  if (p->pos < p->n && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
    p->pos++;
    if (p->pos < p->n && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) {
      p->pos++;
    }
    if (!bdfi_jp_digit(p)) {
      return bdfi_jp_fail(p, "bad number");
    }
    while (bdfi_jp_digit(p)) {
      p->pos++;
    }
  }
  uint32_t i = bdfi_jp_node(p, BDFI_JSON_NUMBER);
  if (i == UINT32_MAX) {
    return -1;
  }
  uint32_t off = p->j->buf_n;
  uint8_t nul = 0;
  if (bdfi_jp_put(p, p->s + start, p->pos - start) < 0 || bdfi_jp_put(p, &nul, 1) < 0) {
    return -1;
  }
  p->j->nodes[i].off = off;
  p->j->nodes[i].len = (uint32_t)(p->pos - start);
  *idx = i;
  return 0;
}

static int bdfi_jp_value(bdfi_jp *p, uint32_t *idx);

/* bdfi_jp_container parses an object or an array whose opening bracket is
 * at pos. */
static int bdfi_jp_container(bdfi_jp *p, uint32_t *idx, int object) {
  if (++p->depth > BDFI_JSON_MAX_DEPTH) {
    return bdfi_jp_fail(p, "JSON nesting too deep");
  }
  uint32_t i = bdfi_jp_node(p, object ? BDFI_JSON_OBJECT : BDFI_JSON_ARRAY);
  if (i == UINT32_MAX) {
    return -1;
  }
  uint8_t close = object ? '}' : ']';
  p->pos++;
  bdfi_jp_ws(p);
  if (p->pos < p->n && p->s[p->pos] == close) {
    p->pos++;
  } else {
    uint32_t last = 0;
    for (;;) {
      bdfi_jp_ws(p);
      uint32_t koff = 0, klen = 0;
      if (object) {
        if (p->pos >= p->n || p->s[p->pos] != '"') {
          return bdfi_jp_fail(p, "expected a key");
        }
        if (bdfi_jp_string(p, &koff, &klen) < 0) {
          return -1;
        }
        bdfi_jp_ws(p);
        if (p->pos >= p->n || p->s[p->pos] != ':') {
          return bdfi_jp_fail(p, "expected ':'");
        }
        p->pos++;
        bdfi_jp_ws(p);
      }
      uint32_t child;
      if (bdfi_jp_value(p, &child) < 0) {
        return -1;
      }
      bdfi_json_node *nodes = p->j->nodes;
      if (object) {
        nodes[child].has_key = 1;
        nodes[child].key_off = koff;
        nodes[child].key_len = klen;
      }
      if (nodes[i].count == 0) {
        nodes[i].child = child;
      } else {
        nodes[last].next = child;
      }
      nodes[i].count++;
      last = child;
      bdfi_jp_ws(p);
      if (p->pos < p->n && p->s[p->pos] == ',') {
        p->pos++;
        continue;
      }
      if (p->pos < p->n && p->s[p->pos] == close) {
        p->pos++;
        break;
      }
      return bdfi_jp_fail(p, object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
  }
  p->depth--;
  *idx = i;
  return 0;
}

static int bdfi_jp_literal(bdfi_jp *p, uint32_t *idx, const char *word, uint32_t kind) {
  size_t n = strlen(word);
  if (p->n - p->pos < n || memcmp(p->s + p->pos, word, n) != 0) {
    return bdfi_jp_fail(p, "unexpected character");
  }
  uint32_t i = bdfi_jp_node(p, kind);
  if (i == UINT32_MAX) {
    return -1;
  }
  p->pos += n;
  *idx = i;
  return 0;
}

static int bdfi_jp_value(bdfi_jp *p, uint32_t *idx) {
  if (p->pos >= p->n) {
    return bdfi_jp_fail(p, "unexpected end of JSON");
  }
  uint8_t c = p->s[p->pos];
  switch (c) {
  case '{':
    return bdfi_jp_container(p, idx, 1);
  case '[':
    return bdfi_jp_container(p, idx, 0);
  case '"': {
    uint32_t i = bdfi_jp_node(p, BDFI_JSON_STRING);
    if (i == UINT32_MAX) {
      return -1;
    }
    uint32_t off, len;
    if (bdfi_jp_string(p, &off, &len) < 0) {
      return -1;
    }
    p->j->nodes[i].off = off;
    p->j->nodes[i].len = len;
    *idx = i;
    return 0;
  }
  case 't':
    return bdfi_jp_literal(p, idx, "true", BDFI_JSON_TRUE);
  case 'f':
    return bdfi_jp_literal(p, idx, "false", BDFI_JSON_FALSE);
  case 'n':
    return bdfi_jp_literal(p, idx, "null", BDFI_JSON_NULL);
  default:
    if (c == '-' || (c >= '0' && c <= '9')) {
      return bdfi_jp_number(p, idx);
    }
    return bdfi_jp_fail(p, "unexpected character");
  }
}

bdf_status bdfi_json_parse(const uint8_t *data, size_t len, bdfi_json *j, bdf_error *err) {
  bdfi_error_clear(err);
  memset(j, 0, sizeof *j);
  bdfi_jp p;
  memset(&p, 0, sizeof p);
  p.s = data;
  p.n = len;
  p.j = j;
  uint32_t root;
  bdfi_jp_ws(&p);
  int rc = bdfi_jp_value(&p, &root);
  if (rc == 0) {
    bdfi_jp_ws(&p);
    if (p.pos != p.n) {
      rc = bdfi_jp_fail(&p, "data after the JSON value");
    }
  }
  if (rc < 0) {
    bdfi_json_free(j);
    if (p.oom) {
      bdfi_error_set(err, BDF_ERR_NOMEM, p.pos, "out of memory");
      return BDF_ERR_NOMEM;
    }
    bdfi_error_set(err, BDF_ERR_FORMAT, p.err_off, p.err);
    return BDF_ERR_FORMAT;
  }
  return BDF_OK;
}

void bdfi_json_free(bdfi_json *j) {
  free(j->nodes);
  free(j->buf);
  memset(j, 0, sizeof *j);
}

const bdfi_json_node *bdfi_json_root(const bdfi_json *j) { return j->n ? &j->nodes[0] : NULL; }

const bdfi_json_node *bdfi_json_first(const bdfi_json *j, const bdfi_json_node *c) {
  if (!c || (c->kind != BDFI_JSON_ARRAY && c->kind != BDFI_JSON_OBJECT) || c->count == 0) {
    return NULL;
  }
  return &j->nodes[c->child];
}

const bdfi_json_node *bdfi_json_next(const bdfi_json *j, const bdfi_json_node *n) {
  return n && n->next ? &j->nodes[n->next] : NULL;
}

const bdfi_json_node *bdfi_json_member(const bdfi_json *j, const bdfi_json_node *obj,
                                       const char *key) {
  if (!obj || obj->kind != BDFI_JSON_OBJECT) {
    return NULL;
  }
  size_t klen = strlen(key);
  const bdfi_json_node *found = NULL;
  for (const bdfi_json_node *m = bdfi_json_first(j, obj); m; m = bdfi_json_next(j, m)) {
    if (m->key_len == klen && memcmp(j->buf + m->key_off, key, klen) == 0) {
      found = m;
    }
  }
  return found;
}

const char *bdfi_json_text(const bdfi_json *j, const bdfi_json_node *n, int key, size_t *len) {
  if (key) {
    *len = n->key_len;
    return (const char *)j->buf + n->key_off;
  }
  *len = n->len;
  return (const char *)j->buf + n->off;
}

static const double bdfi_pow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                       1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                       1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

int bdfi_json_number(const bdfi_json *j, const bdfi_json_node *n, double *out) {
  if (n->kind != BDFI_JSON_NUMBER) {
    return -1;
  }
  size_t len;
  const char *t = bdfi_json_text(j, n, 0, &len);
  size_t i = 0;
  int neg = 0;
  if (i < len && t[i] == '-') {
    neg = 1;
    i++;
  }
  uint64_t mant = 0;
  int digits = 0, exp10 = 0;
  for (; i < len && t[i] >= '0' && t[i] <= '9'; i++) {
    if (digits < 19) {
      if (mant != 0 || t[i] != '0') {
        mant = mant * 10 + (uint64_t)(t[i] - '0');
        digits++;
      }
    } else {
      exp10++;
    }
  }
  if (i < len && t[i] == '.') {
    i++;
    for (; i < len && t[i] >= '0' && t[i] <= '9'; i++) {
      if (digits < 19) {
        if (mant != 0 || t[i] != '0') {
          mant = mant * 10 + (uint64_t)(t[i] - '0');
          digits++;
        }
        exp10--;
      }
    }
  }
  if (i < len && (t[i] == 'e' || t[i] == 'E')) {
    i++;
    int esign = 1;
    if (i < len && (t[i] == '+' || t[i] == '-')) {
      esign = t[i] == '-' ? -1 : 1;
      i++;
    }
    int e = 0;
    for (; i < len && t[i] >= '0' && t[i] <= '9'; i++) {
      if (e < 100000) {
        e = e * 10 + (t[i] - '0');
      }
    }
    exp10 += esign * e;
  }
  double v;
  if (mant == 0) {
    v = 0;
  } else if (exp10 >= -22 && exp10 <= 22 && mant < ((uint64_t)1 << 53)) {
    v = (double)mant;
    v = exp10 >= 0 ? v * bdfi_pow10[exp10] : v / bdfi_pow10[-exp10];
  } else {
    v = (double)mant;
    int e = exp10;
    while (e > 0 && v <= DBL_MAX) {
      int step = e > 22 ? 22 : e;
      v *= bdfi_pow10[step];
      e -= step;
    }
    while (e < 0 && v > 0) {
      int step = -e > 22 ? 22 : -e;
      v /= bdfi_pow10[step];
      e += step;
    }
  }
  if (v > DBL_MAX) {
    return -1;
  }
  *out = neg ? -v : v;
  return 0;
}

int bdfi_json_int64(const bdfi_json *j, const bdfi_json_node *n, int64_t *out) {
  if (n->kind != BDFI_JSON_NUMBER) {
    return -1;
  }
  size_t len;
  const char *t = bdfi_json_text(j, n, 0, &len);
  size_t i = 0;
  int neg = 0;
  if (i < len && t[i] == '-') {
    neg = 1;
    i++;
  }
  uint64_t v = 0;
  uint64_t max = neg ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1;
  for (; i < len; i++) {
    if (t[i] < '0' || t[i] > '9') {
      return -1; /* a fraction or an exponent */
    }
    uint64_t d = (uint64_t)(t[i] - '0');
    if (v > (max - d) / 10) {
      return -1;
    }
    v = v * 10 + d;
  }
  *out = neg ? (int64_t)(0 - v) : (int64_t)v;
  return 0;
}

int bdfi_base64_decode(const char *s, size_t n, bdfi_arena *a, uint8_t **out, size_t *out_len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (n % 4 != 0) {
    return -1;
  }
  size_t pad = 0;
  if (n >= 1 && s[n - 1] == '=') {
    pad++;
  }
  if (n >= 2 && s[n - 2] == '=') {
    pad++;
  }
  size_t len = n / 4 * 3 - pad;
  uint8_t *o = bdfi_arena_alloc(a, len + 1);
  if (!o) {
    return -1;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i += 4) {
    uint32_t v = 0;
    for (size_t q = 0; q < 4; q++) {
      char c = s[i + q];
      int d;
      if (c == '=' && i + q >= n - pad) {
        d = 0;
      } else {
        const char *pos = c ? strchr(alphabet, c) : NULL;
        if (!pos) {
          return -1;
        }
        d = (int)(pos - alphabet);
      }
      v = (v << 6) | (uint32_t)d;
    }
    for (size_t q = 0; q < 3 && k < len; q++) {
      o[k++] = (uint8_t)(v >> (16 - 8 * q));
    }
  }
  *out = o;
  *out_len = len;
  return 0;
}
