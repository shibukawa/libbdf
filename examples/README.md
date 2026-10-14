# Examples

Programs that use libbdf the way an application would, each a project of its own that builds against this checkout. Every one opens `../../samples/two-pages.bdf`, and any document the bdf tools write.

| Example | Language | Draws with | Shows |
|---|---|---|---|
| [bdf-tool](bdf-tool/) | C | libbdf's rasterizer | the document, its views and pages; a page as a PNG image; the text; a search |
| [single-header](single-header/) | C | (no drawing) | libbdf as one header: the text of a document, with nothing to build or link |
| [sdl-viewer](sdl-viewer/) | C, SDL3 | libbdf's rasterizer | a window on any platform: pages, scroll views and sheets, zoom, drawing only what the window shows |
| [gtk-viewer](gtk-viewer/) | C, GTK 4 | cairo (`bdf::cairo`) | a GTK application drawing pages through cairo |
| [qt-viewer](qt-viewer/) | C++, Qt 6 | QPainter (`bdf::qt`) | a Qt Widgets application, and a page written as a PDF file through the same adapter |
| [swiftui-viewer](swiftui-viewer/) | Swift | Core Graphics (the `BDF` Swift package) | a macOS app with SwiftUI, an encrypted document asking for its password |
| [dotnet-thumbnails](dotnet-thumbnails/) | C# | SkiaSharp (`LibBdf.SkiaSharp`) | thumbnails of the pages, drawn on an `SKCanvas` or by the rasterizer |
| [android/sample](../android/sample/) | Kotlin | libbdf's rasterizer, into `Bitmap`s | an Android app: a bundled document and any the user opens, pinch to zoom, pages, scroll views and sheets; built with the library in [android/](../android/) |

What they share:

- **Encrypted documents** open with `-password` or `$BDF_PASSWORD`. The library normalises the password (NFKC), so full-width and half-width spellings are one password.
- **Fonts.** A document made from HTML, Markdown or EPUB names its fonts instead of carrying them. libbdf reads no font directories: the application gives it the fonts to draw such text with. Without `-font FILE` the examples add a few fonts the system usually has (Helvetica and Hiragino on macOS, DejaVu and Noto CJK on Linux, Arial and Yu Gothic on Windows), those of them that are there.
- **The split form**: the C examples open a directory with `manifest.json` and `parts/` as well as a `.bdf` file. They read a file where the reader asks (`bdf_source`), not whole.
- [`common/`](common/) has what the C examples share: opening a document (`open_doc.c`) and a PNG writer with no dependency (`png_write.c`).
- **Sheets** are drawn by bdf-tool and sdl-viewer, with `bdf_render_region`. The adapters draw a sheet tile by tile (`bdf_cairo_draw_part`, `bdf_qt_draw_part`); the viewers of this directory that draw through an adapter show the views that have pages.

CI builds them all and runs those that can run without a screen (`examples` in [ci.yml](../.github/workflows/ci.yml)).
