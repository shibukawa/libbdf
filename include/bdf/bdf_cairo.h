/*
 * libbdf through cairo: the pages of a BDF document drawn on a cairo
 * context, with cairo's own paths, gradients, images and groups (and so on
 * whatever cairo draws on: an image, a PDF or SVG file, a window of GTK).
 *
 *   bdf_renderer *r;
 *   bdf_renderer_new(doc, NULL, &r, &err);
 *   bdf_cairo *bc = bdf_cairo_new(r);
 *   cairo_scale(cr, zoom, zoom);            // a unit of the page is a point
 *   cairo_set_source_rgb(cr, 1, 1, 1);      // the paper is the caller's
 *   cairo_paint(cr);
 *   bdf_cairo_draw_page(bc, view, page, cr, &err);
 *
 * Text is drawn as the outlines of its glyphs, laid out by libbdf (see
 * bdf_fonts for the text that names its font). What cairo has no word for
 * is left out: shadows are not drawn, and of the composite operations
 * "copy" only replaces what the shape covers.
 */
#ifndef BDF_CAIRO_H
#define BDF_CAIRO_H

#include <cairo.h>

#include "bdf/bdf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A renderer drawing through cairo, and what is kept from one drawing to
 * the next: the cairo surfaces of the images the renderer has decoded. It
 * is freed before the renderer. */
typedef struct bdf_cairo bdf_cairo;

bdf_cairo *bdf_cairo_new(bdf_renderer *renderer); /* NULL when out of memory */
void bdf_cairo_free(bdf_cairo *bc);

/* Draws a page (bdf_play_page) in the user space of cr as it is. */
bdf_status bdf_cairo_draw_page(bdf_cairo *bc, size_t view, size_t page, cairo_t *cr,
                               bdf_error *err);
/* Draws a rectangle (x, y, w, h in units) of a view (bdf_play_region): of a
 * page, of the strips of a scroll view, or of a sheet with its gridlines,
 * with its corner at the origin of the user space of cr. */
bdf_status bdf_cairo_draw_region(bdf_cairo *bc, size_t view, size_t page, const float region[4],
                                 cairo_t *cr, bdf_error *err);
/* Draws an object part (bdf_play_part) in its own coordinates. */
bdf_status bdf_cairo_draw_part(bdf_cairo *bc, const uint8_t hash[16], cairo_t *cr, bdf_error *err);

/* The canvas the two draw through, for a caller that plays by itself. It
 * is valid while bc is and cr is the context it was made for. */
void bdf_cairo_canvas(bdf_cairo *bc, cairo_t *cr, bdf_canvas *out);

#ifdef __cplusplus
}
#endif

#endif /* BDF_CAIRO_H */
