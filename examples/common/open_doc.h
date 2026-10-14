/*
 * What the C examples share: opening a document from a path (the
 * single-file form read as it is needed, or the split form from its
 * directory), unlocking it with a password, and a set of fonts for the text
 * that names its font.
 */
#ifndef BDF_EXAMPLES_OPEN_DOC_H
#define BDF_EXAMPLES_OPEN_DOC_H

#include <stddef.h>
#include <stdio.h>

#include "bdf/bdf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A document and what it is read from, which must outlive it. */
typedef struct example_doc {
  bdf_doc *doc;
  FILE *file; /* the single-file form */
  char *dir;  /* the split form: its directory */
} example_doc;

/*
 * example_doc_open opens path: a .bdf file, or the directory of the split
 * form (manifest.json and parts/). An encrypted document is unlocked with
 * password, or when that is NULL with $BDF_PASSWORD. It returns 0, or -1
 * with a message in msg.
 */
int example_doc_open(example_doc *d, const char *path, const char *password, char *msg,
                     size_t cap);
void example_doc_close(example_doc *d);

/*
 * example_fonts makes the set of fonts a renderer draws text that names its
 * font with (documents made from HTML, Markdown or EPUB, and characters an
 * embedded font lacks): the given files, or when there are none a few fonts
 * the system usually has, those of them that are there. libbdf reads no
 * font directories by itself. It returns NULL when no font was added (such
 * text is then left out, with a warning).
 */
bdf_fonts *example_fonts(const char *const *files, size_t n);

/* example_view_kind is the kind of a view as a word. */
const char *example_view_kind(bdf_view_kind kind);

#ifdef __cplusplus
}
#endif

#endif
