// A viewer of BDF documents with Avalonia, on Windows, macOS and Linux:
// pages are drawn by libbdf through SkiaSharp (LibBdf.SkiaSharp) on the
// canvas Avalonia draws its window with.
//
//   dotnet run -- doc.bdf
//   dotnet run -- doc.bdf --snapshot page.png [--page N]   (a page, or the top left
//                                                          of a sheet, without a screen)
//   dotnet run -- doc.bdf --snapshot window.png --window   (the window as it opens)
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Headless;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.Themes.Fluent;
using AvaloniaViewer;
using LibBdf;

string? path = args.FirstOrDefault(a => !a.StartsWith("--"));
int snapshot = Array.IndexOf(args, "--snapshot");
if (snapshot >= 0 && snapshot + 1 < args.Length && path != null)
{
    if (args.Contains("--window"))
    {
        return Snapshot.Window(path, args[snapshot + 1]);
    }
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
        private static void Setup() =>
            AppBuilder.Configure(() => new App(null)).UseSkia()
                .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
                .SetupWithoutStarting();

        /// <summary>The window, as it shows the document when it opens.</summary>
        public static int Window(string path, string output)
        {
            Setup();
            var window = new MainWindow(path);
            window.Show();
            Dispatcher.UIThread.RunJobs(); // it opens the document when it is shown
            var frame = window.CaptureRenderedFrame();
            if (frame == null)
            {
                Console.Error.WriteLine($"{path}: the window was not drawn");
                return 1;
            }
            frame.Save(output, new PngBitmapEncoderOptions());
            Console.WriteLine($"{output}: {frame.PixelSize.Width} × {frame.PixelSize.Height}");
            window.Close();
            return 0;
        }

        public static int Write(string path, string output, int page)
        {
            Setup();
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
                    Console.Error.WriteLine($"{path}: no view with pages, and no sheet");
                    return 1;
                }
                var (view, info) = viewer.Views[0];
                bool sheet = info.Kind == BdfViewKind.Sheet;
                var size = viewer.Size(view, page) ?? (0, 0);
                var control = new PageView();
                control.Show(viewer.Skia, new Size(size.Width, size.Height), view, page, sheet, 2);
                control.Measure(Size.Infinity);
                control.Arrange(new Rect(control.DesiredSize));
                // (of a large sheet, only what fits in 2000 pixels each way)
                var pixels = new PixelSize((int)Math.Min(2000, Math.Ceiling(control.DesiredSize.Width)),
                    (int)Math.Min(2000, Math.Ceiling(control.DesiredSize.Height)));
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
