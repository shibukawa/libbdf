/*
 * The renderer: it draws the pages of fixed and flow views, stretches of
 * scroll views, regions of sheets and objects that are in no document into
 * images, without a browser. A port of raster/imagebdf/imagebdf.go and
 * object.go of the reference implementation.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"

static const char bdfi_path_limit[] = "a path is too complex to rasterize: it is not drawn";

/* ---- Warnings ------------------------------------------------------------ */

void bdfi_r_warn(struct bdf_renderer *r, const char *message) {
  if (!r) {
    return;
  }
  for (size_t i = 0; i < r->nwarnings; i++) {
    if (strcmp(r->warnings[i], message) == 0) {
      return;
    }
  }
  if (r->nwarnings == r->cap_warnings) {
    size_t cap = r->cap_warnings ? r->cap_warnings * 2 : 8;
    char **w = bdfi_realloc_array(r->warnings, cap, sizeof *w);
    if (!w) {
      return; /* a warning is not worth failing for */
    }
    r->warnings = w;
    r->cap_warnings = cap;
  }
  size_t n = strlen(message);
  char *copy = malloc(n + 1);
  if (!copy) {
    return;
  }
  memcpy(copy, message, n + 1);
  r->warnings[r->nwarnings++] = copy;
}

void bdfi_r_warnf(struct bdf_renderer *r, const char *format, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof buf, format, ap);
  va_end(ap);
  bdfi_r_warn(r, buf);
}

void bdfi_scratch_stop(bdfi_scratch *sc) {
  sc->limited = 1;
  bdfi_r_warn(sc->r, bdfi_path_limit);
}

size_t bdf_renderer_warning_count(const bdf_renderer *r) { return r ? r->nwarnings : 0; }

const char *bdf_renderer_warning(const bdf_renderer *r, size_t i) {
  return r && i < r->nwarnings ? r->warnings[i] : "";
}

/* ---- What a renderer keeps ----------------------------------------------- */

static void bdfi_robj_clear(bdfi_robj *o) {
  for (size_t i = 0; i < o->npaths; i++) {
    if (o->paths[i]) {
      bdfi_rpath_clear(o->paths[i]);
      free(o->paths[i]);
    }
  }
  free(o->paths);
  free(o->path_state);
  for (size_t i = 0; i < o->nluts; i++) {
    free(o->luts[i]);
  }
  free(o->luts);
  bdf_paths_free(o->coll);
  bdf_free(o->coll_data);
  bdfi_face_free(o->font);
  bdfi_svg_image_free(o->svg);
  bdf_object_free(o->owned);
  memset(o, 0, sizeof *o);
}

void bdf_renderer_free(bdf_renderer *r) {
  if (!r) {
    return;
  }
  for (size_t i = 0; i < r->nparts; i++) {
    bdfi_robj_clear(&r->parts[i]);
  }
  free(r->parts);
  bdfi_robj_clear(&r->alone);
  for (size_t i = 0; i < r->nkept; i++) {
    bdfi_picture_free(r->kept[i]);
  }
  free(r->kept);
  bdfi_r_fonts_free(r);
  for (size_t i = 0; i < r->nwarnings; i++) {
    free(r->warnings[i]);
  }
  free(r->warnings);
  bdfi_scratch_free(&r->sc);
  free(r);
}

static bdf_renderer *bdfi_renderer_alloc(const bdf_doc *doc, const bdf_render_options *opts) {
  bdf_renderer *r = calloc(1, sizeof *r);
  if (!r) {
    return NULL;
  }
  r->doc = doc;
  if (opts) {
    r->has_background = opts->has_background;
    r->background = opts->background;
    r->fonts = opts->fonts;
  }
  r->sc.r = r;
  r->room = BDF_MAX_PICTURE_PIXELS;
  return r;
}

bdf_status bdf_renderer_new(const bdf_doc *doc, const bdf_render_options *opts, bdf_renderer **out,
                            bdf_error *err) {
  bdfi_error_clear(err);
  if (!doc || !out) {
    bdfi_error_set(err, BDF_ERR_ARG, 0, "bad argument");
    return BDF_ERR_ARG;
  }
  *out = bdfi_renderer_alloc(doc, opts);
  if (!*out) {
    bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
    return BDF_ERR_NOMEM;
  }
  return BDF_OK;
}

/* The slot of a part: the parts of a document may grow (segments). */
bdfi_robj *bdfi_r_part_slot(struct bdf_renderer *r, const uint8_t hash[16]) {
  if (!r->doc) {
    return NULL;
  }
  int64_t i = bdfi_doc_part_index(r->doc, hash);
  if (i < 0) {
    return NULL;
  }
  if ((size_t)i >= r->nparts) {
    size_t n = bdf_doc_part_count(r->doc);
    if ((size_t)i >= n) {
      return NULL;
    }
    bdfi_robj *parts = bdfi_realloc_array(r->parts, n, sizeof *parts);
    if (!parts) {
      r->oom = 1;
      return NULL;
    }
    memset(parts + r->nparts, 0, (n - r->nparts) * sizeof *parts);
    r->parts = parts;
    r->nparts = n;
  }
  return &r->parts[i];
}

static void bdfi_hex(const uint8_t hash[16], char out[33]) { bdf_hash_format(hash, out); }

bdfi_robj *bdfi_r_object(struct bdf_renderer *r, const uint8_t hash[16]) {
  char name[33];
  bdfi_robj *o = bdfi_r_part_slot(r, hash);
  if (!o) {
    if (!r->oom) {
      bdfi_hex(hash, name);
      bdfi_r_warnf(r, "object %s is missing", name);
    }
    return NULL;
  }
  if (o->state == 0) {
    bdf_error e;
    o->state = 2;
    bdf_status st = bdf_doc_object(r->doc, hash, &o->owned, &e);
    if (st == BDF_ERR_NOMEM) {
      r->oom = 1;
      o->state = 0;
    } else if (st != BDF_OK) {
      bdfi_hex(hash, name);
      bdfi_r_warnf(r, "object %s: %s", name, e.message);
    } else {
      o->obj = o->owned;
      o->state = 1;
    }
  }
  return o->state == 1 ? o : NULL;
}

/* ---- Pictures ------------------------------------------------------------ */

/*
 * makeRoom: room for a picture of n pixels among those kept. Pictures that
 * the image being drawn has not used are let go, the ones used longest ago
 * first (nothing drawn before refers to them still). It reports whether
 * there is room.
 */
int bdfi_r_make_room(struct bdf_renderer *r, int64_t n) {
  if (n > r->room) {
    return 0;
  }
  while (r->pixels + n > r->room) {
    size_t oldest = r->nkept;
    for (size_t i = 0; i < r->nkept; i++) {
      if (r->kept[i]->used < r->drawing &&
          (oldest == r->nkept || r->kept[i]->used < r->kept[oldest]->used)) {
        oldest = i;
      }
    }
    if (oldest == r->nkept) {
      return 0;
    }
    bdfi_picture *p = r->kept[oldest];
    if (p->owner) {
      *p->owner = NULL;
    }
    if (p->owner_flag) {
      *p->owner_flag = 0;
    }
    r->pixels -= (int64_t)p->w * p->h;
    memmove(r->kept + oldest, r->kept + oldest + 1, (r->nkept - oldest - 1) * sizeof *r->kept);
    r->nkept--;
    bdfi_picture_free(p);
  }
  return 1;
}

int bdfi_r_keep(struct bdf_renderer *r, bdfi_picture *p, bdfi_picture **owner, uint8_t *flag) {
  if (r->nkept == r->cap_kept) {
    size_t cap = r->cap_kept ? 2 * r->cap_kept : 8;
    bdfi_picture **kept = bdfi_realloc_array(r->kept, cap, sizeof *kept);
    if (!kept) {
      r->oom = 1;
      return 0;
    }
    r->kept = kept;
    r->cap_kept = cap;
  }
  p->used = r->drawing;
  p->owner = owner;
  p->owner_flag = flag;
  p->serial = ++r->pictures;
  r->kept[r->nkept++] = p;
  r->pixels += (int64_t)p->w * p->h;
  return 1;
}

bdfi_picture *bdfi_r_decode(struct bdf_renderer *r, const uint8_t *data, size_t len,
                            bdfi_picture **owner, uint8_t *flag, int *full) {
  *full = 0;
  /* an image whose size can be read makes room for itself before it is
   * decoded; one there is no room for while this image is drawn may find
   * some in the next */
  int32_t w, h;
  if (bdfi_image_config(data, len, &w, &h) && w > 0 && h > 0 &&
      (int64_t)w * h <= BDFI_MAX_DECODE_PIXELS && !bdfi_r_make_room(r, (int64_t)w * h)) {
    bdfi_r_warnf(r, BDFI_TOO_MANY_PIXELS, (long long)r->room);
    *full = 1;
    return NULL;
  }
  bdfi_picture *p = NULL;
  int format = BDFI_IMAGE_NONE;
  bdf_status st = bdfi_picture_decode(data, len, &p, &format);
  if (st == BDF_OK && !bdfi_r_keep(r, p, owner, flag)) {
    bdfi_picture_free(p);
    return NULL;
  }
  if (st == BDF_ERR_NOMEM) {
    r->oom = 1;
    return NULL;
  }
  if (st == BDF_ERR_TOO_LARGE) {
    bdfi_r_warnf(r, "an image of more than %lld pixels is not drawn",
                 (long long)BDFI_MAX_DECODE_PIXELS);
    return NULL;
  }
  if (st != BDF_OK) {
    if (format == BDFI_IMAGE_AVIF) {
      bdfi_r_warn(r, "AVIF images are not drawn");
    } else {
      bdfi_r_warnf(r, "an image (%s) cannot be decoded", bdfi_image_name(format));
    }
    return NULL;
  }
  return p;
}

bdfi_picture *bdfi_r_picture(struct bdf_renderer *r, const uint8_t hash[16]) {
  char name[33];
  bdfi_robj *o = bdfi_r_part_slot(r, hash);
  if (!o) {
    if (!r->oom) {
      bdfi_hex(hash, name);
      bdfi_r_warnf(r, "image %s is missing", name);
    }
    return NULL;
  }
  if (o->pic_state != 0) {
    if (o->pic) {
      o->pic->used = r->drawing;
    }
    return o->pic;
  }
  o->pic_state = 2;
  uint8_t *data = NULL;
  size_t len = 0;
  bdf_error e;
  bdf_status st = bdf_doc_part(r->doc, hash, &data, &len, &e);
  if (st != BDF_OK) {
    if (st == BDF_ERR_NOMEM) {
      r->oom = 1;
      o->pic_state = 0;
    } else {
      bdfi_hex(hash, name);
      bdfi_r_warnf(r, "image %s: %s", name, e.message);
    }
    return NULL;
  }
  if (bdfi_image_is_svg(data, len)) {
    /* an SVG image is no picture: its rasters are (bdfi_r_svg) */
    bdfi_svg_image *si = bdfi_svg_image_new(data, len, &st);
    bdf_free(data);
    if (!si) {
      if (st == BDF_ERR_NOMEM) {
        r->oom = 1;
        o->pic_state = 0;
      } else {
        bdfi_r_warn(r, "an SVG image cannot be read");
      }
      return NULL;
    }
    const char *const *warnings;
    size_t n = bdfi_svg_image_warnings(si, &warnings);
    for (size_t i = 0; i < n; i++) {
      bdfi_r_warnf(r, "%s", warnings[i]);
    }
    o->svg = si;
    return NULL;
  }
  int full;
  bdfi_picture *p = bdfi_r_decode(r, data, len, &o->pic, &o->pic_state, &full);
  bdf_free(data);
  if (!p) {
    if (full || r->oom) {
      o->pic_state = 0; /* there may be room for it in the next image drawn */
    }
    return NULL;
  }
  o->pic = p;
  o->pic_state = 1;
  return p;
}

bdfi_svg_image *bdfi_r_svg(struct bdf_renderer *r, const uint8_t hash[16]) {
  bdfi_r_picture(r, hash);
  bdfi_robj *o = bdfi_r_part_slot(r, hash);
  return o ? o->svg : NULL;
}

/* The paths of a path collection part, or NULL. */
static const bdf_paths *bdfi_r_collection(struct bdf_renderer *r, const uint8_t hash[16]) {
  char name[33];
  bdfi_robj *o = bdfi_r_part_slot(r, hash);
  if (!o) {
    if (!r->oom) {
      bdfi_hex(hash, name);
      bdfi_r_warnf(r, "path collection %s is missing", name);
    }
    return NULL;
  }
  if (o->coll_state == 0) {
    uint8_t *data = NULL;
    size_t len = 0;
    bdf_error e;
    o->coll_state = 2;
    bdf_status st = bdf_doc_part(r->doc, hash, &data, &len, &e);
    if (st == BDF_OK) {
      st = bdf_paths_decode(data, len, &o->coll, &e);
      if (st == BDF_OK) {
        /* the collection points into the part: it keeps the bytes */
        o->coll_state = 1;
      }
    }
    if (st == BDF_ERR_NOMEM) {
      r->oom = 1;
      o->coll_state = 0;
    } else if (st != BDF_OK) {
      bdfi_hex(hash, name);
      bdfi_r_warnf(r, "path collection %s: %s", name, e.message);
    }
    if (o->coll_state != 1) {
      bdf_free(data);
    } else {
      o->coll_data = data;
    }
  }
  return o->coll_state == 1 ? o->coll : NULL;
}

bdfi_rpath *bdfi_r_path(struct bdf_renderer *r, bdfi_robj *o, size_t i) {
  size_t n = bdf_object_path_count(o->obj);
  if (i >= n) {
    return NULL;
  }
  if (!o->paths) {
    o->paths = bdfi_calloc_array(n, sizeof *o->paths);
    o->path_state = bdfi_calloc_array(n, sizeof *o->path_state);
    if (!o->paths || !o->path_state) {
      free(o->paths);
      free(o->path_state);
      o->paths = NULL;
      o->path_state = NULL;
      r->oom = 1;
      return NULL;
    }
    o->npaths = n;
  }
  if (o->path_state[i]) {
    return o->paths[i];
  }
  bdf_path bp;
  if (bdf_object_path(o->obj, i, &bp) != BDF_OK) {
    o->path_state[i] = 1;
    return NULL;
  }
  if (bp.external) {
    const bdf_paths *coll = bdfi_r_collection(r, bp.hash);
    if (r->oom) {
      return NULL;
    }
    if (!coll || bp.index >= bdf_paths_count(coll) ||
        bdf_paths_get(coll, bp.index, &bp) != BDF_OK) {
      o->path_state[i] = 1;
      return NULL;
    }
  }
  bdfi_rpath *p = calloc(1, sizeof *p);
  if (p) {
    bdfi_rpath_build(p, &bp);
  }
  if (!p || p->oom) {
    if (p) {
      bdfi_rpath_clear(p);
      free(p);
    }
    r->oom = 1;
    return NULL;
  }
  o->paths[i] = p;
  o->path_state[i] = 1;
  return p;
}

/* The most gradients whose colours a renderer keeps (16 KiB each). */
#define BDFI_LUTS_KEPT 1024

static void bdfi_r_forget_luts(struct bdf_renderer *r, bdfi_robj *o) {
  for (size_t i = 0; i < o->nluts; i++) {
    free(o->luts[i]);
    o->luts[i] = NULL;
  }
  (void)r;
}

const bdfi_lut *bdfi_r_lut(struct bdf_renderer *r, bdfi_robj *o, size_t i, const bdf_paint *p) {
  if (p->nstops == 0) {
    return NULL;
  }
  size_t n = bdf_object_paint_count(o->obj);
  if (i >= n) {
    return NULL;
  }
  if (!o->luts) {
    o->luts = bdfi_calloc_array(n, sizeof *o->luts);
    if (!o->luts) {
      r->oom = 1;
      return NULL;
    }
    o->nluts = n;
  }
  if (!o->luts[i]) {
    if (r->luts >= BDFI_LUTS_KEPT) {
      for (size_t k = 0; k < r->nparts; k++) {
        bdfi_r_forget_luts(r, &r->parts[k]);
      }
      bdfi_r_forget_luts(r, &r->alone);
      r->luts = 0;
    }
    bdfi_lut *lut = malloc(sizeof *lut);
    if (!lut) {
      r->oom = 1;
      return NULL;
    }
    bdfi_lut_build(lut, p->stops, p->nstops);
    o->luts[i] = lut;
    r->luts++;
  }
  return o->luts[i];
}

bdfi_scratch *bdfi_r_scratch(struct bdf_renderer *r) {
  bdfi_scratch_reset(&r->sc);
  r->sc.r = r;
  return &r->sc;
}

bdfi_surface *bdfi_r_new_surface(struct bdf_renderer *r, int w, int h) {
  bdfi_surface *s = calloc(1, sizeof *s);
  if (s) {
    s->pix = bdfi_calloc_array(4 * (size_t)w * (size_t)h, sizeof *s->pix);
  }
  if (!s || !s->pix) {
    free(s);
    r->oom = 1;
    return NULL;
  }
  s->w = w;
  s->h = h;
  s->cap = 4 * (size_t)w * (size_t)h;
  return s;
}

void bdfi_r_release(struct bdf_renderer *r, bdfi_surface *s) {
  (void)r;
  if (s) {
    free(s->pix);
    free(s->row);
    free(s);
  }
}

/* ---- Images -------------------------------------------------------------- */

void bdf_image_free(bdf_image *image) {
  if (image) {
    bdf_free(image->pixels);
    memset(image, 0, sizeof *image);
  }
}

static bdfi_solid bdfi_r_background(const struct bdf_renderer *r) {
  if (!r->has_background) {
    return bdfi_solid_of(1, 1, 1, 1);
  }
  return bdfi_premul(r->background);
}

/* The image drawn on the canvas of a drawer, which is done with. */
static bdf_status bdfi_finish(bdfi_drawer *d, bdf_image *out, bdf_error *err) {
  struct bdf_renderer *r = d->r;
  const bdfi_surface *s = d->top->target;
  bdf_status st = BDF_OK;
  memset(out, 0, sizeof *out);
  if (r->oom) {
    st = BDF_ERR_NOMEM;
  } else {
    out->pixels = bdfi_malloc_array(4 * (size_t)s->w, (size_t)s->h);
    if (!out->pixels) {
      st = BDF_ERR_NOMEM;
    } else {
      out->width = s->w;
      out->height = s->h;
      bdfi_surface_to_rgba(s, out->pixels);
    }
  }
  bdfi_drawer_free(d);
  r->oom = 0;
  if (st != BDF_OK) {
    bdfi_error_set(err, st, 0, "out of memory");
  }
  return st;
}

/* A drawer on an image for a region, with the device transform that maps
 * the region onto it. */
static bdfi_drawer *bdfi_canvas(struct bdf_renderer *r, const float region[4], int w, int h,
                                bdfi_mat *m) {
  double sx = (double)w / (double)region[2], sy = (double)h / (double)region[3];
  m->v[0] = sx;
  m->v[1] = 0;
  m->v[2] = 0;
  m->v[3] = sy;
  m->v[4] = -(double)region[0] * sx;
  m->v[5] = -(double)region[1] * sy;
  r->oom = 0;
  return bdfi_drawer_new(r, m, w, h);
}

static bdf_status bdfi_nomem(bdf_error *err) {
  bdfi_error_set(err, BDF_ERR_NOMEM, 0, "out of memory");
  return BDF_ERR_NOMEM;
}

static bdf_status bdfi_bad(bdf_error *err, bdf_status st, const char *message) {
  bdfi_error_set(err, st, 0, message);
  return st;
}

static bdf_status bdfi_draw_page(struct bdf_renderer *r, size_t view, size_t page,
                                 const bdf_page *p, const float region[4], int w, int h,
                                 bdf_image *out, bdf_error *err) {
  bdfi_mat m;
  bdfi_drawer *d = bdfi_canvas(r, region, w, h, &m);
  if (!d) {
    return bdfi_nomem(err);
  }
  bdfi_mask *clip = bdfi_draw_clip_to(d, &m, 0, 0, (double)p->w, (double)p->h);
  bdfi_solid bg = bdfi_r_background(r);
  if (bdfi_surface_fill_background(d->top->target, clip, &bg) < 0) {
    r->oom = 1;
  }
  for (size_t l = 0; l < p->nlayers && !r->oom; l++) {
    bdf_layer layer;
    if (bdf_doc_layer(r->doc, view, page, l, &layer) != BDF_OK) {
      continue;
    }
    bdfi_draw_top(d, bdfi_r_object(r, layer.obj), layer.obj, &m, clip);
  }
  bdfi_mask_unref(clip);
  return bdfi_finish(d, out, err);
}

static int bdfi_role_is(const bdf_layer *l, const char *role) {
  size_t n = strlen(role);
  return l->role_len == n && memcmp(l->role, role, n) == 0;
}

/* The strips of a scroll view stacked without gaps, each cut to its body,
 * as the viewer scrolls them. */
static bdf_status bdfi_draw_continuous(struct bdf_renderer *r, size_t view, const bdf_view *v,
                                       const float region[4], int w, int h, bdf_image *out,
                                       bdf_error *err) {
  bdfi_mat m;
  bdfi_drawer *d = bdfi_canvas(r, region, w, h, &m);
  if (!d) {
    return bdfi_nomem(err);
  }
  bdfi_solid bg = bdfi_r_background(r);
  double y = 0.0;
  for (size_t pi = 0; pi < v->npages && !r->oom; pi++) {
    bdf_page p;
    if (bdf_doc_page(r->doc, view, pi, &p) != BDF_OK) {
      continue;
    }
    float body[4] = {0, 0, p.w, p.h};
    if (p.has_body) {
      memcpy(body, p.body, sizeof body);
    }
    double top = y;
    y += (double)body[3];
    if (top >= (double)(region[1] + region[3]) || y <= (double)region[1]) {
      continue;
    }
    bdfi_mat pm = bdfi_mat_translate(m, 0, top);
    bdfi_mask *clip = bdfi_draw_clip_to(d, &pm, 0, 0, (double)body[2], (double)body[3]);
    if (bdfi_surface_fill_background(d->top->target, clip, &bg) < 0) {
      r->oom = 1;
    }
    pm = bdfi_mat_translate(pm, -(double)body[0], -(double)body[1]);
    for (size_t l = 0; l < p.nlayers && !r->oom; l++) {
      bdf_layer layer;
      if (bdf_doc_layer(r->doc, view, pi, l, &layer) != BDF_OK ||
          (!bdfi_role_is(&layer, "body") && !bdfi_role_is(&layer, "annotation"))) {
        continue;
      }
      bdfi_draw_top(d, bdfi_r_object(r, layer.obj), layer.obj, &pm, clip);
    }
    bdfi_mask_unref(clip);
  }
  return bdfi_finish(d, out, err);
}

void bdf_sheet_size(const bdf_doc *doc, size_t view, double *w, double *h) {
  bdf_view v;
  double sw = 0, sh = 0;
  if (doc && bdf_doc_view(doc, view, &v) == BDF_OK) {
    float run[2];
    for (size_t i = 0; i < v.ncols; i++) {
      if (bdf_doc_view_run(doc, view, 0, i, run) == BDF_OK) {
        sw += (double)run[0] * (double)run[1];
      }
    }
    for (size_t i = 0; i < v.nrows; i++) {
      if (bdf_doc_view_run(doc, view, 1, i, run) == BDF_OK) {
        sh += (double)run[0] * (double)run[1];
      }
    }
  }
  if (w) {
    *w = sw;
  }
  if (h) {
    *h = sh;
  }
}

/* Limits of the gridlines of a sheet, across and down: a document decides
 * how many rows and columns a sheet has. */
#define BDFI_MAX_GRID_STEPS ((int64_t)1 << 24)
#define BDFI_MAX_GRID_LINES ((int64_t)1 << 16)

typedef struct bdfi_grid {
  bdfi_drawer *d;
  bdfi_solid color;
  bdfi_mask all;
  float *before;
  size_t cap_before;
  const bdfi_mat *m;
  int rows;                        /* the lines of the rows, else of the columns */
  double top, bottom, left, right; /* where the lines run from and to */
} bdfi_grid;

/* Draws a line; again says that the line before was the same, and the
 * result whether drawing it once more changed a pixel (it changes those the
 * line covers in part, until they have its colour). */
static int bdfi_grid_line(bdfi_grid *g, double ax, double ay, double bx, double by, int again) {
  bdfi_surface *t = g->d->top->target;
  struct bdf_renderer *r = g->d->r;
  bdfi_mask *cov = bdfi_rect_mask(ax, ay, bx, by, g->all.r);
  if (!cov) {
    r->oom = 1;
    return 0;
  }
  int changed = 0;
  if (!again) {
    if (bdfi_surface_fill(t, cov, &g->color.base, 1, BDF_BLEND_SOURCE_OVER, &g->all) < 0) {
      r->oom = 1;
    }
    changed = 1;
  } else if (!bdfi_mask_is_empty(cov) && cov->a) {
    /* a line that covers its pixels whole leaves them its colour: only one
     * that covers some in part changes them again */
    size_t n = 4 * (size_t)BDFI_DX(cov->r), rows = (size_t)BDFI_DY(cov->r);
    if (n * rows > g->cap_before) {
      float *before = bdfi_realloc_array(g->before, n * rows, sizeof *before);
      if (!before) {
        r->oom = 1;
        bdfi_mask_unref(cov);
        return 0;
      }
      g->before = before;
      g->cap_before = n * rows;
    }
    for (size_t i = 0; i < rows; i++) {
      memcpy(g->before + i * n,
             t->pix + 4 * ((size_t)(cov->r.y0 + (int)i) * (size_t)t->w + (size_t)cov->r.x0),
             n * sizeof *g->before);
    }
    if (bdfi_surface_fill(t, cov, &g->color.base, 1, BDF_BLEND_SOURCE_OVER, &g->all) < 0) {
      r->oom = 1;
    }
    for (size_t i = 0; i < rows && !changed; i++) {
      changed = memcmp(g->before + i * n,
                       t->pix + 4 * ((size_t)(cov->r.y0 + (int)i) * (size_t)t->w + (size_t)cov->r.x0),
                       n * sizeof *g->before) != 0;
    }
  }
  bdfi_mask_unref(cov);
  return changed;
}

static int bdfi_grid_draw(bdfi_grid *g, double px, int again) {
  if (g->rows) {
    return bdfi_grid_line(g, g->left, px, g->right, px + 1, again);
  }
  return bdfi_grid_line(g, px, g->top, px + 1, g->bottom, again);
}

/* Goes through the rows or the columns from the first, and draws the line
 * after each one that lies in [from, to]. */
static void bdfi_grid_walk(bdfi_grid *g, const bdf_doc *doc, size_t view, size_t nruns, double from,
                           double to) {
  struct bdf_renderer *r = g->d->r;
  double pos = 0.0, last = NAN;
  int64_t steps = 0, lines = 0;
  int settled = 0;
  for (size_t ri = 0; ri < nruns && !r->oom; ri++) {
    float run[2];
    if (bdf_doc_view_run(doc, view, g->rows, ri, run) != BDF_OK) {
      continue;
    }
    double n = (double)run[0], size = (double)run[1];
    if (!(n >= 1)) {
      continue;
    }
    for (int64_t i = 0; (double)i < floor(n); i++) {
      if (++steps > BDFI_MAX_GRID_STEPS) {
        bdfi_r_warnf(r,
                     "a sheet has more than %lld rows or columns: the gridlines of the rest are not drawn",
                     (long long)BDFI_MAX_GRID_STEPS);
        return;
      }
      pos += size;
      if (pos > to) {
        return;
      }
      if (!(pos >= from)) {
        if (size == 0) {
          break; /* so are the rest of the run */
        }
        continue;
      }
      bdfi_pt at = g->rows ? bdfi_mat_pt(g->m, 0, pos) : bdfi_mat_pt(g->m, pos, 0);
      double px = round(g->rows ? at.y : at.x);
      int again = px == last;
      if (again && settled) {
        if (size == 0) {
          break;
        }
        continue;
      }
      if (++lines > BDFI_MAX_GRID_LINES) {
        bdfi_r_warnf(r,
                     "a sheet has more than %lld gridlines in a region: the rest are not drawn",
                     (long long)BDFI_MAX_GRID_LINES);
        return;
      }
      int changed = bdfi_grid_draw(g, px, again);
      last = px;
      settled = again && !changed;
    }
  }
}

/* The lines between the cells of a sheet as the viewer draws them: one
 * device pixel wide, snapped to pixels, in #d9d9d9. */
static void bdfi_gridlines(bdfi_drawer *d, size_t view, const bdf_view *v, const float region[4],
                           const bdfi_mat *m) {
  const bdf_doc *doc = d->r->doc;
  double sw, sh;
  bdf_sheet_size(doc, view, &sw, &sh);
  double x1 = bdfi_fmin((double)(region[0] + region[2]), sw);
  double y1 = bdfi_fmin((double)(region[1] + region[3]), sh);
  bdfi_grid g;
  memset(&g, 0, sizeof g);
  g.d = d;
  g.color = bdfi_premul(0xd9d9d9ff);
  g.all.r = bdfi_surface_bounds(d->top->target);
  g.m = m;
  g.top = bdfi_mat_pt(m, 0, (double)region[1]).y;
  g.bottom = bdfi_mat_pt(m, 0, y1).y;
  g.rows = 0;
  bdfi_grid_walk(&g, doc, view, v->ncols, (double)region[0], x1);
  g.left = bdfi_mat_pt(m, (double)region[0], 0).x;
  g.right = bdfi_mat_pt(m, x1, 0).x;
  g.rows = 1;
  bdfi_grid_walk(&g, doc, view, v->nrows, (double)region[1], y1);
  free(g.before);
}

typedef struct bdfi_place {
  int64_t tx, ty;
  size_t tile;
} bdfi_place;

static int bdfi_place_cmp(const void *pa, const void *pb) {
  const bdfi_place *a = pa, *b = pb;
  if (a->ty != b->ty) {
    return a->ty < b->ty ? -1 : 1;
  }
  return a->tx < b->tx ? -1 : a->tx > b->tx ? 1 : 0;
}

/* A whole number as a key of a tile writes it: no sign but a minus, no zero
 * before another digit. The end of it, or NULL. */
static const char *bdfi_place_number(const char *s, const char *end, int64_t *out) {
  const char *p = s;
  int negative = p < end && *p == '-';
  if (negative) {
    p++;
  }
  const char *digits = p;
  int64_t v = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (v > (INT64_MAX - 9) / 10) {
      return NULL;
    }
    v = v * 10 + (*p - '0');
    p++;
  }
  if (p == digits || (p - digits > 1 && *digits == '0') || (negative && v == 0)) {
    return NULL;
  }
  *out = negative ? -v : v;
  return p;
}

/* A region of a sheet: the background, the gridlines when the view asks for
 * them, and the tiles. */
static bdf_status bdfi_draw_sheet(struct bdf_renderer *r, size_t view, const bdf_view *v,
                                  const float region[4], int w, int h, bdf_image *out,
                                  bdf_error *err) {
  bdfi_mat m;
  bdfi_drawer *d = bdfi_canvas(r, region, w, h, &m);
  if (!d) {
    return bdfi_nomem(err);
  }
  bdfi_mask all;
  all.r = bdfi_surface_bounds(d->top->target);
  all.a = NULL;
  all.refs = 0;
  bdfi_solid bg = bdfi_r_background(r);
  bdfi_surface_fill_background(d->top->target, &all, &bg);
  if (v->gridlines) {
    bdfi_gridlines(d, view, v, region, &m);
  }
  double tile = (double)v->tile_stated;
  if (tile <= 0) {
    tile = 2048;
  }
  if (!(tile >= 1) || bdfi_isinf(tile)) {
    /* a region would hold any number of them */
    bdfi_r_warnf(r, "the tiles of a sheet are %g units wide: they are not drawn", tile);
    return bdfi_finish(d, out, err);
  }
#define INDEX(x) ((int64_t)floor(bdfi_clampf((x) / tile)))
  int64_t tx0 = INDEX((double)region[0]), ty0 = INDEX((double)region[1]);
  int64_t tx1 = INDEX((double)(region[0] + region[2]) - 1e-6);
  int64_t ty1 = INDEX((double)(region[1] + region[3]) - 1e-6);
#undef INDEX
  bdfi_place *places = v->ntiles ? bdfi_malloc_array(v->ntiles, sizeof *places) : NULL;
  size_t nplaces = 0;
  if (v->ntiles && !places) {
    r->oom = 1;
  }
  for (size_t i = 0; places && i < v->ntiles; i++) {
    bdf_tile t;
    int64_t tx, ty;
    if (bdf_doc_tile(r->doc, view, i, &t) != BDF_OK) {
      continue;
    }
    const char *end = t.key + t.key_len;
    const char *p = bdfi_place_number(t.key, end, &tx);
    if (!p || p >= end || *p != ',') {
      continue;
    }
    p = bdfi_place_number(p + 1, end, &ty);
    if (!p || p != end || tx < tx0 || tx > tx1 || ty < ty0 || ty > ty1) {
      continue;
    }
    places[nplaces].tx = tx;
    places[nplaces].ty = ty;
    places[nplaces].tile = i;
    nplaces++;
  }
  if (nplaces > 1) {
    qsort(places, nplaces, sizeof *places, bdfi_place_cmp);
  }
  for (size_t i = 0; i < nplaces && !r->oom; i++) {
    bdf_tile t;
    uint8_t hash[16];
    if (bdf_doc_tile(r->doc, view, places[i].tile, &t) != BDF_OK) {
      continue;
    }
    if (bdf_hash_parse(t.obj, t.obj_len, hash) != BDF_OK) {
      bdfi_r_warnf(r, "bad tile hash \"%.*s\"", (int)(t.obj_len > 64 ? 64 : t.obj_len), t.obj);
      continue;
    }
    bdfi_mat tm = bdfi_mat_translate(m, (double)places[i].tx * tile, (double)places[i].ty * tile);
    bdfi_mask *clip = bdfi_draw_clip_to(d, &tm, 0, 0, tile, tile);
    bdfi_draw_top(d, bdfi_r_object(r, hash), hash, &tm, clip);
    bdfi_mask_unref(clip);
  }
  free(places);
  return bdfi_finish(d, out, err);
}

/* ---- The entry points ---------------------------------------------------- */

/* The page of a view that has pages, with a size. */
static bdf_status bdfi_drawable_page(bdf_renderer *r, size_t view, size_t page, bdf_page *p,
                                     bdf_error *err) {
  bdf_view v;
  if (bdf_doc_view(r->doc, view, &v) != BDF_OK) {
    return bdfi_bad(err, BDF_ERR_ARG, "no such view");
  }
  if (v.kind_code == BDF_VIEW_SHEET) {
    return bdfi_bad(err, BDF_ERR_ARG, "a sheet has no pages: draw a region of it");
  }
  if (bdf_doc_page(r->doc, view, page, p) != BDF_OK) {
    return bdfi_bad(err, BDF_ERR_ARG, "no such page");
  }
  if (!(p->w > 0) || !(p->h > 0) || bdfi_isinf((double)p->w) || bdfi_isinf((double)p->h)) {
    return bdfi_bad(err, BDF_ERR_ARG, "empty page");
  }
  return BDF_OK;
}

bdf_status bdf_render_page(bdf_renderer *r, size_t view, size_t page, double scale, bdf_image *out,
                           bdf_error *err) {
  bdfi_error_clear(err);
  if (!r || !r->doc || !out) {
    return bdfi_bad(err, BDF_ERR_ARG, "bad argument");
  }
  memset(out, 0, sizeof *out);
  bdf_page p;
  bdf_status st = bdfi_drawable_page(r, view, page, &p, err);
  if (st != BDF_OK) {
    return st;
  }
  if (!(scale > 0)) {
    return bdfi_bad(err, BDF_ERR_ARG, "empty page");
  }
  double fw = bdfi_fmax(ceil((double)p.w * scale - 1e-6), 1);
  double fh = bdfi_fmax(ceil((double)p.h * scale - 1e-6), 1);
  if (!(fw * fh <= BDF_MAX_RENDER_PIXELS)) {
    return bdfi_bad(err, BDF_ERR_TOO_LARGE, "the image would be too large");
  }
  const float region[4] = {0, 0, p.w, p.h};
  return bdfi_draw_page(r, view, page, &p, region, (int)fw, (int)fh, out, err);
}

bdf_status bdf_render_page_size(bdf_renderer *r, size_t view, size_t page, int w, int h,
                                bdf_image *out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!r || !r->doc || !out) {
    return bdfi_bad(err, BDF_ERR_ARG, "bad argument");
  }
  memset(out, 0, sizeof *out);
  bdf_page p;
  bdf_status st = bdfi_drawable_page(r, view, page, &p, err);
  if (st != BDF_OK) {
    return st;
  }
  if (w <= 0 || h <= 0) {
    return bdfi_bad(err, BDF_ERR_ARG, "empty image");
  }
  if (w > BDF_MAX_RENDER_PIXELS / h) {
    return bdfi_bad(err, BDF_ERR_TOO_LARGE, "the image would be too large");
  }
  const float region[4] = {0, 0, p.w, p.h};
  return bdfi_draw_page(r, view, page, &p, region, w, h, out, err);
}

bdf_status bdf_render_region(bdf_renderer *r, size_t view, size_t page, const float region[4],
                             int w, int h, bdf_image *out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!r || !r->doc || !out || !region) {
    return bdfi_bad(err, BDF_ERR_ARG, "bad argument");
  }
  memset(out, 0, sizeof *out);
  bdf_view v;
  if (bdf_doc_view(r->doc, view, &v) != BDF_OK) {
    return bdfi_bad(err, BDF_ERR_ARG, "no such view");
  }
  if (w <= 0 || h <= 0 || !(region[2] > 0) || !(region[3] > 0)) {
    return bdfi_bad(err, BDF_ERR_ARG, "empty region");
  }
  if (w > BDF_MAX_RENDER_PIXELS / h) {
    return bdfi_bad(err, BDF_ERR_TOO_LARGE, "the image would be too large");
  }
  if (v.kind_code == BDF_VIEW_SHEET) {
    return bdfi_draw_sheet(r, view, &v, region, w, h, out, err);
  }
  if (v.kind_code == BDF_VIEW_SCROLL) {
    return bdfi_draw_continuous(r, view, &v, region, w, h, out, err);
  }
  bdf_page p;
  if (bdf_doc_page(r->doc, view, page, &p) != BDF_OK) {
    return bdfi_bad(err, BDF_ERR_ARG, "no such page");
  }
  return bdfi_draw_page(r, view, page, &p, region, w, h, out, err);
}

/* Whether an object needs no other part: no font, image, path collection
 * or object of a document. */
static int bdfi_stands_alone(const bdf_object *o) {
  if (bdf_object_image_count(o) > 0 || bdf_object_object_count(o) > 0) {
    return 0;
  }
  for (size_t i = 0; i < bdf_object_font_count(o); i++) {
    bdf_font f;
    if (bdf_object_font(o, i, &f) == BDF_OK && f.kind == BDF_FONT_EMBEDDED) {
      return 0;
    }
  }
  for (size_t i = 0; i < bdf_object_path_count(o); i++) {
    bdf_path p;
    if (bdf_object_path(o, i, &p) == BDF_OK && p.external) {
      return 0;
    }
  }
  return 1;
}

bdf_status bdf_render_object(const bdf_object *obj, double scale, const bdf_render_options *opts,
                             bdf_image *out, bdf_error *err) {
  bdfi_error_clear(err);
  if (!obj || !out) {
    return bdfi_bad(err, BDF_ERR_ARG, "bad argument");
  }
  memset(out, 0, sizeof *out);
  if (!bdfi_stands_alone(obj)) {
    return bdfi_bad(err, BDF_ERR_ARG, "the object refers to other parts of a document");
  }
  float b[4];
  bdf_object_bbox(obj, b);
  if (!(b[2] > 0) || !(b[3] > 0) || !(scale > 0)) {
    return bdfi_bad(err, BDF_ERR_ARG, "empty object");
  }
  double x0 = floor((double)b[0] * scale + 1e-6), y0 = floor((double)b[1] * scale + 1e-6);
  double fw = bdfi_fmax(ceil((double)(b[0] + b[2]) * scale - 1e-6) - x0, 1);
  double fh = bdfi_fmax(ceil((double)(b[1] + b[3]) * scale - 1e-6) - y0, 1);
  if (!(fw * fh <= BDF_MAX_RENDER_PIXELS) || !(fabs(x0) < 2147483647.0) ||
      !(fabs(y0) < 2147483647.0)) {
    return bdfi_bad(err, BDF_ERR_TOO_LARGE, "the image would be too large");
  }
  bdf_renderer *r = bdfi_renderer_alloc(NULL, opts);
  if (!r) {
    return bdfi_nomem(err);
  }
  r->alone.obj = obj;
  r->alone.state = 1;
  bdfi_mat m = {{scale, 0, 0, scale, -x0, -y0}};
  bdfi_drawer *d = bdfi_drawer_new(r, &m, (int)fw, (int)fh);
  if (!d) {
    bdf_renderer_free(r);
    return bdfi_nomem(err);
  }
  bdfi_mask *clip = bdfi_mask_new(bdfi_surface_bounds(d->top->target), 0);
  if (!clip) {
    r->oom = 1;
  } else {
    if (r->has_background) {
      bdfi_solid bg = bdfi_r_background(r);
      if (bdfi_surface_fill(d->top->target, clip, &bg.base, 1, BDF_BLEND_SOURCE_OVER, clip) < 0) {
        r->oom = 1;
      }
    }
    const uint8_t none[16] = {0};
    bdfi_draw_top(d, &r->alone, none, &m, clip);
    bdfi_mask_unref(clip);
  }
  bdf_status st = bdfi_finish(d, out, err);
  out->x = (int)x0;
  out->y = (int)y0;
  bdf_renderer_free(r);
  return st;
}
