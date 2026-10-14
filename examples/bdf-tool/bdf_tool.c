/*
 * bdf-tool: what a document holds, its pages as PNG images, its text, and a
 * search of it, from the command line. An example of libbdf's C API with
 * nothing but the library: the document, its views and pages, the
 * renderer, the text and the search.
 *
 *   bdf-tool info doc.bdf
 *   bdf-tool render doc.bdf -page 2 -dpi 144 -o page2.png
 *   bdf-tool text doc.bdf
 *   bdf-tool search doc.bdf "query"
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdf/bdf.h"
#include "open_doc.h"
#include "png_write.h"

#define MAX_FONTS 32

typedef struct options {
  const char *command, *path, *query;
  const char *password, *out;
  const char *fonts[MAX_FONTS];
  size_t nfonts;
  long view, page; /* -1 when not given */
  double dpi;
  int case_sensitive, has_region;
  float region[4];
} options;

static int usage(void) {
  fprintf(stderr,
          "usage: bdf-tool <command> <file.bdf | split directory> [options]\n"
          "\n"
          "commands:\n"
          "  info                 metadata, views and pages\n"
          "  render               a page as a PNG image (a region of a sheet or a scroll view)\n"
          "  text                 the text of the document, page by page\n"
          "  search <query>       where a phrase is, as the viewer finds it\n"
          "\n"
          "options:\n"
          "  -view N              the view, from 0 (render: the first that has pages)\n"
          "  -page N              the page, from 1 (render: 1)\n"
          "  -dpi D               render: pixels an inch (96)\n"
          "  -region x,y,w,h      render: the rectangle of the view, in its units\n"
          "  -o FILE              render: the image (page<N>.png)\n"
          "  -case                search: capitals are not small letters\n"
          "  -password P          the password of an encrypted document ($BDF_PASSWORD)\n"
          "  -font FILE           a font for text that names its font (again for more);\n"
          "                       without one, a few fonts of the system are used\n");
  return 2;
}

static int parse(int argc, char **argv, options *o) {
  memset(o, 0, sizeof *o);
  o->view = -1;
  o->page = -1;
  o->dpi = 96;
  int positional = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *next = i + 1 < argc ? argv[i + 1] : NULL;
    if (a[0] == '-' && a[1] != '\0') {
      if (a[1] == '-') {
        a++; /* --flag as -flag */
      }
      if (strcmp(a, "-case") == 0) {
        o->case_sensitive = 1;
        continue;
      }
      if (!next) {
        fprintf(stderr, "%s needs a value\n", argv[i]);
        return -1;
      }
      i++;
      if (strcmp(a, "-view") == 0) {
        o->view = strtol(next, NULL, 10);
      } else if (strcmp(a, "-page") == 0) {
        o->page = strtol(next, NULL, 10);
      } else if (strcmp(a, "-dpi") == 0) {
        o->dpi = strtod(next, NULL);
      } else if (strcmp(a, "-o") == 0) {
        o->out = next;
      } else if (strcmp(a, "-password") == 0) {
        o->password = next;
      } else if (strcmp(a, "-font") == 0) {
        if (o->nfonts == MAX_FONTS) {
          fprintf(stderr, "at most %d fonts\n", MAX_FONTS);
          return -1;
        }
        o->fonts[o->nfonts++] = next;
      } else if (strcmp(a, "-region") == 0) {
        if (sscanf(next, "%f,%f,%f,%f", &o->region[0], &o->region[1], &o->region[2],
                   &o->region[3]) != 4 ||
            o->region[2] <= 0 || o->region[3] <= 0) {
          fprintf(stderr, "-region is x,y,w,h\n");
          return -1;
        }
        o->has_region = 1;
      } else {
        fprintf(stderr, "unknown option %s\n", argv[i]);
        return -1;
      }
      continue;
    }
    switch (positional++) {
    case 0:
      o->command = a;
      break;
    case 1:
      o->path = a;
      break;
    case 2:
      o->query = a;
      break;
    default:
      fprintf(stderr, "too many arguments\n");
      return -1;
    }
  }
  if (!o->command || !o->path || (strcmp(o->command, "search") == 0 && !o->query)) {
    return -1;
  }
  return 0;
}

/* A length-tagged string, as libbdf gives them, for printf's %.*s. */
#define S(p, n) (int)(n), (p)

/* ---- info ---------------------------------------------------------------- */

static void print_dc(const bdf_doc *doc, const char *element) {
  for (size_t i = 0; i < bdf_doc_dc_count(doc, element); i++) {
    size_t n;
    const char *v = bdf_doc_dc(doc, element, i, &n);
    printf("%-10s %.*s\n", i == 0 ? element : "", S(v, n));
  }
}

static int cmd_info(const bdf_doc *doc) {
  bdf_meta meta;
  bdf_doc_meta(doc, &meta);
  printf("libbdf     %s\n", bdf_version());
  printf("form       %s%s\n", bdf_doc_split(doc) ? "split" : "single file",
         bdf_doc_encrypted(doc) ? ", encrypted" : "");
  if (meta.source_len) {
    printf("source     %.*s\n", S(meta.source, meta.source_len));
  }
  if (meta.generator_len) {
    printf("generator  %.*s\n", S(meta.generator, meta.generator_len));
  }
  static const char *const elements[] = {"title", "creator", "subject", "description", "language",
                                         "created", "modified"};
  for (size_t i = 0; i < sizeof elements / sizeof elements[0]; i++) {
    print_dc(doc, elements[i]);
  }
  printf("parts      %zu\n", bdf_doc_part_count(doc));
  size_t nviews = bdf_doc_view_count(doc);
  for (size_t v = 0; v < nviews; v++) {
    bdf_view view;
    if (bdf_doc_view(doc, v, &view) != BDF_OK) {
      continue;
    }
    printf("\nview %zu     %.*s (%s)%s%.*s%s\n", v, S(view.id, view.id_len),
           example_view_kind(view.kind_code), view.title_len ? " \"" : "",
           S(view.title, view.title_len), view.title_len ? "\"" : "");
    if (view.kind_code == BDF_VIEW_SHEET) {
      /* columns and rows come in runs of [count, size] */
      double cols = 0, rows = 0, w, h;
      float run[2];
      for (size_t i = 0; i < view.ncols && bdf_doc_view_run(doc, v, 0, i, run) == BDF_OK; i++) {
        cols += run[0];
      }
      for (size_t i = 0; i < view.nrows && bdf_doc_view_run(doc, v, 1, i, run) == BDF_OK; i++) {
        rows += run[0];
      }
      bdf_sheet_size(doc, v, &w, &h);
      printf("  %.0f columns, %.0f rows, %.0f × %.0f units, %zu inline tiles%s\n", cols, rows, w,
             h, view.ntiles, view.tiles_ref_len ? " and a tile index" : "");
      continue;
    }
    printf("  %zu page%s%s%s\n", view.npages, view.npages == 1 ? "" : "s",
           view.rtl ? ", right to left" : "", view.text_index_len ? ", with a text index" : "");
    for (size_t p = 0; p < view.npages && p < 20; p++) {
      bdf_page page;
      if (bdf_doc_page(doc, v, p, &page) == BDF_OK) {
        printf("  page %-4zu %g × %g, %zu layer%s\n", p + 1, page.w, page.h, page.nlayers,
               page.nlayers == 1 ? "" : "s");
      }
    }
    if (view.npages > 20) {
      printf("  ...\n");
    }
  }
  return 0;
}

/* ---- render -------------------------------------------------------------- */

static int cmd_render(const bdf_doc *doc, const options *o) {
  size_t nviews = bdf_doc_view_count(doc);
  size_t v = 0;
  if (o->view >= 0) {
    v = (size_t)o->view;
  } else {
    /* the first view that has pages or is a sheet */
    for (v = 0; v < nviews; v++) {
      bdf_view view;
      if (bdf_doc_view(doc, v, &view) == BDF_OK &&
          (view.npages > 0 || view.kind_code == BDF_VIEW_SHEET)) {
        break;
      }
    }
  }
  bdf_view view;
  if (v >= nviews || bdf_doc_view(doc, v, &view) != BDF_OK) {
    fprintf(stderr, "no such view\n");
    return 1;
  }
  bdf_fonts *fonts = example_fonts(o->fonts, o->nfonts);
  bdf_render_options ropts = {0};
  ropts.fonts = fonts;
  bdf_renderer *r = NULL;
  bdf_error err;
  if (bdf_renderer_new(doc, &ropts, &r, &err) != BDF_OK) {
    fprintf(stderr, "%s\n", err.message);
    bdf_fonts_free(fonts);
    return 1;
  }
  /* a unit of a page is a point: dpi / 72 pixels a unit */
  double scale = o->dpi / 72;
  long page = o->page > 0 ? o->page : 1;
  bdf_image img = {0};
  bdf_status st;
  if (o->has_region || view.kind_code == BDF_VIEW_SHEET) {
    float region[4];
    if (o->has_region) {
      memcpy(region, o->region, sizeof region);
    } else {
      /* the top left of a sheet, as much as fits in 2,000 pixels a side */
      double w, h;
      bdf_sheet_size(doc, v, &w, &h);
      double most = 2000 / scale;
      region[0] = 0;
      region[1] = 0;
      region[2] = (float)(w < most ? w : most);
      region[3] = (float)(h < most ? h : most);
    }
    int w = (int)(region[2] * scale + 0.5), h = (int)(region[3] * scale + 0.5);
    st = bdf_render_region(r, v, (size_t)(page - 1), region, w > 0 ? w : 1, h > 0 ? h : 1, &img,
                           &err);
  } else {
    if (page > (long)view.npages) {
      fprintf(stderr, "the view has %zu pages\n", view.npages);
      bdf_renderer_free(r);
      bdf_fonts_free(fonts);
      return 1;
    }
    st = bdf_render_page(r, v, (size_t)(page - 1), scale, &img, &err);
  }
  int rc = 0;
  if (st != BDF_OK) {
    fprintf(stderr, "%s\n", st == BDF_ERR_TOO_LARGE ? "too large: ask for fewer dots an inch"
                                                     : err.message);
    rc = 1;
  } else {
    char name[64];
    const char *out = o->out;
    if (!out) {
      snprintf(name, sizeof name, "page%ld.png", page);
      out = name;
    }
    if (png_write_rgba(out, img.pixels, img.width, img.height, 1) != 0) {
      fprintf(stderr, "%s: cannot be written\n", out);
      rc = 1;
    } else {
      printf("%s: %d × %d\n", out, img.width, img.height);
    }
    bdf_image_free(&img);
  }
  /* what could not be drawn as the viewer would draw it */
  for (size_t i = 0; i < bdf_renderer_warning_count(r); i++) {
    fprintf(stderr, "warning: %s\n", bdf_renderer_warning(r, i));
  }
  bdf_renderer_free(r); /* before the fonts and the document it draws */
  bdf_fonts_free(fonts);
  return rc;
}

/* ---- text ---------------------------------------------------------------- */

static int cmd_text(const bdf_doc *doc, const options *o) {
  /* the text of every view, page by page, as a search index would take it */
  bdf_search_text *st = NULL;
  bdf_error err;
  if (bdf_doc_search_text(doc, &st, &err) != BDF_OK) {
    fprintf(stderr, "%s\n", err.message);
    return 1;
  }
  for (size_t v = 0; v < bdf_search_text_view_count(st); v++) {
    bdf_search_view view;
    bdf_search_text_view(st, v, &view);
    if (o->view >= 0 && bdf_doc_view_find(doc, view.id, view.id_len) != o->view) {
      continue;
    }
    printf("== %.*s (%.*s)\n", S(view.id, view.id_len), S(view.kind, view.kind_len));
    for (size_t i = 0; i < view.npages; i++) {
      bdf_search_page page;
      bdf_search_text_page(st, v, i, &page);
      if (o->page > 0 && page.page != o->page) {
        continue;
      }
      if (page.page > 0) {
        printf("-- page %lld\n", (long long)page.page);
      }
      printf("%.*s\n", S(page.text, page.text_len));
    }
  }
  bdf_search_text_free(st);
  return 0;
}

/* ---- search -------------------------------------------------------------- */

static int cmd_search(const bdf_doc *doc, const options *o) {
  size_t found = 0;
  for (size_t v = 0; v < bdf_doc_view_count(doc); v++) {
    bdf_view view;
    if ((o->view >= 0 && (size_t)o->view != v) || bdf_doc_view(doc, v, &view) != BDF_OK) {
      continue;
    }
    /* the view's text index part, or its text extracted from its objects */
    bdf_text_index *index = NULL;
    bdf_text_search *search = NULL;
    bdf_text_hits *hits = NULL;
    bdf_error err;
    if (bdf_doc_view_text(doc, v, 0, &index, &err) != BDF_OK ||
        bdf_text_search_new(index, o->case_sensitive, &search, &err) != BDF_OK ||
        bdf_text_search_find(search, o->query, strlen(o->query), BDF_SEARCH_LIMIT, 30, &hits,
                             &err) != BDF_OK) {
      fprintf(stderr, "view %zu: %s\n", v, err.message);
    } else {
      for (size_t i = 0; i < bdf_text_hits_count(hits); i++) {
        bdf_text_hit hit;
        bdf_text_hit_segment seg;
        bdf_text_hits_at(hits, i, &hit);
        bdf_text_hits_segment(hits, i, 0, &seg);
        /* a hit lies in runs: of a page (a is the page, from 0), or of a tile */
        if (view.kind_code == BDF_VIEW_SHEET) {
          printf("%.*s tile %u,%u: ", S(view.id, view.id_len), seg.a, seg.b);
        } else {
          printf("%.*s page %u: ", S(view.id, view.id_len), seg.a + 1);
        }
        printf("\"%.*s\" in …%.*s…\n", S(hit.text, hit.text_len), S(hit.context, hit.context_len));
        found++;
      }
    }
    bdf_text_hits_free(hits);
    bdf_text_search_free(search);
    bdf_text_index_free(index); /* after the search, which reads it */
  }
  if (found == 0) {
    printf("not found\n");
  }
  return found ? 0 : 1;
}

int main(int argc, char **argv) {
  options o;
  if (parse(argc, argv, &o) != 0) {
    return usage();
  }
  example_doc d;
  char msg[512];
  if (example_doc_open(&d, o.path, o.password, msg, sizeof msg) != 0) {
    fprintf(stderr, "%s\n", msg);
    return 1;
  }
  int rc;
  if (strcmp(o.command, "info") == 0) {
    rc = cmd_info(d.doc);
  } else if (strcmp(o.command, "render") == 0) {
    rc = cmd_render(d.doc, &o);
  } else if (strcmp(o.command, "text") == 0) {
    rc = cmd_text(d.doc, &o);
  } else if (strcmp(o.command, "search") == 0) {
    rc = cmd_search(d.doc, &o);
  } else {
    rc = usage();
  }
  example_doc_close(&d);
  return rc;
}
