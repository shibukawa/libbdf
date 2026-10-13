#!/bin/bash
# packaging/binaries.sh <platform> [<directory>]: the library built on the
# machine this runs on, as an installed tree and an archive of it in the
# directory (build-binaries at the root without one):
#
#   bdf-<version>-<platform>/
#     include/bdf/         the headers
#     lib/                 the static library, its CMake package, bdf.pc
#     shared/              the same tree with the shared library
#     licenses/            the license, and those of what the library carries
#
# <platform> is only the name of the tree (linux-x64, macos-universal,
# windows-x64). On macOS the libraries are for arm64 and x86_64.
set -euo pipefail

platform=${1:?usage: binaries.sh <platform> [<directory>]}
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$root/build-binaries}
mkdir -p "$out"
out=$(cd "$out" && pwd)
version=$(sed -n 's/^#define BDF_VERSION_STRING *"\(.*\)"/\1/p' "$root/include/bdf/bdf.h")
name=bdf-$version-$platform
stage=$out/$name
rm -rf "$stage"

args=(-DCMAKE_BUILD_TYPE=Release -DBDF_BUILD_TESTS=OFF)
if [ "$(uname -s)" = Darwin ]; then
  args+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
fi

cmake -S "$root" -B "$out/static" "${args[@]}"
cmake --build "$out/static" --config Release
cmake --install "$out/static" --config Release --prefix "$stage"

cmake -S "$root" -B "$out/shared" "${args[@]}" -DBUILD_SHARED_LIBS=ON
cmake --build "$out/shared" --config Release
cmake --install "$out/shared" --config Release --prefix "$stage/shared"

mkdir -p "$stage/licenses"
cp "$root/LICENSE" "$stage/licenses/LICENSE"
for notice in brotli go-math go-image go-text unicode; do
  cp "$root/third_party/$notice/LICENSE" "$stage/licenses/LICENSE.$notice"
done

cd "$out"
case "$platform" in
windows-*)
  cmake -E tar cf "$name.zip" --format=zip "$name"
  echo "$out/$name.zip"
  ;;
*)
  cmake -E tar czf "$name.tar.gz" "$name"
  echo "$out/$name.tar.gz"
  ;;
esac
