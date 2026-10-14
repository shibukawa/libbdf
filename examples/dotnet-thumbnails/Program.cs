// Thumbnails of the pages of a BDF document, with libbdf for .NET and
// SkiaSharp: each page drawn on an SKCanvas (LibBdf.SkiaSharp, the way MAUI,
// Avalonia and Uno draw), or with --raster by libbdf's own rasterizer, and
// written as PNG files.
//
//   dotnet run -- doc.bdf [out-dir] [--width 320] [--raster] [--password P] [--font FILE]...
using LibBdf;
using LibBdf.Skia;
using SkiaSharp;

string? path = null, outDir = null, password = Environment.GetEnvironmentVariable("BDF_PASSWORD");
int width = 320;
bool raster = false;
var fontFiles = new List<string>();
for (int i = 0; i < args.Length; i++)
{
    string a = args[i];
    string? next = i + 1 < args.Length ? args[i + 1] : null;
    switch (a)
    {
        case "--raster": raster = true; break;
        case "--width" when next != null: width = int.Parse(next); i++; break;
        case "--password" when next != null: password = next; i++; break;
        case "--font" when next != null: fontFiles.Add(next); i++; break;
        default:
            if (a.StartsWith('-') || outDir != null)
            {
                Console.Error.WriteLine("usage: dotnet-thumbnails <file.bdf> [out-dir] [--width N] [--raster] [--password P] [--font FILE]...");
                return 2;
            }
            if (path == null) path = a; else outDir = a;
            break;
    }
}
if (path == null)
{
    Console.Error.WriteLine("usage: dotnet-thumbnails <file.bdf> [out-dir] [--width N] [--raster] [--password P] [--font FILE]...");
    return 2;
}
outDir ??= "thumbnails";
Directory.CreateDirectory(outDir);

try
{
    using var document = BdfDocument.Open(path);
    if (document.IsLocked)
    {
        // an encrypted document: its views come with the password
        if (password == null)
        {
            Console.Error.WriteLine($"{path} is encrypted: give its password with --password or $BDF_PASSWORD");
            return 1;
        }
        document.Unlock(password);
    }

    // Fonts for text that names its font (documents made from HTML, Markdown
    // or EPUB): libbdf reads no font directories, the application says which
    // fonts there are.
    using var fonts = new BdfFonts();
    foreach (var file in fontFiles.Count > 0 ? fontFiles : SystemFonts().Where(File.Exists))
    {
        fonts.AddFile(file);
    }

    using var renderer = new BdfRenderer(document, fonts.Count > 0 ? fonts : null);
    using var skia = new BdfSkiaRenderer(renderer);
    int written = 0;
    for (int v = 0; v < document.Views.Count; v++)
    {
        var view = document.Views[v];
        if (view.Kind == BdfViewKind.Sheet)
        {
            continue; // a sheet has tiles, not pages
        }
        for (int page = 0; page < view.PageCount; page++)
        {
            var (w, h) = document.PageSize(page, v) ?? (0, 0);
            if (w <= 0 || h <= 0)
            {
                continue;
            }
            float scale = width / w; // a unit of a page is a point
            string file = Path.Combine(outDir, $"{view.Id}-{page + 1}.png");
            using var bitmap = raster ? Raster(renderer, page, v, scale) : Skia(skia, page, v, w, h, scale);
            using var data = bitmap.Encode(SKEncodedImageFormat.Png, 100);
            using (var stream = File.Create(file))
            {
                data.SaveTo(stream);
            }
            Console.WriteLine($"{file}: {bitmap.Width} × {bitmap.Height}");
            written++;
        }
    }
    foreach (var warning in renderer.Warnings)
    {
        Console.Error.WriteLine($"warning: {warning}");
    }
    return written > 0 ? 0 : 1;
}
catch (BdfException e)
{
    Console.Error.WriteLine($"{path}: {e.Message}");
    return 1;
}

// A page drawn on an SKCanvas by Skia, through libbdf's renderer.
static SKBitmap Skia(BdfSkiaRenderer skia, int page, int view, float w, float h, float scale)
{
    var bitmap = new SKBitmap(new SKImageInfo((int)Math.Ceiling(w * scale), (int)Math.Ceiling(h * scale),
                                              SKColorType.Rgba8888, SKAlphaType.Premul));
    using var canvas = new SKCanvas(bitmap);
    canvas.Clear(SKColors.White); // the paper is the caller's
    canvas.Scale(scale);
    skia.DrawPage(canvas, page, view);
    return bitmap;
}

// A page drawn by libbdf's rasterizer: the same pixels as the reference's.
static SKBitmap Raster(BdfRenderer renderer, int page, int view, float scale)
{
    var image = renderer.Render(page, view, scale);
    var bitmap = new SKBitmap(new SKImageInfo(image.Width, image.Height, SKColorType.Rgba8888, SKAlphaType.Premul));
    System.Runtime.InteropServices.Marshal.Copy(image.Pixels, 0, bitmap.GetPixels(), image.Pixels.Length);
    return bitmap;
}

static IEnumerable<string> SystemFonts()
{
    if (OperatingSystem.IsWindows())
    {
        return new[] { @"C:\Windows\Fonts\arial.ttf", @"C:\Windows\Fonts\times.ttf", @"C:\Windows\Fonts\cour.ttf",
                       @"C:\Windows\Fonts\YuGothR.ttc", @"C:\Windows\Fonts\meiryo.ttc" };
    }
    if (OperatingSystem.IsMacOS())
    {
        return new[] { "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Times.ttc",
                       "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
                       "/System/Library/Fonts/Hiragino Sans GB.ttc" };
    }
    return new[] { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
                   "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                   "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc" };
}
