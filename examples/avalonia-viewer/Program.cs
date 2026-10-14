// A viewer of BDF documents with Avalonia, on Windows, macOS and Linux:
// pages are drawn by libbdf through SkiaSharp (LibBdf.SkiaSharp) on the
// canvas Avalonia draws its window with.
//
//   dotnet run -- doc.bdf
//   dotnet run -- doc.bdf --snapshot page.png [--page N]   (a page, without a screen)
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Headless;
using Avalonia.Media.Imaging;
using Avalonia.Themes.Fluent;
using AvaloniaViewer;
using LibBdf;

string? path = args.FirstOrDefault(a => !a.StartsWith("--"));
int snapshot = Array.IndexOf(args, "--snapshot");
if (snapshot >= 0 && snapshot + 1 < args.Length && path != null)
{
    int at = Array.IndexOf(args, "--page");
    int page = at >= 0 && at + 1 < args.Length ? int.Parse(args[at + 1]) - 1 : 0;
    return Snapshot.Write(path, args[snapshot + 1], page);
}
AppBuilder.Configure(() => new App(path)).UsePlatformDetect().StartWithClassicDesktopLifetime(args);
return 0;

namespace AvaloniaViewer
{
    public sealed class App(string? path) : Application
    {
        public override void Initialize() => Styles.Add(new FluentTheme());

        public override void OnFrameworkInitializationCompleted()
        {
            if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            {
                desktop.MainWindow = new MainWindow(path);
            }
            base.OnFrameworkInitializationCompleted();
        }
    }

    /// <summary>
    /// A page drawn by the same control the window shows, on Avalonia's
    /// headless platform with Skia, into a PNG file: to see what the viewer
    /// draws without a screen.
    /// </summary>
    public static class Snapshot
    {
        public static int Write(string path, string output, int page)
        {
            AppBuilder.Configure(() => new App(null)).UseSkia()
                .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
                .SetupWithoutStarting();
            try
            {
                var document = new BdfDocument(File.ReadAllBytes(path));
                if (document.IsLocked)
                {
                    document.Unlock(Environment.GetEnvironmentVariable("BDF_PASSWORD") ?? "");
                }
                using var viewer = new Viewer(document);
                if (viewer.Views.Count == 0)
                {
                    Console.Error.WriteLine($"{path}: no view with pages");
                    return 1;
                }
                int view = viewer.Views[0].Index;
                var size = document.PageSize(page, view) ?? (0, 0);
                var control = new PageView();
                control.Show(viewer.Skia, new Size(size.Width, size.Height), view, page, 2);
                control.Measure(Size.Infinity);
                control.Arrange(new Rect(control.DesiredSize));
                var pixels = new PixelSize((int)Math.Ceiling(control.DesiredSize.Width), (int)Math.Ceiling(control.DesiredSize.Height));
                using var bitmap = new RenderTargetBitmap(pixels);
                bitmap.Render(control);
                bitmap.Save(output, new PngBitmapEncoderOptions());
                Console.WriteLine($"{output}: {pixels.Width} × {pixels.Height}");
                return 0;
            }
            catch (BdfException e)
            {
                Console.Error.WriteLine($"{path}: {e.Message}");
                return 1;
            }
        }
    }
}
