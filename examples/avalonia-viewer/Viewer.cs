using LibBdf;
using LibBdf.Skia;

namespace AvaloniaViewer;

/// <summary>
/// An open document and what draws it: the document, the fonts for text
/// that names its font, libbdf's renderer and the SkiaSharp canvas over it.
/// They are closed together, the drawing ones first.
/// </summary>
public sealed class Viewer : IDisposable
{
    public BdfDocument Document { get; }
    public BdfSkiaRenderer Skia { get; }
    private readonly BdfFonts _fonts;
    private readonly BdfRenderer _renderer;

    public Viewer(BdfDocument document)
    {
        Document = document;
        _fonts = new BdfFonts();
        // libbdf reads no font directories: the application says which fonts there are
        foreach (var file in SystemFonts().Where(File.Exists))
        {
            _fonts.AddFile(file);
        }
        _renderer = new BdfRenderer(document, _fonts.Count > 0 ? _fonts : null);
        Skia = new BdfSkiaRenderer(_renderer);
    }

    /// <summary>The views that can be shown: those with pages, and sheets.</summary>
    public IReadOnlyList<(int Index, BdfView View)> Views =>
        Document.Views.Select((v, i) => (i, v)).Where(x => x.v.Kind == BdfViewKind.Sheet || x.v.PageCount > 0).ToList();

    /// <summary>The size of a page of a view, or of a sheet, in its units; null when there is none.</summary>
    public (double Width, double Height)? Size(int view, int page)
    {
        if (Document.SheetSize(view) is { } sheet)
        {
            return sheet.Width > 0 && sheet.Height > 0 ? sheet : null;
        }
        return Document.PageSize(page, view) is { } p && p.Width > 0 && p.Height > 0 ? (p.Width, p.Height) : null;
    }

    public void Dispose()
    {
        Skia.Dispose();
        _renderer.Dispose();
        _fonts.Dispose();
        Document.Dispose();
    }

    private static IEnumerable<string> SystemFonts()
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
}
