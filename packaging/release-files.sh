#!/bin/bash
# packaging/release-files.sh <directory>: checks what the release workflow
# built before it is attached to a release. The directory holds the
# artifacts of a run, a directory an artifact; every file of a release has
# to be there, of this version, with what it is for inside, and nothing
# else. The paths of the files are printed, one a line.
set -euo pipefail

dir=${1:?usage: release-files.sh <directory>}
root=$(cd "$(dirname "$0")/.." && pwd)
dir=$(cd "$dir" && pwd)
version=$(sed -n 's/^#define BDF_VERSION_STRING *"\(.*\)"/\1/p' "$root/include/bdf/bdf.h")

want=(
  "single-header/bdf.h"
  "binaries-linux-x64/bdf-$version-linux-x64.tar.gz"
  "binaries-macos-universal/bdf-$version-macos-universal.tar.gz"
  "binaries-windows-x64/bdf-$version-windows-x64.zip"
  "xcframework/bdf.xcframework.zip"
  "xcframework/bdf.xcframework.zip.checksum"
  "nuget/LibBdf.$version.nupkg"
  "nuget/LibBdf.SkiaSharp.$version.nupkg"
)

bad=0
say() {
  echo "release-files: $*" >&2
  bad=1
}

for f in "${want[@]}"; do
  [ -s "$dir/$f" ] || say "missing: $f"
done
while IFS= read -r f; do
  rel=${f#"$dir"/}
  known=0
  for w in "${want[@]}"; do
    if [ "$rel" = "$w" ]; then known=1; fi
  done
  [ $known = 1 ] || say "not a file of a release: $rel"
done < <(find "$dir" -type f | sort)
[ $bad = 0 ] || exit 1

# What is inside. (The lists are read whole before they are searched: a
# search that stops at the first match would end the listing with an error.)
has() { # has <what the archive is> <its listing> <a path in it>
  case "$2" in
  *"$3"*) ;;
  *) say "$1 has no $3" ;;
  esac
}
grep -q "^#define BDF_VERSION_STRING *\"$version\"" "$dir/single-header/bdf.h" || say "the single header is not version $version"
grep -q "BDF_IMPLEMENTATION" "$dir/single-header/bdf.h" || say "the single header has no implementation"

list=$(tar tzf "$dir/binaries-linux-x64/bdf-$version-linux-x64.tar.gz")
for p in include/bdf/bdf.h lib/libbdf.a lib/pkgconfig/bdf.pc shared/lib/libbdf.so licenses/LICENSE; do
  has "the Linux archive" "$list" "bdf-$version-linux-x64/$p"
done
list=$(tar tzf "$dir/binaries-macos-universal/bdf-$version-macos-universal.tar.gz")
for p in include/bdf/bdf.h lib/libbdf.a shared/lib/libbdf.dylib licenses/LICENSE; do
  has "the macOS archive" "$list" "bdf-$version-macos-universal/$p"
done
list=$(unzip -Z1 "$dir/binaries-windows-x64/bdf-$version-windows-x64.zip")
for p in include/bdf/bdf.h lib/bdf.lib shared/bin/bdf.dll licenses/LICENSE; do
  has "the Windows archive" "$list" "bdf-$version-windows-x64/$p"
done
list=$(unzip -Z1 "$dir/xcframework/bdf.xcframework.zip")
has "the xcframework" "$list" "bdf.xcframework/Info.plist"
[ "$(wc -c < "$dir/xcframework/bdf.xcframework.zip.checksum")" -ge 64 ] || say "the checksum of the xcframework is short"
list=$(unzip -Z1 "$dir/nuget/LibBdf.$version.nupkg")
for p in lib/net8.0/LibBdf.dll runtimes/linux-x64/native/libbdf.so runtimes/osx-arm64/native/libbdf.dylib \
  runtimes/osx-x64/native/libbdf.dylib runtimes/win-x64/native/bdf.dll; do
  has "the NuGet package" "$list" "$p"
done
list=$(unzip -Z1 "$dir/nuget/LibBdf.SkiaSharp.$version.nupkg")
has "the NuGet package of the adapter" "$list" "lib/net8.0/LibBdf.SkiaSharp.dll"
[ $bad = 0 ] || exit 1

for f in "${want[@]}"; do
  echo "$dir/$f"
done
