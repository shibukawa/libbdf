/*
 * libbdf through QPainter: the pages of a BDF document drawn with Qt's own
 * paths, gradients, images and composition modes (and so on whatever a
 * QPainter draws on: a QImage, a QWidget, a QPdfWriter, a QQuickPaintedItem).
 *
 *   bdf_renderer *r;
 *   bdf_renderer_new(doc, NULL, &r, &err);
 *   bdf_qt *bq = bdf_qt_new(r);
 *   QPainter p(&image);
 *   p.fillRect(image.rect(), Qt::white);    // the paper is the caller's
 *   p.scale(zoom, zoom);                    // a unit of the page is a point
 *   bdf_qt_draw_page(bq, view, page, &p, &err);
 *
 * Qt 6, or Qt 5.15; QtGui is all it needs. Text is drawn as the outlines of
 * its glyphs, laid out by libbdf (see bdf_fonts for the text that names its
 * font). What QPainter has no word for is left out: shadows are not drawn,
 * and the four composite operations of hue, saturation, colour and
 * luminosity draw as "source-over".
 */
#ifndef BDF_QT_H
#define BDF_QT_H

#include <QPainter>

#include "bdf/bdf.h"

/* A renderer drawing through QPainter, and what is kept from one drawing to
 * the next: the QImages of the images the renderer has decoded. It is freed
 * before the renderer. */
struct bdf_qt;

bdf_qt *bdf_qt_new(bdf_renderer *renderer); /* nullptr when out of memory */
void bdf_qt_free(bdf_qt *bq);

/* Draws a page (bdf_play_page) in the coordinates of the painter as they
 * are. The painter is left as it was. */
bdf_status bdf_qt_draw_page(bdf_qt *bq, size_t view, size_t page, QPainter *painter,
                            bdf_error *err);
/* Draws an object part (bdf_play_part): a tile of a sheet. */
bdf_status bdf_qt_draw_part(bdf_qt *bq, const uint8_t hash[16], QPainter *painter, bdf_error *err);

/* The canvas the two draw through, for a caller that plays by itself. It is
 * valid while bq is and painter is the one it was made for. */
void bdf_qt_canvas(bdf_qt *bq, QPainter *painter, bdf_canvas *out);

#endif /* BDF_QT_H */
