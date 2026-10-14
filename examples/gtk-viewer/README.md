# gtk-viewer

A viewer with GTK 4, drawing through libbdf's cairo adapter: libbdf runs the instructions of a page, and `bdf_cairo_draw_page` hands what they draw to cairo, which GTK puts on the screen. Pages stay sharp at any zoom and on any display, since cairo draws them at the resolution of the window.

```bash
sudo apt install libgtk-4-dev         # or brew install gtk4
cmake -B build && cmake --build build
./build/gtk-viewer ../../samples/two-pages.bdf
```

Keys: → ← (Page Down, Page Up, Space) next and previous page, Home End, + − zoom, 0 fit, V next view, Q quit.

The CMake project builds libbdf from this checkout with `BDF_CAIRO=ON` and links `bdf::cairo`; with an installed libbdf it needs one built with the cairo adapter. The paper is the application's: the example fills the page white and clips to it before it draws. The cairo adapter draws no shadows (cairo has none); the rest is drawn as the rasterizer draws it, by cairo's own anti-aliasing.
