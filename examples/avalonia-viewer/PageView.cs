using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Platform;
using Avalonia.Rendering.SceneGraph;
using Avalonia.Skia;
using LibBdf;
using LibBdf.Skia;
using SkiaSharp;

namespace AvaloniaViewer;

/// <summary>
/// A page of a document, or a sheet, drawn by libbdf through Skia: Avalonia
/// draws with Skia, so the page is played on the very SKCanvas the window is
/// drawn on (LibBdf.SkiaSharp), as vectors at the resolution of the screen.
/// Of a sheet, which may be far larger than the window, only what is in
/// sight is drawn.
/// </summary>
public sealed class PageView : Control
{
    private BdfSkiaRenderer? _skia;
    private readonly object _drawing = new(); // a renderer is for one thread at a time
    private int _view, _page;
    private bool _sheet;
    private Size _size;

    /// <summary>Pixels a unit of the page: a unit is a point.</summary>
    public double Zoom { get; private set; } = 1;

    public void Show(BdfSkiaRenderer? skia, Size size, int view, int page, bool sheet, double zoom)
    {
        lock (_drawing)
        {
            _skia = skia;
            _view = view;
            _page = page;
            _sheet = sheet;
            _size = size;
            Zoom = zoom;
        }
        InvalidateMeasure();
        InvalidateVisual();
    }

    protected override Size MeasureOverride(Size available) => new(_size.Width * Zoom, _size.Height * Zoom);

    public override void Render(DrawingContext context)
    {
        if (_skia == null)
        {
            return;
        }
        context.Custom(new Draw(this, new Rect(Bounds.Size)));
    }

    /// <summary>What Avalonia's renderer runs on its thread, with the Skia canvas it draws on.</summary>
    private sealed class Draw(PageView view, Rect bounds) : ICustomDrawOperation
    {
        public Rect Bounds => bounds;

        public bool HitTest(Point p) => bounds.Contains(p);

        public bool Equals(ICustomDrawOperation? other) => false;

        public void Dispose()
        {
        }

        public void Render(ImmediateDrawingContext context)
        {
            var feature = context.TryGetFeature<ISkiaSharpApiLeaseFeature>();
            if (feature == null)
            {
                return; // not drawn with Skia
            }
            using var lease = feature.Lease();
            SKCanvas canvas = lease.SkCanvas;
            lock (view._drawing)
            {
                if (view._skia == null)
                {
                    return;
                }
                int depth = canvas.Save();
                canvas.Scale((float)view.Zoom); // a unit of the page is a point
                var paper = new SKRect(0, 0, (float)view._size.Width, (float)view._size.Height);
                canvas.ClipRect(paper);
                canvas.Clear(SKColors.White); // the paper is the caller's
                try
                {
                    if (view._sheet)
                    {
                        DrawSheet(canvas, view._skia, view._view);
                    }
                    else
                    {
                        view._skia.DrawPage(canvas, view._page, view._view);
                    }
                }
                catch (BdfException e)
                {
                    Console.Error.WriteLine(view._sheet ? $"sheet: {e.Message}" : $"page {view._page + 1}: {e.Message}");
                }
                canvas.RestoreToCount(depth);
            }
        }

        // What is in sight of a sheet (the clip of the canvas), from a corner
        // on a whole pixel, so that the gridlines are sharp.
        private static void DrawSheet(SKCanvas canvas, BdfSkiaRenderer skia, int view)
        {
            SKRect seen = canvas.LocalClipBounds;
            SKMatrix m = canvas.TotalMatrix;
            if (seen.IsEmpty || m.ScaleX <= 0 || m.ScaleY <= 0)
            {
                return;
            }
            float left = (MathF.Floor(m.ScaleX * seen.Left + m.TransX) - m.TransX) / m.ScaleX;
            float top = (MathF.Floor(m.ScaleY * seen.Top + m.TransY) - m.TransY) / m.ScaleY;
            canvas.Translate(left, top);
            skia.DrawRegion(canvas, new SKRect(left, top, seen.Right, seen.Bottom), view);
        }
    }
}
