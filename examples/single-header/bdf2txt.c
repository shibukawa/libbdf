/*
 * bdf2txt: the text of a BDF document, view by view, with libbdf as one
 * header and nothing else: no library to build or link but the C library
 * and its math library.
 *
 *   python3 ../../tools/amalgamate.py -o bdf/bdf.h   (or the bdf.h of a release)
 *   cc -std=c11 -O2 -I. bdf2txt.c -lm -o bdf2txt
 *   ./bdf2txt doc.bdf
 *
 * One C file of a program defines BDF_IMPLEMENTATION before it includes the
 * header, and gets the library; BDF_STATIC keeps the library's functions
 * out of the program's link namespace. Other files include the header
 * without either, for the declarations.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define BDF_IMPLEMENTATION
#define BDF_STATIC
#include "bdf/bdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The whole file in memory: bdf_doc_open_memory reads from it in place. */
static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 1 << 16, n = 0, got;
  uint8_t *data = malloc(cap);
  while (data && (got = fread(data + n, 1, cap - n, f)) > 0) {
    n += got;
    if (n == cap) {
      uint8_t *more = realloc(data, cap * 2);
      if (!more) {
        free(data);
      }
      data = more;
      cap *= 2;
    }
  }
  fclose(f);
  *len = n;
  return data;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bdf2txt <file.bdf>\n");
    return 2;
  }
  size_t len;
  uint8_t *data = read_file(argv[1], &len);
  if (!data) {
    fprintf(stderr, "%s: cannot be read\n", argv[1]);
    return 1;
  }
  bdf_doc *doc = NULL;
  bdf_error err;
  if (bdf_doc_open_memory(data, len, &doc, &err) != BDF_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], err.message);
    free(data);
    return 1;
  }
  const char *password = getenv("BDF_PASSWORD");
  if (bdf_doc_locked(doc) &&
      (!password || bdf_doc_unlock(doc, password, strlen(password), &err) != BDF_OK)) {
    fprintf(stderr, "%s: encrypted; give its password in $BDF_PASSWORD\n", argv[1]);
    bdf_doc_free(doc);
    free(data);
    return 1;
  }
  for (size_t v = 0; v < bdf_doc_view_count(doc); v++) {
    bdf_view view;
    bdf_text_index *index = NULL;
    char *text = NULL;
    size_t text_len = 0;
    bdf_doc_view(doc, v, &view);
    /* the view's text in reading order: its text index part, or the text
     * of its objects; joined with a space or a line break as the runs say */
    if (bdf_doc_view_text(doc, v, 0, &index, &err) != BDF_OK ||
        bdf_text_index_plain(index, &text, &text_len, &err) != BDF_OK) {
      fprintf(stderr, "view %zu: %s\n", v, err.message);
    } else if (text_len > 0) {
      printf("== %.*s\n%.*s\n", (int)view.id_len, view.id, (int)text_len, text);
    }
    bdf_free(text);
    bdf_text_index_free(index);
  }
  bdf_doc_free(doc);
  free(data); /* after the document, which reads from it */
  return 0;
}
