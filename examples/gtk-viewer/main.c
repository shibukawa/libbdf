/*
 * A viewer of BDF documents with GTK 4, drawing through cairo: libbdf runs
 * the instructions of a page, and the cairo adapter hands what they draw to
 * cairo (paths, gradients, images, groups), which GTK puts on the screen.
 * Pages stay sharp at any zoom and on any display, since cairo draws them
 * at the resolution of the window.
 *
 *   gtk-viewer doc.bdf [-password P] [-font FILE]...
 *
 * Keys: → ← (or Page Down, Page Up, Space) next and previous page, Home and
 * End, + − zoom, 0 fit, V next view, Q quit.
 */
#include <gtk/gtk.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdf/bdf_cairo.h"
#include "open_doc.h"

#define MAX_FONTS 32

typedef struct viewer {
  const char *path;
  example_doc doc;
  bdf_fonts *fonts;
  bdf_renderer *renderer;
  bdf_cairo *cairo; /* the renderer drawing through cairo */
  size_t view, page;
  bdf_view info;
  double zoom; /* 0: fit the window */
  GtkWidget *window, *area, *label;
} viewer;

/* A view that has pages: fixed and flow views, and the strips of a scroll
 * view. (A sheet is drawn tile by tile, with bdf_cairo_draw_part; this
 * example leaves sheets to sdl-viewer and bdf-tool.) */
static int has_pages(const bdf_view *info) {
  return info->npages > 0 && info->kind_code != BDF_VIEW_SHEET;
}

static void update(viewer *v) {
  char text[256];
  if (has_pages(&v->info)) {
    snprintf(text, sizeof text, "%.*s — %zu / %zu", (int)v->info.title_len, v->info.title,
             v->page + 1, v->info.npages);
  } else {
    snprintf(text, sizeof text, "no view with pages");
  }
  gtk_label_set_text(GTK_LABEL(v->label), text);
  gtk_widget_queue_draw(v->area);
}

static void show(viewer *v, size_t view, size_t page) {
  if (bdf_doc_view(v->doc.doc, view, &v->info) != BDF_OK) {
    return;
  }
  v->view = view;
  v->page = page < v->info.npages ? page : (v->info.npages ? v->info.npages - 1 : 0);
  update(v);
}

static void next_view(viewer *v, long step) {
  long n = (long)bdf_doc_view_count(v->doc.doc);
  for (long k = 1; k <= n; k++) {
    size_t i = (size_t)((((long)v->view + step * k) % n + n) % n);
    bdf_view info;
    if (bdf_doc_view(v->doc.doc, i, &info) == BDF_OK && has_pages(&info)) {
      show(v, i, 0);
      return;
    }
  }
}

/* GtkDrawingArea's draw function: the page in the middle of the area. */
static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
  viewer *v = data;
  (void)area;
  cairo_set_source_rgb(cr, 0x55 / 255.0, 0x58 / 255.0, 0x5e / 255.0);
  cairo_paint(cr);
  bdf_page page;
  if (!has_pages(&v->info) || bdf_doc_page(v->doc.doc, v->view, v->page, &page) != BDF_OK ||
      page.w <= 0 || page.h <= 0) {
    return;
  }
  double zoom = v->zoom > 0 ? v->zoom : fmin((width - 24) / page.w, (height - 24) / page.h);
  double x = (width - page.w * zoom) / 2, y = (height - page.h * zoom) / 2;
  cairo_save(cr);
  cairo_translate(cr, x > 0 ? x : 12, y > 0 ? y : 12);
  cairo_scale(cr, zoom, zoom); /* a unit of a page is a point */
  /* the paper is the caller's */
  cairo_rectangle(cr, 0, 0, page.w, page.h);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_fill_preserve(cr);
  cairo_clip(cr);
  bdf_error err;
  if (bdf_cairo_draw_page(v->cairo, v->view, v->page, cr, &err) != BDF_OK) {
    fprintf(stderr, "page %zu: %s\n", v->page + 1, err.message);
  }
  cairo_restore(cr);
}

static gboolean on_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType mods,
                       gpointer data) {
  viewer *v = data;
  (void)keys;
  (void)code;
  switch (key) {
  case GDK_KEY_q:
  case GDK_KEY_Escape:
    gtk_window_destroy(GTK_WINDOW(v->window));
    return TRUE;
  case GDK_KEY_Right:
  case GDK_KEY_Page_Down:
  case GDK_KEY_space:
    if (v->page + 1 < v->info.npages) {
      show(v, v->view, v->page + 1);
    }
    return TRUE;
  case GDK_KEY_Left:
  case GDK_KEY_Page_Up:
    if (v->page > 0) {
      show(v, v->view, v->page - 1);
    }
    return TRUE;
  case GDK_KEY_Home:
    show(v, v->view, 0);
    return TRUE;
  case GDK_KEY_End:
    show(v, v->view, v->info.npages ? v->info.npages - 1 : 0);
    return TRUE;
  case GDK_KEY_plus:
  case GDK_KEY_equal:
  case GDK_KEY_minus: {
    bdf_page page;
    if (v->zoom <= 0 && bdf_doc_page(v->doc.doc, v->view, v->page, &page) == BDF_OK) {
      int w = gtk_widget_get_width(v->area), h = gtk_widget_get_height(v->area);
      v->zoom = fmin((w - 24) / page.w, (h - 24) / page.h);
    }
    v->zoom *= key == GDK_KEY_minus ? 0.8 : 1.25;
    gtk_widget_queue_draw(v->area);
    return TRUE;
  }
  case GDK_KEY_0:
    v->zoom = 0;
    gtk_widget_queue_draw(v->area);
    return TRUE;
  case GDK_KEY_v:
  case GDK_KEY_V:
    next_view(v, (mods & GDK_SHIFT_MASK) ? -1 : 1);
    return TRUE;
  default:
    return FALSE;
  }
}

static void step(viewer *v, int by) {
  if (by > 0 && v->page + 1 < v->info.npages) {
    show(v, v->view, v->page + 1);
  } else if (by < 0 && v->page > 0) {
    show(v, v->view, v->page - 1);
  }
}

static void on_previous(GtkButton *b, gpointer data) {
  (void)b;
  step(data, -1);
}

static void on_next(GtkButton *b, gpointer data) {
  (void)b;
  step(data, 1);
}

static void activate(GtkApplication *app, gpointer data) {
  viewer *v = data;
  v->window = gtk_application_window_new(app);
  const char *name = strrchr(v->path, '/');
  gtk_window_set_title(GTK_WINDOW(v->window), name ? name + 1 : v->path);
  gtk_window_set_default_size(GTK_WINDOW(v->window), 900, 700);

  GtkWidget *bar = gtk_header_bar_new();
  GtkWidget *previous = gtk_button_new_from_icon_name("go-previous-symbolic");
  GtkWidget *next = gtk_button_new_from_icon_name("go-next-symbolic");
  g_signal_connect(previous, "clicked", G_CALLBACK(on_previous), v);
  g_signal_connect(next, "clicked", G_CALLBACK(on_next), v);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), previous);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), next);
  v->label = gtk_label_new("");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), v->label);
  gtk_window_set_titlebar(GTK_WINDOW(v->window), bar);

  v->area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->area), draw, v, NULL);
  gtk_window_set_child(GTK_WINDOW(v->window), v->area);

  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), v);
  gtk_widget_add_controller(v->window, keys);

  v->view = bdf_doc_view_count(v->doc.doc) - 1;
  next_view(v, 1); /* the first view with pages */
  update(v);
  gtk_window_present(GTK_WINDOW(v->window));
}

static int usage(void) {
  fprintf(stderr, "usage: gtk-viewer <file.bdf | split directory> [-password P] [-font FILE]...\n");
  return 2;
}

int main(int argc, char **argv) {
  viewer v;
  memset(&v, 0, sizeof v);
  const char *password = NULL, *fonts[MAX_FONTS];
  size_t nfonts = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i][0] == '-' && argv[i][1] == '-' ? argv[i] + 1 : argv[i];
    if (a[0] != '-') {
      v.path = a;
    } else if (i + 1 < argc && strcmp(a, "-password") == 0) {
      password = argv[++i];
    } else if (i + 1 < argc && strcmp(a, "-font") == 0 && nfonts < MAX_FONTS) {
      fonts[nfonts++] = argv[++i];
    } else {
      return usage();
    }
  }
  if (!v.path) {
    return usage();
  }
  char msg[512];
  if (example_doc_open(&v.doc, v.path, password, msg, sizeof msg) != 0) {
    fprintf(stderr, "%s\n", msg);
    return 1;
  }
  v.fonts = example_fonts(fonts, nfonts);
  bdf_render_options ropts = {0};
  ropts.fonts = v.fonts;
  bdf_error err;
  if (bdf_renderer_new(v.doc.doc, &ropts, &v.renderer, &err) != BDF_OK ||
      !(v.cairo = bdf_cairo_new(v.renderer))) {
    fprintf(stderr, "%s\n", err.message);
    return 1;
  }

  /* the document is open: GTK gets no arguments of ours */
  GtkApplication *app = gtk_application_new("io.github.shibukawa.libbdf.GtkViewer",
                                            G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(activate), &v);
  int rc = g_application_run(G_APPLICATION(app), 1, argv);
  g_object_unref(app);

  for (size_t i = 0; i < bdf_renderer_warning_count(v.renderer); i++) {
    fprintf(stderr, "warning: %s\n", bdf_renderer_warning(v.renderer, i));
  }
  bdf_cairo_free(v.cairo); /* before the renderer */
  bdf_renderer_free(v.renderer);
  bdf_fonts_free(v.fonts);
  example_doc_close(&v.doc);
  return rc;
}
