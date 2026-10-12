# libbdf

A C reader for [BDF](https://github.com/shibukawa/bdf) (Browser-specific Document Format) documents, for native applications: a core that owns the format, a software rasterizer, and thin adapters that draw through Skia, CoreGraphics, QPainter and cairo.

What exists today is the core's reading side:

- `include/bdf/bdf.h` — the public C ABI (opaque handles, length-tagged strings, nothing global; see the header's preamble for the rules)
- the single-file and split containers, the manifest (its own JSON reader, the checks of the reference), parts with their deflate-raw decoder, the Object part decoder with its instruction iterator, and path collections — each ported from the reference implementation check for check, with no dependency beyond libc
- unit tests, libFuzzer targets, a differential test harness in Go that runs every document, part and object of the reference's test documents through both readers, and a runner for the reference's conformance suite

Planned, in order: WOFF2 → SFNT, text runs for selection and search, encryption, the rasterizer ported from `raster/imagebdf`, the adapters. [docs/design.md](docs/design.md) (Japanese) holds the decisions behind the layout.

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
| `BDF_CONFORMANCE_DIR` | empty | a suite written by `bdf conformance`, run by `ctest` |

Consumers find it with `find_package(bdf CONFIG)` and link `bdf::bdf`, with `pkg-config bdf`, or with `FetchContent` on this repository.

### The single header

`tools/amalgamate.py` joins the sources into one `bdf.h` (the `bdf_single_header` target writes it to `build/single/bdf/bdf.h`; CI publishes it as an artifact). Include it anywhere for the declarations; define `BDF_IMPLEMENTATION` in exactly one C translation unit before including it to get the library, and `BDF_STATIC` as well to keep it out of the link namespace. The implementation is C11, so a C++ project compiles it in a `.c` file of its own. The unit tests run against the single header too, with the library's warnings as errors.

## Testing

- `ctest` — unit tests of the decoders (`tests/`), against the library and against the single header, and the replay of `fuzz/regress/<target>/`.
- `cd go && go test ./...` — the differential tests. They need Go 1.27 and a C compiler; the C sources are compiled into the package, so no CMake build is needed first. On Linux, `go test -asan ./...` runs the same with AddressSanitizer. `BDF_TESTDATA=/path/to/bdf/testdata` points the tests at a checkout instead of the pinned module.
- The conformance suite: `bdf conformance -testdata testdata <dir>` in a checkout of bdf (or the `bdf-conformance` artifact of its CI and releases) writes it; `cmake -DBDF_CONFORMANCE_DIR=<dir>` adds it to `ctest`, or run `./build/tests/test_conformance <dir>`.
- Fuzzing is run on demand, not in CI (the sanitizer job replays the tests instead). libFuzzer: `cmake -B build -DBDF_BUILD_FUZZ=ON -DBDF_SANITIZE=address,undefined && cmake --build build && ./build/fuzz/doc_fuzz fuzz/corpus/doc -max_total_time=600` (targets: `object`, `inflate`, `manifest`, `doc`). Go: `cd go && go test -run '^$' -fuzz '^FuzzDoc$' -fuzztime 10m` (`FuzzObject`, `FuzzInflate`, `FuzzDoc`). The `fuzz` job of the CI workflow does all of it when dispatched by hand with a number of minutes. What a fuzzer finds is committed by hand to `fuzz/regress/<target>/` (replayed by `ctest`) or `go/testdata/fuzz/` (replayed by `go test`); `fuzz/corpus/` is the working corpus and stays local.

## License

MIT, the same as bdf.
