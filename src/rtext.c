/*
 * The text of the rasterizer (text.go of the reference's raster/imagebdf):
 * the faces a FONT instruction draws with, in the order characters are
 * looked up in them (the embedded font, the families its list names that
 * the application's fonts have, and fallbacks for the rest), and the
 * glyphs of a string placed along the baseline.
 */
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* ---- The faces of a renderer --------------------------------------------- */

/* What a renderer has read of a face of the application's fonts. */
typedef struct bdfi_rfile {
  bdfi_face *face;     /* read whole, or NULL */
  uint8_t state;       /* 0: not asked for; 1: read; 2: cannot be read */
  uint8_t cover_state; /* 0: not asked for; 1: there; 2: none */
  bdfi_coverage cover; /* the characters it has, from its cmap table alone */
  uint8_t *cmap;       /* the table, when the renderer read it from a file */
} bdfi_rfile;

BDF_INTERNAL void bdfi_r_fonts_free(struct bdf_renderer *r) {
  for (size_t i = 0; i < r->nfiles; i++) {
    bdfi_face_free(r->files[i].face);
    free(r->files[i].cmap);
  }
  free(r->files);
  for (size_t i = 0; i < r->nchains; i++) {
    free(r->chains[i]->family);
    free(r->chains[i]->faces);
    free(r->chains[i]->fallbacks);
    free(r->chains[i]);
  }
  free(r->chains);
  free(r->chain_slots);
}

/* fonts.part: the face of an embedded font part, or NULL (warned of once). */
static bdfi_face *bdfi_r_part_face(struct bdf_renderer *r, const uint8_t hash[16]) {
  char name[33];
  bdfi_robj *o = bdfi_r_part_slot(r, hash);
  bdf_hash_format(hash, name);
  if (!o) {
    if (!r->oom) {
      bdfi_r_warnf(r, "font %s is missing", name);
    }
    return NULL;
  }
  if (o->font_state != 0) {
    return o->font;
  }
  o->font_state = 2;
  uint8_t *data = NULL;
  size_t len = 0;
  bdf_error e;
  bdf_status st = bdf_doc_part(r->doc, hash, &data, &len, &e);
  if (st == BDF_OK && len >= 4 && memcmp(data, "wOFF", 4) == 0) {
    bdf_free(data);
    bdfi_r_warn(r, "WOFF 1 fonts are not drawn");
    return NULL;
  }
  if (st == BDF_OK && len >= 4 && memcmp(data, "wOF2", 4) == 0) {
    uint8_t *sfnt = NULL;
    size_t sfnt_len = 0;
    st = bdf_woff2_decode(data, len, &sfnt, &sfnt_len, &e);
    bdf_free(data);
    data = sfnt;
    len = sfnt_len;
  }
  if (st == BDF_ERR_NOMEM) {
    r->oom = 1;
    o->font_state = 0;
    return NULL;
  }
  if (st != BDF_OK) {
    bdfi_r_warnf(r, "font %s: %s", name, e.message);
    return NULL;
  }
  o->font = bdfi_face_new(data, len, 0, 1, &st);
  if (st == BDF_ERR_NOMEM) {
    r->oom = 1;
    o->font_state = 0;
    return NULL;
  }
  if (!o->font) {
    bdfi_r_warnf(r, "font %s cannot be read", name);
    return NULL;
  }
  o->font_state = 1;
  return o->font;
}

BDF_INTERNAL const uint8_t *bdfi_r_font_data(struct bdf_renderer *r, const uint8_t hash[16],
                                             size_t *len, uint64_t *id) {
  bdfi_face *f = bdfi_r_part_face(r, hash);
  *len = 0;
  *id = 0;
  if (!f) {
    return NULL;
  }
  /* the place of the part among the parts, which does not change */
  *id = (uint64_t)(bdfi_r_part_slot(r, hash) - r->parts) + 1;
  return bdfi_face_data(f, len);
}

static bdfi_rfile *bdfi_r_file(struct bdf_renderer *r, size_t face) {
  if (!r->files) {
    size_t n = bdf_fonts_count(r->fonts);
    r->files = bdfi_calloc_array(n ? n : 1, sizeof *r->files);
    if (!r->files) {
      r->oom = 1;
      return NULL;
    }
    r->nfiles = n;
  }
  return face < r->nfiles ? &r->files[face] : NULL;
}

/* fonts.file: a face of the application's fonts read whole (Face.Load: one
 * without a character map or without metrics has no glyphs to draw). */
static bdfi_face *bdfi_r_file_face(struct bdf_renderer *r, size_t face) {
  bdfi_rfile *f = bdfi_r_file(r, face);
  if (!f) {
    return NULL;
  }
  if (f->state != 0) {
    return f->face;
  }
  f->state = 2;
  const uint8_t *data;
  size_t len;
  int owned;
  int rc = bdfi_fonts_read(r->fonts, face, &data, &len, &owned);
  if (rc == 1) {
    bdf_status st;
    f->face = bdfi_face_new(data, len, bdfi_fonts_index(r->fonts, face), owned, &st);
    if (st == BDF_ERR_NOMEM) {
      rc = -1;
    } else if (f->face && !bdfi_face_has_metrics(f->face)) {
      bdfi_face_free(f->face);
      f->face = NULL;
    }
  }
  if (rc < 0) {
    r->oom = 1;
    f->state = 0;
    return NULL;
  }
  if (f->face) {
    f->state = 1;
  }
  return f->face;
}

/* Face.HasRune: whether a face has a character, asked of its cmap table
 * alone where that can be, so that looking for a fallback reads no font
 * that cannot draw the character. */
static int bdfi_r_has_rune(struct bdf_renderer *r, size_t face, uint32_t rune) {
  bdfi_rfile *f = bdfi_r_file(r, face);
  if (!f) {
    return 0;
  }
  if (f->state == 1) {
    return bdfi_face_glyph(f->face, rune) != 0;
  }
  if (f->cover_state == 0) {
    /* (set when there is a table, which gcc at -O1 cannot tell) */
    const uint8_t *data = NULL;
    size_t len = 0;
    int owned = 0;
    int rc = bdfi_fonts_cmap(r->fonts, face, &data, &len, &owned);
    if (rc < 0) {
      r->oom = 1;
      return 0;
    }
    f->cover_state = 2;
    if (rc == 1) {
      if (owned) {
        f->cmap = (uint8_t *)(uintptr_t)data;
      }
      if (bdfi_coverage_parse(data, len, &f->cover)) {
        f->cover_state = 1;
      }
    }
  }
  if (f->cover_state != 1) {
    /* a table that is not read this way: the whole face answers */
    if (!bdfi_fonts_has_cmap(r->fonts, face)) {
      return 0;
    }
    bdfi_face *whole = bdfi_r_file_face(r, face);
    return whole && bdfi_face_glyph(whole, rune) != 0;
  }
  return bdfi_coverage_has(&f->cover, rune);
}

/* ---- Chains -------------------------------------------------------------- */

static int bdfi_chain_add(struct bdf_renderer *r, bdfi_chain *c, bdfi_face *face, int bold,
                          int italic) {
  if (!face) {
    return 0;
  }
  for (size_t i = 0; i < c->nfaces; i++) {
    if (c->faces[i].face == face) {
      return 0;
    }
  }
  if (c->nfaces == c->cap_faces) {
    size_t cap = c->cap_faces ? 2 * c->cap_faces : 4;
    bdfi_face_use *faces = bdfi_realloc_array(c->faces, cap, sizeof *faces);
    if (!faces) {
      r->oom = 1;
      return -1;
    }
    c->faces = faces;
    c->cap_faces = cap;
  }
  c->faces[c->nfaces].face = face;
  c->faces[c->nfaces].bold = bold;
  c->faces[c->nfaces].italic = italic;
  c->nfaces++;
  return 0;
}

/* genericFamilies: the generic family a CSS keyword stands for, or -1. */
static int bdfi_css_generic(const char *name, size_t n) {
  static const struct {
    const char *word;
    int generic;
  } words[] = {
      {"sans-serif", BDFI_GENERIC_SANS},    {"serif", BDFI_GENERIC_SERIF},
      {"monospace", BDFI_GENERIC_MONO},     {"cursive", BDFI_GENERIC_SANS},
      {"fantasy", BDFI_GENERIC_SANS},       {"system-ui", BDFI_GENERIC_SANS},
      {"math", BDFI_GENERIC_SERIF},         {"ui-sans-serif", BDFI_GENERIC_SANS},
      {"ui-serif", BDFI_GENERIC_SERIF},     {"ui-monospace", BDFI_GENERIC_MONO},
      {"ui-rounded", BDFI_GENERIC_SANS},
  };
  /* in lower case, a character at a time: none of the words is longer */
  char low[16];
  size_t w = 0;
  for (size_t i = 0; i < n;) {
    size_t k;
    uint32_t c = bdfi_to_lower(bdfi_utf8_rune(name + i, n - i, &k));
    i += k;
    if (c >= 0x80 || w + 1 >= sizeof low) {
      return -1;
    }
    low[w++] = (char)c;
  }
  low[w] = '\0';
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    if (strcmp(low, words[i].word) == 0) {
      return words[i].generic;
    }
  }
  return -1;
}

/*
 * splitFamilies: the next name of a CSS font-family list, without its
 * quotes, in buf (which is as long as the list). 0 at the end of the list.
 */
static int bdfi_css_next_family(const char *list, size_t n, size_t *pos, char *buf, size_t *len) {
  while (*pos <= n) {
    size_t from = *pos, to = from;
    while (to < n && list[to] != ',') {
      to++;
    }
    *pos = to + 1;
    /* strings.TrimSpace */
    while (from < to) {
      size_t k;
      if (!bdfi_go_is_space(bdfi_utf8_rune(list + from, to - from, &k))) {
        break;
      }
      from += k;
    }
    while (to > from) {
      size_t start = to - 1;
      while (start > from && to - start < 4 && ((uint8_t)list[start] & 0xC0) == 0x80) {
        start--;
      }
      size_t k;
      uint32_t c = bdfi_utf8_rune(list + start, to - start, &k);
      if (start + k != to) {
        break; /* bytes that are no character: no space */
      }
      if (!bdfi_go_is_space(c)) {
        break;
      }
      to = start;
    }
    size_t w = 0;
    if (to - from >= 2 && (list[from] == '"' || list[from] == '\'') && list[to - 1] == list[from]) {
      char q = list[from];
      for (size_t i = from + 1; i < to - 1; i++) {
        if (list[i] == '\\' && i + 1 < to - 1 && list[i + 1] == q) {
          continue; /* an escaped quote is the quote */
        }
        buf[w++] = list[i];
      }
    } else {
      memcpy(buf, list + from, to - from);
      w = to - from;
    }
    if (w > 0) {
      *len = w;
      return 1;
    }
  }
  return 0;
}

/* The families of a font that a chain is made for once, when a character
 * is not in the faces it has (the func init of fontChain). */
static void bdfi_chain_init(struct bdf_renderer *r, bdfi_chain *c) {
  c->inited = 1;
  const bdf_fonts *db = r->fonts;
  char *name = malloc(c->family_len + 1);
  if (!name) {
    r->oom = 1;
    return;
  }
  int generic = -1;
  size_t pos = 0, n;
  bdfi_resolved res;
  while (bdfi_css_next_family(c->family, c->family_len, &pos, name, &n)) {
    int g = bdfi_css_generic(name, n);
    int rc;
    if (g >= 0) {
      static const char *const words[] = {"sans-serif", "serif", "monospace"};
      if (generic < 0) {
        generic = g;
      }
      rc = bdfi_fonts_resolve(db, words[g], strlen(words[g]), c->bold, c->italic, &res);
      if (rc == 0 && res.face != SIZE_MAX) {
        bdfi_chain_add(r, c, bdfi_r_file_face(r, res.face), res.synth_bold, res.synth_italic);
      }
    } else {
      rc = bdfi_fonts_family(db, name, n, c->bold, c->italic, &res);
      if (rc == 1) {
        bdfi_chain_add(r, c, bdfi_r_file_face(r, res.face), res.synth_bold, res.synth_italic);
      }
    }
    if (rc < 0) {
      r->oom = 1;
    }
  }
  if (generic < 0) {
    generic = BDFI_GENERIC_SANS;
    if (c->nfaces == 0) {
      /* a family that is not there: its look-alike or generic kind */
      pos = 0;
      while (bdfi_css_next_family(c->family, c->family_len, &pos, name, &n)) {
        if (bdfi_fonts_resolve(db, name, n, c->bold, c->italic, &res) < 0) {
          r->oom = 1;
          break;
        }
        if (res.face != SIZE_MAX) {
          generic = res.generic;
          bdfi_chain_add(r, c, bdfi_r_file_face(r, res.face), res.synth_bold, res.synth_italic);
          break;
        }
      }
    }
  }
  free(name);
  if (bdfi_fonts_fallbacks(db, generic, c->bold, c->italic, &c->fallbacks, &c->nfallbacks) < 0) {
    r->oom = 1;
  }
  /* even text with no glyph the fonts have needs the advance of the
   * missing glyph and the metrics of the first face that can be used */
  for (size_t i = 0; c->nfaces == 0 && i < c->nfallbacks; i++) {
    bdfi_chain_add(r, c, bdfi_r_file_face(r, c->fallbacks[i].face), c->fallbacks[i].synth_bold,
                   c->fallbacks[i].synth_italic);
  }
  if (c->nfaces == 0 && !r->oom) {
    /* quoted as the reference's %q quotes what is ASCII */
    char quoted[160];
    size_t w = 0;
    for (size_t i = 0; i < c->family_len && w + 3 < sizeof quoted; i++) {
      if (c->family[i] == '"' || c->family[i] == '\\') {
        quoted[w++] = '\\';
      }
      quoted[w++] = c->family[i];
    }
    quoted[w] = '\0';
    bdfi_r_warnf(r, "no font for \"%s\": its text is not drawn", quoted);
  }
}

/* FNV-1a over what tells two font references apart. */
static uint64_t bdfi_chain_hash(uint8_t kind, uint16_t weight, uint8_t style,
                                const uint8_t part[16], const char *family, size_t len) {
  uint64_t h = UINT64_C(14695981039346656037);
  const uint8_t head[4] = {kind, (uint8_t)weight, (uint8_t)(weight >> 8), style};
  for (size_t i = 0; i < sizeof head; i++) {
    h = (h ^ head[i]) * UINT64_C(1099511628211);
  }
  for (size_t i = 0; i < 16; i++) {
    h = (h ^ part[i]) * UINT64_C(1099511628211);
  }
  for (size_t i = 0; i < len; i++) {
    h = (h ^ (uint8_t)family[i]) * UINT64_C(1099511628211);
  }
  return h;
}

/*
 * chain: the faces of a font reference, as a browser would find them: the
 * embedded face (registered without weight or style, so those are
 * synthesized), the families of its list that there are, and fallbacks for
 * the rest. NULL without a font is the font of a canvas that was given
 * none. NULL when memory ran out.
 */
BDF_INTERNAL bdfi_chain *bdfi_r_chain(struct bdf_renderer *r, const bdf_font *f) {
  static const bdf_font sans = {BDF_FONT_SYSTEM, NULL, "sans-serif", 10, 400, BDF_STYLE_NORMAL};
  static const uint8_t no_hash[16] = {0};
  if (!f) {
    f = &sans;
  }
  const uint8_t *part = f->kind == BDF_FONT_EMBEDDED && f->hash ? f->hash : no_hash;
  /* The chains are found by a hash of the font: a document may name as
   * many fonts as it has bytes for. */
  uint64_t hash = bdfi_chain_hash(f->kind, f->weight, f->style, part, f->family, f->family_len);
  if (r->nchain_slots > 0) {
    for (size_t at = (size_t)hash & (r->nchain_slots - 1); r->chain_slots[at] != 0;
         at = (at + 1) & (r->nchain_slots - 1)) {
      bdfi_chain *c = r->chains[r->chain_slots[at] - 1];
      if (c->kind == f->kind && c->weight == f->weight && c->style == f->style &&
          c->family_len == f->family_len &&
          (f->family_len == 0 || memcmp(c->family, f->family, f->family_len) == 0) &&
          memcmp(c->hash, part, 16) == 0) {
        return c;
      }
    }
  }
  bdfi_chain *c = calloc(1, sizeof *c);
  char *family = malloc(f->family_len + 1);
  if (r->nchains == r->cap_chains && c && family) {
    size_t cap = r->cap_chains ? 2 * r->cap_chains : 8;
    bdfi_chain **chains = bdfi_realloc_array(r->chains, cap, sizeof *chains);
    if (chains) {
      r->chains = chains;
      r->cap_chains = cap;
    }
  }
  /* slots for twice the chains at least: one is always free */
  int room = 2 * (r->nchains + 1) <= r->nchain_slots;
  if (!room && c && family && r->nchains < r->cap_chains) {
    size_t n = r->nchain_slots ? 2 * r->nchain_slots : 16;
    size_t *slots = bdfi_calloc_array(n, sizeof *slots);
    if (slots) {
      for (size_t i = 0; i < r->nchain_slots; i++) {
        if (r->chain_slots[i] != 0) {
          const bdfi_chain *old = r->chains[r->chain_slots[i] - 1];
          size_t at = (size_t)bdfi_chain_hash(old->kind, old->weight, old->style, old->hash,
                                              old->family, old->family_len) &
                      (n - 1);
          while (slots[at] != 0) {
            at = (at + 1) & (n - 1);
          }
          slots[at] = r->chain_slots[i];
        }
      }
      free(r->chain_slots);
      r->chain_slots = slots;
      r->nchain_slots = n;
      room = 1;
    }
  }
  if (!c || !family || r->nchains == r->cap_chains || !room) {
    free(c);
    free(family);
    r->oom = 1;
    return NULL;
  }
  if (f->family_len > 0) {
    memcpy(family, f->family, f->family_len);
  }
  family[f->family_len] = '\0';
  c->kind = f->kind;
  c->weight = f->weight;
  c->style = f->style;
  c->family = family;
  c->family_len = f->family_len;
  c->bold = f->weight >= 600;
  c->italic = f->style != BDF_STYLE_NORMAL;
  if (f->kind == BDF_FONT_EMBEDDED && f->hash) {
    memcpy(c->hash, f->hash, 16);
    bdfi_chain_add(r, c, bdfi_r_part_face(r, f->hash), c->bold, c->italic);
  }
  r->chains[r->nchains++] = c;
  size_t at = (size_t)hash & (r->nchain_slots - 1);
  while (r->chain_slots[at] != 0) {
    at = (at + 1) & (r->nchain_slots - 1);
  }
  r->chain_slots[at] = r->nchains;
  return c;
}

/* fontChain.glyph: the face that has a character, and its glyph there;
 * without one, the missing glyph of the first face. The chain has a face. */
static bdfi_face_use bdfi_chain_glyph(struct bdf_renderer *r, bdfi_chain *c, uint32_t rune,
                                      uint16_t *gid) {
  for (;;) {
    for (size_t i = 0; i < c->nfaces; i++) {
      uint16_t g = bdfi_face_glyph(c->faces[i].face, rune);
      if (g != 0) {
        *gid = g;
        return c->faces[i];
      }
    }
    if (c->inited) {
      break;
    }
    bdfi_chain_init(r, c); /* and the faces it found are asked */
  }
  for (size_t i = 0; i < c->nfallbacks; i++) {
    const bdfi_resolved *res = &c->fallbacks[i];
    if (!bdfi_r_has_rune(r, res->face, rune)) {
      continue;
    }
    bdfi_face *face = bdfi_r_file_face(r, res->face);
    uint16_t g = face ? bdfi_face_glyph(face, rune) : 0;
    if (g != 0) {
      bdfi_face_use use = {face, res->synth_bold, res->synth_italic};
      *gid = g;
      return use;
    }
  }
  *gid = 0;
  return c->faces[0];
}

/* ---- Layout -------------------------------------------------------------- */

/* hasRTL */
static int bdfi_text_has_rtl(const char *s, size_t n) {
  for (size_t i = 0; i < n;) {
    size_t k;
    uint32_t c = bdfi_utf8_rune(s + i, n - i, &k);
    i += k;
    if (c >= 0x590 && bdfi_bidi_direction(c) == 1) {
      return 1;
    }
  }
  return 0;
}

/*
 * visualOrder: the characters of a string in the order they are displayed.
 * Runs of right-to-left characters are reversed, and so is the order of the
 * runs of a right-to-left paragraph. It is what previews need, not the
 * whole bidirectional algorithm (no mirroring, no Arabic shaping). The
 * characters are the caller's to free; NULL when memory ran out.
 */
static uint32_t *bdfi_visual_order(const char *s, size_t len, int rtl_base, size_t *count) {
  uint32_t *runes = bdfi_malloc_array(len ? len : 1, sizeof *runes);
  if (!runes) {
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; i < len;) {
    size_t k;
    runes[n++] = bdfi_utf8_rune(s + i, len - i, &k);
    i += k;
  }
  *count = n;
  if (!rtl_base && !bdfi_text_has_rtl(s, len)) {
    return runes;
  }
  /* 0: left to right; 1: right to left; 2: neutral */
  uint8_t *dirs = bdfi_malloc_array(n ? n : 1, 1);
  uint32_t *out = bdfi_malloc_array(n ? n : 1, sizeof *out);
  if (!dirs || !out) {
    free(dirs);
    free(out);
    free(runes);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    int d = bdfi_bidi_direction(runes[i]);
    dirs[i] = (uint8_t)(d < 0 ? 2 : d);
  }
  uint8_t base = rtl_base ? 1 : 0;
  /* neutrals take the direction around them when both sides agree */
  for (size_t i = 0; i < n;) {
    if (dirs[i] != 2) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < n && dirs[j] == 2) {
      j++;
    }
    uint8_t before = i > 0 ? dirs[i - 1] : base, after = j < n ? dirs[j] : base;
    uint8_t d = before == after ? before : base;
    for (size_t k = i; k < j; k++) {
      dirs[k] = d;
    }
    i = j;
  }
  /* the runs, in their order or (in a right-to-left paragraph) backwards */
  size_t w = 0;
  if (base == 1) {
    for (size_t end = n; end > 0;) {
      size_t from = end;
      while (from > 0 && dirs[from - 1] == dirs[end - 1]) {
        from--;
      }
      if (dirs[from] == 1) {
        for (size_t k = end; k > from; k--) {
          out[w++] = runes[k - 1];
        }
      } else {
        for (size_t k = from; k < end; k++) {
          out[w++] = runes[k];
        }
      }
      end = from;
    }
  } else {
    for (size_t from = 0; from < n;) {
      size_t end = from;
      while (end < n && dirs[end] == dirs[from]) {
        end++;
      }
      if (dirs[from] == 1) {
        for (size_t k = end; k > from; k--) {
          out[w++] = runes[k - 1];
        }
      } else {
        for (size_t k = from; k < end; k++) {
          out[w++] = runes[k];
        }
      }
      from = end;
    }
  }
  free(dirs);
  free(runes);
  return out;
}

/*
 * layout: the glyphs of a string at a size with letter spacing, in visual
 * order, and the advance of the whole string. No glyphs when no font can be
 * used. The glyphs are the caller's to free; -1 when memory ran out.
 */
BDF_INTERNAL int bdfi_chain_layout(struct bdf_renderer *r, bdfi_chain *c, const char *text,
                                   size_t len, double size, double spacing, int rtl,
                                   bdfi_placed **out, size_t *count, double *width) {
  *out = NULL;
  *count = 0;
  *width = 0;
  if (c->nfaces == 0) {
    if (!c->inited) {
      bdfi_chain_init(r, c);
    }
    if (c->nfaces == 0) {
      return r->oom ? -1 : 0;
    }
  }
  size_t n;
  uint32_t *runes = bdfi_visual_order(text, len, rtl, &n);
  bdfi_placed *glyphs = runes ? bdfi_malloc_array(n ? n : 1, sizeof *glyphs) : NULL;
  if (!glyphs) {
    free(runes);
    r->oom = 1;
    return -1;
  }
  double x = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t rune = runes[i];
    if (rune == '\t' || rune == '\n' || rune == '\f' || rune == '\r' || rune == '\v') {
      rune = ' '; /* Canvas draws these as spaces */
    }
    glyphs[i].use = bdfi_chain_glyph(r, c, rune, &glyphs[i].gid);
    glyphs[i].x = x;
    x += bdfi_face_advance(glyphs[i].use.face, glyphs[i].gid) * size + spacing;
  }
  free(runes);
  if (r->oom) {
    free(glyphs);
    return -1;
  }
  *out = glyphs;
  *count = n;
  *width = x;
  return 0;
}

/* baselineShift: how far below the anchor the alphabetic baseline lies for
 * a textBaseline, from the em box of the first face (as Blink does). */
BDF_INTERNAL double bdfi_chain_baseline_shift(const bdfi_chain *c, uint8_t baseline, double size) {
  double asc = 0.8, dsc = 0.2;
  if (c->nfaces > 0) {
    asc = bdfi_face_ascent(c->faces[0].face);
    dsc = bdfi_face_descent(c->faces[0].face);
  }
  double em_asc = size * asc / (asc + dsc);
  double em_dsc = size - em_asc;
  switch (baseline) {
  case BDF_BASELINE_TOP:
    return em_asc;
  case BDF_BASELINE_HANGING:
    return asc * size * 0.8;
  case BDF_BASELINE_MIDDLE:
    return (em_asc - em_dsc) / 2;
  case BDF_BASELINE_IDEOGRAPHIC:
    return -dsc * size;
  case BDF_BASELINE_BOTTOM:
    return -em_dsc;
  default:
    return 0;
  }
}

/* fakeBoldWidth: how much a synthesized bold grows an outline, for a size
 * in device pixels (Skia's interpolation between 1/24 at 9 px and 1/32 at
 * 36 px). */
BDF_INTERNAL double bdfi_fake_bold_width(double size) {
  if (size <= 9) {
    return size / 24;
  }
  if (size >= 36) {
    return size / 32;
  }
  /* 1/24 and 1/32 - 1/24, each rounded once as the reference's compiler
   * rounds a constant */
  double k = 0.041666666666666664 + (size - 9) / 27 * -0.010416666666666666;
  return size * k;
}
