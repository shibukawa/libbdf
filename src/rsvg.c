/*
 * SVG image parts drawn: a port of raster/imagebdf/svg.go and svgpath.go of
 * the reference implementation.
 *
 * The viewer draws SVG images with the browser; here the reader of
 * svg_internal.h reads them and they are drawn by a renderer of the SVG
 * that previews meet: shapes and paths, fills and strokes, linear and
 * radial gradients, transforms, use and symbol, nested svg, clip paths,
 * group opacity, text in the fonts of the application, and embedded images
 * (data: URLs). Masks, filters, patterns and markers are not drawn.
 *
 * An SVG image is drawn into a raster at the scale it is shown at (rounded
 * up to a power of the square root of two, and bounded), which is then
 * drawn as any other picture is; the rasters are among the pictures the
 * renderer keeps.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raster_internal.h"
#include "svg_internal.h"

/* svgRasterPixels bounds the size of a raster of an SVG image: the scale it
 * is drawn at is no more than gives as many pixels, rounded up to a power
 * of the square root of two (so a raster has up to twice as many, 16 bytes
 * each while it is drawn). A build may define it lower. */
#ifndef BDF_MAX_SVG_RASTER_PIXELS
#define BDF_MAX_SVG_RASTER_PIXELS (4096 * 4096)
#endif
#define BDFI_SVG_RASTER_PIXELS ((int64_t)BDF_MAX_SVG_RASTER_PIXELS)

/* Limits of drawing an SVG image (svg.go): the elements drawn of one (a
 * build may define BDF_MAX_SVG_ELEMENTS lower), the translucent elements
 * drawn inside each other (each on a canvas of its own, as large as the
 * raster) and the pixels of their canvases in use, the pixels of the
 * canvases of all the translucent elements of a raster, and how deep SVG
 * images may be in SVG images. */
#ifndef BDF_MAX_SVG_ELEMENTS
#define BDF_MAX_SVG_ELEMENTS 200000
#endif
#define BDFI_SVG_BUDGET       BDF_MAX_SVG_ELEMENTS
#define BDFI_SVG_LAYERS       8
#define BDFI_SVG_LIVE_PIXELS  (4 * BDFI_SVG_RASTER_PIXELS)
#define BDFI_SVG_LAYER_PIXELS ((int64_t)1 << 30)
#define BDFI_SVG_INSIDE       4

#define BDFI_SVG_STEPS 129 /* the scales of the rasters: 2^(step/2), step from -64 to 64 */

/* svgEmbed: the image of an image element of an SVG document. */
typedef struct bdfi_svg_embed {
  bdfi_picture *pic; /* among the pictures the renderer keeps */
  struct bdfi_svg_image *image;
  uint8_t tried; /* the data: URL was read, with an image or without */
} bdfi_svg_embed;

/* svgImage and svgDoc: an SVG image part, the rasters drawn of it, and what
 * drawing found of its elements, kept for the next time they are drawn (a
 * use element draws them again): the paths of path data, and the pictures
 * or the SVG images of the data: URLs of image elements. */
struct bdfi_svg_image {
  bdfi_svg_doc *doc;
  double w, h; /* natural size in CSS px (as the viewer measures it) */
  bdfi_picture *rasters[BDFI_SVG_STEPS];
  bdfi_rpath **paths;      /* by the index of the node; NULL until one is asked for */
  bdfi_svg_embed **embeds; /* the same */
};

/* ---- Go's strings -------------------------------------------------------- */

#define BDFI_LIT(s) (s), (sizeof(s) - 1)

static bdfi_svg_str bdfi_rs_lit(const char *s, size_t n) { return bdfi_svg_str_of(s, n); }

static int bdfi_rs_has_prefix(bdfi_svg_str s, const char *p, size_t n) {
  return s.n >= n && memcmp(s.p, p, n) == 0;
}

static int bdfi_rs_has_suffix(bdfi_svg_str s, const char *p, size_t n) {
  return s.n >= n && memcmp(s.p + s.n - n, p, n) == 0;
}

/* strings.Contains for a short needle. */
static int bdfi_rs_contains(bdfi_svg_str s, const char *p, size_t n) {
  for (size_t i = 0; i + n <= s.n; i++) {
    if (memcmp(s.p + i, p, n) == 0) {
      return 1;
    }
  }
  return 0;
}

static bdfi_svg_str bdfi_rs_from(bdfi_svg_str s, size_t i) {
  return bdfi_svg_str_of(s.p + i, s.n - i);
}

/* strings.TrimSpace(s) == "%" */
static int bdfi_rs_is_percent(const char *s, size_t n) {
  bdfi_svg_str t = bdfi_svg_trim_space(bdfi_svg_str_of(s, n));
  return t.n == 1 && t.p[0] == '%';
}

/* n.Attr[name]: "" for an element without the attribute. */
static bdfi_svg_str bdfi_rs_attr(const bdfi_svg_node *n, const char *name, size_t len) {
  bdfi_svg_str v;
  bdfi_svg_attr(n, name, len, &v);
  return v;
}

/* v, _ := n.Declared(name) */
static bdfi_svg_str bdfi_rs_declared(const bdfi_svg_node *n, const char *name, size_t len) {
  bdfi_svg_str v;
  bdfi_svg_declared(n, name, len, &v);
  return v;
}

static int bdfi_rs_named(const bdfi_svg_node *n, const char *name, size_t len) {
  return bdfi_svg_str_eq(n->name, name, len);
}

/* n.Name == svg.TextNode */
static int bdfi_rs_is_text(const bdfi_svg_node *n) {
  return bdfi_rs_named(n, BDFI_LIT(BDFI_SVG_TEXT_NODE));
}

/* A number with the per cent sign that may follow it: f, rest, ok :=
 * svg.ParseNumber(v), and f /= 100 when strings.TrimSpace(rest) == "%". */
static int bdfi_rs_number(bdfi_svg_str v, double *f) {
  size_t rest;
  int ok = bdfi_svg_parse_number(v.p, v.n, f, &rest);
  if (bdfi_rs_is_percent(v.p + rest, v.n - rest)) {
    *f /= 100;
  }
  return ok;
}

/* math.Min(1, math.Max(0, f)) */
static double bdfi_rs_unit(double f) { return bdfi_fmin(1, bdfi_fmax(0, f)); }

/* ---- Styles -------------------------------------------------------------- */

/* svgStyle: the computed style of an element. Its strings are those of the
 * document, or constants. */
typedef struct bdfi_svg_style {
  bdfi_svg_str fill, stroke;
  double fill_opacity, stroke_opacity;
  uint8_t fill_rule, clip_rule;
  bdfi_svg_str stroke_width;
  uint8_t cap, join;
  double miter;
  bdfi_svg_str dash, dash_offset;
  bdfi_svg_color color;
  bdfi_svg_str font_family;
  double font_size;
  int64_t font_weight;
  int italic;
  bdfi_svg_str anchor;
  int visible;
  double letter_spacing;
} bdfi_svg_style;

static bdfi_svg_style bdfi_svg_default_style(void) {
  bdfi_svg_style st;
  memset(&st, 0, sizeof st);
  st.fill = bdfi_rs_lit(BDFI_LIT("black"));
  st.stroke = bdfi_rs_lit(BDFI_LIT("none"));
  st.fill_opacity = 1;
  st.stroke_opacity = 1;
  st.stroke_width = bdfi_rs_lit(BDFI_LIT("1"));
  st.miter = 4;
  st.dash = bdfi_rs_lit(BDFI_LIT(""));
  st.dash_offset = bdfi_rs_lit(BDFI_LIT(""));
  st.color.a = 1;
  st.font_family = bdfi_rs_lit(BDFI_LIT("serif"));
  st.font_size = 16;
  st.font_weight = 400;
  st.anchor = bdfi_rs_lit(BDFI_LIT("start"));
  st.visible = 1;
  return st;
}

/* get of compute: what an element declares for a property, when that is
 * neither nothing nor "inherit". */
static int bdfi_svg_get(const bdfi_svg_node *n, const char *name, size_t len, bdfi_svg_str *v) {
  if (!bdfi_svg_declared(n, name, len, v) || v->n == 0 || bdfi_svg_str_eq(*v, BDFI_LIT("inherit"))) {
    return 0;
  }
  return 1;
}

/* strconv.Atoi without an error: a sign and digits that an int64 holds. */
static int bdfi_rs_atoi(bdfi_svg_str s, int64_t *out) {
  size_t i = 0;
  int neg = 0;
  if (s.n > 0 && (s.p[0] == '+' || s.p[0] == '-')) {
    neg = s.p[0] == '-';
    i = 1;
  }
  if (i == s.n) {
    return 0;
  }
  uint64_t v = 0;
  const uint64_t most = (uint64_t)1 << 63; /* of a negative number; one less of a positive one */
  for (; i < s.n; i++) {
    unsigned d = (unsigned)((unsigned char)s.p[i] - '0');
    if (d > 9 || v > (most - d) / 10) {
      return 0;
    }
    v = v * 10 + d;
  }
  if (!neg && v == most) {
    return 0;
  }
  *out = neg ? (int64_t)(0 - v) : (int64_t)v;
  return 1;
}

/* a + b as Go adds ints: the sum wraps. */
static int64_t bdfi_rs_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }

/* compute: resolves the inherited properties of an element. */
static bdfi_svg_style bdfi_svg_compute(const bdfi_svg_node *n, const bdfi_svg_style *parent) {
  bdfi_svg_style st = *parent;
  bdfi_svg_str v;
  double f;
  if (bdfi_svg_get(n, BDFI_LIT("color"), &v)) {
    bdfi_svg_color c;
    if (bdfi_svg_parse_color(v.p, v.n, parent->color, &c)) {
      st.color = c;
    }
  }
  if (bdfi_svg_get(n, BDFI_LIT("fill"), &v)) {
    st.fill = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke"), &v)) {
    st.stroke = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("fill-opacity"), &v) && bdfi_rs_number(v, &f)) {
    st.fill_opacity = bdfi_rs_unit(f);
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-opacity"), &v) && bdfi_rs_number(v, &f)) {
    st.stroke_opacity = bdfi_rs_unit(f);
  }
  if (bdfi_svg_get(n, BDFI_LIT("fill-rule"), &v)) {
    st.fill_rule = bdfi_svg_str_eq(v, BDFI_LIT("evenodd")) ? BDF_FILL_EVENODD : BDF_FILL_NONZERO;
  }
  if (bdfi_svg_get(n, BDFI_LIT("clip-rule"), &v)) {
    st.clip_rule = bdfi_svg_str_eq(v, BDFI_LIT("evenodd")) ? BDF_FILL_EVENODD : BDF_FILL_NONZERO;
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-width"), &v)) {
    st.stroke_width = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-linecap"), &v)) {
    st.cap = bdfi_svg_str_eq(v, BDFI_LIT("round"))    ? BDF_CAP_ROUND
             : bdfi_svg_str_eq(v, BDFI_LIT("square")) ? BDF_CAP_SQUARE
                                                      : BDF_CAP_BUTT;
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-linejoin"), &v)) {
    st.join = bdfi_svg_str_eq(v, BDFI_LIT("round"))   ? BDF_JOIN_ROUND
              : bdfi_svg_str_eq(v, BDFI_LIT("bevel")) ? BDF_JOIN_BEVEL
                                                      : BDF_JOIN_MITER;
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-miterlimit"), &v)) {
    size_t rest;
    if (bdfi_svg_parse_number(v.p, v.n, &f, &rest) && f >= 1) {
      st.miter = f;
    }
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-dasharray"), &v)) {
    st.dash = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("stroke-dashoffset"), &v)) {
    st.dash_offset = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("font-family"), &v)) {
    st.font_family = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("font-size"), &v)) {
    static const struct {
      const char *name;
      size_t len;
      double size;
    } sizes[] = {{BDFI_LIT("xx-small"), 9}, {BDFI_LIT("x-small"), 10}, {BDFI_LIT("small"), 13},
                 {BDFI_LIT("medium"), 16},  {BDFI_LIT("large"), 18},   {BDFI_LIT("x-large"), 24},
                 {BDFI_LIT("xx-large"), 32}};
    if (bdfi_svg_parse_length(v.p, v.n, parent->font_size, parent->font_size, &f) && f > 0) {
      st.font_size = f;
    } else {
      for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        if (bdfi_svg_str_eq(v, sizes[i].name, sizes[i].len)) {
          st.font_size = sizes[i].size;
        }
      }
    }
  }
  if (bdfi_svg_get(n, BDFI_LIT("font-weight"), &v)) {
    int64_t w;
    if (bdfi_svg_str_eq(v, BDFI_LIT("normal"))) {
      st.font_weight = 400;
    } else if (bdfi_svg_str_eq(v, BDFI_LIT("bold"))) {
      st.font_weight = 700;
    } else if (bdfi_svg_str_eq(v, BDFI_LIT("bolder"))) {
      w = bdfi_rs_add(st.font_weight, 300);
      st.font_weight = w < 900 ? w : 900;
    } else if (bdfi_svg_str_eq(v, BDFI_LIT("lighter"))) {
      w = bdfi_rs_add(st.font_weight, -300);
      st.font_weight = w > 100 ? w : 100;
    } else if (bdfi_rs_atoi(v, &w)) {
      st.font_weight = w;
    }
  }
  if (bdfi_svg_get(n, BDFI_LIT("font-style"), &v)) {
    st.italic = bdfi_svg_str_eq(v, BDFI_LIT("italic")) || bdfi_rs_has_prefix(v, BDFI_LIT("oblique"));
  }
  if (bdfi_svg_get(n, BDFI_LIT("text-anchor"), &v)) {
    st.anchor = v;
  }
  if (bdfi_svg_get(n, BDFI_LIT("visibility"), &v)) {
    st.visible = bdfi_svg_str_eq(v, BDFI_LIT("visible"));
  }
  if (bdfi_svg_get(n, BDFI_LIT("letter-spacing"), &v)) {
    /* the length, or 0 for what is none */
    bdfi_svg_parse_length(v.p, v.n, 0, st.font_size, &st.letter_spacing);
  }
  return st;
}

/* ---- Sizes and view boxes ------------------------------------------------ */

/* abs of svgNaturalSize: a length that is no percentage and more than 0. */
static int bdfi_svg_abs(bdfi_svg_str s, double *v) {
  bdfi_svg_str t = bdfi_svg_trim_space(s);
  *v = 0;
  if (bdfi_rs_has_suffix(t, BDFI_LIT("%"))) {
    return 0;
  }
  return bdfi_svg_parse_length(s.p, s.n, 0, 16, v) && *v > 0;
}

/* svgNaturalSize: the size of an SVG image: the width and height of its
 * root, a missing one following from the other and the view box's
 * proportions, else the view box's size, else 300 × 150. */
static void bdfi_svg_natural_size(const bdfi_svg_node *root, double *ow, double *oh) {
  double w, h, vb[4], vw = 0, vh = 0;
  int okw = bdfi_svg_abs(bdfi_rs_attr(root, BDFI_LIT("width")), &w);
  int okh = bdfi_svg_abs(bdfi_rs_attr(root, BDFI_LIT("height")), &h);
  bdfi_svg_str box = bdfi_rs_attr(root, BDFI_LIT("viewBox"));
  if (bdfi_svg_parse_numbers(box.p, box.n, vb, 4) == 4 && vb[2] > 0 && vb[3] > 0) {
    vw = vb[2];
    vh = vb[3];
  }
  if (okw && okh) {
    *ow = w;
    *oh = h;
  } else if (okw && vw > 0) {
    *ow = w;
    *oh = w * vh / vw;
  } else if (okh && vw > 0) {
    *ow = h * vw / vh;
    *oh = h;
  } else if (vw > 0) {
    *ow = vw;
    *oh = vh;
  } else {
    *ow = okw ? w : 300;
    *oh = okh ? h : 150;
  }
}

/* viewBox: the four numbers of a view box that has a size. */
static int bdfi_svg_view_box(bdfi_svg_str s, double vb[4]) {
  return bdfi_svg_parse_numbers(s.p, s.n, vb, 4) == 4 && vb[2] > 0 && vb[3] > 0;
}

/* aspectTransform: maps a view box into the viewport (x, y, w, h) as
 * preserveAspectRatio says. */
static bdfi_mat bdfi_svg_aspect(const double vb[4], bdfi_svg_str par, double x, double y, double w,
                                double h) {
  double sx = w / vb[2], sy = h / vb[3];
  bdfi_svg_str f[2], field;
  size_t nf = 0, pos = 0;
  int first = 1;
  /* f := strings.Fields(par), without a first field that is "defer" */
  while (nf < 2 && bdfi_svg_next_field(par, &pos, &field)) {
    if (first && bdfi_svg_str_eq(field, BDFI_LIT("defer"))) {
      first = 0;
      continue;
    }
    first = 0;
    f[nf++] = field;
  }
  bdfi_svg_str align = nf > 0 ? f[0] : bdfi_rs_lit(BDFI_LIT("xMidYMid"));
  int slice = nf > 1 && bdfi_svg_str_eq(f[1], BDFI_LIT("slice"));
  int none = bdfi_svg_str_eq(align, BDFI_LIT("none"));
  if (!none) {
    double s = slice ? bdfi_fmax(sx, sy) : bdfi_fmin(sx, sy);
    sx = s;
    sy = s;
  }
  double tx = x - vb[0] * sx, ty = y - vb[1] * sy;
  if (!none) {
    if (bdfi_rs_contains(align, BDFI_LIT("xMid"))) {
      tx += (w - vb[2] * sx) / 2;
    } else if (bdfi_rs_contains(align, BDFI_LIT("xMax"))) {
      tx += w - vb[2] * sx;
    }
    if (bdfi_rs_contains(align, BDFI_LIT("YMid"))) {
      ty += (h - vb[3] * sy) / 2;
    } else if (bdfi_rs_contains(align, BDFI_LIT("YMax"))) {
      ty += h - vb[3] * sy;
    }
  }
  bdfi_mat m = {{sx, 0, 0, sy, tx, ty}};
  return m;
}

/* ---- The renderer of a document ------------------------------------------ */

/* svgRenderer: draws an SVG document. */
typedef struct bdfi_svgr {
  struct bdf_renderer *r;
  bdfi_svg_image *si;
  int depth;
  /* the number of elements left to draw: use elements can repeat content
   * exponentially, as they can in a browser */
  int64_t budget;
  /* the canvases of translucent elements in use, their pixels, and the
   * pixels of all that were */
  int layers;
  int64_t live, layer_pixels;
  int inside; /* the number of SVG images this one is in */
} bdfi_svgr;

/* svgCtx: where drawing goes: a surface, the user space, the clip (which
 * the context does not own) and the viewport that percentages refer to. */
typedef struct bdfi_svg_ctx {
  bdfi_surface *s;
  bdfi_mat m;
  bdfi_mask *clip;
  double vw, vh;
} bdfi_svg_ctx;

static void bdfi_svgr_node(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *ctx,
                           const bdfi_svg_style *parent);

/* A mask that could not be made: nothing, and the drawing is over. */
static bdfi_mask *bdfi_svgr_made(bdfi_svgr *sr, bdfi_mask *m) {
  if (!m) {
    sr->r->oom = 1;
    return bdfi_mask_empty();
  }
  return m;
}

/* length: resolves a length attribute: dir is 'x' or 'y' for percentages of
 * the viewport's width or height, anything else for its diagonal. */
static double bdfi_svg_length(const bdfi_svg_ctx *ctx, bdfi_svg_str s, char dir, double em) {
  double ref = bdfi_hypot(ctx->vw, ctx->vh) / BDFI_SQRT2, v;
  if (dir == 'x') {
    ref = ctx->vw;
  } else if (dir == 'y') {
    ref = ctx->vh;
  }
  bdfi_svg_parse_length(s.p, s.n, ref, em, &v);
  return v;
}

/* ref: the element a reference names: "#id" or "url(#id)". */
static const bdfi_svg_node *bdfi_svgr_ref(const bdfi_svgr *sr, bdfi_svg_str s) {
  s = bdfi_svg_trim_space(s);
  if (bdfi_rs_has_prefix(s, BDFI_LIT("url("))) {
    const char *end = memchr(s.p, ')', s.n);
    if (!end) {
      return NULL;
    }
    s = bdfi_svg_trim_space(bdfi_svg_str_of(s.p + 4, (size_t)(end - s.p) - 4));
    /* strings.Trim(s, `"'`) */
    while (s.n > 0 && (s.p[0] == '"' || s.p[0] == '\'')) {
      s = bdfi_rs_from(s, 1);
    }
    while (s.n > 0 && (s.p[s.n - 1] == '"' || s.p[s.n - 1] == '\'')) {
      s.n--;
    }
  }
  if (s.n == 0 || s.p[0] != '#') {
    return NULL;
  }
  return bdfi_svg_by_id(sr->si->doc, s.p + 1, s.n - 1);
}

/* coverage: a path in user space under m rasterized within bounds. */
static bdfi_mask *bdfi_svgr_coverage(bdfi_svgr *sr, bdfi_rpath *p, const bdfi_mat *m, uint8_t rule,
                                     bdfi_irect bounds) {
  if (bdfi_hidden(p, 0, m, bounds)) {
    return bdfi_mask_empty();
  }
  bdfi_scratch *sc = bdfi_r_scratch(sr->r);
  bdfi_rpath_flatten(p, sc, m, BDFI_FLAT_TOL);
  bdfi_mask *cov = bdfi_rasterize(sc, sc->pts, sc->polys, sc->npolys, rule, bounds);
  if (sc->oom) {
    sr->r->oom = 1;
  }
  return bdfi_svgr_made(sr, cov);
}

/* ---- Path data (svgpath.go) ---------------------------------------------- */

/* arcTo: adds an SVG elliptical arc from (x0, y0) to (x, y). */
static void bdfi_svg_arc_to(bdfi_rpath *p, double x0, double y0, double rx, double ry,
                            double rot_deg, int large, int sweep, double x, double y) {
  if (x0 == x && y0 == y) {
    return;
  }
  rx = fabs(rx);
  ry = fabs(ry);
  if (rx == 0 || ry == 0) {
    bdfi_rpath_line_to(p, x, y);
    return;
  }
  double phi = rot_deg * BDFI_PI / 180, sn, cs;
  bdfi_sincos(phi, &sn, &cs);
  double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
  double x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
  double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
  if (lambda > 1) {
    double k = sqrt(lambda);
    rx = rx * k;
    ry = ry * k;
  }
  double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
  double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
  double co = 0;
  if (den > 0 && num > 0) {
    co = sqrt(num / den);
  }
  if (large == sweep) {
    co = -co;
  }
  double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
  double cx = cs * cxp - sn * cyp + (x0 + x) / 2;
  double cy = sn * cxp + cs * cyp + (y0 + y) / 2;
  /* ang(ux, uy, vx, vy) = atan2(ux*vy - uy*vx, ux*vx + uy*vy) */
  double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
  double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
  double t1 = bdfi_atan2(1 * uy - 0 * ux, 1 * ux + 0 * uy);
  double dt = bdfi_atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  if (!sweep && dt > 0) {
    dt -= 2 * BDFI_PI;
  } else if (sweep && dt < 0) {
    dt += 2 * BDFI_PI;
  }
  bdfi_rpath_ellipse(p, cx, cy, rx, ry, phi, t1, t1 + dt, !sweep);
}

/* What is left of path data while it is read. */
typedef struct bdfi_svg_data {
  const char *s;
  size_t n;
} bdfi_svg_data;

/* strings.TrimLeft(s, " ,\t\n\r") */
static void bdfi_svg_data_skip(bdfi_svg_data *d) {
  while (d->n > 0 && (d->s[0] == ' ' || d->s[0] == ',' || d->s[0] == '\t' || d->s[0] == '\n' ||
                      d->s[0] == '\r')) {
    d->s++;
    d->n--;
  }
}

/* nums: the next count numbers, or 0 when one of them is none. */
static int bdfi_svg_data_nums(bdfi_svg_data *d, double *out, int count) {
  for (int i = 0; i < count; i++) {
    size_t rest;
    if (!bdfi_svg_parse_number(d->s, d->n, &out[i], &rest)) {
      return 0;
    }
    d->s += rest;
    d->n -= rest;
  }
  return 1;
}

/* flag: a 0 or a 1, which need nothing between them and what follows. */
static int bdfi_svg_data_flag(bdfi_svg_data *d, int *flag) {
  bdfi_svg_data_skip(d);
  if (d->n == 0 || (d->s[0] != '0' && d->s[0] != '1')) {
    return 0;
  }
  *flag = d->s[0] == '1';
  d->s++;
  d->n--;
  return 1;
}

/* parsePathData: reads SVG path data into a path, up to the first error. */
static void bdfi_svg_path_data(bdfi_rpath *p, bdfi_svg_str data) {
  bdfi_svg_data d = {data.p, data.n};
  char cmd = 0, last = 0;
  double cx = 0, cy = 0, sx = 0, sy = 0; /* current point, sub-path start */
  double lcx = 0, lcy = 0;               /* last control point */
  double a[6], e[2];
  for (;;) {
    bdfi_svg_data_skip(&d);
    if (d.n == 0 || p->oom) {
      return;
    }
    char c = d.s[0];
    if (c != '\0' && strchr("MmLlHhVvCcSsQqTtAaZz", c)) {
      cmd = c;
      d.s++;
      d.n--;
    } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
      /* Not a command, and none to repeat. After a Z the reference closes
       * the path again and again without reading on, until it has no
       * memory left: here the data ends, as it does at any other error. */
      return;
    }
    int rel = cmd >= 'a';
    double ox = rel ? cx : 0, oy = rel ? cy : 0;
    switch (cmd) {
    case 'M':
    case 'm':
      if (!bdfi_svg_data_nums(&d, a, 2)) {
        return;
      }
      cx = a[0] + ox;
      cy = a[1] + oy;
      sx = cx;
      sy = cy;
      bdfi_rpath_move_to(p, cx, cy);
      /* further pairs are line-tos */
      cmd = cmd == 'M' ? 'L' : 'l';
      last = 'M';
      continue;
    case 'L':
    case 'l':
      if (!bdfi_svg_data_nums(&d, a, 2)) {
        return;
      }
      cx = a[0] + ox;
      cy = a[1] + oy;
      bdfi_rpath_line_to(p, cx, cy);
      break;
    case 'H':
    case 'h':
      if (!bdfi_svg_data_nums(&d, a, 1)) {
        return;
      }
      cx = a[0] + ox;
      bdfi_rpath_line_to(p, cx, cy);
      break;
    case 'V':
    case 'v':
      if (!bdfi_svg_data_nums(&d, a, 1)) {
        return;
      }
      cy = a[0] + oy;
      bdfi_rpath_line_to(p, cx, cy);
      break;
    case 'C':
    case 'c':
      if (!bdfi_svg_data_nums(&d, a, 6)) {
        return;
      }
      bdfi_rpath_cubic_to(p, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy);
      lcx = a[2] + ox;
      lcy = a[3] + oy;
      cx = a[4] + ox;
      cy = a[5] + oy;
      break;
    case 'S':
    case 's': {
      if (!bdfi_svg_data_nums(&d, a, 4)) {
        return;
      }
      double x1 = cx, y1 = cy;
      if (last == 'C' || last == 'S') {
        x1 = 2 * cx - lcx;
        y1 = 2 * cy - lcy;
      }
      bdfi_rpath_cubic_to(p, x1, y1, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
      lcx = a[0] + ox;
      lcy = a[1] + oy;
      cx = a[2] + ox;
      cy = a[3] + oy;
      break;
    }
    case 'Q':
    case 'q':
      if (!bdfi_svg_data_nums(&d, a, 4)) {
        return;
      }
      bdfi_rpath_quad_to(p, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
      lcx = a[0] + ox;
      lcy = a[1] + oy;
      cx = a[2] + ox;
      cy = a[3] + oy;
      break;
    case 'T':
    case 't': {
      if (!bdfi_svg_data_nums(&d, a, 2)) {
        return;
      }
      double x1 = cx, y1 = cy;
      if (last == 'Q' || last == 'T') {
        x1 = 2 * cx - lcx;
        y1 = 2 * cy - lcy;
      }
      bdfi_rpath_quad_to(p, x1, y1, a[0] + ox, a[1] + oy);
      lcx = x1;
      lcy = y1;
      cx = a[0] + ox;
      cy = a[1] + oy;
      break;
    }
    case 'A':
    case 'a': {
      int large = 0, sweep = 0;
      if (!bdfi_svg_data_nums(&d, a, 3)) {
        return;
      }
      /* all three are read before any of them counts */
      int ok1 = bdfi_svg_data_flag(&d, &large);
      int ok2 = bdfi_svg_data_flag(&d, &sweep);
      int ok3 = bdfi_svg_data_nums(&d, e, 2);
      if (!ok1 || !ok2 || !ok3) {
        return;
      }
      double x = e[0] + ox, y = e[1] + oy;
      bdfi_svg_arc_to(p, cx, cy, a[0], a[1], a[2], large, sweep, x, y);
      cx = x;
      cy = y;
      break;
    }
    default: /* Z, z */
      bdfi_rpath_close(p);
      cx = sx;
      cy = sy;
      break;
    }
    last = (char)(cmd & ~0x20); /* upper case */
  }
}

/* ---- Shapes -------------------------------------------------------------- */

/* A slot by node for each node of a document: the paths, or the embeds. */
static void *bdfi_svg_slots(bdfi_svgr *sr, void *have, size_t size) {
  if (have) {
    return have;
  }
  size_t n = sr->si->doc->nnodes;
  void *slots = bdfi_calloc_array(n ? n : 1, size);
  if (!slots) {
    sr->r->oom = 1;
  }
  return slots;
}

static bdfi_rpath *bdfi_svg_new_path(bdfi_svgr *sr) {
  bdfi_rpath *p = calloc(1, sizeof *p);
  if (!p) {
    sr->r->oom = 1;
  }
  return p;
}

static void bdfi_svg_free_path(bdfi_rpath *p) {
  if (p) {
    bdfi_rpath_clear(p);
    free(p);
  }
}

/* shape: the path of a basic shape or path element, or NULL. *owned says
 * whether it is the caller's to free (that of a path element is kept with
 * the document for the next time it is drawn). */
static bdfi_rpath *bdfi_svgr_shape(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *c,
                                   const bdfi_svg_style *st, int *owned) {
#define A(name)  bdfi_rs_attr(n, BDFI_LIT(name))
#define LX(name) bdfi_svg_length(c, A(name), 'x', st->font_size)
#define LY(name) bdfi_svg_length(c, A(name), 'y', st->font_size)
  *owned = 0;
  if (bdfi_rs_named(n, BDFI_LIT("path"))) {
    bdfi_svg_image *si = sr->si;
    si->paths = bdfi_svg_slots(sr, si->paths, sizeof *si->paths);
    if (!si->paths || n->index >= si->doc->nnodes) {
      return NULL;
    }
    if (!si->paths[n->index]) {
      bdfi_rpath *d = bdfi_svg_new_path(sr);
      if (!d) {
        return NULL;
      }
      bdfi_svg_path_data(d, A("d"));
      if (d->oom) {
        sr->r->oom = 1;
        bdfi_svg_free_path(d);
        return NULL;
      }
      si->paths[n->index] = d;
    }
    return si->paths[n->index];
  }
  int rect = bdfi_rs_named(n, BDFI_LIT("rect")), circle = bdfi_rs_named(n, BDFI_LIT("circle"));
  int ellipse = bdfi_rs_named(n, BDFI_LIT("ellipse")), line = bdfi_rs_named(n, BDFI_LIT("line"));
  int polyline = bdfi_rs_named(n, BDFI_LIT("polyline"));
  int polygon = bdfi_rs_named(n, BDFI_LIT("polygon"));
  if (!rect && !circle && !ellipse && !line && !polyline && !polygon) {
    return NULL;
  }
  bdfi_rpath *p = bdfi_svg_new_path(sr);
  if (!p) {
    return NULL;
  }
  int none = 0;
  if (rect) {
    double x = LX("x"), y = LY("y"), w = LX("width"), h = LY("height");
    if (!(w > 0 && h > 0)) {
      none = 1;
    } else {
      double rx, ry;
      bdfi_svg_str srx = A("rx"), sry = A("ry");
      int okx = bdfi_svg_parse_length(srx.p, srx.n, c->vw, st->font_size, &rx);
      int oky = bdfi_svg_parse_length(sry.p, sry.n, c->vh, st->font_size, &ry);
      if (okx && !oky) {
        ry = rx;
      } else if (oky && !okx) {
        rx = ry;
      }
      rx = bdfi_fmin(bdfi_fmax(rx, 0), w / 2);
      ry = bdfi_fmin(bdfi_fmax(ry, 0), h / 2);
      if (rx == 0 || ry == 0) {
        bdfi_rpath_rect(p, x, y, w, h);
      } else {
        bdfi_rpath_move_to(p, x + rx, y);
        bdfi_rpath_line_to(p, x + w - rx, y);
        bdfi_rpath_ellipse(p, x + w - rx, y + ry, rx, ry, 0, -BDFI_PI_2, 0, 0);
        bdfi_rpath_line_to(p, x + w, y + h - ry);
        bdfi_rpath_ellipse(p, x + w - rx, y + h - ry, rx, ry, 0, 0, BDFI_PI_2, 0);
        bdfi_rpath_line_to(p, x + rx, y + h);
        bdfi_rpath_ellipse(p, x + rx, y + h - ry, rx, ry, 0, BDFI_PI_2, BDFI_PI, 0);
        bdfi_rpath_line_to(p, x, y + ry);
        bdfi_rpath_ellipse(p, x + rx, y + ry, rx, ry, 0, BDFI_PI, BDFI_3PI_2, 0);
        bdfi_rpath_close(p);
      }
    }
  } else if (circle) {
    double r = bdfi_svg_length(c, A("r"), 'd', st->font_size);
    if (!(r > 0)) {
      none = 1;
    } else {
      bdfi_rpath_move_to(p, LX("cx") + r, LY("cy"));
      bdfi_rpath_ellipse(p, LX("cx"), LY("cy"), r, r, 0, 0, BDFI_2PI, 0);
      bdfi_rpath_close(p);
    }
  } else if (ellipse) {
    double rx = LX("rx"), ry = LY("ry");
    bdfi_svg_str srx = A("rx"), sry = A("ry");
    if (srx.n == 0 || bdfi_svg_str_eq(srx, BDFI_LIT("auto"))) {
      rx = ry;
    }
    if (sry.n == 0 || bdfi_svg_str_eq(sry, BDFI_LIT("auto"))) {
      ry = rx;
    }
    if (!(rx > 0 && ry > 0)) {
      none = 1;
    } else {
      bdfi_rpath_move_to(p, LX("cx") + rx, LY("cy"));
      bdfi_rpath_ellipse(p, LX("cx"), LY("cy"), rx, ry, 0, 0, BDFI_2PI, 0);
      bdfi_rpath_close(p);
    }
  } else if (line) {
    bdfi_rpath_move_to(p, LX("x1"), LY("y1"));
    bdfi_rpath_line_to(p, LX("x2"), LY("y2"));
  } else {
    /* polyline, polygon: the numbers are read twice, the first time for how
     * many there are */
    bdfi_svg_str pts = A("points");
    size_t count = bdfi_svg_parse_numbers(pts.p, pts.n, NULL, 0);
    double *v = count >= 4 ? bdfi_malloc_array(count, sizeof *v) : NULL;
    if (count < 4) {
      none = 1;
    } else if (!v) {
      sr->r->oom = 1;
      none = 1;
    } else {
      bdfi_svg_parse_numbers(pts.p, pts.n, v, count);
      bdfi_rpath_move_to(p, v[0], v[1]);
      for (size_t i = 2; i + 1 < count; i += 2) {
        bdfi_rpath_line_to(p, v[i], v[i + 1]);
      }
      if (polygon) {
        bdfi_rpath_close(p);
      }
    }
    free(v);
  }
  if (p->oom) {
    sr->r->oom = 1;
    none = 1;
  }
  if (none) {
    bdfi_svg_free_path(p);
    return NULL;
  }
  *owned = 1;
  return p;
#undef A
#undef LX
#undef LY
}

/* ---- Paint servers ------------------------------------------------------- */

/* The shader of a fill or stroke value, and what it is made of. */
typedef struct bdfi_svg_paint {
  union {
    bdfi_shader base;
    bdfi_solid solid;
    bdfi_gradient_shader gradient;
  } sh;
  bdfi_lut *lut; /* the colours of a gradient, freed with the paint */
} bdfi_svg_paint;

static void bdfi_svg_paint_free(bdfi_svg_paint *p) { free(p->lut); }

/* packColor */
static uint32_t bdfi_svg_pack(bdfi_svg_color c) {
  const double v[4] = {c.r, c.g, c.b, c.a};
  uint32_t out = 0;
  for (int i = 0; i < 4; i++) {
    /* uint32(math.Round(...)) as the reference's processor converts it */
    out = out << 8 | (uint32_t)(uint64_t)bdfi_to_int(round(bdfi_rs_unit(v[i]) * 255));
  }
  return out;
}

/* The gradients a gradient element extends, itself first (href, eight deep). */
typedef struct bdfi_svg_chain {
  const bdfi_svg_node *n[8];
  size_t count;
} bdfi_svg_chain;

/* attr[k] of gradient: the attribute of the first gradient of the chain
 * that has it. */
static bdfi_svg_str bdfi_svg_chain_attr(const bdfi_svg_chain *ch, const char *name, size_t len) {
  bdfi_svg_str v;
  for (size_t i = 0; i < ch->count; i++) {
    if (bdfi_svg_attr(ch->n[i], name, len, &v)) {
      return v;
    }
  }
  return bdfi_rs_lit(BDFI_LIT(""));
}

typedef struct bdfi_svg_coord {
  const bdfi_svg_chain *chain;
  const bdfi_svg_ctx *c;
  double em;
  int bbox_units;
} bdfi_svg_coord;

/* coord of gradient: a coordinate of a gradient, in the units of the
 * bounding box or of the user space. */
static double bdfi_svg_coord_of(const bdfi_svg_coord *k, const char *name, size_t len,
                                const char *def, size_t def_len, char dir) {
  bdfi_svg_str v = bdfi_svg_chain_attr(k->chain, name, len);
  if (v.n == 0) {
    v = bdfi_rs_lit(def, def_len);
  }
  if (k->bbox_units) {
    double f;
    bdfi_rs_number(v, &f);
    return f;
  }
  return bdfi_svg_length(k->c, v, dir, k->em);
}

/* gradient: the shader of a gradient element for a shape with the bounding
 * box bbox (x, y, w, h): 0 for a gradient that paints nothing. */
static int bdfi_svgr_gradient(bdfi_svgr *sr, const bdfi_svg_node *g, const bdfi_svg_style *st,
                              const double bbox[4], const bdfi_svg_ctx *c, bdfi_svg_paint *out) {
  /* attributes and stops, following href to the gradients it extends */
  bdfi_svg_chain chain;
  chain.count = 0;
  const bdfi_svg_node *with_stops = NULL;
  size_t nstops = 0;
  for (const bdfi_svg_node *n = g; n && chain.count < 8;
       n = bdfi_svgr_ref(sr, bdfi_rs_attr(n, BDFI_LIT("href")))) {
    if (!bdfi_rs_named(n, BDFI_LIT("linearGradient")) &&
        !bdfi_rs_named(n, BDFI_LIT("radialGradient"))) {
      break;
    }
    chain.n[chain.count++] = n;
    if (!with_stops) {
      for (const bdfi_svg_node *s = n->first_child; s; s = s->next) {
        if (bdfi_rs_named(s, BDFI_LIT("stop"))) {
          nstops++;
        }
      }
      if (nstops > 0) {
        with_stops = n;
      }
    }
  }
  if (nstops == 0) {
    return 0;
  }
  bdfi_svg_coord k = {&chain, c, st->font_size, 0};
  k.bbox_units = !bdfi_svg_str_eq(bdfi_svg_chain_attr(&chain, BDFI_LIT("gradientUnits")),
                                  BDFI_LIT("userSpaceOnUse"));
  if (k.bbox_units && !(bbox[2] > 0 && bbox[3] > 0)) {
    return 0; /* a gradient in the units of an empty box does not paint */
  }
  bdfi_mat m = c->m;
  if (k.bbox_units) {
    bdfi_mat box = {{bbox[2], 0, 0, bbox[3], bbox[0], bbox[1]}};
    m = bdfi_mat_mul2(m, box);
  }
  bdfi_svg_str t = bdfi_svg_chain_attr(&chain, BDFI_LIT("gradientTransform"));
  if (t.n > 0) {
    m = bdfi_mat_mul2(m, bdfi_svg_parse_transform(t.p, t.n));
  }
#define COORD(name, def, dir) bdfi_svg_coord_of(&k, BDFI_LIT(name), BDFI_LIT(def), dir)
  float coords[6];
  bdf_paint paint;
  memset(&paint, 0, sizeof paint);
  if (bdfi_rs_named(g, BDFI_LIT("linearGradient"))) {
    paint.kind = BDF_PAINT_LINEAR;
    coords[0] = (float)COORD("x1", "0%", 'x');
    coords[1] = (float)COORD("y1", "0%", 'y');
    coords[2] = (float)COORD("x2", "100%", 'x');
    coords[3] = (float)COORD("y2", "0%", 'y');
    paint.ncoords = 4;
  } else {
    double cx = COORD("cx", "50%", 'x'), cy = COORD("cy", "50%", 'y'), r = COORD("r", "50%", 'd');
    double fx = cx, fy = cy;
    if (bdfi_svg_chain_attr(&chain, BDFI_LIT("fx")).n > 0) {
      fx = COORD("fx", "", 'x');
    }
    if (bdfi_svg_chain_attr(&chain, BDFI_LIT("fy")).n > 0) {
      fy = COORD("fy", "", 'y');
    }
    paint.kind = BDF_PAINT_RADIAL;
    coords[0] = (float)fx;
    coords[1] = (float)fy;
    coords[2] = (float)COORD("fr", "0%", 'd');
    coords[3] = (float)cx;
    coords[4] = (float)cy;
    coords[5] = (float)r;
    paint.ncoords = 6;
  }
#undef COORD
  paint.coords = coords;
  bdf_stop *stops = bdfi_malloc_array(nstops, sizeof *stops);
  out->lut = malloc(sizeof *out->lut);
  if (!stops || !out->lut) {
    free(stops);
    free(out->lut);
    out->lut = NULL;
    sr->r->oom = 1;
    return 0;
  }
  double last = 0;
  size_t i = 0;
  for (const bdfi_svg_node *s = with_stops->first_child; s; s = s->next) {
    if (!bdfi_rs_named(s, BDFI_LIT("stop"))) {
      continue;
    }
    double off = 0, f;
    if (bdfi_rs_number(bdfi_rs_attr(s, BDFI_LIT("offset")), &f)) {
      off = f;
    }
    off = bdfi_fmax(last, bdfi_fmin(1, bdfi_fmax(0, off)));
    last = off;
    bdfi_svg_style sst = bdfi_svg_compute(s, st);
    bdfi_svg_color col = {0, 0, 0, 1}, cc;
    bdfi_svg_str v;
    if (bdfi_svg_declared(s, BDFI_LIT("stop-color"), &v) &&
        bdfi_svg_parse_color(v.p, v.n, sst.color, &cc)) {
      col = cc;
    }
    if (bdfi_svg_declared(s, BDFI_LIT("stop-opacity"), &v) && bdfi_rs_number(v, &f)) {
      col.a *= bdfi_rs_unit(f);
    }
    stops[i].offset = (float)off;
    stops[i].color = bdfi_svg_pack(col);
    i++;
  }
  paint.stops = stops;
  paint.nstops = nstops;
  /* newGradientShader: transparent black for a gradient under a transform
   * that has no inverse */
  bdfi_lut_build(out->lut, stops, nstops);
  bdfi_gradient_shader gs;
  if (bdfi_gradient_shader_init(&gs, &paint, &m, out->lut)) {
    out->sh.gradient = gs;
  } else {
    out->sh.solid = bdfi_solid_of(0, 0, 0, 0);
  }
  free(stops);
  return 1;
}

/* paintServer: the shader of a fill or stroke value: 0 for none. */
static int bdfi_svgr_paint_server(bdfi_svgr *sr, bdfi_svg_str v, const bdfi_svg_style *st,
                                  const double bbox[4], const bdfi_svg_ctx *c,
                                  bdfi_svg_paint *out) {
  out->lut = NULL;
  v = bdfi_svg_trim_space(v);
  if (v.n == 0 || bdfi_svg_str_eq(v, BDFI_LIT("none"))) {
    return 0;
  }
  if (bdfi_rs_has_prefix(v, BDFI_LIT("url("))) {
    bdfi_svg_str fallback = bdfi_rs_lit(BDFI_LIT(""));
    const char *end = memchr(v.p, ')', v.n);
    if (end) {
      fallback = bdfi_svg_trim_space(bdfi_rs_from(v, (size_t)(end - v.p) + 1));
    }
    const bdfi_svg_node *g = bdfi_svgr_ref(sr, v);
    if (g && (bdfi_rs_named(g, BDFI_LIT("linearGradient")) ||
              bdfi_rs_named(g, BDFI_LIT("radialGradient")))) {
      return bdfi_svgr_gradient(sr, g, st, bbox, c, out);
    }
    if (g && bdfi_rs_named(g, BDFI_LIT("pattern"))) {
      bdfi_r_warn(sr->r, "SVG patterns are not drawn");
    }
    if (fallback.n == 0) {
      return 0;
    }
    v = fallback;
  }
  bdfi_svg_color col;
  if (!bdfi_svg_parse_color(v.p, v.n, st->color, &col)) {
    return 0;
  }
  out->sh.solid = bdfi_solid_of((float)(col.r * col.a), (float)(col.g * col.a),
                                (float)(col.b * col.a), (float)col.a);
  return 1;
}

/* ---- Painting ------------------------------------------------------------ */

/* The fields of a dash list: strings.FieldsFunc(s, comma or space). */
static int bdfi_svg_next_dash(bdfi_svg_str s, size_t *pos, bdfi_svg_str *field) {
  size_t i = *pos;
  while (i < s.n && (s.p[i] == ',' || s.p[i] == ' ')) {
    i++;
  }
  if (i == s.n) {
    return 0;
  }
  size_t from = i;
  while (i < s.n && s.p[i] != ',' && s.p[i] != ' ') {
    i++;
  }
  *field = bdfi_svg_str_of(s.p + from, i - from);
  *pos = i;
  return 1;
}

/* paint: fills and strokes a path with an element's style. */
static void bdfi_svgr_paint(bdfi_svgr *sr, bdfi_rpath *p, const bdfi_svg_ctx *c,
                            const bdfi_svg_style *st) {
  if (!st->visible) {
    return;
  }
  double box[4];
  bdfi_rpath_extent(p, box);
  const double bbox[4] = {box[0], box[1], box[2] - box[0], box[3] - box[1]};
  bdfi_irect bounds = bdfi_irect_intersect(c->clip->r, bdfi_surface_bounds(c->s));
  bdfi_svg_paint paint;
  if (bdfi_svgr_paint_server(sr, st->fill, st, bbox, c, &paint)) {
    bdfi_mask *cov = bdfi_svgr_coverage(sr, p, &c->m, st->fill_rule, bounds);
    if (bdfi_surface_fill(c->s, cov, &paint.sh.base, (float)st->fill_opacity,
                          BDF_BLEND_SOURCE_OVER, c->clip) < 0) {
      sr->r->oom = 1;
    }
    bdfi_mask_unref(cov);
    bdfi_svg_paint_free(&paint);
  }
  if (sr->r->oom) {
    return;
  }
  if (bdfi_svgr_paint_server(sr, st->stroke, st, bbox, c, &paint)) {
    bdfi_line_style ls;
    memset(&ls, 0, sizeof ls);
    ls.width = bdfi_svg_length(c, st->stroke_width, 'd', st->font_size);
    ls.cap = st->cap;
    ls.join = st->join;
    ls.miter = st->miter;
    if (!(ls.width > 0)) {
      bdfi_svg_paint_free(&paint);
      return;
    }
    if (st->dash.n > 0 && !bdfi_svg_str_eq(st->dash, BDFI_LIT("none"))) {
      size_t count = 0, pos = 0;
      bdfi_svg_str f;
      while (bdfi_svg_next_dash(st->dash, &pos, &f)) {
        count++;
      }
      float *segs = bdfi_malloc_array(count ? count : 1, sizeof *segs);
      if (!segs) {
        sr->r->oom = 1;
        bdfi_svg_paint_free(&paint);
        return;
      }
      count = 0;
      pos = 0;
      while (bdfi_svg_next_dash(st->dash, &pos, &f)) {
        segs[count++] = (float)bdfi_svg_length(c, f, 'd', st->font_size);
      }
      int rc = bdfi_line_set_dash(&ls, segs, count,
                                  (float)bdfi_svg_length(c, st->dash_offset, 'd', st->font_size));
      free(segs);
      if (rc < 0) {
        sr->r->oom = 1;
        bdfi_svg_paint_free(&paint);
        return;
      }
    }
    float alpha;
    int cut;
    bdfi_scratch *sc = bdfi_r_scratch(sr->r);
    bdfi_mask *cov = bdfi_stroke_mask(sc, p, &c->m, ls, bounds, &alpha, &cut);
    if (sc->oom) {
      sr->r->oom = 1;
    }
    cov = bdfi_svgr_made(sr, cov);
    if (cut) {
      bdfi_r_warn(sr->r, BDFI_STROKE_CUT);
    }
    if (bdfi_surface_fill(c->s, cov, &paint.sh.base, alpha * (float)st->stroke_opacity,
                          BDF_BLEND_SOURCE_OVER, c->clip) < 0) {
      sr->r->oom = 1;
    }
    bdfi_mask_unref(cov);
    bdfi_dash_unref(ls.dash);
    bdfi_svg_paint_free(&paint);
  }
}

/* clipMask: a clipPath rasterized for an element: the union of its shapes.
 * The mask is the caller's. */
static bdfi_mask *bdfi_svgr_clip_mask(bdfi_svgr *sr, const bdfi_svg_node *cp,
                                      const bdfi_svg_node *el, const bdfi_svg_ctx *c,
                                      const bdfi_svg_style *st) {
  bdfi_mat m = c->m;
  int owned;
  if (bdfi_svg_str_eq(bdfi_rs_attr(cp, BDFI_LIT("clipPathUnits")), BDFI_LIT("objectBoundingBox"))) {
    bdfi_rpath *p = bdfi_svgr_shape(sr, el, c, st, &owned);
    if (p) {
      double box[4];
      bdfi_rpath_extent(p, box);
      bdfi_mat unit = {{box[2] - box[0], 0, 0, box[3] - box[1], box[0], box[1]}};
      m = bdfi_mat_mul2(m, unit);
      if (owned) {
        bdfi_svg_free_path(p);
      }
    }
  }
  bdfi_svg_str t = bdfi_rs_attr(cp, BDFI_LIT("transform"));
  if (t.n > 0) {
    m = bdfi_mat_mul2(m, bdfi_svg_parse_transform(t.p, t.n));
  }
  bdfi_mask *out = NULL;
  bdfi_irect bounds = bdfi_surface_bounds(c->s);
  for (const bdfi_svg_node *ch = cp->first_child; ch && !sr->r->oom; ch = ch->next) {
    if (bdfi_rs_is_text(ch)) {
      continue;
    }
    if (bdfi_svg_str_eq(bdfi_rs_declared(ch, BDFI_LIT("display")), BDFI_LIT("none"))) {
      continue;
    }
    /* the shapes of a clip path count as elements drawn */
    if (sr->budget <= 0) {
      break;
    }
    sr->budget--;
    bdfi_svg_style cst = bdfi_svg_compute(ch, st);
    bdfi_mat cm = m;
    t = bdfi_rs_attr(ch, BDFI_LIT("transform"));
    if (t.n > 0) {
      cm = bdfi_mat_mul2(cm, bdfi_svg_parse_transform(t.p, t.n));
    }
    const bdfi_svg_node *shape = ch;
    if (bdfi_rs_named(ch, BDFI_LIT("use"))) {
      const bdfi_svg_node *target = bdfi_svgr_ref(sr, bdfi_rs_attr(ch, BDFI_LIT("href")));
      if (target && !bdfi_rs_named(target, BDFI_LIT("use"))) {
        shape = target;
        cm = bdfi_mat_translate(
            cm, bdfi_svg_length(c, bdfi_rs_attr(ch, BDFI_LIT("x")), 'x', cst.font_size),
            bdfi_svg_length(c, bdfi_rs_attr(ch, BDFI_LIT("y")), 'y', cst.font_size));
        cst = bdfi_svg_compute(target, &cst);
      }
    }
    bdfi_svg_ctx cc = *c;
    cc.m = cm;
    bdfi_rpath *p = bdfi_svgr_shape(sr, shape, &cc, &cst, &owned);
    if (!p) {
      continue;
    }
    bdfi_mask *cov = bdfi_svgr_coverage(sr, p, &cm, cst.clip_rule, bounds);
    if (owned) {
      bdfi_svg_free_path(p);
    }
    if (!out) {
      out = cov;
    } else {
      bdfi_mask *both = bdfi_svgr_made(sr, bdfi_mask_union(out, cov));
      bdfi_mask_unref(out);
      bdfi_mask_unref(cov);
      out = both;
    }
  }
  return out ? out : bdfi_mask_empty();
}

/* ---- Text ---------------------------------------------------------------- */

/* A run of text of a chunk, in one style. */
typedef struct bdfi_svg_piece {
  char *s; /* the text with its white space collapsed */
  size_t n;
  bdfi_svg_style st;
  bdfi_placed *glyphs;
  size_t nglyphs;
  double width;
} bdfi_svg_piece;

/* What a tspan with x or y starts: text-anchor places each chunk. */
typedef struct bdfi_svg_chunk {
  double x, y;
  size_t first, count; /* its pieces */
} bdfi_svg_chunk;

typedef struct bdfi_svg_text {
  bdfi_svgr *sr;
  bdfi_svg_chunk *chunks;
  size_t nchunks, cap_chunks;
  bdfi_svg_piece *pieces;
  size_t npieces, cap_pieces;
  double x, y;
} bdfi_svg_text;

/* collapseSpace: line ends and tabs are spaces, and runs of spaces are one. */
static char *bdfi_svg_collapse(bdfi_svg_str s, size_t *len) {
  char *out = malloc(s.n ? s.n : 1);
  if (!out) {
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
    if (c == ' ' && n > 0 && out[n - 1] == ' ') {
      continue;
    }
    out[n++] = c;
  }
  *len = n;
  return out;
}

/* The first number of a list, when it has one. */
static int bdfi_svg_first(const bdfi_svg_node *n, const char *name, size_t len, double *v) {
  bdfi_svg_str s = bdfi_rs_attr(n, name, len);
  return bdfi_svg_parse_numbers(s.p, s.n, v, 1) > 0;
}

/* walk of text: the text of an element and of the tspans in it. */
static void bdfi_svg_text_walk(bdfi_svg_text *t, const bdfi_svg_node *n, const bdfi_svg_style *st) {
  struct bdf_renderer *r = t->sr->r;
  double v = 0, xs = 0, ys = 0; /* (set when there is one, which gcc 12 cannot tell) */
  int has_x = bdfi_svg_first(n, BDFI_LIT("x"), &xs), has_y = bdfi_svg_first(n, BDFI_LIT("y"), &ys);
  if (has_x || has_y || t->nchunks == 0) {
    if (has_x) {
      t->x = xs;
    }
    if (has_y) {
      t->y = ys;
    }
    if (bdfi_svg_first(n, BDFI_LIT("dx"), &v)) {
      t->x += v;
    }
    if (bdfi_svg_first(n, BDFI_LIT("dy"), &v)) {
      t->y += v;
    }
    if (t->nchunks == t->cap_chunks) {
      size_t cap = t->cap_chunks ? 2 * t->cap_chunks : 8;
      bdfi_svg_chunk *chunks = bdfi_realloc_array(t->chunks, cap, sizeof *chunks);
      if (!chunks) {
        r->oom = 1;
        return;
      }
      t->chunks = chunks;
      t->cap_chunks = cap;
    }
    bdfi_svg_chunk *c = &t->chunks[t->nchunks++];
    c->x = t->x;
    c->y = t->y;
    c->first = t->npieces;
    c->count = 0;
  }
  for (const bdfi_svg_node *ch = n->first_child; ch && !r->oom; ch = ch->next) {
    if (bdfi_rs_is_text(ch)) {
      size_t len;
      char *s = bdfi_svg_collapse(ch->text, &len);
      if (!s) {
        r->oom = 1;
        return;
      }
      if (len == 0) {
        free(s);
        continue;
      }
      /* The pieces of a chunk are those from its first to the next chunk's:
       * a piece is of the last chunk, and that one has all the pieces made
       * since it began. */
      if (t->npieces == t->cap_pieces) {
        size_t cap = t->cap_pieces ? 2 * t->cap_pieces : 8;
        bdfi_svg_piece *pieces = bdfi_realloc_array(t->pieces, cap, sizeof *pieces);
        if (!pieces) {
          free(s);
          r->oom = 1;
          return;
        }
        t->pieces = pieces;
        t->cap_pieces = cap;
      }
      bdfi_svg_piece *p = &t->pieces[t->npieces++];
      memset(p, 0, sizeof *p);
      p->s = s;
      p->n = len;
      p->st = *st;
      t->chunks[t->nchunks - 1].count++;
    } else if (bdfi_rs_named(ch, BDFI_LIT("tspan")) || bdfi_rs_named(ch, BDFI_LIT("a")) ||
               bdfi_rs_named(ch, BDFI_LIT("textPath"))) {
      if (!bdfi_svg_str_eq(bdfi_rs_declared(ch, BDFI_LIT("display")), BDFI_LIT("none"))) {
        bdfi_svg_style cst = bdfi_svg_compute(ch, st);
        bdfi_svg_text_walk(t, ch, &cst);
      }
    }
  }
}

/* fakeItalicSkew of the rasterizer: the slant of a synthesized italic. */
#define BDFI_SVG_ITALIC_SKEW 0.25

/* glyphs: fills (and strokes) laid-out glyphs at (x, y) on the baseline. */
static void bdfi_svgr_glyphs(bdfi_svgr *sr, const bdfi_placed *gs, size_t count, double x,
                             double y, const bdfi_svg_style *style, const bdfi_svg_ctx *c) {
  if (!style->visible || count == 0) {
    return;
  }
  bdfi_rpath *p = bdfi_svg_new_path(sr);
  if (!p) {
    return;
  }
  for (size_t gi = 0; gi < count && !p->oom && !sr->r->oom; gi++) {
    const bdfi_placed *g = &gs[gi];
    bdfi_rpath *o = bdfi_face_outline(g->use.face, g->gid, &sr->r->oom);
    if (!o) {
      continue;
    }
    double k = style->font_size / bdfi_face_upem(g->use.face);
    bdfi_mat at = {{1, 0, 0, 1, x + g->x, y}};
    bdfi_mat gm = bdfi_mat_scale(at, k, -k);
    if (g->use.italic) {
      bdfi_mat skew = {{1, 0, BDFI_SVG_ITALIC_SKEW, 1, 0, 0}};
      gm = bdfi_mat_mul2(gm, skew);
    }
    for (size_t si = 0; si < o->nsubs && !p->oom; si++) {
      const bdfi_sub *s = &o->subs[si];
      bdfi_pt q = bdfi_mat_pt(&gm, s->start.x, s->start.y);
      /* a sub-path of its own, whatever the one before it was */
      bdfi_rpath_move_to(p, q.x, q.y);
      for (size_t j = 0; j < s->nsegs; j++) {
        const bdfi_seg *sg = &o->segs[s->seg0 + j];
        bdfi_pt a = bdfi_mat_pt(&gm, sg->p[0], sg->p[1]);
        bdfi_pt b = bdfi_mat_pt(&gm, sg->p[2], sg->p[3]);
        bdfi_pt e = bdfi_mat_pt(&gm, sg->p[4], sg->p[5]);
        if (sg->kind == BDFI_SEG_LINE) {
          bdfi_rpath_line_to(p, a.x, a.y);
        } else if (sg->kind == BDFI_SEG_QUAD) {
          bdfi_rpath_quad_to(p, a.x, a.y, b.x, b.y);
        } else {
          bdfi_rpath_cubic_to(p, a.x, a.y, b.x, b.y, e.x, e.y);
        }
      }
      if (s->closed && p->nsubs > 0) {
        p->subs[p->nsubs - 1].closed = 1;
      }
    }
  }
  if (p->oom) {
    sr->r->oom = 1;
  }
  if (!sr->r->oom) {
    bdfi_svg_style st = *style;
    st.fill_rule = BDF_FILL_NONZERO;
    bdfi_svgr_paint(sr, p, c, &st);
  }
  bdfi_svg_free_path(p);
}

/* text: a text element with its tspans. A tspan with x or y starts a new
 * chunk (text-anchor places each chunk); other runs of text continue from
 * the end of the one before. */
static void bdfi_svgr_text(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *c,
                           const bdfi_svg_style *st) {
  struct bdf_renderer *r = sr->r;
  bdfi_svg_text t;
  memset(&t, 0, sizeof t);
  t.sr = sr;
  bdfi_svg_text_walk(&t, n, st);
  for (size_t ci = 0; ci < t.nchunks && !r->oom; ci++) {
    bdfi_svg_chunk *ch = &t.chunks[ci];
    if (ch->count == 0) {
      continue;
    }
    bdfi_svg_piece *pieces = t.pieces + ch->first;
    if (ci == 0) {
      /* strings.TrimLeft(s, " ") */
      bdfi_svg_piece *p = &pieces[0];
      size_t skip = 0;
      while (skip < p->n && p->s[skip] == ' ') {
        skip++;
      }
      memmove(p->s, p->s + skip, p->n - skip);
      p->n -= skip;
    }
    if (ci == t.nchunks - 1) {
      bdfi_svg_piece *p = &pieces[ch->count - 1];
      while (p->n > 0 && p->s[p->n - 1] == ' ') {
        p->n--;
      }
    }
    double total = 0;
    for (size_t i = 0; i < ch->count && !r->oom; i++) {
      bdfi_svg_piece *p = &pieces[i];
      bdf_font f;
      memset(&f, 0, sizeof f);
      f.kind = BDF_FONT_SYSTEM;
      f.family = p->st.font_family.p;
      f.family_len = p->st.font_family.n;
      f.weight = (uint16_t)(uint64_t)p->st.font_weight;
      f.style = p->st.italic ? BDF_STYLE_ITALIC : BDF_STYLE_NORMAL;
      bdfi_chain *chain = bdfi_r_chain(r, &f);
      if (!chain) {
        break;
      }
      if (bdfi_chain_layout(r, chain, p->s, p->n, p->st.font_size, p->st.letter_spacing, 0,
                            &p->glyphs, &p->nglyphs, &p->width) < 0) {
        break;
      }
      total += p->width;
    }
    if (r->oom) {
      break;
    }
    double pen = ch->x;
    if (bdfi_svg_str_eq(pieces[0].st.anchor, BDFI_LIT("middle"))) {
      pen -= total / 2;
    } else if (bdfi_svg_str_eq(pieces[0].st.anchor, BDFI_LIT("end"))) {
      pen -= total;
    }
    for (size_t i = 0; i < ch->count && !r->oom; i++) {
      bdfi_svgr_glyphs(sr, pieces[i].glyphs, pieces[i].nglyphs, pen, ch->y, &pieces[i].st, c);
      pen += pieces[i].width;
    }
  }
  for (size_t i = 0; i < t.npieces; i++) {
    free(t.pieces[i].s);
    free(t.pieces[i].glyphs);
  }
  free(t.pieces);
  free(t.chunks);
}

/* ---- Embedded images ----------------------------------------------------- */

/* base64.StdEncoding (padded) or RawStdEncoding decoding src, as Go's
 * decoder reads: line ends are passed over, bits left over are not looked
 * at. It returns 0 for input the decoder refuses; out has room for
 * 3 n / 4 + 3 bytes. */
static int bdfi_svg_base64(const char *src, size_t n, int padded, uint8_t *out, size_t *len) {
  size_t si = 0, o = 0;
  while (si < n) {
    uint8_t d[4] = {0, 0, 0, 0};
    int dlen = 4, garbage = 0;
    for (int j = 0; j < 4; j++) {
      if (si == n) {
        if (j == 0) {
          *len = o;
          return 1;
        }
        if (j == 1 || padded) {
          return 0;
        }
        dlen = j;
        break;
      }
      unsigned char in = (unsigned char)src[si++];
      int v = in >= 'A' && in <= 'Z'   ? in - 'A'
              : in >= 'a' && in <= 'z' ? in - 'a' + 26
              : in >= '0' && in <= '9' ? in - '0' + 52
              : in == '+'              ? 62
              : in == '/'              ? 63
                                       : -1;
      if (v >= 0) {
        d[j] = (uint8_t)v;
        continue;
      }
      if (in == '\n' || in == '\r') {
        j--;
        continue;
      }
      if (!padded || in != '=') {
        return 0;
      }
      /* the end, with padding */
      if (j < 2) {
        return 0;
      }
      if (j == 2) {
        /* "==" is expected, and the first "=" is read */
        while (si < n && (src[si] == '\n' || src[si] == '\r')) {
          si++;
        }
        if (si == n || src[si] != '=') {
          return 0;
        }
        si++;
      }
      while (si < n && (src[si] == '\n' || src[si] == '\r')) {
        si++;
      }
      garbage = si < n;
      dlen = j;
      break;
    }
    uint32_t val = (uint32_t)d[0] << 18 | (uint32_t)d[1] << 12 | (uint32_t)d[2] << 6 | d[3];
    if (dlen >= 2) {
      out[o] = (uint8_t)(val >> 16);
    }
    if (dlen >= 3) {
      out[o + 1] = (uint8_t)(val >> 8);
    }
    if (dlen == 4) {
      out[o + 2] = (uint8_t)val;
    }
    o += (size_t)dlen - 1;
    if (garbage) {
      return 0;
    }
  }
  *len = o;
  return 1;
}

static int bdfi_svg_hex(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* dataURL: decodes a data: URL: 1 and its bytes (the caller's to free), 0
 * for what is none or cannot be decoded, -1 when out of memory. */
static int bdfi_svg_data_url(bdfi_svg_str s, uint8_t **out, size_t *len) {
  *out = NULL;
  *len = 0;
  s = bdfi_svg_trim_space(s);
  if (!bdfi_rs_has_prefix(s, BDFI_LIT("data:"))) {
    return 0;
  }
  s = bdfi_rs_from(s, 5);
  const char *comma = memchr(s.p, ',', s.n);
  if (!comma) {
    return 0;
  }
  bdfi_svg_str meta = bdfi_svg_str_of(s.p, (size_t)(comma - s.p));
  bdfi_svg_str payload = bdfi_rs_from(s, meta.n + 1);
  uint8_t *b = malloc(payload.n + 4);
  if (!b) {
    return -1;
  }
  if (bdfi_rs_has_suffix(meta, BDFI_LIT(";base64"))) {
    /* the payload without its white space (cleared: an empty payload reads
     * none of it, and gcc cannot tell) */
    char *text = calloc(payload.n ? payload.n : 1, 1);
    if (!text) {
      free(b);
      return -1;
    }
    size_t n = 0;
    for (size_t i = 0; i < payload.n; i++) {
      char c = payload.p[i];
      if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
        text[n++] = c;
      }
    }
    int ok = bdfi_svg_base64(text, n, 1, b, len);
    if (!ok) {
      /* without its padding: strings.TrimRight(payload, "=") */
      while (n > 0 && text[n - 1] == '=') {
        n--;
      }
      ok = bdfi_svg_base64(text, n, 0, b, len);
    }
    free(text);
    if (!ok) {
      free(b);
      return 0;
    }
    *out = b;
    return 1;
  }
  /* url.PathUnescape */
  size_t n = 0;
  for (size_t i = 0; i < payload.n; i++) {
    if (payload.p[i] != '%') {
      b[n++] = (uint8_t)payload.p[i];
      continue;
    }
    if (i + 2 >= payload.n || bdfi_svg_hex(payload.p[i + 1]) < 0 ||
        bdfi_svg_hex(payload.p[i + 2]) < 0) {
      free(b);
      return 0;
    }
    b[n++] = (uint8_t)(bdfi_svg_hex(payload.p[i + 1]) << 4 | bdfi_svg_hex(payload.p[i + 2]));
    i += 2;
  }
  *out = b;
  *len = n;
  return 1;
}

/* sizeOr: the width and height of an image element: its attributes, or the
 * image's own size for those missing (keeping its proportions). */
static void bdfi_svg_size_or(const bdfi_svg_ctx *c, const bdfi_svg_node *n,
                             const bdfi_svg_style *st, double iw, double ih, double *ow,
                             double *oh) {
  double w = bdfi_svg_length(c, bdfi_rs_attr(n, BDFI_LIT("width")), 'x', st->font_size);
  double h = bdfi_svg_length(c, bdfi_rs_attr(n, BDFI_LIT("height")), 'y', st->font_size);
  if (w > 0 && h > 0) {
    *ow = w;
    *oh = h;
  } else if (w > 0) {
    *ow = w;
    *oh = w * ih / iw;
  } else if (h > 0) {
    *ow = h * iw / ih;
    *oh = h;
  } else {
    *ow = iw;
    *oh = ih;
  }
}

static bdfi_picture *bdfi_svg_raster_in(struct bdf_renderer *r, bdfi_svg_image *si, double k,
                                        int inside, double *scale);

/* The coverage of a rectangle in user space under m, scanned. */
static bdfi_mask *bdfi_svgr_rect(bdfi_svgr *sr, double x, double y, double w, double h,
                                 const bdfi_mat *m, bdfi_irect bounds) {
  bdfi_rpath p;
  memset(&p, 0, sizeof p);
  bdfi_rpath_rect(&p, x, y, w, h);
  bdfi_mask *cov = NULL;
  if (!p.oom) {
    bdfi_scratch *sc = bdfi_r_scratch(sr->r);
    bdfi_rpath_flatten(&p, sc, m, BDFI_FLAT_TOL);
    cov = bdfi_rasterize(sc, sc->pts, sc->polys, sc->npolys, BDF_FILL_NONZERO, bounds);
  }
  bdfi_rpath_clear(&p);
  return bdfi_svgr_made(sr, cov);
}

/* image: an image element: an embedded raster image or SVG document (data:
 * URL) fitted into its box. */
static void bdfi_svgr_image(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *c,
                            const bdfi_svg_style *st) {
  struct bdf_renderer *r = sr->r;
  bdfi_svg_image *doc = sr->si;
  double x = bdfi_svg_length(c, bdfi_rs_attr(n, BDFI_LIT("x")), 'x', st->font_size);
  double y = bdfi_svg_length(c, bdfi_rs_attr(n, BDFI_LIT("y")), 'y', st->font_size);
  doc->embeds = bdfi_svg_slots(sr, doc->embeds, sizeof *doc->embeds);
  if (!doc->embeds || n->index >= doc->doc->nnodes) {
    return;
  }
  bdfi_svg_embed *e = doc->embeds[n->index];
  if (!e) {
    e = calloc(1, sizeof *e);
    if (!e) {
      r->oom = 1;
      return;
    }
    doc->embeds[n->index] = e;
  }
  if (!e->tried) {
    /* the image of the element is kept for the next time it is drawn */
    e->tried = 1;
    bdfi_svg_str href = bdfi_rs_attr(n, BDFI_LIT("href"));
    uint8_t *data;
    size_t len;
    int ok = bdfi_svg_data_url(href, &data, &len);
    if (ok < 0) {
      r->oom = 1;
      e->tried = 0;
      return;
    }
    if (!ok) {
      if (href.n > 0) {
        bdfi_r_warn(r, "SVG images linking to other files are not drawn");
      }
      return;
    }
    if (bdfi_image_is_svg(data, len)) {
      bdf_status status;
      bdfi_svg_image *si = bdfi_svg_image_new(data, len, &status);
      free(data);
      if (!si) {
        if (status == BDF_ERR_NOMEM) {
          r->oom = 1;
          e->tried = 0;
        }
        return;
      }
      for (size_t i = 0; i < si->doc->nwarnings; i++) {
        bdfi_r_warnf(r, "%s", si->doc->warnings[i]);
      }
      e->image = si;
    } else {
      int full = 0;
      e->pic = bdfi_r_decode(r, data, len, &e->pic, &e->tried, &full);
      free(data);
      if (!e->pic) {
        if (r->oom) {
          e->tried = 0;
          return;
        }
        if (!full) {
          bdfi_r_warn(r, "an image in an SVG image cannot be decoded");
        }
        e->tried = !full; /* there may be room for it in the next image drawn */
        return;
      }
    }
  }
  if (!e->pic && !e->image) {
    return;
  }
  bdfi_picture *pic;
  double iw, ih, w, h;
  if (e->image) {
    bdfi_svg_image *si = e->image;
    if (sr->depth > 8) {
      return;
    }
    if (sr->inside >= BDFI_SVG_INSIDE) {
      bdfi_r_warnf(r, "SVG images are more than %d deep in SVG images: the rest are not drawn",
                   BDFI_SVG_INSIDE);
      return;
    }
    iw = si->w;
    ih = si->h;
    bdfi_svg_size_or(c, n, st, iw, ih, &w, &h);
    double scale;
    pic = bdfi_svg_raster_in(r, si, bdfi_mat_max_scale(&c->m) * bdfi_fmax(w / iw, h / ih),
                             sr->inside + 1, &scale);
    if (!pic) {
      return;
    }
  } else {
    pic = e->pic;
    pic->used = r->drawing;
    iw = (double)pic->w;
    ih = (double)pic->h;
  }
  bdfi_svg_size_or(c, n, st, iw, ih, &w, &h);
  if (!(w > 0 && h > 0)) {
    return;
  }
  const double whole[4] = {0, 0, iw, ih};
  bdfi_mat m = bdfi_mat_mul2(
      c->m, bdfi_svg_aspect(whole, bdfi_rs_attr(n, BDFI_LIT("preserveAspectRatio")), x, y, w, h));
  /* image pixels of the raster to the device */
  bdfi_mat pixels = {{iw / (double)pic->w, 0, 0, ih / (double)pic->h, 0, 0}};
  bdfi_mat full = bdfi_mat_mul2(m, pixels), inv;
  if (!bdfi_mat_invert(&full, &inv)) {
    return;
  }
  bdfi_irect bounds = bdfi_surface_bounds(c->s);
  bdfi_mask *box = bdfi_svgr_rect(sr, 0, 0, iw, ih, &m, bdfi_irect_intersect(c->clip->r, bounds));
  bdfi_mask *port = bdfi_svgr_rect(sr, x, y, w, h, &c->m, bounds);
  bdfi_mask *cov = bdfi_svgr_made(sr, bdfi_mask_intersect(box, port));
  bdfi_mask_unref(box);
  bdfi_mask_unref(port);
  bdfi_image_shader sh;
  bdfi_image_shader_init(&sh, pic, &inv, 1, 0, 0, (double)pic->w, (double)pic->h, &r->oom);
  if (!r->oom &&
      bdfi_surface_fill(c->s, cov, &sh.base, 1, BDF_BLEND_SOURCE_OVER, c->clip) < 0) {
    r->oom = 1;
  }
  bdfi_mask_unref(cov);
}

/* ---- Elements ------------------------------------------------------------ */

/* children: the children of an element drawn; of a switch, the first that
 * browsers draw: those without requirements. */
static void bdfi_svgr_children(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *ctx,
                               const bdfi_svg_style *st) {
  int is_switch = bdfi_rs_named(n, BDFI_LIT("switch"));
  for (const bdfi_svg_node *c = n->first_child; c && !sr->r->oom; c = c->next) {
    if (is_switch) {
      if (!bdfi_rs_is_text(c) && bdfi_rs_attr(c, BDFI_LIT("requiredExtensions")).n == 0 &&
          bdfi_rs_attr(c, BDFI_LIT("systemLanguage")).n == 0) {
        bdfi_svgr_node(sr, c, ctx, st);
        return;
      }
      continue;
    }
    bdfi_svgr_node(sr, c, ctx, st);
  }
}

/* nested: the content of an svg or symbol element in a new viewport at
 * (x, y), ws × hs (the element's own width and height when empty, else
 * 100%). via_use says that a use element shows it, whose style it takes. */
static void bdfi_svgr_nested(bdfi_svgr *sr, const bdfi_svg_node *content, int via_use,
                             const bdfi_svg_ctx *c, const bdfi_svg_style *style, double x,
                             double y, bdfi_svg_str ws, bdfi_svg_str hs) {
  if (ws.n == 0) {
    ws = bdfi_rs_attr(content, BDFI_LIT("width"));
  }
  if (hs.n == 0) {
    hs = bdfi_rs_attr(content, BDFI_LIT("height"));
  }
  if (ws.n == 0) {
    ws = bdfi_rs_lit(BDFI_LIT("100%"));
  }
  if (hs.n == 0) {
    hs = bdfi_rs_lit(BDFI_LIT("100%"));
  }
  double w = bdfi_svg_length(c, ws, 'x', style->font_size);
  double h = bdfi_svg_length(c, hs, 'y', style->font_size);
  if (!(w > 0 && h > 0)) {
    return;
  }
  bdfi_svg_ctx nc = *c;
  bdfi_svg_style st = *style;
  if (via_use) {
    st = bdfi_svg_compute(content, &st);
  }
  bdfi_mask *made = NULL;
  bdfi_svg_str ov = bdfi_rs_declared(content, BDFI_LIT("overflow"));
  if (!bdfi_svg_str_eq(ov, BDFI_LIT("visible")) && !bdfi_svg_str_eq(ov, BDFI_LIT("auto"))) {
    bdfi_rpath p;
    memset(&p, 0, sizeof p);
    bdfi_rpath_rect(&p, x, y, w, h);
    if (p.oom) {
      sr->r->oom = 1;
      bdfi_rpath_clear(&p);
      return;
    }
    bdfi_mask *port =
        bdfi_svgr_coverage(sr, &p, &nc.m, BDF_FILL_NONZERO, bdfi_surface_bounds(nc.s));
    bdfi_rpath_clear(&p);
    made = bdfi_svgr_made(sr, bdfi_mask_intersect(nc.clip, port));
    bdfi_mask_unref(port);
    nc.clip = made;
  }
  double vb[4];
  if (bdfi_svg_view_box(bdfi_rs_attr(content, BDFI_LIT("viewBox")), vb)) {
    nc.m = bdfi_mat_mul2(
        nc.m, bdfi_svg_aspect(vb, bdfi_rs_attr(content, BDFI_LIT("preserveAspectRatio")), x, y, w, h));
    nc.vw = vb[2];
    nc.vh = vb[3];
  } else {
    nc.m = bdfi_mat_translate(nc.m, x, y);
    nc.vw = w;
    nc.vh = h;
  }
  if (!sr->r->oom) {
    bdfi_svgr_children(sr, content, &nc, &st);
  }
  bdfi_mask_unref(made);
}

static int bdfi_svg_is_ancestor(const bdfi_svg_node *a, const bdfi_svg_node *n) {
  for (const bdfi_svg_node *p = n; p; p = p->parent) {
    if (p == a) {
      return 1;
    }
  }
  return 0;
}

/* element: an element drawn in its own user space. */
static void bdfi_svgr_element(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *c,
                              const bdfi_svg_style *st) {
#define A(name) bdfi_rs_attr(n, BDFI_LIT(name))
  if (bdfi_rs_named(n, BDFI_LIT("svg"))) {
    bdfi_svgr_nested(sr, n, 0, c, st, bdfi_svg_length(c, A("x"), 'x', st->font_size),
                     bdfi_svg_length(c, A("y"), 'y', st->font_size), A("width"), A("height"));
  } else if (bdfi_rs_named(n, BDFI_LIT("g")) || bdfi_rs_named(n, BDFI_LIT("a")) ||
             bdfi_rs_named(n, BDFI_LIT("switch"))) {
    bdfi_svgr_children(sr, n, c, st);
  } else if (bdfi_rs_named(n, BDFI_LIT("use"))) {
    const bdfi_svg_node *target = bdfi_svgr_ref(sr, A("href"));
    if (!target || bdfi_svg_is_ancestor(target, n)) {
      return;
    }
    double x = bdfi_svg_length(c, A("x"), 'x', st->font_size);
    double y = bdfi_svg_length(c, A("y"), 'y', st->font_size);
    if (bdfi_rs_named(target, BDFI_LIT("symbol")) || bdfi_rs_named(target, BDFI_LIT("svg"))) {
      bdfi_svgr_nested(sr, target, 1, c, st, x, y, A("width"), A("height"));
    } else {
      bdfi_svg_ctx uc = *c;
      uc.m = bdfi_mat_translate(uc.m, x, y);
      bdfi_svgr_node(sr, target, &uc, st);
    }
  } else if (bdfi_rs_named(n, BDFI_LIT("text"))) {
    bdfi_svgr_text(sr, n, c, st);
  } else if (bdfi_rs_named(n, BDFI_LIT("image"))) {
    bdfi_svgr_image(sr, n, c, st);
  } else {
    int owned;
    bdfi_rpath *p = bdfi_svgr_shape(sr, n, c, st, &owned);
    if (p) {
      bdfi_svgr_paint(sr, p, c, st);
      if (owned) {
        bdfi_svg_free_path(p);
      }
    }
  }
#undef A
}

/* The elements that draw nothing where they are. */
static int bdfi_svg_not_drawn(const bdfi_svg_node *n) {
  static const char *const names[] = {"defs",           "symbol", "clipPath", "mask",   "linearGradient",
                                      "radialGradient", "pattern", "marker",  "title",  "desc",
                                      "metadata",       "style",  "script",   "filter", "foreignObject"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (bdfi_rs_named(n, names[i], strlen(names[i]))) {
      return 1;
    }
  }
  return 0;
}

/* node: an element drawn. */
static void bdfi_svgr_node(bdfi_svgr *sr, const bdfi_svg_node *n, const bdfi_svg_ctx *ctx,
                           const bdfi_svg_style *parent) {
  struct bdf_renderer *r = sr->r;
  if (bdfi_rs_is_text(n)) {
    return;
  }
  if (bdfi_svg_str_eq(bdfi_rs_declared(n, BDFI_LIT("display")), BDFI_LIT("none"))) {
    return;
  }
  if (bdfi_svg_not_drawn(n)) {
    return;
  }
  if (sr->depth > 32 || sr->budget <= 0) {
    if (sr->budget == 0) {
      sr->budget--;
      bdfi_r_warn(r, "an SVG image has too many elements: the rest are not drawn");
    }
    return;
  }
  sr->budget--;
  sr->depth++;
  bdfi_svg_style st = bdfi_svg_compute(n, parent);
  bdfi_svg_ctx c = *ctx;
  bdfi_mask *made = NULL; /* the clip, when the element has one of its own */
  bdfi_svg_str t = bdfi_rs_attr(n, BDFI_LIT("transform"));
  if (t.n > 0) {
    c.m = bdfi_mat_mul2(c.m, bdfi_svg_parse_transform(t.p, t.n));
  }
  bdfi_svg_str cp = bdfi_rs_declared(n, BDFI_LIT("clip-path"));
  if (cp.n > 0 && !bdfi_svg_str_eq(cp, BDFI_LIT("none"))) {
    const bdfi_svg_node *target = bdfi_svgr_ref(sr, cp);
    if (target && bdfi_rs_named(target, BDFI_LIT("clipPath"))) {
      bdfi_mask *shape = bdfi_svgr_clip_mask(sr, target, n, &c, &st);
      made = bdfi_svgr_made(sr, bdfi_mask_intersect(c.clip, shape));
      bdfi_mask_unref(shape);
      c.clip = made;
    }
  }
  double opacity = 1, f;
  bdfi_svg_str v;
  if (bdfi_mask_is_empty(c.clip) || r->oom) {
    goto done;
  }
  if (bdfi_svg_declared(n, BDFI_LIT("opacity"), &v) && bdfi_rs_number(v, &f)) {
    opacity = bdfi_rs_unit(f);
  }
  if (opacity <= 0) {
    goto done;
  }
  v = bdfi_rs_declared(n, BDFI_LIT("mask"));
  if (v.n > 0 && !bdfi_svg_str_eq(v, BDFI_LIT("none"))) {
    bdfi_r_warn(r, "SVG masks are not applied");
  }
  if (opacity < 1) {
    /* the element drawn alone, then blended in with its opacity */
    int64_t pixels = (int64_t)c.s->w * c.s->h;
    if (sr->layers >= BDFI_SVG_LAYERS || pixels > BDFI_SVG_LIVE_PIXELS - sr->live ||
        pixels > BDFI_SVG_LAYER_PIXELS - sr->layer_pixels) {
      bdfi_r_warn(r, "an SVG image has too many translucent elements: the rest of them are not "
                     "drawn");
      goto done;
    }
    sr->layers++;
    sr->live += pixels;
    sr->layer_pixels += pixels;
    bdfi_surface *layer = bdfi_r_new_surface(r, c.s->w, c.s->h);
    if (layer) {
      bdfi_svg_ctx lc = c;
      lc.s = layer;
      bdfi_svgr_element(sr, n, &lc, &st);
    }
    sr->layers--;
    sr->live -= pixels;
    if (layer && !r->oom) {
      bdfi_mask *cov = bdfi_mask_new(c.clip->r, 0);
      bdfi_mask *all = bdfi_mask_new(bdfi_surface_bounds(c.s), 0);
      bdfi_surface_shader sh = bdfi_surface_shader_of(layer, 0, 0);
      if (!cov || !all ||
          bdfi_surface_fill(c.s, cov, &sh.base, (float)opacity, BDF_BLEND_SOURCE_OVER, all) < 0) {
        r->oom = 1;
      }
      bdfi_mask_unref(cov);
      bdfi_mask_unref(all);
    }
    bdfi_r_release(r, layer);
    goto done;
  }
  bdfi_svgr_element(sr, n, &c, &st);
done:
  bdfi_mask_unref(made);
  sr->depth--;
}

/* render: the document drawn on a surface whose user space is m, for a
 * viewport of w × h CSS px. */
static void bdfi_svgr_render(bdfi_svgr *sr, bdfi_surface *s, bdfi_mat m, double w, double h) {
  const bdfi_svg_node *root = sr->si->doc->root;
  bdfi_mask *clip = bdfi_mask_new(bdfi_surface_bounds(s), 0);
  if (!clip) {
    sr->r->oom = 1;
    return;
  }
  bdfi_svg_ctx ctx = {s, m, clip, w, h};
  bdfi_svg_style initial = bdfi_svg_default_style();
  bdfi_svg_style st = bdfi_svg_compute(root, &initial);
  double vb[4];
  if (bdfi_svg_view_box(bdfi_rs_attr(root, BDFI_LIT("viewBox")), vb)) {
    ctx.m = bdfi_mat_mul2(
        ctx.m, bdfi_svg_aspect(vb, bdfi_rs_attr(root, BDFI_LIT("preserveAspectRatio")), 0, 0, w, h));
    ctx.vw = vb[2];
    ctx.vh = vb[3];
  }
  bdfi_svgr_children(sr, root, &ctx, &st);
  bdfi_mask_unref(clip);
}

/* ---- Images and their rasters -------------------------------------------- */

/* newSVGImage: an SVG image part read. NULL for what is no SVG document
 * (*status is BDF_ERR_FORMAT, or BDF_ERR_TOO_LARGE past the limits of the
 * reader) and when out of memory (BDF_ERR_NOMEM). */
BDF_INTERNAL bdfi_svg_image *bdfi_svg_image_new(const uint8_t *data, size_t len,
                                                bdf_status *status) {
  bdfi_svg_image *si = calloc(1, sizeof *si);
  if (!si) {
    *status = BDF_ERR_NOMEM;
    return NULL;
  }
  *status = bdfi_svg_parse(data, len, NULL, &si->doc, NULL);
  if (*status != BDF_OK) {
    free(si);
    return NULL;
  }
  bdfi_svg_natural_size(si->doc->root, &si->w, &si->h);
  if (bdfi_isinf(si->w) || bdfi_isinf(si->h)) {
    bdfi_svg_image_free(si);
    *status = BDF_ERR_FORMAT;
    return NULL;
  }
  return si;
}

/* The rasters of an image, and the pictures of its image elements, are
 * among the pictures the renderer keeps, and are not freed here. */
BDF_INTERNAL void bdfi_svg_image_free(bdfi_svg_image *si) {
  if (!si) {
    return;
  }
  size_t n = si->doc ? si->doc->nnodes : 0;
  for (size_t i = 0; si->paths && i < n; i++) {
    bdfi_svg_free_path(si->paths[i]);
  }
  free(si->paths);
  for (size_t i = 0; si->embeds && i < n; i++) {
    if (si->embeds[i]) {
      bdfi_svg_image_free(si->embeds[i]->image);
      free(si->embeds[i]);
    }
  }
  free(si->embeds);
  bdfi_svg_free(si->doc);
  free(si);
}

BDF_INTERNAL void bdfi_svg_image_size(const bdfi_svg_image *si, double *w, double *h) {
  *w = si->w;
  *h = si->h;
}

BDF_INTERNAL size_t bdfi_svg_image_warnings(const bdfi_svg_image *si, const char *const **out) {
  *out = si->doc->warnings;
  return si->doc->nwarnings;
}

/* math.Pow(2, float64(step)/2) as the reference's math computes it: exact
 * for an even step; for an odd one it is the square root of two for 1 and
 * below -1, and a bit less of it for -1 and above 1. */
static double bdfi_svg_pow_half(int step) {
  int fl = step >= 0 ? step / 2 : -((1 - step) / 2);
  if ((step & 1) == 0) {
    return ldexp(1, fl);
  }
  double mant = step == 1 || step <= -3 ? 1.4142135623730951 : 1.4142135623730949;
  return ldexp(mant, fl);
}

/* rasterSize: the scale a raster for k device pixels per image pixel is
 * drawn at, which is 2 to the power of step/2, and its size; no size for an
 * image that has no raster. */
static void bdfi_svg_raster_size(const bdfi_svg_image *si, double k, int *step, double *scale,
                                 int *w, int *h) {
  if (!(k > 0)) {
    k = 1;
  }
  double most = sqrt((double)BDFI_SVG_RASTER_PIXELS / (si->w * si->h));
  if (k > most) {
    k = most;
  }
  /* between 2^-32 and 2^32: no raster is scaled more, and the scale of an
   * image of no size is no number */
  int s = (int)bdfi_to_int(bdfi_fmin(bdfi_fmax(ceil(2 * bdfi_log2(k)), -64), 64));
  double fw, fh;
#define SIZE()                                                                                     \
  do {                                                                                             \
    k = bdfi_svg_pow_half(s);                                                                      \
    fw = bdfi_fmax(1, ceil(si->w * k));                                                            \
    fh = bdfi_fmax(1, ceil(si->h * k));                                                            \
  } while (0)
  SIZE();
  while (fw * fh > 2 * (double)BDFI_SVG_RASTER_PIXELS && bdfi_fmin(si->w, si->h) * k < 1 &&
         s > -64) {
    /* less than a pixel wide or high, which is one all the same: the other
     * side gives the scale */
    s--;
    SIZE();
  }
#undef SIZE
  *step = s;
  *scale = k;
  if (!(fw * fh <= 4 * (double)BDFI_SVG_RASTER_PIXELS + 2)) {
    *w = 0;
    *h = 0;
    return;
  }
  *w = (int)fw;
  *h = (int)fh;
}

/* rasterIn: a raster of the image drawn with k device pixels per image
 * pixel (rounded up to a power of the square root of two, and bounded), for
 * an image that is inside as many SVG images as inside says. NULL when the
 * renderer has no room for it; *scale is the scale of the raster, that of
 * its width, whole pixels, to the image's. */
static bdfi_picture *bdfi_svg_raster_in(struct bdf_renderer *r, bdfi_svg_image *si, double k,
                                        int inside, double *scale) {
  int step, w, h;
  bdfi_svg_raster_size(si, k, &step, &k, &w, &h);
  *scale = k;
  if (w == 0 || step < -64 || step > 64) {
    return NULL;
  }
  bdfi_picture **slot = &si->rasters[step + 64];
  if (*slot) {
    (*slot)->used = r->drawing;
    /* the scale it was given when it was made, below */
    *scale = (double)(*slot)->w / si->w;
    return *slot;
  }
  if (!bdfi_r_make_room(r, (int64_t)w * h)) {
    bdfi_r_warnf(r, BDFI_TOO_MANY_PIXELS, (long long)r->room);
    return NULL;
  }
  bdfi_surface *s = bdfi_r_new_surface(r, w, h);
  bdfi_picture *p = calloc(1, sizeof *p);
  uint8_t *pix = bdfi_malloc_array((size_t)w * (size_t)h, 4);
  if (!s || !p || !pix) {
    bdfi_r_release(r, s);
    free(p);
    free(pix);
    r->oom = 1;
    return NULL;
  }
  bdfi_svgr sr;
  memset(&sr, 0, sizeof sr);
  sr.r = r;
  sr.si = si;
  sr.budget = BDFI_SVG_BUDGET;
  sr.inside = inside;
  bdfi_mat m = {{k, 0, 0, k, 0, 0}};
  bdfi_svgr_render(&sr, s, m, si->w, si->h);
  if (r->oom) {
    bdfi_r_release(r, s);
    free(p);
    free(pix);
    return NULL;
  }
  bdfi_surface_to_rgba(s, pix);
  bdfi_r_release(r, s);
  p->w = w;
  p->h = h;
  p->levels[0].w = w;
  p->levels[0].h = h;
  p->levels[0].pix = pix;
  p->nlevels = 1;
  if (!bdfi_r_keep(r, p, slot, NULL)) {
    bdfi_picture_free(p);
    return NULL;
  }
  *slot = p;
  *scale = (double)w / si->w;
  return p;
}

/* raster: a raster of the image drawn with k device pixels per image pixel. */
BDF_INTERNAL bdfi_picture *bdfi_svg_image_raster(struct bdf_renderer *r, bdfi_svg_image *si,
                                                 double k, double *scale) {
  return bdfi_svg_raster_in(r, si, k, 0, scale);
}
