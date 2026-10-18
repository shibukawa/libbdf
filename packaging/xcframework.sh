#!/bin/bash
# packaging/xcframework.sh [<directory>]: builds bdf.xcframework in the
# directory (build-xcframework at the root without one): the library and its
# Core Graphics adapter as one static library for macOS, iOS and the iOS
# simulator, with their headers and a module, CBDF, for Swift.
#
# Swift projects are better served by the package (Package.swift), which
# builds the same from the sources; this is for projects that take binaries.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build-xcframework}
mkdir -p "$out"
out=$(cd "$out" && pwd)

# build <name> <cmake arguments>: the two libraries of one platform, as one
build() {
  local name=$1
  shift
  cmake -S "$root" -B "$out/$name" -DCMAKE_BUILD_TYPE=Release -DBDF_BUILD_TESTS=OFF \
    -DBDF_INSTALL=OFF -DBDF_COREGRAPHICS=ON "$@" >"$out/$name.log" 2>&1
  cmake --build "$out/$name" --config Release >>"$out/$name.log" 2>&1
  libtool -static -o "$out/$name/libbdf.xc.a" "$out/$name/libbdf.a" \
    "$out/$name/adapters/libbdf_coregraphics.a" 2>>"$out/$name.log"
  mkdir -p "$out/$name/lib"
  cp "$out/$name/libbdf.xc.a" "$out/$name/lib/libbdf.a"
}

build macos -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
build ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
build ios-simulator -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0

# the headers, and the module in a directory of its own name: module maps at
# the top of the headers of two xcframeworks would collide
headers=$out/headers
rm -rf "$headers"
mkdir -p "$headers/bdf" "$headers/CBDF"
cp "$root/include/bdf/bdf.h" "$root/include/bdf/bdf_coregraphics.h" "$headers/bdf/"
cat >"$headers/CBDF/module.modulemap" <<'MAP'
module CBDF {
    header "../bdf/bdf.h"
    header "../bdf/bdf_coregraphics.h"
    link framework "CoreGraphics"
    link framework "CoreFoundation"
    export *
}
MAP

rm -rf "$out/bdf.xcframework"
xcodebuild -create-xcframework \
  -library "$out/macos/lib/libbdf.a" -headers "$headers" \
  -library "$out/ios/lib/libbdf.a" -headers "$headers" \
  -library "$out/ios-simulator/lib/libbdf.a" -headers "$headers" \
  -output "$out/bdf.xcframework" >>"$out/xcframework.log" 2>&1
mkdir -p "$out/bdf.xcframework/licenses"
cp "$root/third_party/bearssl/LICENSE" "$out/bdf.xcframework/licenses/LICENSE.bearssl"
echo "$out/bdf.xcframework"
