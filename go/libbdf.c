/*
 * cgo compiles the C files of a package directory only, so this file pulls
 * the library's sources in: the harness always tests the commit it sits in,
 * needs no CMake build first, and `go test -asan` instruments the C code
 * along with the Go code. The order is that of tools/amalgamate.py.
 */
#include "../src/reader.c"
#include "../src/object.c"
#include "../src/container.c"
#include "../src/inflate.c"
#include "../src/json.c"
#include "../src/manifest.c"
#include "../src/doc.c"
#include "../src/version.c"
