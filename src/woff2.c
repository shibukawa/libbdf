/*
 * WOFF2 → SFNT (W3C WOFF File Format 2.0): a port of font/woff2/decode.go of
 * the reference implementation, so that the two produce the same bytes. The
 * glyf and loca tables are rebuilt from the transformed glyf table (loca in
 * the long format, and head.indexToLocFormat set to 1), hmtx from its
 * transform, everything else is copied; tables go out sorted by tag, 4-byte
 * aligned, with their checksums, and head.checkSumAdjustment is left as it
 * came. Font collections are not supported. The Brotli stream is read up to
 * the size the directory states and one byte beyond, never more.
 */
#include <stdlib.h>
#include <string.h>

#include "bdf_internal.h"

/* The tables of a font, decompressed, are at most this large together. */
#ifndef BDF_MAX_WOFF2_SIZE
#define BDF_MAX_WOFF2_SIZE ((size_t)256 << 20)
#endif
#define BDFI_WOFF2_MAX_SIZE BDF_MAX_WOFF2_SIZE

/* The known table tags of WOFF2; a tag's index is its flag value. */
static const char *const bdfi_woff2_known_tags[63] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf",
    "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT",
    "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT",
    "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar",
    "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd",
    "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

static uint32_t bdfi_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t bdfi_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void bdfi_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void bdfi_put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

/* ---- A growable byte buffer ---------------------------------------------- */

typedef struct bdfi_wbuf {
  uint8_t *b;
  size_t n, cap;
  int oom, limited;
} bdfi_wbuf;

static int bdfi_wbuf_reserve(bdfi_wbuf *w, size_t extra) {
  if (w->oom || w->limited) {
    return -1;
  }
  if (extra > BDFI_WOFF2_MAX_SIZE - w->n) {
    w->limited = 1;
    return -1;
  }
  if (w->n + extra <= w->cap) {
    return 0;
  }
  size_t cap = w->cap ? w->cap : 1024;
  while (cap < w->n + extra) {
    if (cap > SIZE_MAX / 2) {
      cap = w->n + extra;
      break;
    }
    cap *= 2;
  }
  if (cap > BDFI_WOFF2_MAX_SIZE) {
    cap = BDFI_WOFF2_MAX_SIZE;
  }
  uint8_t *b = realloc(w->b, cap);
  if (!b) {
    w->oom = 1;
    return -1;
  }
  w->b = b;
  w->cap = cap;
  return 0;
}

static void bdfi_wbuf_bytes(bdfi_wbuf *w, const uint8_t *p, size_t n) {
  if (!p || n == 0 || bdfi_wbuf_reserve(w, n) < 0) {
    return;
  }
  memcpy(w->b + w->n, p, n);
  w->n += n;
}

static void bdfi_wbuf_u8(bdfi_wbuf *w, uint8_t v) { bdfi_wbuf_bytes(w, &v, 1); }

static void bdfi_wbuf_u16(bdfi_wbuf *w, uint16_t v) {
  uint8_t b[2];
  bdfi_put16(b, v);
  bdfi_wbuf_bytes(w, b, 2);
}

/* ---- A cursor over a stream --------------------------------------------- */

typedef struct bdfi_ws {
  const uint8_t *b;
  size_t n, pos;
  int err;
} bdfi_ws;

static unsigned bdfi_ws_u8(bdfi_ws *s) {
  if (s->pos >= s->n) {
    s->err = 1;
    return 0;
  }
  return s->b[s->pos++];
}

static unsigned bdfi_ws_u16(bdfi_ws *s) {
  if (s->n - s->pos < 2) {
    s->err = 1;
    return 0;
  }
  unsigned v = bdfi_be16(s->b + s->pos);
  s->pos += 2;
  return v;
}

static const uint8_t *bdfi_ws_bytes(bdfi_ws *s, size_t n) {
  if (n > s->n - s->pos) {
    s->err = 1;
    return NULL;
  }
  const uint8_t *p = s->b + s->pos;
  s->pos += n;
  return p;
}

/* bdfi_ws_u255 reads a 255UInt16. */
static unsigned bdfi_ws_u255(bdfi_ws *s) {
  unsigned c = bdfi_ws_u8(s);
  switch (c) {
  case 253:
    return bdfi_ws_u16(s);
  case 255:
    return bdfi_ws_u8(s) + 253;
  case 254:
    return bdfi_ws_u8(s) + 506;
  default:
    return c;
  }
}

int bdfi_woff2_read_base128(const uint8_t *b, size_t len, size_t *pos, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 5; i++) {
    if (*pos >= len) {
      return -1;
    }
    uint8_t c = b[(*pos)++];
    if ((i == 0 && c == 0x80) || (v & 0xfe000000u) != 0) {
      return -1;
    }
    v = (v << 7) | (c & 0x7f);
    if ((c & 0x80) == 0) {
      if (v > ((uint32_t)1 << 30)) {
        return -1;
      }
      *out = v;
      return 0;
    }
  }
  return -1;
}

/* ---- The glyf transform (WOFF2 §5.1) ------------------------------------ */

enum {
  BDFI_GLYF_ON_CURVE = 0x01,
  BDFI_GLYF_OVERLAP = 0x40,
  BDFI_COMP_ARGS_ARE_WORDS = 0x0001,
  BDFI_COMP_HAVE_SCALE = 0x0008,
  BDFI_COMP_MORE = 0x0020,
  BDFI_COMP_HAVE_XY_SCALE = 0x0040,
  BDFI_COMP_HAVE_2X2 = 0x0080,
  BDFI_COMP_HAVE_INSTR = 0x0100
};

/* bdfi_sign applies the sign bit of a triplet flag. */
static int bdfi_sign(unsigned f, int v) { return (f & 1) ? v : -v; }

/* bdfi_triplet reads the data bytes of a point delta for a flag byte without
 * its on-curve bit (WOFF2 §5.2). */
static void bdfi_triplet(unsigned flag, bdfi_ws *s, int *dx, int *dy) {
  *dx = 0;
  *dy = 0;
  if (flag < 10) {
    *dy = bdfi_sign(flag, (int)(((flag & 14) << 7) + bdfi_ws_u8(s)));
  } else if (flag < 20) {
    *dx = bdfi_sign(flag, (int)((((flag - 10) & 14) << 7) + bdfi_ws_u8(s)));
  } else if (flag < 84) {
    unsigned b0 = flag - 20, b1 = bdfi_ws_u8(s);
    *dx = bdfi_sign(flag, (int)(1 + (b0 & 0x30) + (b1 >> 4)));
    *dy = bdfi_sign(flag >> 1, (int)(1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f)));
  } else if (flag < 120) {
    unsigned b0 = flag - 84;
    *dx = bdfi_sign(flag, (int)(1 + ((b0 / 12) << 8) + bdfi_ws_u8(s)));
    *dy = bdfi_sign(flag >> 1, (int)(1 + (((b0 % 12) >> 2) << 8) + bdfi_ws_u8(s)));
  } else if (flag < 124) {
    unsigned b1 = bdfi_ws_u8(s), b2 = bdfi_ws_u8(s), b3 = bdfi_ws_u8(s);
    *dx = bdfi_sign(flag, (int)((b1 << 4) + (b2 >> 4)));
    *dy = bdfi_sign(flag >> 1, (int)(((b2 & 0x0f) << 8) + b3));
  } else {
    unsigned b1 = bdfi_ws_u8(s), b2 = bdfi_ws_u8(s), b3 = bdfi_ws_u8(s), b4 = bdfi_ws_u8(s);
    *dx = bdfi_sign(flag, (int)((b1 << 8) + b2));
    *dy = bdfi_sign(flag >> 1, (int)((b3 << 8) + b4));
  }
}

/* The working memory of one simple glyph: at most 65535 points. */
typedef struct bdfi_glyph_scratch {
  uint16_t *ends;
  int32_t *xs, *ys;
  uint8_t *on;
  size_t contours, points;
} bdfi_glyph_scratch;

/* Reuse only as much scratch as the largest glyph so far needs. A font
 * containing empty or composite glyphs needs none. On failure, every
 * successful allocation remains owned by g. */
static int bdfi_glyph_scratch_reserve(bdfi_glyph_scratch *g, size_t contours, size_t points) {
  if (contours > g->contours) {
    uint16_t *p = bdfi_realloc_array(g->ends, contours, sizeof *p);
    if (!p) {
      return -1;
    }
    g->ends = p;
    g->contours = contours;
  }
  if (points > g->points) {
    int32_t *p = bdfi_realloc_array(g->xs, points, sizeof *p);
    if (!p) {
      return -1;
    }
    g->xs = p;
    p = bdfi_realloc_array(g->ys, points, sizeof *p);
    if (!p) {
      return -1;
    }
    g->ys = p;
    uint8_t *on = bdfi_realloc_array(g->on, points, sizeof *on);
    if (!on) {
      return -1;
    }
    g->on = on;
    g->points = points;
  }
  return 0;
}

static void bdfi_glyph_scratch_free(bdfi_glyph_scratch *g) {
  free(g->ends);
  free(g->xs);
  free(g->ys);
  free(g->on);
}

bdf_status bdfi_woff2_rebuild_glyf(const uint8_t *t, size_t n, uint8_t **glyf_out,
                                   size_t *glyf_len, uint8_t **loca_out, size_t *loca_len) {
  *glyf_out = NULL;
  *glyf_len = 0;
  *loca_out = NULL;
  *loca_len = 0;
  if (n < 36) {
    return BDF_ERR_FORMAT;
  }
  unsigned option_flags = bdfi_be16(t + 2);
  size_t num_glyphs = bdfi_be16(t + 4);
  size_t pos = 36;
  bdfi_ws ss[7];
  for (int i = 0; i < 7; i++) {
    size_t len = bdfi_be32(t + 8 + 4 * i);
    if (len > n - pos) {
      return BDF_ERR_FORMAT;
    }
    ss[i].b = t + pos;
    ss[i].n = len;
    ss[i].pos = 0;
    ss[i].err = 0;
    pos += len;
  }
  bdfi_ws *n_contour = &ss[0], *n_points = &ss[1], *flag_s = &ss[2], *glyph_s = &ss[3],
          *comp_s = &ss[4], *bbox_s = &ss[5], *instr_s = &ss[6];
  size_t bitmap_len = ((num_glyphs + 31) >> 5) << 2;
  const uint8_t *bbox_bitmap = bdfi_ws_bytes(bbox_s, bitmap_len);
  const uint8_t *overlap = NULL;
  if (option_flags & 1) {
    size_t olen = (num_glyphs + 7) >> 3;
    if (olen > n - pos) {
      return BDF_ERR_FORMAT;
    }
    overlap = t + pos;
  }
  if (bbox_s->err) {
    return BDF_ERR_FORMAT;
  }

  bdf_status st = BDF_ERR_FORMAT;
  bdfi_wbuf glyf = {0};
  uint8_t *loca = malloc((num_glyphs + 1) * 4);
  bdfi_glyph_scratch sc = {0};
  if (!loca) {
    st = BDF_ERR_NOMEM;
    goto done;
  }
  for (size_t g = 0; g < num_glyphs; g++) {
    if (glyf.n > UINT32_MAX || glyf.n > BDFI_WOFF2_MAX_SIZE) {
      goto done; /* a glyf table no loca can address */
    }
    bdfi_put32(loca + 4 * g, (uint32_t)glyf.n);
    int has_bbox = (bbox_bitmap[g >> 3] & (0x80 >> (g & 7))) != 0;
    int nc = (int16_t)bdfi_ws_u16(n_contour);
    if (nc == 0) {
      if (has_bbox) {
        goto done;
      }
    } else if (nc < 0) {
      /* a composite glyph: its component records, as they are */
      if (!has_bbox) {
        goto done;
      }
      bdfi_wbuf_u16(&glyf, 0xffff);
      bdfi_wbuf_bytes(&glyf, bdfi_ws_bytes(bbox_s, 8), 8);
      int have_instr = 0;
      for (;;) {
        unsigned flags = bdfi_ws_u16(comp_s);
        size_t size = 4; /* glyph index and two byte arguments */
        if (flags & BDFI_COMP_ARGS_ARE_WORDS) {
          size += 2;
        }
        if (flags & BDFI_COMP_HAVE_SCALE) {
          size += 2;
        } else if (flags & BDFI_COMP_HAVE_XY_SCALE) {
          size += 4;
        } else if (flags & BDFI_COMP_HAVE_2X2) {
          size += 8;
        }
        bdfi_wbuf_u16(&glyf, (uint16_t)flags);
        bdfi_wbuf_bytes(&glyf, bdfi_ws_bytes(comp_s, size), size);
        if (flags & BDFI_COMP_HAVE_INSTR) {
          have_instr = 1;
        }
        if (comp_s->err || (flags & BDFI_COMP_MORE) == 0) {
          break;
        }
      }
      if (have_instr) {
        unsigned ilen = bdfi_ws_u255(glyph_s);
        bdfi_wbuf_u16(&glyf, (uint16_t)ilen);
        bdfi_wbuf_bytes(&glyf, bdfi_ws_bytes(instr_s, ilen), ilen);
      }
    } else {
      /* a simple glyph: contours, points, instructions */
      if ((size_t)nc > n_points->n - n_points->pos) {
        goto done;
      }
      if (bdfi_glyph_scratch_reserve(&sc, (size_t)nc, 0) < 0) {
        st = BDF_ERR_NOMEM;
        goto done;
      }
      size_t total = 0;
      for (int i = 0; i < nc; i++) {
        total += bdfi_ws_u255(n_points);
        if (total > 0xffff || n_points->err) {
          goto done;
        }
        sc.ends[i] = (uint16_t)(total - 1);
      }
      if (total > flag_s->n - flag_s->pos) {
        goto done;
      }
      if (bdfi_glyph_scratch_reserve(&sc, (size_t)nc, total) < 0) {
        st = BDF_ERR_NOMEM;
        goto done;
      }
      int x = 0, y = 0;
      for (size_t i = 0; i < total; i++) {
        unsigned f = bdfi_ws_u8(flag_s);
        sc.on[i] = (f & 0x80) == 0;
        int dx, dy;
        bdfi_triplet(f & 0x7f, glyph_s, &dx, &dy);
        int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
        if (nx < INT32_MIN || nx > INT32_MAX || ny < INT32_MIN || ny > INT32_MAX ||
            glyph_s->err) {
          goto done;
        }
        x = (int32_t)nx;
        y = (int32_t)ny;
        sc.xs[i] = x;
        sc.ys[i] = y;
      }
      unsigned ilen = bdfi_ws_u255(glyph_s);
      const uint8_t *instr = bdfi_ws_bytes(instr_s, ilen);
      int x_min = 0, y_min = 0, x_max = 0, y_max = 0;
      if (has_bbox) {
        const uint8_t *b = bdfi_ws_bytes(bbox_s, 8);
        if (b) {
          x_min = (int16_t)bdfi_be16(b);
          y_min = (int16_t)bdfi_be16(b + 2);
          x_max = (int16_t)bdfi_be16(b + 4);
          y_max = (int16_t)bdfi_be16(b + 6);
        }
      } else if (total > 0) {
        x_min = x_max = sc.xs[0];
        y_min = y_max = sc.ys[0];
        for (size_t i = 1; i < total; i++) {
          if (sc.xs[i] < x_min) {
            x_min = sc.xs[i];
          }
          if (sc.xs[i] > x_max) {
            x_max = sc.xs[i];
          }
          if (sc.ys[i] < y_min) {
            y_min = sc.ys[i];
          }
          if (sc.ys[i] > y_max) {
            y_max = sc.ys[i];
          }
        }
      }
      bdfi_wbuf_u16(&glyf, (uint16_t)nc);
      bdfi_wbuf_u16(&glyf, (uint16_t)x_min);
      bdfi_wbuf_u16(&glyf, (uint16_t)y_min);
      bdfi_wbuf_u16(&glyf, (uint16_t)x_max);
      bdfi_wbuf_u16(&glyf, (uint16_t)y_max);
      for (int i = 0; i < nc; i++) {
        bdfi_wbuf_u16(&glyf, (uint16_t)sc.ends[i]);
      }
      bdfi_wbuf_u16(&glyf, (uint16_t)ilen);
      bdfi_wbuf_bytes(&glyf, instr, ilen);
      /* flags, then coordinates as 16-bit deltas */
      int overlap_bit = overlap && (overlap[g >> 3] & (0x80 >> (g & 7))) != 0;
      for (size_t i = 0; i < total; i++) {
        uint8_t f = sc.on[i] ? BDFI_GLYF_ON_CURVE : 0;
        if (i == 0 && overlap_bit) {
          f |= BDFI_GLYF_OVERLAP;
        }
        bdfi_wbuf_u8(&glyf, f);
      }
      int px = 0;
      for (size_t i = 0; i < total; i++) {
        bdfi_wbuf_u16(&glyf, (uint16_t)(sc.xs[i] - px));
        px = sc.xs[i];
      }
      int py = 0;
      for (size_t i = 0; i < total; i++) {
        bdfi_wbuf_u16(&glyf, (uint16_t)(sc.ys[i] - py));
        py = sc.ys[i];
      }
    }
    static const uint8_t padding[3] = {0};
    bdfi_wbuf_bytes(&glyf, padding, (4 - glyf.n % 4) % 4);
    if (glyf.oom) {
      st = BDF_ERR_NOMEM;
      goto done;
    }
    if (glyf.limited) {
      goto done;
    }
    /* a stream that ended reads as zeros: stop at the glyph that read past
     * it, before the glyphs after it are made of nothing */
    for (int i = 0; i < 7; i++) {
      if (ss[i].err) {
        goto done;
      }
    }
  }
  if (glyf.n > UINT32_MAX) {
    goto done;
  }
  bdfi_put32(loca + 4 * num_glyphs, (uint32_t)glyf.n);
  *glyf_out = glyf.b ? glyf.b : malloc(1);
  if (!*glyf_out) {
    st = BDF_ERR_NOMEM;
    goto done;
  }
  *glyf_len = glyf.n;
  *loca_out = loca;
  *loca_len = (num_glyphs + 1) * 4;
  glyf.b = NULL;
  loca = NULL;
  st = BDF_OK;
done:
  free(glyf.b);
  free(loca);
  bdfi_glyph_scratch_free(&sc);
  return st;
}

/* ---- The hmtx transform (WOFF2 §5.4) ------------------------------------ */

/* bdfi_glyph_xmin is the xMin of a glyph's header, or 0 for an empty glyph or
 * one that loca does not reach. */
static int bdfi_glyph_xmin(const uint8_t *glyf, size_t glyf_len, const uint8_t *loca,
                           size_t loca_len, size_t g) {
  if (loca_len < (g + 2) * 4) {
    return 0;
  }
  uint32_t o = bdfi_be32(loca + g * 4), next = bdfi_be32(loca + g * 4 + 4);
  if ((size_t)o + 10 > glyf_len || o == next) {
    return 0;
  }
  return (int16_t)bdfi_be16(glyf + o + 2);
}

bdf_status bdfi_woff2_rebuild_hmtx(const uint8_t *t, size_t n, const uint8_t *hhea,
                                   size_t hhea_len, const uint8_t *maxp, size_t maxp_len,
                                   const uint8_t *glyf, size_t glyf_len, const uint8_t *loca,
                                   size_t loca_len, uint8_t **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0;
  if (n < 1 || hhea_len < 36 || maxp_len < 6) {
    return BDF_ERR_FORMAT;
  }
  size_t num_h = bdfi_be16(hhea + 34), num_glyphs = bdfi_be16(maxp + 4);
  if (num_h < 1 || num_h > num_glyphs) {
    return BDF_ERR_FORMAT;
  }
  bdfi_ws s = {t, n, 1, 0};
  unsigned flags = t[0];
  uint16_t *adv = malloc(num_h * sizeof *adv);
  int16_t *lsb = malloc(num_glyphs * sizeof *lsb);
  uint8_t *o = malloc(num_h * 4 + (num_glyphs - num_h) * 2 + 1);
  bdf_status st = BDF_ERR_FORMAT;
  if (!adv || !lsb || !o) {
    st = BDF_ERR_NOMEM;
    goto done;
  }
  for (size_t i = 0; i < num_h; i++) {
    adv[i] = (uint16_t)bdfi_ws_u16(&s);
  }
  for (size_t g = 0; g < num_glyphs; g++) {
    if ((g < num_h && (flags & 1) == 0) || (g >= num_h && (flags & 2) == 0)) {
      lsb[g] = (int16_t)bdfi_ws_u16(&s);
    } else {
      lsb[g] = (int16_t)bdfi_glyph_xmin(glyf, glyf_len, loca, loca_len, g);
    }
  }
  if (s.err) {
    goto done;
  }
  size_t k = 0;
  for (size_t g = 0; g < num_glyphs; g++) {
    if (g < num_h) {
      bdfi_put16(o + k, adv[g]);
      k += 2;
    }
    bdfi_put16(o + k, (uint16_t)lsb[g]);
    k += 2;
  }
  *out = o;
  *out_len = k;
  o = NULL;
  st = BDF_OK;
done:
  free(adv);
  free(lsb);
  free(o);
  return st;
}

/* ---- The font ----------------------------------------------------------- */

typedef struct bdfi_w2_entry {
  char tag[4];
  unsigned version;
  uint32_t orig_len, trans_len;
  int transformed;
  size_t offset, stream_size;
} bdfi_w2_entry;

typedef struct bdfi_w2_table {
  char tag[4];
  const uint8_t *p;
  size_t n;
  uint8_t *owned; /* when the bytes are the table's own */
} bdfi_w2_table;

typedef struct bdfi_w2_tables {
  bdfi_w2_table *t;
  size_t n, cap;
} bdfi_w2_tables;

static bdfi_w2_table *bdfi_w2_find(bdfi_w2_tables *ts, const char tag[4]) {
  for (size_t i = 0; i < ts->n; i++) {
    if (memcmp(ts->t[i].tag, tag, 4) == 0) {
      return &ts->t[i];
    }
  }
  return NULL;
}

/* bdfi_w2_set stores a table, replacing one of the same tag (the last entry
 * of a tag counts, as in the reference's map); owned bytes are taken over. */
static int bdfi_w2_set(bdfi_w2_tables *ts, const char tag[4], const uint8_t *p, size_t n,
                       uint8_t *owned) {
  bdfi_w2_table *t = bdfi_w2_find(ts, tag);
  if (!t) {
    if (ts->n == ts->cap) {
      free(owned);
      return -1;
    }
    t = &ts->t[ts->n++];
    memcpy(t->tag, tag, 4);
    t->owned = NULL;
  }
  free(t->owned);
  t->p = p;
  t->n = n;
  t->owned = owned;
  return 0;
}

static int bdfi_w2_cmp(const void *a, const void *b) {
  return memcmp(((const bdfi_w2_table *)a)->tag, ((const bdfi_w2_table *)b)->tag, 4);
}

static uint32_t bdfi_sfnt_checksum(const uint8_t *b, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i += 4) {
    uint8_t w[4] = {0, 0, 0, 0};
    memcpy(w, b + i, n - i < 4 ? n - i : 4);
    sum += bdfi_be32(w);
  }
  return sum;
}

/* bdfi_build_sfnt writes the font with its tables in tag order. */
static bdf_status bdfi_build_sfnt(uint32_t flavor, bdfi_w2_tables *ts, uint8_t **out,
                                  size_t *out_len) {
  qsort(ts->t, ts->n, sizeof *ts->t, bdfi_w2_cmp);
  size_t n = ts->n;
  size_t total = 12 + 16 * n;
  for (size_t i = 0; i < n; i++) {
    size_t padded = (ts->t[i].n + 3) & ~(size_t)3;
    if (padded < ts->t[i].n || padded > SIZE_MAX - total) {
      return BDF_ERR_FORMAT;
    }
    total += padded;
  }
  if (total > UINT32_MAX || total > BDFI_WOFF2_MAX_SIZE) {
    return BDF_ERR_FORMAT; /* offsets are 32-bit */
  }
  uint8_t *o = calloc(total, 1);
  if (!o) {
    return BDF_ERR_NOMEM;
  }
  unsigned es = 0, sr = 1;
  while (sr * 2 <= n) {
    sr *= 2;
    es++;
  }
  bdfi_put32(o, flavor);
  bdfi_put16(o + 4, (uint16_t)n);
  bdfi_put16(o + 6, (uint16_t)(sr * 16));
  bdfi_put16(o + 8, (uint16_t)es);
  bdfi_put16(o + 10, (uint16_t)(n * 16 - sr * 16));
  size_t off = 12 + 16 * n;
  for (size_t i = 0; i < n; i++) {
    uint8_t *rec = o + 12 + 16 * i;
    memcpy(rec, ts->t[i].tag, 4);
    bdfi_put32(rec + 4, bdfi_sfnt_checksum(ts->t[i].p, ts->t[i].n));
    bdfi_put32(rec + 8, (uint32_t)off);
    bdfi_put32(rec + 12, (uint32_t)ts->t[i].n);
    if (ts->t[i].n > 0) {
      memcpy(o + off, ts->t[i].p, ts->t[i].n);
    }
    off += (ts->t[i].n + 3) & ~(size_t)3;
  }
  *out = o;
  *out_len = total;
  return BDF_OK;
}

static void bdfi_w2_tables_free(bdfi_w2_tables *ts) {
  for (size_t i = 0; i < ts->n; i++) {
    free(ts->t[i].owned);
  }
  free(ts->t);
}

bdf_status bdf_woff2_decode(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len,
                            bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || !out_len || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  if (len < 48 || memcmp(data, "wOF2", 4) != 0) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "not a WOFF2 font");
    return BDF_ERR_FORMAT;
  }
  uint32_t flavor = bdfi_be32(data + 4);
  if (flavor == 0x74746366) { /* 'ttcf' */
    bdfi_error_set(err, BDF_ERR_UNSUPPORTED, 4, "font collections are not supported");
    return BDF_ERR_UNSUPPORTED;
  }
  size_t num_tables = bdfi_be16(data + 12);
  size_t compressed_len = bdfi_be32(data + 20);
  bdfi_w2_entry *entries = calloc(num_tables ? num_tables : 1, sizeof *entries);
  uint8_t *stream = NULL;
  bdfi_w2_tables ts = {0};
  bdf_status st = BDF_ERR_FORMAT;
  const char *msg = "malformed WOFF2 font";
  size_t pos = 48, size = 0, fail_at = 0;
  if (!entries) {
    st = BDF_ERR_NOMEM;
    msg = "out of memory";
    goto done;
  }
  for (size_t i = 0; i < num_tables; i++) {
    bdfi_w2_entry *e = &entries[i];
    fail_at = pos;
    if (pos >= len) {
      goto done;
    }
    uint8_t flags = data[pos++];
    e->version = flags >> 6;
    unsigned idx = flags & 0x3f;
    if (idx == 0x3f) {
      if (len - pos < 4) {
        goto done;
      }
      memcpy(e->tag, data + pos, 4);
      pos += 4;
    } else {
      memcpy(e->tag, bdfi_woff2_known_tags[idx], 4);
    }
    if (bdfi_woff2_read_base128(data, len, &pos, &e->orig_len) < 0) {
      goto done;
    }
    if (memcmp(e->tag, "glyf", 4) == 0 || memcmp(e->tag, "loca", 4) == 0) {
      e->transformed = e->version == 0;
    } else {
      e->transformed = e->version != 0;
    }
    e->stream_size = e->orig_len;
    if (e->transformed) {
      if (bdfi_woff2_read_base128(data, len, &pos, &e->trans_len) < 0) {
        goto done;
      }
      e->stream_size = e->trans_len;
    }
    size += e->stream_size;
    if (size > BDFI_WOFF2_MAX_SIZE) {
      msg = "the tables of the font are too large";
      goto done;
    }
  }
  fail_at = pos;
  if (compressed_len > len - pos) {
    goto done;
  }
  st = bdfi_brotli_decompress(data + pos, compressed_len, size, &stream, err);
  if (st != BDF_OK) {
    free(entries);
    return st;
  }
  st = BDF_ERR_FORMAT;
  ts.cap = num_tables + 2; /* glyf and loca may come from a transform */
  ts.t = calloc(ts.cap, sizeof *ts.t);
  if (!ts.t) {
    st = BDF_ERR_NOMEM;
    msg = "out of memory";
    goto done;
  }
  size_t off = 0;
  for (size_t i = 0; i < num_tables; i++) {
    bdfi_w2_entry *e = &entries[i];
    if (e->stream_size > size - off) {
      goto done;
    }
    e->offset = off;
    off += e->stream_size;
    if (!e->transformed && bdfi_w2_set(&ts, e->tag, stream + e->offset, e->stream_size, NULL) < 0) {
      goto done;
    }
  }
  for (size_t i = 0; i < num_tables; i++) {
    bdfi_w2_entry *e = &entries[i];
    if (!e->transformed) {
      continue;
    }
    if (memcmp(e->tag, "glyf", 4) == 0) {
      uint8_t *glyf, *loca;
      size_t glyf_len, loca_len;
      st = bdfi_woff2_rebuild_glyf(stream + e->offset, e->stream_size, &glyf, &glyf_len, &loca,
                                   &loca_len);
      if (st != BDF_OK) {
        msg = st == BDF_ERR_NOMEM ? "out of memory" : "malformed WOFF2 font";
        goto done;
      }
      st = BDF_ERR_FORMAT;
      if (bdfi_w2_set(&ts, "glyf", glyf, glyf_len, glyf) < 0) {
        free(loca);
        goto done;
      }
      if (bdfi_w2_set(&ts, "loca", loca, loca_len, loca) < 0) {
        goto done;
      }
      bdfi_w2_table *head = bdfi_w2_find(&ts, "head");
      if (head && head->n >= 54) {
        uint8_t *h = malloc(head->n);
        if (!h) {
          st = BDF_ERR_NOMEM;
          msg = "out of memory";
          goto done;
        }
        memcpy(h, head->p, head->n);
        bdfi_put16(h + 50, 1); /* loca is rebuilt in the long format */
        bdfi_w2_set(&ts, "head", h, head->n, h);
      }
    } else if (memcmp(e->tag, "loca", 4) == 0 || memcmp(e->tag, "hmtx", 4) == 0) {
      /* loca comes with glyf; hmtx once glyf is known */
    } else {
      msg = "unknown WOFF2 table transform";
      goto done;
    }
  }
  for (size_t i = 0; i < num_tables; i++) {
    bdfi_w2_entry *e = &entries[i];
    if (!e->transformed || memcmp(e->tag, "hmtx", 4) != 0) {
      continue;
    }
    const bdfi_w2_table *hhea = bdfi_w2_find(&ts, "hhea"), *maxp = bdfi_w2_find(&ts, "maxp"),
                        *glyf = bdfi_w2_find(&ts, "glyf"), *loca = bdfi_w2_find(&ts, "loca");
    uint8_t *h;
    size_t h_len;
    st = bdfi_woff2_rebuild_hmtx(stream + e->offset, e->stream_size, hhea ? hhea->p : NULL,
                                 hhea ? hhea->n : 0, maxp ? maxp->p : NULL, maxp ? maxp->n : 0,
                                 glyf ? glyf->p : NULL, glyf ? glyf->n : 0, loca ? loca->p : NULL,
                                 loca ? loca->n : 0, &h, &h_len);
    if (st != BDF_OK) {
      msg = st == BDF_ERR_NOMEM ? "out of memory" : "malformed WOFF2 font";
      goto done;
    }
    st = BDF_ERR_FORMAT;
    if (bdfi_w2_set(&ts, "hmtx", h, h_len, h) < 0) {
      goto done;
    }
  }
  st = bdfi_build_sfnt(flavor, &ts, out, out_len);
  if (st != BDF_OK) {
    msg = st == BDF_ERR_NOMEM ? "out of memory" : "malformed WOFF2 font";
    goto done;
  }
  fail_at = 0;
done:
  free(entries);
  free(stream);
  bdfi_w2_tables_free(&ts);
  if (st != BDF_OK) {
    bdfi_error_set(err, st, fail_at, msg);
  }
  return st;
}

bdf_status bdf_font_sfnt(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len,
                         bdf_error *err) {
  bdfi_error_clear(err);
  if (!out || !out_len || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = NULL;
  *out_len = 0;
  if (len >= 4 && memcmp(data, "wOF2", 4) == 0) {
    return bdf_woff2_decode(data, len, out, out_len, err);
  }
  if (len >= 4 && memcmp(data, "wOFF", 4) == 0) {
    bdfi_error_set(err, BDF_ERR_UNSUPPORTED, 0, "WOFF 1.0 fonts are not supported");
    return BDF_ERR_UNSUPPORTED;
  }
  if (len < 4 || !(bdfi_be32(data) == 0x00010000 || memcmp(data, "OTTO", 4) == 0 ||
                   memcmp(data, "true", 4) == 0 || memcmp(data, "ttcf", 4) == 0)) {
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "not a font");
    return BDF_ERR_FORMAT;
  }
  uint8_t *copy = malloc(len);
  if (!copy) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  memcpy(copy, data, len);
  *out = copy;
  *out_len = len;
  return BDF_OK;
}
