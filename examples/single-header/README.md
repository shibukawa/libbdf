# single-header

libbdf as one header: `bdf2txt` prints the text of a document, view by view, and there is no library to build or link but the C library and its math library.

```bash
python3 ../../tools/amalgamate.py -o bdf/bdf.h     # or take bdf.h from a release
cc -std=c11 -O2 -I. bdf2txt.c -lm -o bdf2txt
./bdf2txt doc.bdf
```

or with CMake, which writes the header from this checkout:

```bash
cmake -B build && cmake --build build
./build/bdf2txt doc.bdf
```

One C file of a program defines `BDF_IMPLEMENTATION` before it includes the header, and gets the library in it; `BDF_STATIC` keeps the library's functions out of the program's link namespace. The other files include the header for the declarations only. The header carries no Brotli decoder, so WOFF2 fonts are left out unless `BDF_HAVE_BROTLI` is defined and libbrotlidec is linked. That matters for drawing, not for text.

The document is read into memory and opened in place with `bdf_doc_open_memory`; `bdf_doc_view_text` and `bdf_text_index_plain` give its text.
