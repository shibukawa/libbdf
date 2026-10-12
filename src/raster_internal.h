/*
 * Internals of the rasterizer, a port of the reference implementation's
 * raster/imagebdf.
 *
 * The arithmetic of the rasterizer is that of the Go code, in the same
 * order and the same types, so that it draws the same pixels; and it must
 * not be fused: a compiler that makes one operation of a product and the
 * sum after it rounds once where the reference rounds twice, and a pixel in
 * a million comes out a step lighter. The sources of the rasterizer turn
 * that off below, and CMake compiles them with -ffp-contract=off as well.
 */
#ifndef BDF_RASTER_INTERNAL_H
#define BDF_RASTER_INTERNAL_H

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#include "bdf_internal.h"

/* ---- Go's math ----------------------------------------------------------- */

/* Constants as Go rounds them: its constant expressions are exact until they
 * are used, so 3*Pi/2 is the double nearest to 3π/2, not 3 times the double
 * nearest to π. */
#define BDFI_PI    3.1415926535897931  /* 0x400921fb54442d18 */
#define BDFI_PI_2  1.5707963267948966  /* 0x3ff921fb54442d18 */
#define BDFI_PI_4  0.78539816339744828 /* 0x3fe921fb54442d18 */
#define BDFI_2PI   6.2831853071795862  /* 0x401921fb54442d18 */
#define BDFI_3PI_2 4.7123889803846897  /* 0x4012d97c7f3321d2 */
#define BDFI_3PI_4 2.3561944901923448  /* 0x4002d97c7f3321d2 */
#define BDFI_SQRT2 1.4142135623730951  /* 0x3ff6a09e667f3bcd */

BDF_INTERNAL int bdfi_isnan(double v);
BDF_INTERNAL int bdfi_isinf(double v);
BDF_INTERNAL int bdfi_signbit(double v);
BDF_INTERNAL void bdfi_sincos(double x, double *sn, double *cs);
BDF_INTERNAL double bdfi_sin(double x);
BDF_INTERNAL double bdfi_cos(double x);
BDF_INTERNAL double bdfi_tan(double x);
BDF_INTERNAL double bdfi_atan(double x);
BDF_INTERNAL double bdfi_atan2(double y, double x);
BDF_INTERNAL double bdfi_asin(double x);
BDF_INTERNAL double bdfi_acos(double x);
BDF_INTERNAL double bdfi_hypot(double p, double q);
BDF_INTERNAL double bdfi_fmax(double x, double y);
BDF_INTERNAL double bdfi_fmin(double x, double y);
BDF_INTERNAL double bdfi_mod(double x, double y);
#ifndef BDF_IMPLEMENTATION
/* a*b + c as the sources of the rasterizer compute it, for the tests of the
 * build: two operations, whatever the processor offers */
BDF_INTERNAL double bdfi_r_muladd(double a, double b, double c);
#endif

/* ---- Geometry ------------------------------------------------------------ */

typedef struct bdfi_pt {
  double x, y;
} bdfi_pt;

/* A rectangle of pixels: [x0, x1) × [y0, y1). One that holds none is empty,
 * and what makes rectangles gives the one of zeros for it (Go's image.ZR),
 * so that two empty ones are equal. */
typedef struct bdfi_irect {
  int x0, y0, x1, y1;
} bdfi_irect;

BDF_INTERNAL bdfi_irect bdfi_irect_of(int x0, int y0, int x1, int y1);
BDF_INTERNAL int bdfi_irect_empty(bdfi_irect r);
BDF_INTERNAL bdfi_irect bdfi_irect_intersect(bdfi_irect r, bdfi_irect s);
BDF_INTERNAL int bdfi_irect_overlaps(bdfi_irect r, bdfi_irect s);
BDF_INTERNAL int bdfi_irect_eq(bdfi_irect r, bdfi_irect s);
#define BDFI_DX(r) ((r).x1 - (r).x0)
#define BDFI_DY(r) ((r).y1 - (r).y0)

/* An affine transform as Canvas 2D keeps it: x' = a x + c y + e,
 * y' = b x + d y + f. */
typedef struct bdfi_mat {
  double v[6];
} bdfi_mat;

BDF_INTERNAL bdfi_mat bdfi_mat_identity(void);
/* m × n: n applies first. */
BDF_INTERNAL bdfi_mat bdfi_mat_mul2(bdfi_mat m, bdfi_mat n);
BDF_INTERNAL bdfi_pt bdfi_mat_pt(const bdfi_mat *m, double x, double y);
BDF_INTERNAL bdfi_mat bdfi_mat_translate(bdfi_mat m, double x, double y);
BDF_INTERNAL bdfi_mat bdfi_mat_scale(bdfi_mat m, double x, double y);
BDF_INTERNAL double bdfi_mat_det(const bdfi_mat *m);
BDF_INTERNAL int bdfi_mat_invert(const bdfi_mat *m, bdfi_mat *out);
BDF_INTERNAL double bdfi_mat_max_scale(const bdfi_mat *m);
BDF_INTERNAL double bdfi_mat_area_scale(const bdfi_mat *m);
BDF_INTERNAL int bdfi_mat_finite(const bdfi_mat *m);

/* clampF: a coordinate in a range that converts to an int safely. */
BDF_INTERNAL double bdfi_clampf(double v);

/* ---- Paths --------------------------------------------------------------- */

enum { BDFI_SEG_LINE = 0, BDFI_SEG_QUAD = 1, BDFI_SEG_CUBIC = 2 };

typedef struct bdfi_seg {
  int kind;
  double p[6]; /* the control points and the end point */
} bdfi_seg;

/* A run of segments from a start point: those of segs[seg0 … seg0+nsegs). */
typedef struct bdfi_sub {
  bdfi_pt start;
  size_t seg0, nsegs;
  int closed;
} bdfi_sub;

/* A Path2D: sub-paths in user space. */
typedef struct bdfi_rpath {
  bdfi_sub *subs;
  size_t nsubs, cap_subs;
  bdfi_seg *segs;
  size_t nsegs, cap_segs;
  double box[4]; /* of its points, once bdfi_rpath_extent found it */
  int boxed;
  int oom;
} bdfi_rpath;

BDF_INTERNAL void bdfi_rpath_clear(bdfi_rpath *p);
BDF_INTERNAL void bdfi_rpath_move_to(bdfi_rpath *p, double x, double y);
BDF_INTERNAL void bdfi_rpath_line_to(bdfi_rpath *p, double x, double y);
BDF_INTERNAL void bdfi_rpath_quad_to(bdfi_rpath *p, double cx, double cy, double x, double y);
BDF_INTERNAL void bdfi_rpath_cubic_to(bdfi_rpath *p, double c1x, double c1y, double c2x,
                                      double c2y, double x, double y);
BDF_INTERNAL void bdfi_rpath_close(bdfi_rpath *p);
BDF_INTERNAL void bdfi_rpath_rect(bdfi_rpath *p, double x, double y, double w, double h);
BDF_INTERNAL void bdfi_rpath_ellipse(bdfi_rpath *p, double cx, double cy, double rx, double ry,
                                     double rot, double a0, double a1, int ccw);
BDF_INTERNAL void bdfi_rpath_arc_to(bdfi_rpath *p, double x1, double y1, double x2, double y2,
                                    double r);
BDF_INTERNAL void bdfi_rpath_round_rect(bdfi_rpath *p, double x, double y, double w, double h,
                                        double r);
/* bdfi_rpath_build adds a path of an object or of a path collection. */
BDF_INTERNAL void bdfi_rpath_build(bdfi_rpath *p, const bdf_path *bp);
/* The bounding box of the points, and whether the path has one. */
BDF_INTERNAL int bdfi_rpath_extent(bdfi_rpath *p, double box[4]);
/* Whether the path is one axis-aligned rectangle, and its corners. */
BDF_INTERNAL int bdfi_rpath_is_rect(const bdfi_rpath *p, double *x0, double *y0, double *x1,
                                    double *y1);

/* A flattened sub-path: points [off, off+n) of the buffer it was made in. */
typedef struct bdfi_poly {
  size_t off, n;
  int closed;
} bdfi_poly;

typedef struct bdfi_edge {
  double x0, y0, y1;
  double slope; /* dx/dy */
  int8_t dir;
} bdfi_edge;

typedef struct bdfi_crossing {
  double x;
  int8_t dir;
  uint32_t edge;
} bdfi_crossing;

struct bdf_renderer;

/* The working memory of the rasterizer, emptied for each drawing instruction
 * (bdfi_scratch_reset) and kept from one to the next. */
typedef struct bdfi_scratch {
  bdfi_edge *edges;
  size_t cap_edges;
  uint32_t *active;
  size_t cap_active;
  bdfi_crossing *xs;
  size_t cap_xs;
  float *area, *cover;
  size_t cap_area;
  /* the points of the paths flattened for a drawing, and their polylines */
  bdfi_pt *pts;
  size_t npts, cap_pts;
  bdfi_poly *polys;
  size_t npolys, cap_polys;
  /* limits of a whole drawing instruction: a fill that passed one is not
   * painted */
  int limited;
  int64_t work;
  int oom;
  struct bdf_renderer *r; /* for the warning; may be NULL */
} bdfi_scratch;

#define BDFI_MAX_PATH_POINTS ((size_t)1 << 20)
#define BDFI_MAX_SCAN_WORK   ((int64_t)1 << 24)
#define BDFI_FLAT_TOL        0.1 /* how far flattened curves may stray, in device pixels */

BDF_INTERNAL void bdfi_scratch_reset(bdfi_scratch *sc);
BDF_INTERNAL void bdfi_scratch_free(bdfi_scratch *sc);
BDF_INTERNAL void bdfi_scratch_stop(bdfi_scratch *sc);
/* bdfi_r_warn notes what could not be drawn as the viewer would, once. */
BDF_INTERNAL void bdfi_r_warn(struct bdf_renderer *r, const char *message);
/* bdfi_rpath_flatten adds the polylines of a path under m to sc, curves
 * split until they stray from their chords by less than tol. */
BDF_INTERNAL void bdfi_rpath_flatten(const bdfi_rpath *p, bdfi_scratch *sc, const bdfi_mat *m,
                                     double tol);
/* Whether a box in user space, with reach more on every side, lies outside
 * bounds under m by more than a pixel. */
BDF_INTERNAL int bdfi_outside(const double box[4], double reach, const bdfi_mat *m,
                              bdfi_irect bounds);
BDF_INTERNAL int bdfi_hidden(bdfi_rpath *p, double reach, const bdfi_mat *m, bdfi_irect bounds);

/* ---- Coverage ------------------------------------------------------------ */

/* The coverage of the pixels of a rectangle, 0 to 1; without a, the whole
 * rectangle is covered. Masks are shared (the clip of saved states) and
 * counted. */
typedef struct bdfi_mask {
  bdfi_irect r;
  float *a;
  int refs; /* 0: the one empty mask, which is nobody's */
} bdfi_mask;

BDF_INTERNAL bdfi_mask *bdfi_mask_empty(void);
/* A mask of a rectangle: wholly covered, or with zeroed coverage to fill in.
 * NULL when out of memory. */
BDF_INTERNAL bdfi_mask *bdfi_mask_new(bdfi_irect r, int with_coverage);
BDF_INTERNAL bdfi_mask *bdfi_mask_ref(bdfi_mask *m);
BDF_INTERNAL void bdfi_mask_unref(bdfi_mask *m);
BDF_INTERNAL int bdfi_mask_is_empty(const bdfi_mask *m);
BDF_INTERNAL size_t bdfi_mask_bytes(const bdfi_mask *m);
/* The coverage of pixels [x0, x1) of row y, or NULL for full coverage. */
BDF_INTERNAL const float *bdfi_mask_row(const bdfi_mask *m, int y, int x0);
BDF_INTERNAL bdfi_mask *bdfi_mask_intersect(const bdfi_mask *a, const bdfi_mask *b);
/* The exact coverage of an axis-aligned rectangle in device space. */
BDF_INTERNAL bdfi_mask *bdfi_rect_mask(double x0, double y0, double x1, double y1,
                                       bdfi_irect bounds);
/* The coverage of polylines (each closed) of a buffer of points under a
 * fill rule. */
BDF_INTERNAL bdfi_mask *bdfi_rasterize(bdfi_scratch *sc, const bdfi_pt *pts, const bdfi_poly *polys,
                                       size_t npolys, uint8_t rule, bdfi_irect bounds);

/* ---- Strokes ------------------------------------------------------------- */

/* A list of dashes that states share. */
typedef struct bdfi_dash {
  int refs;
  size_t n; /* even */
  double v[1];
} bdfi_dash;

typedef struct bdfi_line_style {
  double width;
  uint8_t cap, join;
  double miter;
  bdfi_dash *dash; /* or NULL */
  double dash_off;
} bdfi_line_style;

BDF_INTERNAL double bdfi_miter_reach(const bdfi_line_style *ls);
/* setLineDash: -1 when out of memory. */
BDF_INTERNAL int bdfi_line_set_dash(bdfi_line_style *ls, const float *segs, size_t n, float off);
BDF_INTERNAL void bdfi_dash_unref(bdfi_dash *d);

#define BDFI_STROKE_POINTS      ((int64_t)1 << 20)
#define BDFI_STROKE_POINTS_EACH 16
#define BDFI_STROKE_DASHES      ((int64_t)1 << 20)

/* The outline of strokes: polygons that all wind the same way, for a
 * nonzero fill. */
typedef struct bdfi_stroker {
  const bdfi_line_style *ls;
  double hw, tol;
  bdfi_pt *pts;
  size_t npts, cap_pts;
  bdfi_poly *out;
  size_t nout, cap_out;
  bdfi_mat m; /* with cull: polygons outside bounds under m are left out */
  bdfi_irect bounds;
  int cull;
  int64_t points, dashes; /* what the outline may still take */
  int cut;                /* a limit was passed: the rest is left out */
  int oom;
  /* working memory: the points of a line without its repeats, those of a
   * piece of it, and the dash being cut */
  bdfi_pt *line, *piece, *cur;
  size_t cap_line, cap_piece, ncur, cap_cur;
} bdfi_stroker;

BDF_INTERNAL void bdfi_stroker_init(bdfi_stroker *s, const bdfi_line_style *ls, double dev_scale);
BDF_INTERNAL void bdfi_stroker_within(bdfi_stroker *s, const bdfi_mat *m, bdfi_irect bounds);
BDF_INTERNAL void bdfi_stroker_free(bdfi_stroker *s);
/* bdfi_stroker_stroke adds the outline of polylines (user space). */
BDF_INTERNAL void bdfi_stroker_stroke(bdfi_stroker *s, const bdfi_pt *pts, const bdfi_poly *lines,
                                      size_t nlines);
/* bdfi_stroke_mask rasterizes the stroke of a path under m within bounds:
 * the coverage, the alpha to draw it with (a stroke thinner than a pixel is
 * a pixel wide with its width as alpha), and whether the outline was cut. */
BDF_INTERNAL bdfi_mask *bdfi_stroke_mask(bdfi_scratch *sc, bdfi_rpath *p, const bdfi_mat *m,
                                         bdfi_line_style ls, bdfi_irect bounds, float *alpha,
                                         int *cut);

/* ---- Surfaces ------------------------------------------------------------ */

/* A canvas of premultiplied RGBA pixels, 0 to 1. */
typedef struct bdfi_surface {
  int w, h;
  float *pix;
  size_t cap; /* floats pix holds */
  float *row; /* the colours of a row being filled */
  size_t cap_row;
} bdfi_surface;

/* What gives the premultiplied colours of pixels [x, x+n) of row y. */
typedef struct bdfi_shader {
  void (*shade)(const struct bdfi_shader *sh, int x, int y, int n, float *dst);
  int flat; /* one colour: the same in every row */
} bdfi_shader;

typedef struct bdfi_solid {
  bdfi_shader base;
  float c[4];
} bdfi_solid;

BDF_INTERNAL bdfi_solid bdfi_solid_of(float r, float g, float b, float a);
/* A packed 0xRRGGBBAA colour, premultiplied. */
BDF_INTERNAL bdfi_solid bdfi_premul(uint32_t c);

/* The pixels of another surface placed at (dx, dy). */
typedef struct bdfi_surface_shader {
  bdfi_shader base;
  const bdfi_surface *s;
  int dx, dy;
} bdfi_surface_shader;

BDF_INTERNAL bdfi_surface_shader bdfi_surface_shader_of(const bdfi_surface *s, int dx, int dy);

BDF_INTERNAL int bdfi_unbounded(uint8_t mode);
BDF_INTERNAL bdfi_irect bdfi_surface_bounds(const bdfi_surface *s);
/* bdfi_surface_fill composites a shader through a coverage (NULL: all),
 * scaled by alpha, with a composite operation, inside a clip. -1 when out of
 * memory. */
BDF_INTERNAL int bdfi_surface_fill(bdfi_surface *s, const bdfi_mask *cov, const bdfi_shader *sh,
                                   float alpha, uint8_t mode, const bdfi_mask *clip);
BDF_INTERNAL int bdfi_surface_fill_background(bdfi_surface *s, const bdfi_mask *cov,
                                              const bdfi_solid *c);
BDF_INTERNAL void bdfi_composite(uint8_t mode, const float s[4], const float d[4], float out[4]);
/* The surface as premultiplied 8-bit pixels. */
BDF_INTERNAL void bdfi_surface_to_rgba(const bdfi_surface *s, uint8_t *out);

/* ---- Paints -------------------------------------------------------------- */

/* The colours of a gradient at 1024 steps from 0 to 1, premultiplied. */
typedef struct bdfi_lut {
  float c[1025][4];
} bdfi_lut;

BDF_INTERNAL void bdfi_lut_build(bdfi_lut *lut, const bdf_stop *stops, size_t n);

typedef struct bdfi_gradient_shader {
  bdfi_shader base;
  bdfi_mat inv;
  uint8_t kind;
  float coords[6];
  const bdfi_lut *lut;
} bdfi_gradient_shader;

/* A gradient drawn under m: 0 when it has nothing to draw (the caller draws
 * transparent black). */
BDF_INTERNAL int bdfi_gradient_shader_init(bdfi_gradient_shader *g, const bdf_paint *p,
                                           const bdfi_mat *m, const bdfi_lut *lut);

/* ---- The renderer -------------------------------------------------------- */

/* What a renderer keeps of an object: the object, and what was made of its
 * tables (a slot is filled when it is first asked for). */
typedef struct bdfi_robj {
  const bdf_object *obj;
  bdf_object *owned; /* obj, when the renderer decoded it */
  uint8_t state;     /* 0: not asked for; 1: there; 2: missing or malformed */
  bdfi_rpath **paths;
  uint8_t *path_state; /* 0: not asked for; 1: built (or none: NULL) */
  size_t npaths;
  bdfi_lut **luts;
  size_t nluts;
  bdf_paths *coll; /* the part as a path collection, and its bytes */
  uint8_t *coll_data;
  uint8_t coll_state;
} bdfi_robj;

struct bdf_renderer {
  const bdf_doc *doc; /* NULL for bdf_render_object */
  int has_background;
  uint32_t background;
  bdfi_robj *parts; /* by index of the part in the document */
  size_t nparts;
  bdfi_robj alone; /* the object of bdf_render_object */
  size_t luts;     /* how many gradients have their colours kept */
  char **warnings;
  size_t nwarnings, cap_warnings;
  bdfi_scratch sc;
  int oom;
};

BDF_INTERNAL void bdfi_r_warnf(struct bdf_renderer *r, const char *format, ...);
/* The object of a part, or NULL (missing, malformed: warned of once). */
BDF_INTERNAL bdfi_robj *bdfi_r_object(struct bdf_renderer *r, const uint8_t hash[16]);
/* Path i of an object, or NULL. */
BDF_INTERNAL bdfi_rpath *bdfi_r_path(struct bdf_renderer *r, bdfi_robj *o, size_t i);
/* The colours of gradient i of an object, or NULL. */
BDF_INTERNAL const bdfi_lut *bdfi_r_lut(struct bdf_renderer *r, bdfi_robj *o, size_t i,
                                        const bdf_paint *p);
BDF_INTERNAL bdfi_scratch *bdfi_r_scratch(struct bdf_renderer *r);
BDF_INTERNAL bdfi_surface *bdfi_r_new_surface(struct bdf_renderer *r, int w, int h);
BDF_INTERNAL void bdfi_r_release(struct bdf_renderer *r, bdfi_surface *s);

/* ---- Drawing ------------------------------------------------------------- */

/* A fill or stroke style: a colour, or a paint of an object. */
typedef struct bdfi_style {
  uint32_t color;
  int has_paint;
  bdf_paint paint; /* points into owner */
  bdfi_robj *owner;
  size_t index;
} bdfi_style;

/* The drawing state of a context: what SAVE and RESTORE keep. */
typedef struct bdfi_state {
  bdfi_mat m;
  bdfi_mask *clip; /* counted */
  bdfi_style fill, stroke;
  bdfi_line_style line; /* its dashes counted */
  float alpha;
  uint8_t blend;
  uint32_t shadow;
  double shadow_blur; /* device pixels */
  double shadow_dx, shadow_dy;
  bdfi_robj *font_owner; /* NULL: no font was set */
  size_t font_index;
  double size;
  uint8_t align, baseline, dir;
  double spacing;
  int smooth;
  /* the clip, if it was made after the SAVE of this state: its memory is
   * let go of with the state */
  bdfi_mask *made;
} bdfi_state;

/* A canvas being drawn on: the page, or that of a group or a soft mask. */
typedef struct bdfi_ctx {
  bdfi_surface *target;
  bdfi_state st;
  bdfi_state *stack;
  size_t nstack, cap_stack;
  int64_t unsaved; /* the SAVEs that kept no state: as many RESTOREs restore none */
  int64_t held;    /* the memory counted for this canvas */
  int done;        /* its group or soft mask has ended */
  int pins;        /* what looks at it still (see rdraw.c) */
} bdfi_ctx;

typedef struct bdfi_group {
  uint64_t id;
  bdfi_ctx *parent, *ctx;
  int x, y; /* the place of the group's canvas on the parent's */
  float alpha;
  uint8_t blend;
} bdfi_group;

typedef struct bdfi_soft_mask {
  bdfi_ctx *parent, *ctx;
  uint8_t kind;
  const uint8_t *transfer; /* 256 bytes, or NULL */
  uint64_t group;          /* the id of the group it masks; 0: none */
} bdfi_soft_mask;

/*
 * Limits of what the drawing of an image takes (see draw.go): the
 * instructions drawn from objects that were drawn before in the same image,
 * the groups and soft masks open at a time, the states kept for RESTORE, the
 * memory of the open canvases and clips (in canvases of the image, and what
 * they may take of a small image all the same), and the blur of a shadow in
 * device pixels. A build may define the first and the last lower
 * (BDF_MAX_REUSED_DRAWN, BDF_MAX_SHADOW_BLUR): the fuzzers do, which learn
 * nothing from the seconds an input at the limits takes.
 */
#ifndef BDF_MAX_REUSED_DRAWN
#define BDF_MAX_REUSED_DRAWN (1 << 24)
#endif
#ifndef BDF_MAX_SHADOW_BLUR
#define BDF_MAX_SHADOW_BLUR 1024
#endif
#define BDFI_MAX_REUSED_DRAWN ((int64_t)BDF_MAX_REUSED_DRAWN)
#define BDFI_MAX_LAYERS       64
#define BDFI_MAX_STATES       1024
#define BDFI_MAX_LIVE         8
#define BDFI_MIN_LIVE         ((int64_t)64 << 20)
#define BDFI_MAX_SHADOW_BLUR  BDF_MAX_SHADOW_BLUR

/* What runs the instructions of objects on a context. */
typedef struct bdfi_drawer {
  struct bdf_renderer *r;
  bdfi_ctx *ctx, *top;
  bdfi_robj *obj;
  bdfi_group *groups;
  size_t ngroups, cap_groups;
  bdfi_soft_mask *masks;
  size_t nmasks, cap_masks;
  uint64_t next_group;
  int depth;
  bdfi_hash_set seen; /* the objects drawn in this image */
  int again;          /* the object being drawn was drawn before */
  int64_t reused;     /* the instructions of such objects */
  int64_t skipped;    /* the groups and soft masks that are open but not drawn */
  int64_t live, limit; /* the memory of the open canvases and clips, and the most it may be */
  int64_t most;        /* the most reused may be */
} bdfi_drawer;

/* A drawer on a new canvas of w × h pixels with the device transform m:
 * NULL when out of memory. */
BDF_INTERNAL bdfi_drawer *bdfi_drawer_new(struct bdf_renderer *r, const bdfi_mat *m, int w, int h);
BDF_INTERNAL void bdfi_drawer_free(bdfi_drawer *d);
/* Draws a top-level object from the initial state with a transform and a
 * clip. */
BDF_INTERNAL void bdfi_draw_top(bdfi_drawer *d, bdfi_robj *o, const uint8_t *hash,
                                const bdfi_mat *m, bdfi_mask *clip);
/* The coverage of a rectangle in units under m, within the canvas. */
BDF_INTERNAL bdfi_mask *bdfi_draw_clip_to(bdfi_drawer *d, const bdfi_mat *m, double x, double y,
                                          double w, double h);

#endif /* BDF_RASTER_INTERNAL_H */
