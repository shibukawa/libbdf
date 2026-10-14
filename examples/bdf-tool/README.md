# bdf-tool

A command-line program over libbdf's C API, with nothing but the library: the document, its views and pages, the renderer, the text and the search.

```bash
cmake -B build && cmake --build build
./build/bdf-tool info ../../samples/two-pages.bdf
./build/bdf-tool render doc.bdf -page 2 -dpi 144 -o page2.png
./build/bdf-tool render sheet.bdf -region 0,0,800,600 -o region.png
./build/bdf-tool text doc.bdf
./build/bdf-tool search doc.bdf "query"
BDF_PASSWORD=secret ./build/bdf-tool info encrypted.bdf
```

| Command | What it calls |
|---|---|
| `info` | `bdf_doc_meta`, `bdf_doc_dc`, `bdf_doc_view`, `bdf_doc_page`, `bdf_doc_view_run`, `bdf_sheet_size` |
| `render` | `bdf_renderer_new` with a `bdf_fonts` set, `bdf_render_page`, or `bdf_render_region` for a sheet or with `-region`; the warnings of the renderer |
| `text` | `bdf_doc_search_text`: the text of every view, page by page |
| `search` | `bdf_doc_view_text`, `bdf_text_search_new`, `bdf_text_search_find`: hits found as the viewer finds them (width, case, kana, accents), with the page each lies on |

The CMake project takes an installed libbdf (`find_package(bdf)`), or else this checkout with `add_subdirectory`. A project of your own can take it with `FetchContent` from the repository the same way.
