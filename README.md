# libbdf

A C reader for [BDF](https://github.com/shibukawa/bdf) (Browser-specific Document Format) documents, for native applications: a core that owns the format, a software rasterizer, and thin adapters that draw through Skia, CoreGraphics, QPainter and cairo.

What exists today is the core's reading side:

- `include/bdf/bdf.h` — the public C ABI (opaque handles, length-tagged strings, nothing global; see the header's preamble for the rules)
- the single-file and split containers, the manifest (its own JSON reader, the checks of the reference), parts with their deflate-raw decoder, the Object part decoder with its instruction iterator, and path collections — each ported from the reference implementation check for check, with no dependency beyond libc
- WOFF2 fonts unpacked to sfnt (`bdf_woff2_decode`, `bdf_font_sfnt`), byte for byte what the reference's `font/woff2` makes of them, over the Brotli decoder vendored under `third_party/brotli` (MIT; the one dependency, and optional)
- text: the runs of an object and the objects it draws (`bdf_text_extract`, `bdf_doc_text`) with the reference's order, ordinals, separators and limits, the LINK areas and what their urls lead to (`bdf_link_parse`), text index and cue index parts, the text of a view (`bdf_doc_view_text`) and of a document for a search index (`bdf_doc_search_text`)
- encrypted documents (`bdf_doc_unlock` with the password, `bdf_doc_unlock_ecdh` with a P-256 private key) and documents that come in segments sealed for one request (`bdf_doc_add_segment`), with the primitives they need written here: SHA-256, HMAC, PBKDF2, HKDF, AES-256 with key unwrap and GCM, ECDH over P-256, and form C of passwords
- unit tests, libFuzzer targets, a differential test harness in Go that runs every document, part, object, font and text of the reference's test documents through both readers, and a runner for the reference's conformance suite

Planned, in order: the rasterizer ported from `raster/imagebdf`, the adapters. Of the text layer, the structure for accessibility (headings, lists, tables, figures) and a search that normalises (NFKC, case, kana) are still to come; the second can be done today by the host's own normaliser over `bdf_doc_view_text`. [docs/design.md](docs/design.md) (Japanese) holds the decisions behind the layout.

## Relation to bdf

- The format is specified by `docs/spec.md` of the bdf repository: wire format version 1, opset 1.
- The Go implementation there is the reference. libbdf is checked against it, not the other way round: `go/` holds a cgo harness that decodes the same bytes with both and compares the result, and the testdata of the pinned bdf module (`go/go.mod`) are its inputs. `bdf conformance` writes a suite of inputs and expected outcomes that `test_conformance` runs without Go.
- Dependencies point one way. libbdf depends on bdf; bdf does not know libbdf exists.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

CMake 3.21 or later and a C11 compiler (gcc, clang, MSVC). Options:

| Option | Default | Meaning |
|---|---|---|
| `BUILD_SHARED_LIBS` | OFF | shared instead of static |
| `BDF_BUILD_TESTS` | ON when top level | the unit tests |
| `BDF_BUILD_FUZZ` | OFF | the libFuzzer harnesses (clang) |
| `BDF_SANITIZE` | empty | e.g. `address,undefined` |
| `BDF_WERROR` | OFF | warnings as errors (CI turns it on) |
| `BDF_WITH_BROTLI` | ON | decode WOFF2 fonts with the vendored Brotli decoder; OFF makes `bdf_woff2_decode` report `BDF_ERR_UNSUPPORTED` |
| `BDF_CONFORMANCE_DIR` | empty | a suite written by `bdf conformance`, run by `ctest` |

Two limits can be lowered when compiling the library (`-DBDF_MAX_KDF_ITERATIONS=...`, `-DBDF_MAX_REUSED_INSTRUCTIONS=...`): the iterations of the key derivation an encrypted document may ask for (ten million, the format's limit: seconds on a desktop, far more on a small processor), and the instructions read again from objects drawn many times.

Consumers find it with `find_package(bdf CONFIG)` and link `bdf::bdf`, with `pkg-config bdf`, or with `FetchContent` on this repository.

### The single header

`tools/amalgamate.py` joins the sources into one `bdf.h` (the `bdf_single_header` target writes it to `build/single/bdf/bdf.h`; CI publishes it as an artifact). Include it anywhere for the declarations; define `BDF_IMPLEMENTATION` in exactly one C translation unit before including it to get the library, and `BDF_STATIC` as well to keep it out of the link namespace. The implementation is C11, so a C++ project compiles it in a `.c` file of its own. The single header carries no Brotli decoder: define `BDF_HAVE_BROTLI` and link libbrotlidec (or compile `third_party/brotli` alongside) to decode WOFF2 fonts; without it they are `BDF_ERR_UNSUPPORTED`. The unit tests run against the single header too, with the library's warnings as errors.

## Testing

- `ctest` — unit tests of the decoders (`tests/`), against the library and against the single header, and the replay of `fuzz/regress/<target>/`. The vectors that take a writer or a second implementation to make (encrypted documents, the answers of Go's `crypto` packages and of `x/text`) are written by `go run -C go ./genvectors tests`; the tables of form C (`src/unicode_nfc.h`) by `go run -C go ./genunicode src/unicode_nfc.h`.
- `cd go && go test ./...` — the differential tests. They need Go 1.27 and a C compiler; the C sources are compiled into the package through symlinks (`go/*.c` → `src/`, `third_party/brotli/`), so no CMake build is needed first and Go's build cache follows edits to the sources. On Linux, `go test -asan ./...` runs the same with AddressSanitizer. Positions of text are compared to the bit on amd64; on arm64 Go fuses multiply-adds and libbdf does not, so there they are compared within a tolerance (`GOARCH=amd64 CGO_ENABLED=1 go test` runs the exact comparison on an Apple silicon Mac, through Rosetta). `BDF_TESTDATA=/path/to/bdf/testdata` points the tests at a checkout instead of the pinned module.
- The conformance suite: `bdf conformance -testdata testdata <dir>` in a checkout of bdf (or the `bdf-conformance` artifact of its CI and releases) writes it; `cmake -DBDF_CONFORMANCE_DIR=<dir>` adds it to `ctest`, or run `./build/tests/test_conformance <dir>`.
- Fuzzing is run on demand, not in CI (the sanitizer job replays the tests instead). libFuzzer: `cmake -B build -DBDF_BUILD_FUZZ=ON -DBDF_SANITIZE=address,undefined && cmake --build build && ./build/fuzz/doc_fuzz fuzz/corpus/doc -max_total_time=600` (targets: `object`, `inflate`, `manifest`, `doc`, `woff2`, `text`, `unlock`; `LIBBDF_UNLOCK_SEEDS=$PWD/fuzz/corpus/unlock go test -C go -run TestWriteUnlockSeeds .` writes the seeds of the last, documents sealed for its key). Go: `cd go && go test -run '^$' -fuzz '^FuzzDoc$' -fuzztime 10m` (`FuzzObject`, `FuzzInflate`, `FuzzDoc`, `FuzzWOFF2`, `FuzzText`, `FuzzIndexPart`, `FuzzUnlock`, `FuzzSealed`, `FuzzNFC`, `FuzzGCM`, `FuzzP256`). Add `-fuzzminimizetime 20x` for the targets that take whole documents: Go minimises every input that reaches new code for up to a minute, and a run that looks stalled (0 execs/sec) is doing that. `LIBBDF_FUZZ_SLOW_DIR=<dir>` makes `FuzzUnlock` and `FuzzSealed` write there any input that takes more than two seconds. The `fuzz` job of the CI workflow does all of it when dispatched by hand with a number of minutes. What a fuzzer finds is committed by hand to `fuzz/regress/<target>/` (replayed by `ctest`) or `go/testdata/fuzz/` (replayed by `go test`); `fuzz/corpus/` is the working corpus and stays local.

## License

MIT, the same as bdf. The vendored Brotli decoder under `third_party/brotli` is MIT as well (see its LICENSE and README).
