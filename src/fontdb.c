/*
 * The fonts a renderer draws text with when a document names them instead
 * of carrying them: a set of font files that the application gives, and the
 * reference's way of finding a face for a family in it (internal/fontdb of
 * bdf: fontdb.go and resolve.go): the family itself, a family that looks
 * alike, a generic family of the same kind, and for the characters a face
 * lacks the faces that may have them.
 *
 * A set is made once and then only read: renderers on several threads may
 * share one. What a renderer reads of a file it keeps for itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

/* ---- The set ------------------------------------------------------------- */

/* A font file: bytes the set holds, or a path that is read when needed. The
 * faces of a collection share one. */
typedef struct bdfi_fdb_file {
  uint8_t *data;
  size_t len;
  char *path;
} bdfi_fdb_file;

/* A face (fontdb.Face): one font of a file, with what it is found by. */
typedef struct bdfi_fdb_entry {
  size_t file;
  int index;    /* in a collection */
  char *family; /* the English family name (the typographic one when there is one) */
  int weight;   /* OS/2 usWeightClass */
  int italic;
  char **names; /* the family names in every language, folded */
  size_t nnames;
  int64_t cmap_off, cmap_len;
} bdfi_fdb_entry;

struct bdf_fonts {
  bdfi_fdb_file *files;
  size_t nfiles, cap_files;
  bdfi_fdb_entry *faces;
  size_t nfaces, cap_faces;
};

bdf_status bdf_fonts_new(bdf_fonts **out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = calloc(1, sizeof **out);
  if (!*out) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  return BDF_OK;
}

static void bdfi_fdb_entry_clear(bdfi_fdb_entry *e) {
  free(e->family);
  for (size_t i = 0; i < e->nnames; i++) {
    free(e->names[i]);
  }
  free(e->names);
}

void bdf_fonts_free(bdf_fonts *fonts) {
  if (!fonts) {
    return;
  }
  for (size_t i = 0; i < fonts->nfaces; i++) {
    bdfi_fdb_entry_clear(&fonts->faces[i]);
  }
  free(fonts->faces);
  for (size_t i = 0; i < fonts->nfiles; i++) {
    free(fonts->files[i].data);
    free(fonts->files[i].path);
  }
  free(fonts->files);
  free(fonts);
}

size_t bdf_fonts_count(const bdf_fonts *fonts) { return fonts ? fonts->nfaces : 0; }

/* ---- Reading a file ------------------------------------------------------ */

/* What a file is read from while its faces are looked at. */
typedef struct bdfi_fdb_src {
  const uint8_t *mem;
  size_t len;
  FILE *fp;
} bdfi_fdb_src;

/* ReadAt: n bytes at off, all of them or none. */
static int bdfi_fdb_read_at(bdfi_fdb_src *s, uint64_t off, uint8_t *buf, size_t n) {
  if (s->mem) {
    if (off > s->len || n > s->len - off) {
      return 0;
    }
    if (n > 0) {
      memcpy(buf, s->mem + off, n);
    }
    return 1;
  }
  if (off > 0x7fffffff || fseek(s->fp, (long)off, SEEK_SET) != 0) {
    return 0;
  }
  return fread(buf, 1, n, s->fp) == n;
}

static uint32_t bdfi_fd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static uint32_t bdfi_fd32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A name of the name table: its language and its text. */
typedef struct bdfi_fdb_name {
  uint32_t lang;
  char *val;
  size_t len;
} bdfi_fdb_name;

typedef struct bdfi_fdb_names {
  bdfi_fdb_name *v;
  size_t n, cap;
} bdfi_fdb_names;

static void bdfi_fdb_names_free(bdfi_fdb_names *l) {
  for (size_t i = 0; i < l->n; i++) {
    free(l->v[i].val);
  }
  free(l->v);
}

/*
 * A name record as text: UTF-16 of the Windows and Unicode platforms, or
 * the bytes of the Macintosh one as they are (family names are ASCII in
 * practice), without the white space around it. NULL when out of memory;
 * *len is 0 for a name that is empty.
 */
static char *bdfi_fdb_text(const uint8_t *raw, size_t n, int utf16, size_t *len) {
  char *s = bdfi_malloc_array(n / 2 + 1, utf16 ? 3 : 2);
  *len = 0;
  if (!s) {
    return NULL;
  }
  size_t w = 0;
  if (utf16) {
    for (size_t i = 0; i + 1 < n; i += 2) {
      uint32_t c = bdfi_fd16(raw + i);
      if (c >= 0xD800 && c < 0xDC00 && i + 3 < n && bdfi_fd16(raw + i + 2) >= 0xDC00 &&
          bdfi_fd16(raw + i + 2) < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (bdfi_fd16(raw + i + 2) - 0xDC00);
        i += 2;
      } else if (c >= 0xD800 && c < 0xE000) {
        c = 0xFFFD; /* half of a pair */
      }
      w += bdfi_utf8_put(c, s + w);
    }
  } else {
    memcpy(s, raw, n);
    w = n;
  }
  /* strings.TrimSpace */
  size_t from = 0;
  while (from < w) {
    size_t k;
    if (!bdfi_go_is_space(bdfi_utf8_rune(s + from, w - from, &k))) {
      break;
    }
    from += k;
  }
  size_t to = w;
  while (to > from) {
    /* the last character: its first byte is the last that is not a
     * continuation, within four bytes */
    size_t start = to - 1;
    while (start > from && to - start < 4 && ((uint8_t)s[start] & 0xC0) == 0x80) {
      start--;
    }
    size_t k;
    uint32_t c = bdfi_utf8_rune(s + start, to - start, &k);
    if (start + k != to) {
      start = to - 1; /* bytes that are no character end in one that is none */
      c = bdfi_utf8_rune(s + start, 1, &k);
    }
    if (!bdfi_go_is_space(c)) {
      break;
    }
    to = start;
  }
  memmove(s, s + from, to - from);
  s[to - from] = '\0';
  *len = to - from;
  return s;
}

/*
 * parseNames: the family and style names of a name table by name ID (1, 2,
 * 16 and 17, here 0 to 3), decoded from the Windows, Unicode and Macintosh
 * Roman platforms. -1 when out of memory.
 */
static int bdfi_fdb_parse_names(const uint8_t *b, size_t len, bdfi_fdb_names out[4]) {
  if (len < 6) {
    return 0;
  }
  size_t count = bdfi_fd16(b + 2), str_off = bdfi_fd16(b + 4);
  for (size_t i = 0; i < count; i++) {
    size_t rec = 6 + i * 12;
    if (rec + 12 > len) {
      break;
    }
    uint32_t pid = bdfi_fd16(b + rec), eid = bdfi_fd16(b + rec + 2);
    uint32_t lang = bdfi_fd16(b + rec + 4), id = bdfi_fd16(b + rec + 6);
    size_t l = bdfi_fd16(b + rec + 8), o = bdfi_fd16(b + rec + 10);
    int slot = id == 1 ? 0 : id == 2 ? 1 : id == 16 ? 2 : id == 17 ? 3 : -1;
    if (slot < 0 || str_off + o + l > len) {
      continue;
    }
    int utf16;
    if ((pid == 3 && (eid == 1 || eid == 10 || eid == 0)) || pid == 0) {
      utf16 = 1;
    } else if (pid == 1 && eid == 0) {
      utf16 = 0;
      lang = 0x0409;
    } else {
      continue;
    }
    size_t n;
    char *val = bdfi_fdb_text(b + str_off + o, l, utf16, &n);
    if (!val) {
      return -1;
    }
    if (n == 0) {
      free(val);
      continue;
    }
    bdfi_fdb_names *list = &out[slot];
    if (list->n == list->cap) {
      size_t cap = list->cap ? 2 * list->cap : 4;
      bdfi_fdb_name *v = bdfi_realloc_array(list->v, cap, sizeof *v);
      if (!v) {
        free(val);
        return -1;
      }
      list->v = v;
      list->cap = cap;
    }
    list->v[list->n].lang = lang;
    list->v[list->n].val = val;
    list->v[list->n].len = n;
    list->n++;
  }
  return 0;
}

/* pickEnglish: the English name of a list, or its first, or NULL. */
static const bdfi_fdb_name *bdfi_fdb_english(const bdfi_fdb_names *l) {
  for (size_t i = 0; i < l->n; i++) {
    if (l->v[i].lang == 0x0409) {
      return &l->v[i];
    }
  }
  return l->n > 0 ? &l->v[0] : NULL;
}

static int bdfi_fdb_contains(const char *s, size_t n, const char *sub) {
  size_t m = strlen(sub);
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(s + i, sub, m) == 0) {
      return 1;
    }
  }
  return 0;
}

/* The most bytes of a table the scan reads (and of a cmap, for the
 * characters a face has: maxCoverageTable). */
#define BDFI_FDB_TABLE    ((uint32_t)1 << 20)
#define BDFI_FDB_COVERAGE ((int64_t)16 << 20)

/* table of scanFace: a whole table of at most limit bytes, or NULL (*len
 * says how long it is). *oom when memory ran out. */
static uint8_t *bdfi_fdb_table(bdfi_fdb_src *src, const uint8_t *dir, size_t n, const char *name,
                                size_t *len, int *oom) {
  *len = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t *rec = dir + i * 16;
    if (memcmp(rec, name, 4) != 0) {
      continue;
    }
    uint32_t toff = bdfi_fd32(rec + 8), tlen = bdfi_fd32(rec + 12);
    if (tlen == 0 || tlen > BDFI_FDB_TABLE) {
      return NULL;
    }
    uint8_t *b = malloc(tlen);
    if (!b) {
      *oom = 1;
      return NULL;
    }
    if (!bdfi_fdb_read_at(src, toff, b, tlen)) {
      free(b);
      return NULL;
    }
    *len = tlen;
    return b;
  }
  return NULL;
}

/*
 * scanFace: the naming and style of the font at off, without its glyphs.
 * 1 when there is a face (in *e), 0 when the font is passed over, -1 when
 * memory ran out.
 */
static int bdfi_fdb_scan_face(bdfi_fdb_src *src, uint64_t off, bdfi_fdb_entry *e) {
  uint8_t hdr[12];
  memset(e, 0, sizeof *e);
  if (!bdfi_fdb_read_at(src, off, hdr, 12)) {
    return 0;
  }
  if (memcmp(hdr, "\0\1\0\0", 4) != 0 && memcmp(hdr, "true", 4) != 0 &&
      memcmp(hdr, "OTTO", 4) != 0) {
    return 0;
  }
  size_t n = bdfi_fd16(hdr + 4);
  uint8_t *dir = malloc(16 * n + 1);
  if (!dir) {
    return -1;
  }
  if (!bdfi_fdb_read_at(src, off + 12, dir, 16 * n)) {
    free(dir);
    return 0;
  }
  e->weight = 400;
  for (size_t i = 0; i < n; i++) {
    if (memcmp(dir + i * 16, "cmap", 4) == 0) {
      e->cmap_off = bdfi_fd32(dir + i * 16 + 8);
      e->cmap_len = bdfi_fd32(dir + i * 16 + 12);
      break;
    }
  }
  int oom = 0, rc = 0;
  size_t len;
  bdfi_fdb_names names[4];
  memset(names, 0, sizeof names);
  uint8_t *t = bdfi_fdb_table(src, dir, n, "name", &len, &oom);
  if (t && bdfi_fdb_parse_names(t, len, names) < 0) {
    oom = 1;
  }
  free(t);
  const bdfi_fdb_name *family = bdfi_fdb_english(&names[2]);
  if (!family) {
    family = bdfi_fdb_english(&names[0]);
  }
  if (!oom && names[0].n > 0 && family) {
    rc = 1;
    e->family = malloc(family->len + 1);
    if (e->family) {
      memcpy(e->family, family->val, family->len + 1);
    } else {
      oom = 1;
    }
    /* the names it is found by: the family names of every language */
    size_t most = names[0].n + names[2].n;
    e->names = bdfi_calloc_array(most ? most : 1, sizeof *e->names);
    if (!e->names) {
      oom = 1;
    }
    for (int slot = 0; !oom && slot <= 2; slot += 2) {
      for (size_t i = 0; !oom && i < names[slot].n; i++) {
        char *key;
        size_t klen;
        if (bdfi_family_key(names[slot].v[i].val, names[slot].v[i].len, &key, &klen) < 0) {
          oom = 1;
          break;
        }
        int seen = klen == 0;
        for (size_t k = 0; !seen && k < e->nnames; k++) {
          seen = strcmp(e->names[k], key) == 0;
        }
        if (seen) {
          free(key);
        } else {
          e->names[e->nnames++] = key;
        }
      }
    }
    t = oom ? NULL : bdfi_fdb_table(src, dir, n, "OS/2", &len, &oom);
    if (t && len >= 64) {
      e->weight = (int)bdfi_fd16(t + 4);
      e->italic = (bdfi_fd16(t + 62) & 1) != 0;
    }
    free(t);
    if (e->weight < 100 || e->weight > 1000) {
      e->weight = 400;
    }
    /* a style that says so makes the face italic too */
    const bdfi_fdb_name *style = bdfi_fdb_english(&names[3]);
    if (!style) {
      style = bdfi_fdb_english(&names[1]);
    }
    if (style) {
      /* in lower case: a character is at most three bytes longer there */
      char *low = bdfi_malloc_array(style->len + 1, 3);
      if (!low) {
        oom = 1;
      } else {
        size_t w = 0;
        for (size_t i = 0; i < style->len;) {
          size_t k;
          w += bdfi_utf8_put(bdfi_to_lower(bdfi_utf8_rune(style->val + i, style->len - i, &k)),
                             low + w);
          i += k;
        }
        if (bdfi_fdb_contains(low, w, "italic") || bdfi_fdb_contains(low, w, "oblique")) {
          e->italic = 1;
        }
        free(low);
      }
    }
  }
  for (int i = 0; i < 4; i++) {
    bdfi_fdb_names_free(&names[i]);
  }
  free(dir);
  if (oom) {
    bdfi_fdb_entry_clear(e);
    memset(e, 0, sizeof *e);
    return -1;
  }
  return rc;
}

/* scanFaces: the faces of a file are added to the set. How many, or -1
 * when memory ran out. */
static int64_t bdfi_fdb_scan(bdf_fonts *db, bdfi_fdb_src *src, size_t file) {
  uint8_t hdr[12];
  if (!bdfi_fdb_read_at(src, 0, hdr, 12)) {
    return 0;
  }
  size_t count = 1;
  uint8_t *offsets = NULL;
  if (memcmp(hdr, "ttcf", 4) == 0) {
    uint32_t n = bdfi_fd32(hdr + 8);
    if (n == 0 || n > 256) {
      return 0;
    }
    offsets = malloc(4 * (size_t)n);
    if (!offsets) {
      return -1;
    }
    if (!bdfi_fdb_read_at(src, 12, offsets, 4 * (size_t)n)) {
      free(offsets);
      return 0;
    }
    count = n;
  }
  int64_t added = 0;
  for (size_t i = 0; i < count; i++) {
    bdfi_fdb_entry e;
    int rc = bdfi_fdb_scan_face(src, offsets ? bdfi_fd32(offsets + 4 * i) : 0, &e);
    if (rc == 1 && db->nfaces == db->cap_faces) {
      size_t cap = db->cap_faces ? 2 * db->cap_faces : 16;
      bdfi_fdb_entry *faces = bdfi_realloc_array(db->faces, cap, sizeof *faces);
      if (faces) {
        db->faces = faces;
        db->cap_faces = cap;
      } else {
        bdfi_fdb_entry_clear(&e);
        rc = -1;
      }
    }
    if (rc < 0) {
      added = -1;
      break;
    }
    if (rc == 1) {
      e.file = file;
      e.index = (int)i;
      db->faces[db->nfaces++] = e;
      added++;
    }
  }
  free(offsets);
  return added;
}

/* Adds a file to the set, and its faces. The file is the set's (also when
 * this fails). */
static bdf_status bdfi_fonts_add_file(bdf_fonts *db, bdfi_fdb_file f, bdfi_fdb_src *src,
                                      bdf_error *err) {
  size_t before = db->nfaces;
  bdf_status st = BDF_OK;
  if (db->nfiles == db->cap_files) {
    size_t cap = db->cap_files ? 2 * db->cap_files : 16;
    bdfi_fdb_file *files = bdfi_realloc_array(db->files, cap, sizeof *files);
    if (!files) {
      st = BDF_ERR_NOMEM;
    } else {
      db->files = files;
      db->cap_files = cap;
    }
  }
  int64_t added = st == BDF_OK ? bdfi_fdb_scan(db, src, db->nfiles) : 0;
  if (added < 0) {
    st = BDF_ERR_NOMEM;
  } else if (st == BDF_OK && added == 0) {
    st = BDF_ERR_FORMAT;
  }
  if (st != BDF_OK) {
    /* nothing of a file that was not read through is kept */
    while (db->nfaces > before) {
      bdfi_fdb_entry_clear(&db->faces[--db->nfaces]);
    }
    free(f.data);
    free(f.path);
    bdfi_error_set(err, st, 0, st == BDF_ERR_NOMEM ? "out of memory" : "no font that can be read");
    return st;
  }
  db->files[db->nfiles++] = f;
  return BDF_OK;
}

bdf_status bdf_fonts_add(bdf_fonts *fonts, const uint8_t *data, size_t len, bdf_error *err) {
  bdfi_error_clear(err);
  if (!fonts || (!data && len > 0)) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  bdfi_fdb_file f = {malloc(len ? len : 1), len, NULL};
  if (!f.data) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  if (len > 0) {
    memcpy(f.data, data, len);
  }
  bdfi_fdb_src src = {f.data, len, NULL};
  return bdfi_fonts_add_file(fonts, f, &src, err);
}

bdf_status bdf_fonts_add_file(bdf_fonts *fonts, const char *path, bdf_error *err) {
  bdfi_error_clear(err);
  if (!fonts || !path) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  /* a file that was added is not added again (New of the reference) */
  for (size_t i = 0; i < fonts->nfiles; i++) {
    if (fonts->files[i].path && strcmp(fonts->files[i].path, path) == 0) {
      return BDF_OK;
    }
  }
  size_t n = strlen(path);
  bdfi_fdb_file f = {NULL, 0, malloc(n + 1)};
  if (!f.path) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  memcpy(f.path, path, n + 1);
  bdfi_fdb_src src = {NULL, 0, fopen(path, "rb")};
  if (!src.fp) {
    free(f.path);
    bdfi_error_set(err, BDF_ERR_FORMAT, 0, "the font file cannot be opened");
    return BDF_ERR_FORMAT;
  }
  bdf_status st = bdfi_fonts_add_file(fonts, f, &src, err);
  fclose(src.fp);
  return st;
}

/* ---- The families that stand in for others ------------------------------- */

/* The tables of resolve.go of the reference's internal/fontdb, with the
 * names folded as fontdb.Normalize folds them (see bdfi_family_key). */
static const char *const bdfi_fn_ja_gothic[] = {"notosanscjkjp", "notosansjp", "sourcehansansjp", "ipaexgothic", "ipapgothic", "ipagothic", "takaopgothic", "vlpgothic", "mplus1p", "hiraginosans", "yugothic", "meiryo", "wenquanyizenhei", NULL};
static const char *const bdfi_fn_ja_gothic_mono[] = {"ipagothic", "takaogothic", "vlgothic", "notosansmonocjkjp", "msgothic", "notosanscjkjp", "ipaexgothic", "wenquanyizenheimono", NULL};
static const char *const bdfi_fn_ja_mincho[] = {"notoserifcjkjp", "notoserifjp", "sourcehanserifjp", "ipaexmincho", "ipapmincho", "ipamincho", "takaopmincho", "hiraginominchopron", "yumincho", NULL};
static const char *const bdfi_fn_ja_mincho_mono[] = {"ipamincho", "takaomincho", "msmincho", "notoserifcjkjp", "ipaexmincho", NULL};
static const char *const bdfi_fn_zh_hans[] = {"notosanscjksc", "notosanssc", "sourcehansanssc", "wenquanyizenhei", "wenquanyimicrohei", "microsoftyahei", "pingfangsc", NULL};
static const char *const bdfi_fn_zh_hans_serif[] = {"notoserifcjksc", "notoserifsc", "sourcehanserifsc", "arplumingcn", "simsun", NULL};
static const char *const bdfi_fn_zh_hant[] = {"notosanscjktc", "notosanstc", "sourcehansanstc", "wenquanyizenhei", "microsoftjhenghei", "pingfangtc", NULL};
static const char *const bdfi_fn_zh_hant_serif[] = {"notoserifcjktc", "notoseriftc", "arplumingtw", "pmingliu", NULL};
static const char *const bdfi_fn_ko_gothic[] = {"notosanscjkkr", "notosanskr", "sourcehansanskr", "nanumgothic", "undotum", "malgungothic", NULL};
static const char *const bdfi_fn_ko_serif[] = {"notoserifcjkkr", "notoserifkr", "nanummyeongjo", "unbatang", "batang", NULL};
static const char *const bdfi_fn_symbols[] = {"dejavusans", "notosanssymbols", "notosanssymbols2", "segoeuisymbol", "opensymbol", "freeserif", "freesans", "arialunicodems", "notocoloremoji", "unifont", NULL};
static const char *const bdfi_fn_sans[] = {"liberationsans", "arimo", "arial", "helvetica", "carlito", "dejavusans", "freesans", "notosans", "opensans", "roboto", NULL};
static const char *const bdfi_fn_serif[] = {"liberationserif", "tinos", "timesnewroman", "times", "caladea", "dejavuserif", "freeserif", "notoserif", NULL};
static const char *const bdfi_fn_mono[] = {"liberationmono", "cousine", "couriernew", "dejavusansmono", "freemono", "notosansmono", NULL};
static const char *const bdfi_fn_sub_0[] = {"carlito", NULL};
static const char *const bdfi_fn_sub_1[] = {"carlito", NULL};
static const char *const bdfi_fn_sub_2[] = {"caladea", NULL};
static const char *const bdfi_fn_sub_3[] = {"caladea", NULL};
static const char *const bdfi_fn_sub_4[] = {"liberationsans", "arimo", "helvetica", "helveticaneue", NULL};
static const char *const bdfi_fn_sub_5[] = {"liberationsans", "arimo", "arial", "helveticaneue", NULL};
static const char *const bdfi_fn_sub_6[] = {"liberationsans", "arimo", "helvetica", "arial", NULL};
static const char *const bdfi_fn_sub_7[] = {"liberationsansnarrow", NULL};
static const char *const bdfi_fn_sub_8[] = {"liberationserif", "tinos", "times", NULL};
static const char *const bdfi_fn_sub_9[] = {"liberationserif", "tinos", "timesnewroman", NULL};
static const char *const bdfi_fn_sub_10[] = {"liberationmono", "cousine", "courier", NULL};
static const char *const bdfi_fn_sub_11[] = {"liberationmono", "cousine", "couriernew", NULL};
static const char *const bdfi_fn_sub_12[] = {"gelasio", NULL};
static const char *const bdfi_fn_sub_13[] = {"opensymbol", "standardsymbolsps", NULL};
static const char *const bdfi_fn_sub_14[] = {"opensymbol", NULL};
static const char *const bdfi_fn_sub_15[] = {"notosanssymbols", "notosanssymbols2", "dejavusans", NULL};
static const char *const bdfi_fn_sub_16[] = {"notocoloremoji", NULL};

/* substitutes: families that look alike, or have the same metrics, to try
 * when a family is not there, by its folded name */
static const struct {
  const char *family;
  const char *const *names;
} bdfi_fn_substitutes[] = {
    {"calibri", bdfi_fn_sub_0},
    {"calibrilight", bdfi_fn_sub_1},
    {"cambria", bdfi_fn_sub_2},
    {"cambriamath", bdfi_fn_sub_3},
    {"arial", bdfi_fn_sub_4},
    {"helvetica", bdfi_fn_sub_5},
    {"helveticaneue", bdfi_fn_sub_6},
    {"arialnarrow", bdfi_fn_sub_7},
    {"timesnewroman", bdfi_fn_sub_8},
    {"times", bdfi_fn_sub_9},
    {"couriernew", bdfi_fn_sub_10},
    {"courier", bdfi_fn_sub_11},
    {"georgia", bdfi_fn_sub_12},
    {"symbol", bdfi_fn_sub_13},
    {"wingdings", bdfi_fn_sub_14},
    {"segoeuisymbol", bdfi_fn_sub_15},
    {"segoeuiemoji", bdfi_fn_sub_16},
    {"meiryo", bdfi_fn_ja_gothic},
    {"\xe3\x83\xa1\xe3\x82\xa4\xe3\x83\xaa\xe3\x82\xaa", bdfi_fn_ja_gothic},
    {"meiryoui", bdfi_fn_ja_gothic},
    {"yugothic", bdfi_fn_ja_gothic},
    {"\xe6\xb8\xb8\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf", bdfi_fn_ja_gothic},
    {"yugothicui", bdfi_fn_ja_gothic},
    {"yugothicmedium", bdfi_fn_ja_gothic},
    {"\xe6\xb8\xb8\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xafmedium", bdfi_fn_ja_gothic},
    {"\xe6\xb8\xb8\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaflight", bdfi_fn_ja_gothic},
    {"mspgothic", bdfi_fn_ja_gothic},
    {"msp\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf", bdfi_fn_ja_gothic},
    {"msuigothic", bdfi_fn_ja_gothic},
    {"hiraginosans", bdfi_fn_ja_gothic},
    {"\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4pron", bdfi_fn_ja_gothic},
    {"\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf", bdfi_fn_ja_gothic},
    {"msgothic", bdfi_fn_ja_gothic_mono},
    {"ms\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf", bdfi_fn_ja_gothic_mono},
    {"yumincho", bdfi_fn_ja_mincho},
    {"\xe6\xb8\xb8\xe6\x98\x8e\xe6\x9c\x9d", bdfi_fn_ja_mincho},
    {"mspmincho", bdfi_fn_ja_mincho},
    {"msp\xe6\x98\x8e\xe6\x9c\x9d", bdfi_fn_ja_mincho},
    {"msmincho", bdfi_fn_ja_mincho_mono},
    {"ms\xe6\x98\x8e\xe6\x9c\x9d", bdfi_fn_ja_mincho_mono},
    {"hiraginomincho", bdfi_fn_ja_mincho},
    {"\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe6\x98\x8e\xe6\x9c\x9dpron", bdfi_fn_ja_mincho},
    {"microsoftyahei", bdfi_fn_zh_hans},
    {"\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91", bdfi_fn_zh_hans},
    {"simsun", bdfi_fn_zh_hans_serif},
    {"\xe5\xae\x8b\xe4\xbd\x93", bdfi_fn_zh_hans_serif},
    {"simhei", bdfi_fn_zh_hans},
    {"\xe9\xbb\x91\xe4\xbd\x93", bdfi_fn_zh_hans},
    {"dengxian", bdfi_fn_zh_hans},
    {"\xe7\xad\x89\xe7\xba\xbf", bdfi_fn_zh_hans},
    {"microsoftjhenghei", bdfi_fn_zh_hant},
    {"\xe5\xbe\xae\xe8\xbb\x9f\xe6\xad\xa3\xe9\xbb\x91\xe9\xab\x94", bdfi_fn_zh_hant},
    {"pmingliu", bdfi_fn_zh_hant_serif},
    {"\xe6\x96\xb0\xe7\xb4\xb0\xe6\x98\x8e\xe9\xab\x94", bdfi_fn_zh_hant_serif},
    {"mingliu", bdfi_fn_zh_hant_serif},
    {"malgungothic", bdfi_fn_ko_gothic},
    {"\xeb\xa7\x91\xec\x9d\x80\xea\xb3\xa0\xeb\x94\x95", bdfi_fn_ko_gothic},
    {"gulim", bdfi_fn_ko_gothic},
    {"\xea\xb5\xb4\xeb\xa6\xbc", bdfi_fn_ko_gothic},
    {"dotum", bdfi_fn_ko_gothic},
    {"\xeb\x8f\x8b\xec\x9b\x80", bdfi_fn_ko_gothic},
    {"batang", bdfi_fn_ko_serif},
    {"\xeb\xb0\x94\xed\x83\x95", bdfi_fn_ko_serif},
};

/* ---- Finding a face ------------------------------------------------------ */

/* Match, over the faces of the set that have the folded name key: the one
 * closest to the weight and slant asked for, and what the renderer has to
 * fake of them. 0 when the set has no such family. */
static int bdfi_fonts_pick(const bdf_fonts *db, const char *key, int bold, int italic,
                           bdfi_resolved *out) {
  int want = bold ? 700 : 400;
  int best_score = 1 << 30;
  size_t best = SIZE_MAX;
  if (!db || key[0] == '\0') {
    return 0;
  }
  for (size_t i = 0; i < db->nfaces; i++) {
    const bdfi_fdb_entry *f = &db->faces[i];
    int has = 0;
    for (size_t k = 0; !has && k < f->nnames; k++) {
      has = strcmp(f->names[k], key) == 0;
    }
    if (!has) {
      continue;
    }
    int score = abs(f->weight - want) * 2;
    if ((bold && f->weight < 600) || (!bold && f->weight >= 600)) {
      score += 1000;
    }
    if (f->italic != italic) {
      score += 5000;
    }
    if (score < best_score) {
      best = i;
      best_score = score;
    }
  }
  if (best == SIZE_MAX) {
    return 0;
  }
  out->face = best;
  out->synth_bold = bold && db->faces[best].weight < 600;
  out->synth_italic = italic && !db->faces[best].italic;
  return 1;
}

/* db.Family and Match for a name as a document has it. 0 when the set has
 * no such family, -1 when memory ran out. */
BDF_INTERNAL int bdfi_fonts_family(const bdf_fonts *db, const char *name, size_t n, int bold,
                                   int italic, bdfi_resolved *out) {
  char *key;
  size_t klen;
  if (bdfi_family_key(name, n, &key, &klen) < 0) {
    return -1;
  }
  int rc = bdfi_fonts_pick(db, key, bold, italic, out);
  free(key);
  return rc;
}

static int bdfi_fonts_try(const bdf_fonts *db, const char *const *keys, int bold, int italic,
                          bdfi_resolved *out) {
  for (size_t i = 0; keys && keys[i]; i++) {
    if (bdfi_fonts_pick(db, keys[i], bold, italic, out)) {
      return 1;
    }
  }
  return 0;
}

static int bdfi_key_has(const char *key, const char *sub) { return strstr(key, sub) != NULL; }

/* Classify: the generic family a folded family name suggests. */
static int bdfi_fonts_classify(const char *n) {
  if (bdfi_key_has(n, "mono") || bdfi_key_has(n, "courier") || bdfi_key_has(n, "consola") ||
      bdfi_key_has(n, "code") || strcmp(n, "msgothic") == 0 ||
      strcmp(n, "ms\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf") == 0 ||
      strcmp(n, "msmincho") == 0 || strcmp(n, "ms\xe6\x98\x8e\xe6\x9c\x9d") == 0 ||
      bdfi_key_has(n, "lucidaconsole")) {
    return BDFI_GENERIC_MONO;
  }
  if (bdfi_key_has(n, "sans") || bdfi_key_has(n, "gothic") ||
      bdfi_key_has(n, "\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf") ||
      bdfi_key_has(n, "\xe9\xbb\x91") || bdfi_key_has(n, "hei")) {
    return BDFI_GENERIC_SANS;
  }
  if (bdfi_key_has(n, "serif") || bdfi_key_has(n, "times") || bdfi_key_has(n, "mincho") ||
      bdfi_key_has(n, "\xe6\x98\x8e\xe6\x9c\x9d") || bdfi_key_has(n, "cambria") ||
      bdfi_key_has(n, "georgia") || bdfi_key_has(n, "garamond") || bdfi_key_has(n, "century") ||
      bdfi_key_has(n, "palatino") || bdfi_key_has(n, "bodoni") || bdfi_key_has(n, "bookantiqua") ||
      bdfi_key_has(n, "roman") || bdfi_key_has(n, "song") || bdfi_key_has(n, "\xe5\xae\x8b") ||
      bdfi_key_has(n, "ming") || bdfi_key_has(n, "\xe6\x98\x8e") || bdfi_key_has(n, "batang") ||
      bdfi_key_has(n, "\xeb\xb0\x94\xed\x83\x95")) {
    return BDFI_GENERIC_SERIF;
  }
  return BDFI_GENERIC_SANS;
}

/* heavyName: a family that is the bold of another ("Arial Black"). */
static int bdfi_fonts_heavy(const char *n) {
  static const char *const words[] = {"black", "heavy", "bold", "extrabold", "ultrabold",
                                      "semibold", "demibold"};
  size_t len = strlen(n);
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    size_t m = strlen(words[i]);
    char with[32];
    memcpy(with, words[i], m);
    memcpy(with + m, "italic", 7);
    if ((len >= m && strcmp(n + len - m, words[i]) == 0) || bdfi_key_has(n, with)) {
      return 1;
    }
  }
  return 0;
}

static const char *const *bdfi_fonts_generic(int generic) {
  return generic == BDFI_GENERIC_SERIF  ? bdfi_fn_serif
         : generic == BDFI_GENERIC_MONO ? bdfi_fn_mono
                                        : bdfi_fn_sans;
}

/*
 * Resolve: a family request mapped to a face: the family itself, a known
 * substitute, a generic family of the same kind, and finally any face at
 * all. The face is SIZE_MAX when the set has none. -1 when memory ran out.
 */
BDF_INTERNAL int bdfi_fonts_resolve(const bdf_fonts *db, const char *family, size_t n, int bold,
                                    int italic, bdfi_resolved *res) {
  char *key;
  size_t klen;
  res->face = SIZE_MAX;
  res->synth_bold = 0;
  res->synth_italic = 0;
  res->generic = BDFI_GENERIC_SANS;
  if (bdfi_family_key(family, n, &key, &klen) < 0) {
    return -1;
  }
  res->generic = bdfi_fonts_classify(key);
  int found = n > 0 && bdfi_fonts_pick(db, key, bold, italic, res);
  for (size_t i = 0; !found && i < sizeof bdfi_fn_substitutes / sizeof bdfi_fn_substitutes[0]; i++) {
    if (strcmp(bdfi_fn_substitutes[i].family, key) == 0) {
      found = bdfi_fonts_try(db, bdfi_fn_substitutes[i].names, bold, italic, res);
      break;
    }
  }
  if (!found) {
    /* a heavy family stands in as the bold face of its substitute */
    if (bdfi_fonts_heavy(key)) {
      bold = 1;
    }
    found = bdfi_fonts_try(db, bdfi_fonts_generic(res->generic), bold, italic, res) ||
            bdfi_fonts_try(db, bdfi_fn_sans, bold, italic, res) ||
            bdfi_fonts_try(db, bdfi_fn_ja_gothic, bold, italic, res) ||
            bdfi_fonts_try(db, bdfi_fn_zh_hans, bold, italic, res);
  }
  free(key);
  if (!found && db && db->nfaces > 0) {
    /* firstFamily: the faces of the family that comes first in the
     * alphabet, the last resort that is the same every time */
    const char *first = db->faces[0].family;
    for (size_t i = 1; i < db->nfaces; i++) {
      if (strcmp(db->faces[i].family, first) < 0) {
        first = db->faces[i].family;
      }
    }
    int want = bold ? 700 : 400, best_score = 1 << 30;
    for (size_t i = 0; i < db->nfaces; i++) {
      const bdfi_fdb_entry *f = &db->faces[i];
      if (strcmp(f->family, first) != 0) {
        continue;
      }
      int score = abs(f->weight - want) * 2;
      if ((bold && f->weight < 600) || (!bold && f->weight >= 600)) {
        score += 1000;
      }
      if (f->italic != italic) {
        score += 5000;
      }
      if (score < best_score) {
        res->face = i;
        best_score = score;
      }
    }
    res->synth_bold = bold && db->faces[res->face].weight < 600;
    res->synth_italic = italic && !db->faces[res->face].italic;
  }
  return 0;
}

typedef struct bdfi_fallbacks {
  const bdf_fonts *db;
  int bold, italic, generic, oom;
  bdfi_resolved *v;
  size_t n, cap;
} bdfi_fallbacks;

static void bdfi_fallbacks_add(bdfi_fallbacks *fb, const char *key) {
  bdfi_resolved r;
  if (fb->oom || !bdfi_fonts_pick(fb->db, key, fb->bold, fb->italic, &r)) {
    return;
  }
  for (size_t i = 0; i < fb->n; i++) {
    if (fb->v[i].face == r.face) {
      return;
    }
  }
  if (fb->n == fb->cap) {
    size_t cap = fb->cap ? 2 * fb->cap : 16;
    bdfi_resolved *v = bdfi_realloc_array(fb->v, cap, sizeof *v);
    if (!v) {
      fb->oom = 1;
      return;
    }
    fb->v = v;
    fb->cap = cap;
  }
  r.generic = fb->generic;
  fb->v[fb->n++] = r;
}

static void bdfi_fallbacks_all(bdfi_fallbacks *fb, const char *const *keys) {
  for (size_t i = 0; keys[i]; i++) {
    bdfi_fallbacks_add(fb, keys[i]);
  }
}

/*
 * Fallbacks: the faces to try, in order, for characters the resolved face
 * lacks: the generic family, the CJK chains, symbol fonts, and then every
 * other face, so that a small set of fonts is used completely. The list is
 * the caller's to free; -1 when memory ran out.
 */
BDF_INTERNAL int bdfi_fonts_fallbacks(const bdf_fonts *db, int generic, int bold, int italic,
                                      bdfi_resolved **out, size_t *n) {
  bdfi_fallbacks fb;
  memset(&fb, 0, sizeof fb);
  fb.db = db;
  fb.bold = bold;
  fb.italic = italic;
  fb.generic = generic;
  *out = NULL;
  *n = 0;
  if (!db) {
    return 0;
  }
  bdfi_fallbacks_all(&fb, bdfi_fonts_generic(generic));
  if (generic == BDFI_GENERIC_SERIF) {
    bdfi_fallbacks_all(&fb, bdfi_fn_ja_mincho);
  }
  bdfi_fallbacks_all(&fb, bdfi_fn_ja_gothic);
  bdfi_fallbacks_all(&fb, bdfi_fn_zh_hans);
  bdfi_fallbacks_all(&fb, bdfi_fn_zh_hant);
  bdfi_fallbacks_all(&fb, bdfi_fn_ko_gothic);
  bdfi_fallbacks_all(&fb, bdfi_fn_sans);
  bdfi_fallbacks_all(&fb, bdfi_fn_symbols);
  if (db->nfaces <= 64) {
    /* the families of the set, each once, in the order of its faces */
    for (size_t i = 0; i < db->nfaces && !fb.oom; i++) {
      int seen = 0;
      for (size_t k = 0; !seen && k < i; k++) {
        seen = strcmp(db->faces[k].family, db->faces[i].family) == 0;
      }
      if (seen) {
        continue;
      }
      char *key;
      size_t klen;
      if (bdfi_family_key(db->faces[i].family, strlen(db->faces[i].family), &key, &klen) < 0) {
        fb.oom = 1;
        break;
      }
      bdfi_fallbacks_add(&fb, key);
      free(key);
    }
  }
  if (fb.oom) {
    free(fb.v);
    return -1;
  }
  *out = fb.v;
  *n = fb.n;
  return 0;
}

/* ---- What a renderer reads of a face ------------------------------------- */

BDF_INTERNAL int bdfi_fonts_index(const bdf_fonts *db, size_t face) { return db->faces[face].index; }

/*
 * The bytes of the file of a face: those the set holds (*owned is 0), or
 * the file read whole into memory that is the caller's (*owned is 1). 0
 * when the file cannot be read, -1 when memory ran out.
 */
BDF_INTERNAL int bdfi_fonts_read(const bdf_fonts *db, size_t face, const uint8_t **data,
                                 size_t *len, int *owned) {
  const bdfi_fdb_file *f = &db->files[db->faces[face].file];
  *owned = 0;
  if (f->data) {
    *data = f->data;
    *len = f->len;
    return 1;
  }
  FILE *fp = fopen(f->path, "rb");
  if (!fp) {
    return 0;
  }
  size_t cap = 1 << 16, n = 0;
  uint8_t *buf = malloc(cap);
  int rc = buf ? 1 : -1;
  while (rc == 1) {
    n += fread(buf + n, 1, cap - n, fp);
    if (n < cap) {
      rc = ferror(fp) ? 0 : 1;
      break;
    }
    uint8_t *grown = cap > SIZE_MAX / 2 ? NULL : realloc(buf, cap * 2);
    if (!grown) {
      rc = -1;
      break;
    }
    buf = grown;
    cap *= 2;
  }
  fclose(fp);
  if (rc != 1) {
    free(buf);
    return rc;
  }
  *data = buf;
  *len = n;
  *owned = 1;
  return 1;
}

/* cmapSubtableLength */
static int bdfi_cov_length(const uint8_t *b, size_t n, uint64_t *len) {
  if (n < 4) {
    return 0;
  }
  switch (bdfi_fd16(b)) {
  case 0:
  case 4:
  case 6:
    *len = bdfi_fd16(b + 2);
    return 1;
  case 12:
    if (n < 8) {
      return 0;
    }
    *len = bdfi_fd32(b + 4);
    return 1;
  default:
    return 0;
  }
}

/* validCoverageSubtable: a subtable that is whole and in order. */
static int bdfi_cov_valid(const uint8_t *b, size_t n, uint32_t format) {
  switch (format) {
  case 0:
    return n >= 262;
  case 4: {
    if (n < 16) {
      return 0;
    }
    size_t seg_x2 = bdfi_fd16(b + 6);
    if (seg_x2 == 0 || (seg_x2 & 1) != 0 || 16 + 4 * seg_x2 > n) {
      return 0;
    }
    uint32_t prev = 0;
    for (size_t i = 0; i < seg_x2 / 2; i++) {
      uint32_t end = bdfi_fd16(b + 14 + i * 2), start = bdfi_fd16(b + 16 + seg_x2 + i * 2);
      if (start > end || (i > 0 && start <= prev)) {
        return 0;
      }
      prev = end;
    }
    return 1;
  }
  case 6:
    return n >= 10 && 10 + 2 * (size_t)bdfi_fd16(b + 8) <= n;
  case 12: {
    if (n < 16) {
      return 0;
    }
    size_t count = bdfi_fd32(b + 12);
    if (count > (n - 16) / 12 || count > 0x110000) {
      return 0;
    }
    uint32_t prev = 0;
    for (size_t i = 0; i < count; i++) {
      uint32_t start = bdfi_fd32(b + 16 + i * 12), end = bdfi_fd32(b + 16 + i * 12 + 4);
      if (start > end || (i > 0 && start <= prev)) {
        return 0;
      }
      prev = end;
    }
    return 1;
  }
  default:
    return 0;
  }
}

/*
 * ParseCmapCoverage: the best Unicode subtable of a cmap table that is well
 * formed (the preference of the character map of a face), for asking which
 * characters a font has without reading its glyphs. It points into the
 * table; 0 when the table has none.
 */
BDF_INTERNAL int bdfi_coverage_parse(const uint8_t *t, size_t len, bdfi_coverage *cov) {
  cov->data = NULL;
  cov->len = 0;
  cov->format = 0;
  if (len < 4) {
    return 0;
  }
  size_t n = bdfi_fd16(t + 2);
  if (n > (len - 4) / 8) {
    n = (len - 4) / 8;
  }
  int best = -1;
  for (size_t i = 0; i < n; i++) {
    const uint8_t *rec = t + 4 + i * 8;
    uint32_t pid = bdfi_fd16(rec), eid = bdfi_fd16(rec + 2);
    size_t off = bdfi_fd32(rec + 4);
    uint64_t length;
    if (off > len || len - off < 2) {
      continue;
    }
    uint32_t format = bdfi_fd16(t + off);
    if (!bdfi_cov_length(t + off, len - off, &length) || length > len - off ||
        !bdfi_cov_valid(t + off, (size_t)length, format)) {
      continue;
    }
    int score = pid == 3 && eid == 10 ? 3 : pid == 3 && eid == 1 ? 2 : pid == 0 ? 1 : -1;
    if (score > best) {
      best = score;
      cov->data = t + off;
      cov->len = (size_t)length;
      cov->format = format;
    }
  }
  return cov->data != NULL;
}

/* A 16-bit value of a subtable, 0 past its end (be16 of the reference). */
static uint32_t bdfi_cov16(const bdfi_coverage *c, uint64_t i) {
  return i + 1 < c->len ? bdfi_fd16(c->data + i) : 0;
}

static uint32_t bdfi_cov32(const bdfi_coverage *c, uint64_t i) {
  return i + 3 < c->len ? bdfi_fd32(c->data + i) : 0;
}

/* CmapCoverage.Has: whether the subtable maps a character to a glyph that
 * is not glyph 0. */
BDF_INTERNAL int bdfi_coverage_has(const bdfi_coverage *c, uint32_t code) {
  const uint8_t *b = c->data;
  if (!b) {
    return 0;
  }
  switch (c->format) {
  case 0:
    return code < 256 && 6 + (size_t)code < c->len && b[6 + code] != 0;
  case 4: {
    if (code > 0xfffe) {
      return 0;
    }
    uint64_t seg_x2 = bdfi_cov16(c, 6), count = seg_x2 / 2;
    uint64_t ends = 14, starts = 16 + seg_x2, deltas = 16 + 2 * seg_x2, range_offs = 16 + 3 * seg_x2;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
      uint64_t mid = lo + (hi - lo) / 2;
      if (bdfi_cov16(c, ends + mid * 2) < code) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo == count) {
      return 0;
    }
    uint32_t start = bdfi_cov16(c, starts + lo * 2);
    if (code < start) {
      return 0;
    }
    uint32_t delta = bdfi_cov16(c, deltas + lo * 2), ro = bdfi_cov16(c, range_offs + lo * 2);
    uint32_t glyph;
    if (ro == 0) {
      glyph = (code + delta) & 0xffff;
    } else {
      uint64_t addr = range_offs + lo * 2 + ro + (uint64_t)(code - start) * 2;
      if (addr + 2 > c->len) {
        return 0;
      }
      glyph = bdfi_cov16(c, addr);
      if (glyph != 0) {
        glyph = (glyph + delta) & 0xffff;
      }
    }
    return glyph != 0;
  }
  case 6: {
    uint32_t first = bdfi_cov16(c, 6), count = bdfi_cov16(c, 8);
    if (code < first || code - first >= count) {
      return 0;
    }
    return bdfi_cov16(c, 10 + (uint64_t)(code - first) * 2) != 0;
  }
  case 12: {
    if (code > 0x10ffff) {
      return 0;
    }
    uint64_t count = bdfi_cov32(c, 12), lo = 0, hi = count;
    while (lo < hi) {
      uint64_t mid = lo + (hi - lo) / 2;
      if (bdfi_cov32(c, 16 + mid * 12) <= code) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo == 0) {
      return 0;
    }
    uint64_t rec = 16 + (lo - 1) * 12;
    uint32_t start = bdfi_cov32(c, rec), end = bdfi_cov32(c, rec + 4), glyph = bdfi_cov32(c, rec + 8);
    if (code > end || code - start > 0xffff) {
      return 0;
    }
    return ((glyph + (code - start)) & 0xffff) != 0;
  }
  default:
    return 0;
  }
}

/*
 * The cmap table of a face, for its coverage (readCmap): bytes of the set
 * (*owned is 0) or read from its file (*owned is 1). 0 when there is none
 * to read, or it is too long to keep (the face is read whole then), -1
 * when memory ran out.
 */
BDF_INTERNAL int bdfi_fonts_cmap(const bdf_fonts *db, size_t face, const uint8_t **data,
                                 size_t *len, int *owned) {
  const bdfi_fdb_entry *e = &db->faces[face];
  const bdfi_fdb_file *f = &db->files[e->file];
  *owned = 0;
  if (e->cmap_len <= 0 || e->cmap_len > BDFI_FDB_COVERAGE) {
    return 0;
  }
  if (f->data) {
    if ((uint64_t)e->cmap_off + (uint64_t)e->cmap_len > f->len) {
      return 0;
    }
    *data = f->data + e->cmap_off;
    *len = (size_t)e->cmap_len;
    return 1;
  }
  FILE *fp = fopen(f->path, "rb");
  if (!fp) {
    return 0;
  }
  uint8_t *buf = malloc((size_t)e->cmap_len);
  int rc = buf ? 1 : -1;
  if (rc == 1) {
    bdfi_fdb_src src = {NULL, 0, fp};
    rc = bdfi_fdb_read_at(&src, (uint64_t)e->cmap_off, buf, (size_t)e->cmap_len);
  }
  fclose(fp);
  if (rc != 1) {
    free(buf);
    return rc;
  }
  *data = buf;
  *len = (size_t)e->cmap_len;
  *owned = 1;
  return 1;
}

/* Whether the face has a cmap table at all (cmapLength > 0): one without a
 * coverage is then read whole to ask it. */
BDF_INTERNAL int bdfi_fonts_has_cmap(const bdf_fonts *db, size_t face) {
  return db->faces[face].cmap_len > 0;
}
