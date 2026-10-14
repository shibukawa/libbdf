# Packaging

How libbdf reaches the build systems and package managers it is used from.
The CI builds a consumer through each of them (`packaging` in
`.github/workflows/ci.yml`).

| For | What | Where |
|---|---|---|
| any C or C++ build | the single header `bdf.h` | an asset of every release; `cmake --build build --target bdf_single_header` |
| CMake | `find_package(bdf CONFIG)`, `bdf::bdf`; or `FetchContent` on this repository | installed by `cmake --install` |
| pkg-config (GTK, autotools) | `bdf.pc`, `bdf-cairo.pc` | installed by `cmake --install` |
| Meson | `packaging/meson/bdf.wrap` → `dependency('bdf')` | copy it to `subprojects/` |
| Conan 2 | `conanfile.py` at the root | `conan create .` |
| vcpkg | `packaging/vcpkg/bdf`, an overlay port | `vcpkg install bdf --overlay-ports=packaging/vcpkg` |
| Swift, Xcode | `Package.swift` at the root: products `BDF` (Swift), `CBDF`, `CBDFCoreGraphics` | add the repository as a package dependency |
| Xcode without Swift packages | `bdf.xcframework` | an asset of every release (`packaging/xcframework.sh`) |
| .NET (MAUI, SkiaSharp) | the NuGet packages `LibBdf` and `LibBdf.SkiaSharp` | `dotnet/`; packed by the release workflow |
| Android (Kotlin, Java) | `io.github.shibukawa:libbdf`, an AAR for arm64-v8a, armeabi-v7a and x86_64 | `android/`; the AAR and a Maven repository of it are assets of every release |

`packaging/consumer/main.c` is the program each of them builds.

## At a release

The release workflow (`.github/workflows/release.yml`) runs on a tag
`v<version>` and makes a draft release with the single header, the binaries
of each platform, the xcframework and the NuGet packages. Before it attaches
them, `packaging/release-files.sh` checks that each is there, of the version
of the header, with what it is for inside (the libraries of every platform
in the NuGet package, those of every ABI in the AAR, say). The workflow also runs without a tag, when it is
dispatched by hand and for a pull request that changes it or its scripts:
then it builds and checks the same and makes no release, so that what a tag
will do has been done before. Two things are still by hand afterwards:

- `packaging/vcpkg/bdf/portfile.cmake`: the `SHA512` of the release's
  source archive (vcpkg prints it), and `vcpkg.json`'s version;
- `packaging/meson/bdf.wrap`: its `revision`.

A tag also uploads the Android library to Maven Central, when the
repository has the secrets for it (android/README.md lists them): it
waits there to be published by hand. Neither a registry of vcpkg,
ConanCenter, the WrapDB nor nuget.org has libbdf yet: these files are
what would be sent there.
