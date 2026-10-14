/*
 * libbdf through Core Graphics (Quartz 2D): the pages of a BDF document
 * drawn on a CGContext, for Cocoa and UIKit views, PDF contexts and
 * bitmaps.
 *
 *   bdf_renderer *r;
 *   bdf_renderer_new(doc, NULL, &r, &err);
 *   bdf_cg *cg = bdf_cg_new(r);
 *   // the user space of a page has its origin at the top left, y down:
 *   CGContextTranslateCTM(ctx, 0, height);
 *   CGContextScaleCTM(ctx, zoom, -zoom);
 *   bdf_cg_draw_page(cg, view, page, ctx, &err);
 *
 * The paper is the caller's (fill it first). Text is drawn as the outlines
 * of its glyphs, laid out by libbdf (see bdf_fonts for the text that names
 * its font).
 */
#ifndef BDF_COREGRAPHICS_H
#define BDF_COREGRAPHICS_H

#include <CoreGraphics/CoreGraphics.h>

#include "bdf/bdf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A renderer drawing through Core Graphics, and what is kept from one
 * drawing to the next: the CGImages of the images the renderer has decoded.
 * It is freed before the renderer. */
typedef struct bdf_cg bdf_cg;

bdf_cg *bdf_cg_new(bdf_renderer *renderer); /* NULL when out of memory */
void bdf_cg_free(bdf_cg *cg);

/* Draws a page (bdf_play_page) in the user space of ctx as it is. */
bdf_status bdf_cg_draw_page(bdf_cg *cg, size_t view, size_t page, CGContextRef ctx, bdf_error *err);
/* Draws a rectangle (x, y, w, h in units) of a view (bdf_play_region): of a
 * page, of the strips of a scroll view, or of a sheet with its gridlines,
 * with its corner at the origin of the user space of ctx. */
bdf_status bdf_cg_draw_region(bdf_cg *cg, size_t view, size_t page, const float region[4],
                              CGContextRef ctx, bdf_error *err);
/* Draws an object part (bdf_play_part) in its own coordinates. */
bdf_status bdf_cg_draw_part(bdf_cg *cg, const uint8_t hash[16], CGContextRef ctx, bdf_error *err);

/* The canvas the two draw through, for a caller that plays by itself. It
 * is valid while cg is and ctx is the context it was made for, in the user
 * space ctx has when this is called. */
void bdf_cg_canvas(bdf_cg *cg, CGContextRef ctx, bdf_canvas *out);

#ifdef __cplusplus
}
#endif

#endif /* BDF_COREGRAPHICS_H */
