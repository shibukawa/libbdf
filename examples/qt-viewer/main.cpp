/*
 * A viewer of BDF documents with Qt Widgets, drawing through QPainter:
 * libbdf runs the instructions of a page, and the Qt adapter hands what
 * they draw to QPainter (QPainterPath, gradients, QImage, composition
 * modes). The same call draws on a QImage, a QPdfWriter or a printer. A
 * sheet (a spreadsheet, a table of Parquet) is drawn only where it is in
 * sight, with its gridlines.
 *
 *   qt-viewer doc.bdf [--password P] [--font FILE]... [--pdf FILE | --png FILE]
 *
 * Keys: → ← (or Page Down, Page Up, Space) next and previous page, Home and
 * End, + − zoom, 0 fit, V next view, Ctrl+S save the page as a PDF file. On
 * a sheet the arrows and the page keys scroll.
 */
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QScrollBar>
#include <QStatusBar>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bdf/bdf_qt.h"
#include "open_doc.h"

namespace {

// The document, its renderer and the Qt adapter over it, freed in the
// order they depend on each other.
struct Document {
  example_doc doc{};
  bdf_fonts *fonts = nullptr;
  bdf_renderer *renderer = nullptr;
  bdf_qt *qt = nullptr;

  ~Document() {
    bdf_qt_free(qt); // before the renderer
    bdf_renderer_free(renderer);
    bdf_fonts_free(fonts);
    example_doc_close(&doc);
  }
};

bool isSheet(const bdf_view &v) { return v.kind_code == BDF_VIEW_SHEET; }
bool hasPages(const bdf_view &v) { return v.npages > 0 && !isSheet(v); }

class PageView : public QAbstractScrollArea {
public:
  explicit PageView(Document &d, QStatusBar *status) : d_(d), status_(status) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    view_ = bdf_doc_view_count(d_.doc.doc) - 1;
    nextView(1); // the first view with pages, or a sheet
  }

  // Draws the current page on a painter whose unit is a point.
  bool drawPage(QPainter &p) {
    bdf_error err;
    if (bdf_qt_draw_page(d_.qt, view_, page_, &p, &err) != BDF_OK) {
      std::fprintf(stderr, "page %zu: %s\n", page_ + 1, err.message);
      return false;
    }
    return true;
  }

  // Draws a rectangle of the current sheet, with its corner at the origin
  // of the painter.
  bool drawRegion(QPainter &p, const QRectF &region) {
    const float r[4] = {float(region.x()), float(region.y()), float(region.width()),
                        float(region.height())};
    bdf_error err;
    if (bdf_qt_draw_region(d_.qt, view_, 0, r, &p, &err) != BDF_OK) {
      std::fprintf(stderr, "sheet: %s\n", err.message);
      return false;
    }
    return true;
  }

  // The size of the current page, or of the sheet, in its units.
  bool contentSize(QSizeF &size) const {
    bdf_page page;
    if (isSheet(info_)) {
      double w, h;
      bdf_sheet_size(d_.doc.doc, view_, &w, &h);
      size = QSizeF(w, h);
    } else if (hasPages(info_) && bdf_doc_page(d_.doc.doc, view_, page_, &page) == BDF_OK) {
      size = QSizeF(page.w, page.h);
    } else {
      return false;
    }
    return size.width() > 0 && size.height() > 0;
  }

  // The page, or the top left of a sheet, as a PDF file: QPainter draws the
  // same paths on it, as vectors.
  void savePdf(const QString &file) {
    QSizeF size;
    if (!contentSize(size)) {
      return;
    }
    size = size.boundedTo(QSizeF(14400, 14400)); // a page of PDF is 200 inches at most
    QPdfWriter pdf(file);
    pdf.setPageSize(QPageSize(size, QPageSize::Point, QString(), QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setResolution(72); // a unit of the page is a point
    QPainter p(&pdf);
    if (isSheet(info_)) {
      drawRegion(p, QRectF(QPointF(0, 0), size));
    } else {
      drawPage(p);
    }
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), QColor(0x55, 0x58, 0x5e));
    QSizeF size;
    if (!contentSize(size)) {
      return;
    }
    const double zoom = currentZoom(size);
    p.translate(corner(size * zoom));
    p.scale(zoom, zoom); // a unit of a page is a point
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF paper(QPointF(0, 0), size);
    const QRectF seen = p.transform().inverted().mapRect(QRectF(viewport()->rect())) & paper;
    if (seen.isEmpty()) {
      return;
    }
    p.fillRect(seen, Qt::white); // the paper is the caller's
    p.setClipRect(paper);
    if (!isSheet(info_)) {
      drawPage(p);
      return;
    }
    // what is in sight of a sheet, which may be far larger than the window,
    // from a corner on a whole pixel, so that the gridlines are sharp
    const QTransform t = p.deviceTransform();
    const double x = (std::floor(t.m11() * seen.left() + t.dx()) - t.dx()) / t.m11();
    const double y = (std::floor(t.m22() * seen.top() + t.dy()) - t.dy()) / t.m22();
    p.translate(x, y);
    drawRegion(p, QRectF(QPointF(x, y), seen.bottomRight()));
  }

  void resizeEvent(QResizeEvent *e) override {
    QAbstractScrollArea::resizeEvent(e);
    updateScrollBars();
  }

  void scrollContentsBy(int, int) override { viewport()->update(); }

  void keyPressEvent(QKeyEvent *e) override {
    QSizeF size;
    const bool sheet = isSheet(info_);
    switch (e->key()) {
    case Qt::Key_Right:
    case Qt::Key_PageDown:
    case Qt::Key_Space:
      if (sheet) {
        QAbstractScrollArea::keyPressEvent(e);
      } else {
        show(view_, page_ + 1);
      }
      break;
    case Qt::Key_Left:
    case Qt::Key_PageUp:
      if (sheet) {
        QAbstractScrollArea::keyPressEvent(e);
      } else if (page_ > 0) {
        show(view_, page_ - 1);
      }
      break;
    case Qt::Key_Home:
      sheet ? verticalScrollBar()->setValue(0) : show(view_, 0);
      break;
    case Qt::Key_End:
      sheet ? verticalScrollBar()->setValue(verticalScrollBar()->maximum())
            : show(view_, info_.npages ? info_.npages - 1 : 0);
      break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
    case Qt::Key_Minus:
      if (contentSize(size)) {
        zoom_ = currentZoom(size) * (e->key() == Qt::Key_Minus ? 0.8 : 1.25);
        zoom_ = std::clamp(zoom_, 0.05, 32.0);
        updateScrollBars();
        viewport()->update();
      }
      break;
    case Qt::Key_0:
      zoom_ = 0;
      updateScrollBars();
      viewport()->update();
      break;
    case Qt::Key_V:
      nextView(e->modifiers() & Qt::ShiftModifier ? -1 : 1);
      break;
    default:
      QAbstractScrollArea::keyPressEvent(e);
    }
  }

private:
  // Pixels a unit: what was chosen, else a page as a whole in the window,
  // or a sheet as wide as the window (at most twice its size).
  double currentZoom(const QSizeF &size) const {
    if (zoom_ > 0) {
      return zoom_;
    }
    const QSize room = viewport()->size();
    const double across = (room.width() - 24) / size.width();
    if (isSheet(info_)) {
      return std::clamp(across, 0.05, 2.0);
    }
    return std::max(0.05, std::min(across, (room.height() - 24) / size.height()));
  }

  // Where the corner of the page or the sheet is in the window: in the
  // middle when it fits, else where the scroll bars say.
  QPointF corner(const QSizeF &shown) const {
    const QSize room = viewport()->size();
    const double x = shown.width() + 24 <= room.width() ? (room.width() - shown.width()) / 2
                                                        : 12 - horizontalScrollBar()->value();
    const double y = shown.height() + 24 <= room.height() ? (room.height() - shown.height()) / 2
                                                          : 12 - verticalScrollBar()->value();
    return QPointF(x, y);
  }

  void updateScrollBars() {
    QSizeF size;
    QSizeF shown(0, 0);
    if (contentSize(size)) {
      shown = size * currentZoom(size);
    }
    const QSize room = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, int(std::ceil(shown.width() + 24 - room.width()))));
    horizontalScrollBar()->setPageStep(room.width());
    horizontalScrollBar()->setSingleStep(24);
    verticalScrollBar()->setRange(0, std::max(0, int(std::ceil(shown.height() + 24 - room.height()))));
    verticalScrollBar()->setPageStep(room.height());
    verticalScrollBar()->setSingleStep(24);
  }

  void show(size_t view, size_t page) {
    bdf_view info;
    if (bdf_doc_view(d_.doc.doc, view, &info) != BDF_OK ||
        (isSheet(info) ? page != 0 : page >= info.npages)) {
      return;
    }
    view_ = view;
    page_ = page;
    info_ = info;
    const QString title = QString::fromUtf8(info.title, int(info.title_len));
    if (isSheet(info)) {
      double w, h;
      bdf_sheet_size(d_.doc.doc, view, &w, &h);
      status_->showMessage(QString("%1 — a sheet of %2 × %3").arg(title).arg(w).arg(h));
    } else {
      status_->showMessage(QString("%1 — %2 / %3").arg(title).arg(page + 1).arg(info.npages));
    }
    horizontalScrollBar()->setValue(0);
    verticalScrollBar()->setValue(0);
    updateScrollBars();
    viewport()->update();
  }

  void nextView(long step) {
    long n = long(bdf_doc_view_count(d_.doc.doc));
    for (long k = 1; k <= n; k++) {
      size_t i = size_t(((long(view_) + step * k) % n + n) % n);
      bdf_view info;
      if (bdf_doc_view(d_.doc.doc, i, &info) == BDF_OK && (hasPages(info) || isSheet(info))) {
        show(i, 0);
        return;
      }
    }
  }

  Document &d_;
  QStatusBar *status_;
  size_t view_ = 0, page_ = 0;
  bdf_view info_{};
  double zoom_ = 0; // 0: fit the window
};

} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("qt-viewer");
  QCommandLineParser args;
  args.setApplicationDescription("A viewer of BDF documents with Qt and libbdf");
  args.addHelpOption();
  args.addPositionalArgument("file", "a .bdf file, or the directory of the split form");
  QCommandLineOption passwordOption("password", "the password of an encrypted document", "P");
  QCommandLineOption fontOption("font", "a font for text that names its font", "FILE");
  QCommandLineOption pdfOption("pdf", "write the first page (or the top left of a sheet) to a PDF file, and quit", "FILE");
  QCommandLineOption pngOption("png", "write what the window shows to a PNG file, and quit", "FILE");
  args.addOption(passwordOption);
  args.addOption(fontOption);
  args.addOption(pdfOption);
  args.addOption(pngOption);
  args.process(app);
  if (args.positionalArguments().size() != 1) {
    args.showHelp(2);
  }

  Document d;
  const std::string path = args.positionalArguments().first().toStdString();
  const std::string password = args.value(passwordOption).toStdString();
  char msg[512];
  if (example_doc_open(&d.doc, path.c_str(), args.isSet(passwordOption) ? password.c_str() : nullptr,
                       msg, sizeof msg) != 0) {
    std::fprintf(stderr, "%s\n", msg);
    return 1;
  }
  std::vector<std::string> fontFiles;
  for (const QString &f : args.values(fontOption)) {
    fontFiles.push_back(f.toStdString());
  }
  std::vector<const char *> fonts;
  for (const std::string &f : fontFiles) {
    fonts.push_back(f.c_str());
  }
  d.fonts = example_fonts(fonts.data(), fonts.size());
  bdf_render_options options{};
  options.fonts = d.fonts;
  bdf_error err;
  if (bdf_renderer_new(d.doc.doc, &options, &d.renderer, &err) != BDF_OK ||
      !(d.qt = bdf_qt_new(d.renderer))) {
    std::fprintf(stderr, "%s\n", err.message);
    return 1;
  }

  QMainWindow window;
  auto *view = new PageView(d, window.statusBar());
  if (args.isSet(pdfOption)) {
    view->savePdf(args.value(pdfOption));
    delete view;
    return 0;
  }
  window.setCentralWidget(view);
  window.setWindowTitle(args.positionalArguments().first().section('/', -1));
  window.resize(900, 700);
  auto *save = new QAction("Save page as PDF…", &window);
  save->setShortcut(QKeySequence::Save);
  QObject::connect(save, &QAction::triggered, [&] {
    QString file = QFileDialog::getSaveFileName(&window, "Save page as PDF", "page.pdf", "PDF (*.pdf)");
    if (!file.isEmpty()) {
      view->savePdf(file);
    }
  });
  window.addAction(save);
  if (args.isSet(pngOption)) {
    // (with QT_QPA_PLATFORM=offscreen, without a screen)
    window.show();
    QApplication::processEvents();
    return window.grab().save(args.value(pngOption)) ? 0 : 1;
  }
  window.show();
  view->setFocus();
  int rc = app.exec();
  for (size_t i = 0; i < bdf_renderer_warning_count(d.renderer); i++) {
    std::fprintf(stderr, "warning: %s\n", bdf_renderer_warning(d.renderer, i));
  }
  return rc;
}
