/* See open_doc.h. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fseeko */
#endif
#include "open_doc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- The single-file form, read where the reader asks ------------------- */

static int seek(FILE *f, uint64_t off) {
#ifdef _WIN32
  return _fseeki64(f, (long long)off, SEEK_SET);
#else
  return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int64_t file_size(FILE *f) {
#ifdef _WIN32
  if (_fseeki64(f, 0, SEEK_END) != 0) {
    return -1;
  }
  return _ftelli64(f);
#else
  if (fseeko(f, 0, SEEK_END) != 0) {
    return -1;
  }
  return (int64_t)ftello(f);
#endif
}

/* bdf_source.read_at: the reader asks for the header, the manifest and each
 * part as it needs them, so a large file is never read whole. */
static int file_read_at(void *ctx, uint64_t off, uint8_t *buf, size_t len) {
  FILE *f = ctx;
  return seek(f, off) == 0 && fread(buf, 1, len, f) == len ? 0 : -1;
}

/* ---- The split form: manifest.json and parts/<name> --------------------- */

static char *join(const char *dir, const char *name) {
  size_t a = strlen(dir), b = strlen(name);
  char *p = malloc(a + b + 2);
  if (p) {
    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, name, b + 1);
  }
  return p;
}

static uint8_t *read_whole(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  int64_t size = file_size(f);
  uint8_t *data = size >= 0 && (uint64_t)size < SIZE_MAX ? malloc((size_t)size + 1) : NULL;
  if (data && (seek(f, 0) != 0 || fread(data, 1, (size_t)size, f) != (size_t)size)) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *len = data ? (size_t)size : 0;
  return data;
}

/* bdf_source.read_part: with buf NULL the size of the part, else its bytes. */
static int dir_read_part(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *size) {
  char parts[64];
  snprintf(parts, sizeof parts, "parts/%s", name);
  char *path = join(ctx, parts);
  FILE *f = path ? fopen(path, "rb") : NULL;
  free(path);
  if (!f) {
    return -1;
  }
  int rc = -1;
  int64_t n = file_size(f);
  if (n >= 0 && seek(f, 0) == 0) {
    if (!buf) {
      *size = (size_t)n;
      rc = 0;
    } else {
      *size = fread(buf, 1, cap < (uint64_t)n ? cap : (size_t)n, f);
      rc = 0;
    }
  }
  fclose(f);
  return rc;
}

/* ---- Opening ------------------------------------------------------------ */

int example_doc_open(example_doc *d, const char *path, const char *password, char *msg,
                     size_t cap) {
  memset(d, 0, sizeof *d);
  bdf_error err;
  bdf_status st;
  char *manifest_path = join(path, "manifest.json");
  size_t manifest_len = 0;
  uint8_t *manifest = manifest_path ? read_whole(manifest_path, &manifest_len) : NULL;
  free(manifest_path);
  if (manifest) {
    /* a directory of the split form */
    d->dir = malloc(strlen(path) + 1);
    if (!d->dir) {
      free(manifest);
      snprintf(msg, cap, "out of memory");
      return -1;
    }
    strcpy(d->dir, path);
    bdf_source src = {0};
    src.ctx = d->dir;
    src.read_part = dir_read_part;
    st = bdf_doc_open_split(manifest, manifest_len, &src, &d->doc, &err);
    free(manifest); /* the document keeps what it needs of it */
  } else {
    d->file = fopen(path, "rb");
    if (!d->file) {
      snprintf(msg, cap, "%s: cannot be opened", path);
      return -1;
    }
    int64_t size = file_size(d->file);
    if (size < 0) {
      snprintf(msg, cap, "%s: cannot be read", path);
      example_doc_close(d);
      return -1;
    }
    bdf_source src = {0};
    src.ctx = d->file;
    src.read_at = file_read_at;
    src.size = (uint64_t)size;
    st = bdf_doc_open(&src, &d->doc, &err);
  }
  if (st != BDF_OK) {
    snprintf(msg, cap, "%s: %s", path, err.message);
    example_doc_close(d);
    return -1;
  }
  if (bdf_doc_locked(d->doc)) {
    if (!password) {
      password = getenv("BDF_PASSWORD");
    }
    if (!password) {
      snprintf(msg, cap, "%s is encrypted: give its password with --password or $BDF_PASSWORD",
               path);
      example_doc_close(d);
      return -1;
    }
    /* (the password is normalised to NFKC by the library, as the format asks) */
    st = bdf_doc_unlock(d->doc, password, strlen(password), &err);
    if (st != BDF_OK) {
      snprintf(msg, cap, "%s: %s", path,
               st == BDF_ERR_PASSWORD ? "the password does not open it" : err.message);
      example_doc_close(d);
      return -1;
    }
  }
  return 0;
}

void example_doc_close(example_doc *d) {
  bdf_doc_free(d->doc);
  if (d->file) {
    fclose(d->file);
  }
  free(d->dir);
  memset(d, 0, sizeof *d);
}

/* ---- Fonts -------------------------------------------------------------- */

/* Fonts most systems of each kind have: a sans-serif face for Latin text
 * and one for Japanese, Chinese and Korean, in the order to prefer them. */
static const char *const system_fonts[] = {
#if defined(_WIN32)
    "C:\\Windows\\Fonts\\arial.ttf",
    "C:\\Windows\\Fonts\\times.ttf",
    "C:\\Windows\\Fonts\\cour.ttf",
    "C:\\Windows\\Fonts\\YuGothR.ttc",
    "C:\\Windows\\Fonts\\meiryo.ttc",
    "C:\\Windows\\Fonts\\msgothic.ttc",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Times.ttc",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Courier.ttc",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/ヒラギノ明朝 ProN.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
#endif
};

bdf_fonts *example_fonts(const char *const *files, size_t n) {
  bdf_fonts *fonts = NULL;
  bdf_error err;
  if (bdf_fonts_new(&fonts, &err) != BDF_OK) {
    return NULL;
  }
  if (n == 0) {
    files = system_fonts;
    n = sizeof system_fonts / sizeof system_fonts[0];
    for (size_t i = 0; i < n; i++) {
      (void)bdf_fonts_add_file(fonts, files[i], &err); /* those that are not there are skipped */
    }
  } else {
    for (size_t i = 0; i < n; i++) {
      if (bdf_fonts_add_file(fonts, files[i], &err) != BDF_OK) {
        fprintf(stderr, "%s: %s\n", files[i], err.message);
      }
    }
  }
  if (bdf_fonts_count(fonts) == 0) {
    bdf_fonts_free(fonts);
    return NULL;
  }
  return fonts;
}

const char *example_view_kind(bdf_view_kind kind) {
  switch (kind) {
  case BDF_VIEW_FIXED:
    return "fixed";
  case BDF_VIEW_FLOW:
    return "flow";
  case BDF_VIEW_SHEET:
    return "sheet";
  case BDF_VIEW_SCROLL:
    return "scroll";
  default:
    return "unknown";
  }
}
