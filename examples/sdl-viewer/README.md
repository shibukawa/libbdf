# sdl-viewer

A viewer with SDL3 and libbdf's own rasterizer: the same pixels as the reference's renderer, on every platform SDL runs on, with no 2D library underneath.

```bash
cmake -B build && cmake --build build           # SDL3 found by CMake (brew install sdl3, vcpkg, ...)
./build/sdl-viewer ../../samples/two-pages.bdf
./build/sdl-viewer doc.bdf -view 1 -page 3 -font /path/to/font.ttf
```

| Keys | |
|---|---|
| → ← Page Down Page Up Space | next and previous page (in a sheet or a scroll view: down and up) |
| Home End | first and last page |
| ↑ ↓, the wheel, dragging | move |
| + − (Ctrl or ⌘ and the wheel) | zoom |
| 0 | fit the page to the window |
| V, Shift+V | next and previous view |
| Q, Esc | quit |

Only what the window shows is drawn, at the size it is shown: `bdf_render_region` takes a rectangle of the view in its units and the pixels to draw it into. A page at any zoom and a sheet of any size cost no more than a window of pixels. Pages of fixed and flow views, the strips of a scroll view stacked, and sheets are all shown so.

Without a screen, `-screenshot out.png` writes what the window would show and quits (with `SDL_VIDEODRIVER=dummy`).
