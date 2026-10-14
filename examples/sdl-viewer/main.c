/*
 * A viewer of BDF documents with SDL3 and libbdf's own rasterizer: the
 * same pixels as the reference's renderer, on any platform SDL runs on,
 * with no 2D library underneath. Only what the window shows is drawn, at
 * the size it is shown, so a page at any zoom and a sheet of any size cost
 * no more than a window of pixels.
 *
 *   sdl-viewer doc.bdf [-view N] [-page N] [-password P] [-font FILE]...
 *
 * Keys: → ← (or Page Down, Page Up, Space) next and previous page, Home and
 * End, ↑ ↓ scroll, + − zoom, 0 fit, V next view, Q quit. The wheel scrolls,
 * with Ctrl or ⌘ it zooms; dragging moves the view.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdf/bdf.h"
#include "open_doc.h"
#include "png_write.h"

#define MAX_FONTS 32

typedef struct viewer {
  const char *path;
  example_doc doc;
  bdf_fonts *fonts;
  bdf_renderer *renderer;
  SDL_Window *window;
  SDL_Renderer *sdl;
  SDL_Texture *texture;
  int tex_w, tex_h;
  SDL_FRect dst; /* where the texture is drawn in the window, in pixels */
  /* what is shown */
  size_t view, page;
  bdf_view info;
  double cw, ch; /* the size of what is shown, in units */
  double zoom;   /* pixels a unit */
  double x, y;   /* the unit at the top left of the window */
  int fit;       /* the zoom follows the window */
  int dirty;
} viewer;

/* ---- What is shown ------------------------------------------------------ */

static int window_pixels(viewer *v, int *w, int *h) {
  return SDL_GetWindowSizeInPixels(v->window, w, h) && *w > 0 && *h > 0;
}

/* The size of what the current view shows: a page, the strips of a scroll
 * view stacked, or a whole sheet. */
static void measure(viewer *v) {
  bdf_doc *doc = v->doc.doc;
  v->cw = v->ch = 0;
  if (v->info.kind_code == BDF_VIEW_SHEET) {
    bdf_sheet_size(doc, v->view, &v->cw, &v->ch);
  } else if (v->info.kind_code == BDF_VIEW_SCROLL) {
    for (size_t p = 0; p < v->info.npages; p++) {
      bdf_page page;
      if (bdf_doc_page(doc, v->view, p, &page) == BDF_OK) {
        v->cw = page.w > v->cw ? page.w : v->cw;
        v->ch += page.h;
      }
    }
  } else {
    bdf_page page;
    if (bdf_doc_page(doc, v->view, v->page, &page) == BDF_OK) {
      v->cw = page.w;
      v->ch = page.h;
    }
  }
}

/* Keeps what is shown in the window: centred when it is smaller. */
static void clamp(viewer *v) {
  int w, h;
  if (!window_pixels(v, &w, &h)) {
    return;
  }
  double vw = w / v->zoom, vh = h / v->zoom;
  v->x = v->cw <= vw ? (v->cw - vw) / 2 : fmin(fmax(v->x, 0), v->cw - vw);
  v->y = v->ch <= vh ? (v->ch - vh) / 2 : fmin(fmax(v->y, 0), v->ch - vh);
}

/* A page fitted to the window; a sheet or a scroll view at its own size
 * (a unit is a point) across its width at most. */
static void fit(viewer *v) {
  int w, h;
  if (!window_pixels(v, &w, &h) || v->cw <= 0 || v->ch <= 0) {
    return;
  }
  double density = SDL_GetWindowPixelDensity(v->window);
  if (v->info.kind_code == BDF_VIEW_SHEET || v->info.kind_code == BDF_VIEW_SCROLL) {
    v->zoom = fmin(density, (w - 16) / v->cw);
    v->x = v->y = 0;
  } else {
    v->zoom = fmin((w - 16) / v->cw, (h - 16) / v->ch);
  }
  v->fit = 1;
  clamp(v);
}

static void update_title(viewer *v) {
  char title[512];
  const char *name = strrchr(v->path, '/');
  name = name ? name + 1 : v->path;
  double density = SDL_GetWindowPixelDensity(v->window);
  int percent = (int)(v->zoom / density * 100 + 0.5);
  if (v->info.kind_code == BDF_VIEW_SHEET || v->info.kind_code == BDF_VIEW_SCROLL) {
    snprintf(title, sizeof title, "%s — %.*s — %d%%", name, (int)v->info.title_len,
             v->info.title, percent);
  } else {
    snprintf(title, sizeof title, "%s — %.*s — %zu / %zu — %d%%", name, (int)v->info.title_len,
             v->info.title, v->page + 1, v->info.npages, percent);
  }
  SDL_SetWindowTitle(v->window, title);
}

static void show(viewer *v, size_t view, size_t page) {
  if (bdf_doc_view(v->doc.doc, view, &v->info) != BDF_OK) {
    return;
  }
  v->view = view;
  v->page = v->info.npages > 0 && page >= v->info.npages ? v->info.npages - 1 : page;
  measure(v);
  fit(v);
  v->dirty = 1;
}

/* The next view that shows something, after (or before) the current one. */
static void next_view(viewer *v, int step) {
  long n = (long)bdf_doc_view_count(v->doc.doc);
  for (long k = 1; k <= n; k++) {
    size_t i = (size_t)((((long)v->view + step * k) % n + n) % n);
    bdf_view info;
    if (bdf_doc_view(v->doc.doc, i, &info) == BDF_OK &&
        (info.npages > 0 || info.kind_code == BDF_VIEW_SHEET)) {
      show(v, i, 0);
      return;
    }
  }
}

static void zoom_at(viewer *v, double factor, double px, double py) {
  double ux = v->x + px / v->zoom, uy = v->y + py / v->zoom;
  v->zoom = fmin(fmax(v->zoom * factor, 0.05), 32);
  v->x = ux - px / v->zoom;
  v->y = uy - py / v->zoom;
  v->fit = 0;
  clamp(v);
  v->dirty = 1;
}

/* ---- Drawing ------------------------------------------------------------- */

/*
 * Draws what the window shows of the view into the texture: the part of the
 * page (or of the sheet, or of the strips) inside the window, at the size
 * it is shown. bdf_render_region takes a rectangle in units and the pixels
 * to draw it into.
 */
static void render(viewer *v) {
  int w, h;
  v->dirty = 0;
  v->dst.w = v->dst.h = 0;
  if (!window_pixels(v, &w, &h) || v->cw <= 0 || v->ch <= 0) {
    return;
  }
  double x0 = fmax(v->x, 0), y0 = fmax(v->y, 0);
  double x1 = fmin(v->x + w / v->zoom, v->cw), y1 = fmin(v->y + h / v->zoom, v->ch);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  /* whole pixels of the window, and the units they hold */
  int px0 = (int)floor((x0 - v->x) * v->zoom), py0 = (int)floor((y0 - v->y) * v->zoom);
  int px1 = (int)ceil((x1 - v->x) * v->zoom), py1 = (int)ceil((y1 - v->y) * v->zoom);
  px0 = px0 < 0 ? 0 : px0;
  py0 = py0 < 0 ? 0 : py0;
  px1 = px1 > w ? w : px1;
  py1 = py1 > h ? h : py1;
  int iw = px1 - px0, ih = py1 - py0;
  if (iw <= 0 || ih <= 0) {
    return;
  }
  float region[4] = {(float)(v->x + px0 / v->zoom), (float)(v->y + py0 / v->zoom),
                     (float)(iw / v->zoom), (float)(ih / v->zoom)};
  bdf_image img;
  bdf_error err;
  if (bdf_render_region(v->renderer, v->view, v->page, region, iw, ih, &img, &err) != BDF_OK) {
    fprintf(stderr, "%s\n", err.message);
    return;
  }
  if (!v->texture || v->tex_w != iw || v->tex_h != ih) {
    SDL_DestroyTexture(v->texture);
    /* RGBA32 is red, green, blue, alpha in memory, as libbdf draws; the
     * page is opaque (white behind it), so nothing is blended */
    v->texture =
        SDL_CreateTexture(v->sdl, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, iw, ih);
    v->tex_w = iw;
    v->tex_h = ih;
    if (v->texture) {
      SDL_SetTextureBlendMode(v->texture, SDL_BLENDMODE_NONE);
    }
  }
  if (v->texture) {
    SDL_UpdateTexture(v->texture, NULL, img.pixels, iw * 4);
    v->dst = (SDL_FRect){(float)px0, (float)py0, (float)iw, (float)ih};
  }
  bdf_image_free(&img);
  update_title(v);
}

static void present(viewer *v) {
  if (v->dirty) {
    render(v);
  }
  SDL_SetRenderDrawColor(v->sdl, 0x55, 0x58, 0x5e, 0xff);
  SDL_RenderClear(v->sdl);
  if (v->texture && v->dst.w > 0) {
    SDL_RenderTexture(v->sdl, v->texture, NULL, &v->dst);
  }
  SDL_RenderPresent(v->sdl);
}

/* Writes what the window shows to a PNG file: for a run without a person
 * at the screen (SDL_VIDEODRIVER=dummy). */
static int screenshot(viewer *v, const char *path) {
  if (v->dirty) {
    render(v);
  }
  SDL_SetRenderDrawColor(v->sdl, 0x55, 0x58, 0x5e, 0xff);
  SDL_RenderClear(v->sdl);
  if (v->texture && v->dst.w > 0) {
    SDL_RenderTexture(v->sdl, v->texture, NULL, &v->dst);
  }
  SDL_Surface *shot = SDL_RenderReadPixels(v->sdl, NULL);
  SDL_Surface *rgba = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32) : NULL;
  int rc = -1, w = 0, h = 0;
  if (rgba) {
    w = rgba->w;
    h = rgba->h;
    /* rows of the surface may be padded: copy them tight */
    uint8_t *tight = malloc((size_t)rgba->w * (size_t)rgba->h * 4);
    if (tight) {
      for (int y = 0; y < rgba->h; y++) {
        memcpy(tight + (size_t)y * (size_t)rgba->w * 4,
               (const uint8_t *)rgba->pixels + (size_t)y * (size_t)rgba->pitch,
               (size_t)rgba->w * 4);
      }
      rc = png_write_rgba(path, tight, rgba->w, rgba->h, 0);
      free(tight);
    }
  }
  SDL_DestroySurface(rgba);
  SDL_DestroySurface(shot);
  if (rc == 0) {
    printf("%s: %d × %d\n", path, w, h);
  } else {
    fprintf(stderr, "%s: %s\n", path, shot ? "cannot be written" : SDL_GetError());
  }
  return rc;
}

/* ---- Events -------------------------------------------------------------- */

static int on_key(viewer *v, const SDL_KeyboardEvent *k) {
  int w, h;
  window_pixels(v, &w, &h);
  int paged = v->info.kind_code != BDF_VIEW_SHEET && v->info.kind_code != BDF_VIEW_SCROLL;
  switch (k->key) {
  case SDLK_Q:
  case SDLK_ESCAPE:
    return 0;
  case SDLK_RIGHT:
  case SDLK_PAGEDOWN:
  case SDLK_SPACE:
    if (paged && v->page + 1 < v->info.npages) {
      show(v, v->view, v->page + 1);
    } else if (!paged) {
      v->y += h / v->zoom * 0.9;
      clamp(v);
      v->dirty = 1;
    }
    break;
  case SDLK_LEFT:
  case SDLK_PAGEUP:
    if (paged && v->page > 0) {
      show(v, v->view, v->page - 1);
    } else if (!paged) {
      v->y -= h / v->zoom * 0.9;
      clamp(v);
      v->dirty = 1;
    }
    break;
  case SDLK_HOME:
    show(v, v->view, 0);
    break;
  case SDLK_END:
    show(v, v->view, v->info.npages ? v->info.npages - 1 : 0);
    if (!paged) {
      v->y = v->ch;
      clamp(v);
    }
    break;
  case SDLK_UP:
  case SDLK_DOWN:
    v->y += (k->key == SDLK_UP ? -1 : 1) * h / v->zoom * 0.1;
    clamp(v);
    v->dirty = 1;
    break;
  case SDLK_EQUALS:
  case SDLK_PLUS:
  case SDLK_KP_PLUS:
    zoom_at(v, 1.25, w / 2.0, h / 2.0);
    break;
  case SDLK_MINUS:
  case SDLK_KP_MINUS:
    zoom_at(v, 0.8, w / 2.0, h / 2.0);
    break;
  case SDLK_0:
    fit(v);
    v->dirty = 1;
    break;
  case SDLK_V:
    next_view(v, (k->mod & SDL_KMOD_SHIFT) ? -1 : 1);
    break;
  default:
    break;
  }
  return 1;
}

static void run(viewer *v) {
  int dragging = 0;
  present(v);
  for (SDL_Event e; SDL_WaitEvent(&e);) {
    float density = SDL_GetWindowPixelDensity(v->window);
    switch (e.type) {
    case SDL_EVENT_QUIT:
      return;
    case SDL_EVENT_KEY_DOWN:
      if (!on_key(v, &e.key)) {
        return;
      }
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      if (v->fit) {
        fit(v);
      } else {
        clamp(v);
      }
      v->dirty = 1;
      break;
    case SDL_EVENT_MOUSE_WHEEL: {
      SDL_Keymod mod = SDL_GetModState();
      if (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
        zoom_at(v, pow(1.1, e.wheel.y), e.wheel.mouse_x * density, e.wheel.mouse_y * density);
      } else {
        v->x -= e.wheel.x * 40 * density / v->zoom;
        v->y -= e.wheel.y * 40 * density / v->zoom;
        clamp(v);
        v->dirty = 1;
      }
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      dragging = e.button.button == SDL_BUTTON_LEFT;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      dragging = 0;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (dragging) {
        v->x -= e.motion.xrel * density / v->zoom;
        v->y -= e.motion.yrel * density / v->zoom;
        clamp(v);
        v->dirty = 1;
      }
      break;
    default:
      break;
    }
    /* draw once the events that came together are handled */
    if (!SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
      present(v);
    }
  }
}

/* ---- Main ---------------------------------------------------------------- */

static int usage(void) {
  fprintf(stderr, "usage: sdl-viewer <file.bdf | split directory> [-view N] [-page N]\n"
                  "                  [-password P] [-font FILE]... [-screenshot FILE.png]\n");
  return 2;
}

int main(int argc, char **argv) {
  const char *path = NULL, *password = NULL, *shot = NULL;
  const char *fonts[MAX_FONTS];
  size_t nfonts = 0;
  long view = -1, page = 1;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i][0] == '-' && argv[i][1] == '-' ? argv[i] + 1 : argv[i];
    const char *next = i + 1 < argc ? argv[i + 1] : NULL;
    if (a[0] != '-') {
      if (path) {
        return usage();
      }
      path = a;
    } else if (!next) {
      return usage();
    } else if (strcmp(a, "-view") == 0) {
      view = strtol(argv[++i], NULL, 10);
    } else if (strcmp(a, "-page") == 0) {
      page = strtol(argv[++i], NULL, 10);
    } else if (strcmp(a, "-password") == 0) {
      password = argv[++i];
    } else if (strcmp(a, "-screenshot") == 0) {
      shot = argv[++i];
    } else if (strcmp(a, "-font") == 0 && nfonts < MAX_FONTS) {
      fonts[nfonts++] = argv[++i];
    } else {
      return usage();
    }
  }
  if (!path) {
    return usage();
  }

  viewer v;
  memset(&v, 0, sizeof v);
  v.path = path;
  char msg[512];
  if (example_doc_open(&v.doc, path, password, msg, sizeof msg) != 0) {
    fprintf(stderr, "%s\n", msg);
    return 1;
  }
  v.fonts = example_fonts(fonts, nfonts);
  bdf_render_options ropts = {0};
  ropts.fonts = v.fonts;
  bdf_error err;
  if (bdf_renderer_new(v.doc.doc, &ropts, &v.renderer, &err) != BDF_OK) {
    fprintf(stderr, "%s\n", err.message);
    return 1;
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  v.window = SDL_CreateWindow("BDF", 900, 700, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  v.sdl = v.window ? SDL_CreateRenderer(v.window, NULL) : NULL;
  if (v.window && !v.sdl) {
    /* no GPU (a server, or SDL_VIDEODRIVER=dummy): SDL's own renderer */
    v.sdl = SDL_CreateRenderer(v.window, SDL_SOFTWARE_RENDERER);
  }
  if (!v.sdl) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  if (view >= 0) {
    show(&v, (size_t)view, page > 0 ? (size_t)(page - 1) : 0);
  } else {
    v.view = bdf_doc_view_count(v.doc.doc) - 1;
    next_view(&v, 1); /* the first view that shows something */
    if (page > 1) {
      show(&v, v.view, (size_t)(page - 1));
    }
  }
  int rc = 0;
  if (shot) {
    rc = screenshot(&v, shot) == 0 ? 0 : 1;
  } else {
    run(&v);
  }

  for (size_t i = 0; i < bdf_renderer_warning_count(v.renderer); i++) {
    fprintf(stderr, "warning: %s\n", bdf_renderer_warning(v.renderer, i));
  }
  SDL_DestroyTexture(v.texture);
  SDL_DestroyRenderer(v.sdl);
  SDL_DestroyWindow(v.window);
  SDL_Quit();
  bdf_renderer_free(v.renderer);
  bdf_fonts_free(v.fonts);
  example_doc_close(&v.doc);
  return rc;
}
