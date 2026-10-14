# qt-viewer

A viewer with Qt Widgets, drawing through libbdf's QPainter adapter: `bdf_qt_draw_page` hands what a page draws to QPainter (`QPainterPath`, gradients, `QImage`, composition modes). The same call draws on a `QImage`, a `QPdfWriter` or a printer; ⌘S/Ctrl+S writes the page as a PDF file of vectors. A sheet (a spreadsheet, a table of CSV or Parquet) scrolls, and `bdf_qt_draw_region` draws only the rectangle in sight, with its gridlines, from a corner on a whole pixel so that they are sharp.

```bash
sudo apt install qt6-base-dev          # or brew install qt
cmake -B build && cmake --build build   # with Homebrew's Qt: -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
./build/qt-viewer ../../samples/two-pages.bdf
QT_QPA_PLATFORM=offscreen ./build/qt-viewer doc.bdf --pdf page1.pdf   # without a screen
QT_QPA_PLATFORM=offscreen ./build/qt-viewer ../../samples/sheet.bdf --png sheet.png   # the window
```

Keys: → ← (Page Down, Page Up, Space) next and previous page, Home End, + − zoom, 0 fit, V next view. On a sheet the arrows and the page keys scroll, and Home End go to the top and the bottom.

The CMake project builds libbdf from this checkout with `BDF_QT=ON` and links `bdf::qt` (Qt 6, or Qt 5.15). The opening of documents is the C code of `../common`, compiled as C beside the C++.
