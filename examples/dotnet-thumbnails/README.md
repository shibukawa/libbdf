# dotnet-thumbnails

Thumbnails of the pages of a document with libbdf for .NET: each page drawn on an `SKCanvas` by `BdfSkiaRenderer` (LibBdf.SkiaSharp, the way .NET MAUI, Avalonia and Uno draw), or with `--raster` by libbdf's own rasterizer (`BdfRenderer.Render`), and written as PNG files.

```bash
cmake -S ../.. -B build -DBUILD_SHARED_LIBS=ON -DBDF_BUILD_TESTS=OFF && cmake --build build
LD_LIBRARY_PATH=$PWD/build dotnet run -- ../../samples/two-pages.bdf thumbnails
LD_LIBRARY_PATH=$PWD/build dotnet run -- doc.bdf thumbnails --width 480 --raster
```

(`DYLD_LIBRARY_PATH` on macOS, `PATH` on Windows.) The project references the two projects of `../../dotnet`, which find the native library as the system finds libraries. The NuGet packages of a release carry it for each platform under `runtimes/`: a project of your own takes `LibBdf` and `LibBdf.SkiaSharp` from them and needs nothing more.
