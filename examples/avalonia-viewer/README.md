# avalonia-viewer

A viewer of BDF documents with [Avalonia](https://avaloniaui.net/), on Windows, macOS and Linux. Avalonia draws its windows with Skia, so the pages are played by libbdf through LibBdf.SkiaSharp on the very `SKCanvas` the window is drawn on: vectors at the resolution of the screen, shadows included.

```bash
cmake -S ../.. -B build -DBUILD_SHARED_LIBS=ON -DBDF_BUILD_TESTS=OFF && cmake --build build
DYLD_LIBRARY_PATH=$PWD/build dotnet run -- ../../samples/two-pages.bdf        # LD_LIBRARY_PATH on Linux, PATH on Windows
DYLD_LIBRARY_PATH=$PWD/build dotnet run -- doc.bdf --snapshot page.png --page 2   # a page, without a screen
```

Ctrl+O opens a document; ← → turn the pages; Ctrl+−, Ctrl+0 and Ctrl++ zoom; the list chooses the view. An encrypted document asks for its password.

- `PageView` is the control: `Render` hands Avalonia a custom draw operation, which leases Avalonia's `SKCanvas` (`ISkiaSharpApiLeaseFeature`) and calls `BdfSkiaRenderer.DrawPage` on it. A renderer is for one thread at a time, and the operation runs on Avalonia's render thread, so it draws under a lock.
- `--snapshot` draws the same control on Avalonia's headless platform with Skia (`RenderTargetBitmap`): what CI runs.
- LibBdf.SkiaSharp uses SkiaSharp 3.119, the version of Avalonia 12 and of SkiaSharp.Views for .NET MAUI.
- The project references the projects of `../../dotnet`; a project of your own takes the NuGet packages `LibBdf` and `LibBdf.SkiaSharp`, which carry the native library of each platform.
