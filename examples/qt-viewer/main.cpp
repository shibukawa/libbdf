/*
 * A viewer of BDF documents with Qt Widgets, drawing through QPainter:
 * libbdf runs the instructions of a page, and the Qt adapter hands what
 * they draw to QPainter (QPainterPath, gradients, QImage, composition
 * modes). The same call draws on a QImage, a QPdfWriter or a printer.
 *
 *   qt-viewer doc.bdf [--password P] [--font FILE]...
 *
 * Keys: → ← (or Page Down, Page Up, Space) next and previous page, Home and
 * End, + − zoom, 0 fit, V next view, Ctrl+S save the page as a PDF file.
 */
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
#include <QStatusBar>
#include <QWidget>

#include <algorithm>
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

bool hasPages(const bdf_view &v) { return v.npages > 0 && v.kind_code != BDF_VIEW_SHEET; }

class PageView : public QWidget {
public:
  explicit PageView(Document &d, QStatusBar *status) : d_(d), status_(status) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    view_ = bdf_doc_view_count(d_.doc.doc) - 1;
    nextView(1); // the first view with pages
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

  bool pageSize(QSizeF &size) const {
    bdf_page page;
    if (!hasPages(info_) || bdf_doc_page(d_.doc.doc, view_, page_, &page) != BDF_OK ||
        page.w <= 0 || page.h <= 0) {
      return false;
    }
    size = QSizeF(page.w, page.h);
    return true;
  }

  // The page as a PDF file: QPainter draws the same paths on it, as vectors.
  void savePdf(const QString &file) {
    QSizeF size;
    if (!pageSize(size)) {
      return;
    }
    QPdfWriter pdf(file);
    pdf.setPageSize(QPageSize(size, QPageSize::Point, QString(), QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setResolution(72); // a unit of the page is a point
    QPainter p(&pdf);
    drawPage(p);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x55, 0x58, 0x5e));
    QSizeF size;
    if (!pageSize(size)) {
      return;
    }
    double zoom = zoom_ > 0 ? zoom_ : fitZoom(size);
    QSizeF shown = size * zoom;
    p.translate(std::max(12.0, (width() - shown.width()) / 2),
                std::max(12.0, (height() - shown.height()) / 2));
    p.scale(zoom, zoom); // a unit of a page is a point
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF paper(QPointF(0, 0), size);
    p.fillRect(paper, Qt::white); // the paper is the caller's
    p.setClipRect(paper);
    drawPage(p);
  }

  void keyPressEvent(QKeyEvent *e) override {
    QSizeF size;
    switch (e->key()) {
    case Qt::Key_Right:
    case Qt::Key_PageDown:
    case Qt::Key_Space:
      show(view_, page_ + 1);
      break;
    case Qt::Key_Left:
    case Qt::Key_PageUp:
      if (page_ > 0) {
        show(view_, page_ - 1);
      }
      break;
    case Qt::Key_Home:
      show(view_, 0);
      break;
    case Qt::Key_End:
      show(view_, info_.npages ? info_.npages - 1 : 0);
      break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
    case Qt::Key_Minus:
      if (zoom_ <= 0 && pageSize(size)) {
        zoom_ = fitZoom(size);
      }
      zoom_ *= e->key() == Qt::Key_Minus ? 0.8 : 1.25;
      update();
      break;
    case Qt::Key_0:
      zoom_ = 0;
      update();
      break;
    case Qt::Key_V:
      nextView(e->modifiers() & Qt::ShiftModifier ? -1 : 1);
      break;
    default:
      QWidget::keyPressEvent(e);
    }
  }

private:
  double fitZoom(const QSizeF &size) const {
    return std::min((width() - 24) / size.width(), (height() - 24) / size.height());
  }

  void show(size_t view, size_t page) {
    bdf_view info;
    if (bdf_doc_view(d_.doc.doc, view, &info) != BDF_OK || page >= info.npages) {
      return;
    }
    view_ = view;
    page_ = page;
    info_ = info;
    status_->showMessage(QString("%1 — %2 / %3")
                             .arg(QString::fromUtf8(info.title, int(info.title_len)))
                             .arg(page + 1)
                             .arg(info.npages));
    update();
  }

  void nextView(long step) {
    long n = long(bdf_doc_view_count(d_.doc.doc));
    for (long k = 1; k <= n; k++) {
      size_t i = size_t(((long(view_) + step * k) % n + n) % n);
      bdf_view info;
      if (bdf_doc_view(d_.doc.doc, i, &info) == BDF_OK && hasPages(info)) {
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
  QCommandLineOption pdfOption("pdf", "write the first page to a PDF file, and quit", "FILE");
  args.addOption(passwordOption);
  args.addOption(fontOption);
  args.addOption(pdfOption);
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
  window.show();
  view->setFocus();
  int rc = app.exec();
  for (size_t i = 0; i < bdf_renderer_warning_count(d.renderer); i++) {
    std::fprintf(stderr, "warning: %s\n", bdf_renderer_warning(d.renderer, i));
  }
  return rc;
}
