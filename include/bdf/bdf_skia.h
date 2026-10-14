/*
 * libbdf through Skia: the pages of a BDF document drawn on an SkCanvas,
 * with Skia's own paths, shaders, images, layers and blend modes (and so on
 * whatever an SkCanvas draws on: a raster surface, a GPU surface, a PDF or
 * SVG document, a picture). Skia is what Chrome draws a Canvas 2D with, so
 * this is the adapter closest to the viewer.
 *
 *   bdf_renderer *r;
 *   bdf_renderer_new(doc, NULL, &r, &err);
 *   bdf_skia *bs = bdf_skia_new(r);
 *   canvas->clear(SK_ColorWHITE);           // the paper is the caller's
 *   canvas->scale(zoom, zoom);              // a unit of the page is a point
 *   bdf_skia_draw_page(bs, view, page, canvas, &err);
 *
 * The headers of Skia are included as Skia's own are ("include/core/..."),
 * with the root of Skia on the include path. Written against milestone 124;
 * Skia changes its API from one milestone to the next, and a newer one may
 * need a line changed here. Text is drawn as the outlines of its glyphs,
 * laid out by libbdf (see bdf_fonts for the text that names its font).
 */
#ifndef BDF_SKIA_H
#define BDF_SKIA_H

#include "include/core/SkCanvas.h"

#include "bdf/bdf.h"

/* A renderer drawing through Skia, and what is kept from one drawing to the
 * next: the SkImages of the images the renderer has decoded. It is freed
 * before the renderer. */
struct bdf_skia;

bdf_skia *bdf_skia_new(bdf_renderer *renderer); /* nullptr when out of memory */
void bdf_skia_free(bdf_skia *bs);

/* Draws a page (bdf_play_page) in the coordinates of the canvas as they
 * are. The canvas is left as it was. */
bdf_status bdf_skia_draw_page(bdf_skia *bs, size_t view, size_t page, SkCanvas *canvas,
                              bdf_error *err);
/* Draws a rectangle (x, y, w, h in units) of a view (bdf_play_region): of a
 * page, of the strips of a scroll view, or of a sheet with its gridlines,
 * with its corner at the origin of the canvas's coordinates. */
bdf_status bdf_skia_draw_region(bdf_skia *bs, size_t view, size_t page, const float region[4],
                                SkCanvas *canvas, bdf_error *err);
/* Draws an object part (bdf_play_part) in its own coordinates. */
bdf_status bdf_skia_draw_part(bdf_skia *bs, const uint8_t hash[16], SkCanvas *canvas,
                              bdf_error *err);

/* The canvas the two draw through, for a caller that plays by itself. It is
 * valid while bs is and canvas is the one it was made for. */
void bdf_skia_canvas(bdf_skia *bs, SkCanvas *canvas, bdf_canvas *out);

#endif /* BDF_SKIA_H */
