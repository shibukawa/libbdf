/*
 * What the adapters' comparison programs share: pages drawn by libbdf's own
 * rasterizer and by an adapter, and how far apart the two pictures are. An
 * adapter draws with another library's anti-aliasing, curves and filters,
 * so the pictures are never the same; they are compared by the mean
 * difference of their bytes, and by how many pixels differ by more than a
 * little.
 */
#ifndef BDF_ADAPTER_COMPARE_H
#define BDF_ADAPTER_COMPARE_H

#include "bdf/bdf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws page of view into premultiplied RGBA pixels of w x h, on white,
 * with sx and sy pixels a unit of the page (bdf_render_page stretches a
 * page to whole pixels, so the two differ a little): 0 when it cannot. */
typedef int (*bdf_compare_draw)(void *ctx, bdf_renderer *r, size_t view, size_t page, double sx,
                                double sy, int w, int h, uint8_t *pixels);

/* What an adapter leaves out, which the pages built in then leave out too. */
enum { BDF_COMPARE_NO_SHADOWS = 1 };

/*
 * The main of a comparison program: who [font.ttf ...] [doc.bdf ...]
 *
 * Without documents it compares the pages built into it (shapes, lines,
 * gradients, images, groups, soft masks, text): the test of an adapter.
 * With documents it compares their pages (the first of each view), with
 * the fonts named before them for the text that names its font. It prints
 * a line a page and returns 1 when a page was further apart than the
 * limits: a mean difference of the bytes above mean_limit (of 255), or
 * more than far_limit of the pixels differing by more than 64 in a
 * channel. With BDF_COMPARE_DUMP set to a directory, the two pictures of
 * every page are written there as PPM files.
 */
int bdf_compare_main(int argc, char **argv, const char *who, bdf_compare_draw draw, void *ctx,
                     unsigned flags, double mean_limit, double far_limit);

#ifdef __cplusplus
}
#endif

#endif
