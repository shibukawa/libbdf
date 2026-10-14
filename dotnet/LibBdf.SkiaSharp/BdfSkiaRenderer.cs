// libbdf through SkiaSharp: the pages of a BDF document drawn on an SKCanvas.
// The same as adapters/skia/bdf_skia.cpp does in C++ with Skia itself: the
// renderer of libbdf plays a page through a canvas of functions (bdf_canvas),
// which are here the methods below.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using SkiaSharp;

namespace LibBdf.Skia;

/// <summary>
/// Draws the pages of a document on an SKCanvas, with Skia's own paths,
/// shaders, images, layers and blend modes. It keeps the images the document
/// has (as SKImages) from one drawing to the next; use it, and its renderer,
/// from one thread at a time.
/// </summary>
/// <example>
/// <code>
/// using var document = BdfDocument.Open("slides.bdf");
/// using var renderer = new BdfRenderer(document);
/// using var skia = new BdfSkiaRenderer(renderer);
/// canvas.Clear(SKColors.White);   // the paper is the caller's
/// canvas.Scale(zoom);             // a unit of the page is a point
/// skia.DrawPage(canvas, page: 0);
/// </code>
/// </example>
public sealed unsafe class BdfSkiaRenderer : IDisposable
{
    private readonly BdfRenderer _renderer;
    private readonly Dictionary<ulong, SKImage> _images = new();
    private SKCanvas? _canvas;
    private SKMatrix _base; // the coordinates of the page on the device: shadows are in its units
    // An exception must not pass through libbdf, which is C: a function of
    // the canvas that meets one keeps it here, and those after it do nothing.
    private Exception? _failure;

    public BdfSkiaRenderer(BdfRenderer renderer)
    {
        _renderer = renderer;
    }

    /// <summary>Draws a page in the coordinates of the canvas as they are. The canvas is left as it was.</summary>
    public void DrawPage(SKCanvas canvas, int page, int view = 0)
    {
        if (page < 0 || view < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(page));
        }
        var self = GCHandle.Alloc(this);
        int depth = canvas.Save();
        try
        {
            _canvas = canvas;
            _base = canvas.TotalMatrix;
            _failure = null;
            var functions = new Native.Canvas
            {
                Ctx = GCHandle.ToIntPtr(self),
                Save = &OnSave,
                Restore = &OnRestore,
                Transform = &OnTransform,
                Clip = &OnClip,
                Fill = &OnFill,
                Stroke = &OnStroke,
                Image = &OnImage,
                BeginLayer = &OnBeginLayer,
                EndLayer = &OnEndLayer,
            };
            // the pixels a unit of the canvas is, for what libbdf draws itself
            double det = Math.Abs((double)_base.ScaleX * _base.ScaleY - (double)_base.SkewX * _base.SkewY);
            Native.Error error;
            int status = Native.bdf_play_page(_renderer.Handle, (nuint)view, (nuint)page, det > 0 ? Math.Sqrt(det) : 1,
                &functions, &error);
            if (_failure != null)
            {
                throw new InvalidOperationException("a page could not be drawn with SkiaSharp", _failure);
            }
            BdfException.Check(status, error);
        }
        finally
        {
            canvas.RestoreToCount(depth);
            _canvas = null;
            self.Free();
        }
    }

    public void Dispose()
    {
        foreach (var image in _images.Values)
        {
            image.Dispose();
        }
        _images.Clear();
    }

    // ---- What a style, a path and an image are to Skia ----

    private static readonly SKBlendMode[] Modes =
    {
        SKBlendMode.SrcOver, SKBlendMode.Multiply, SKBlendMode.Screen, SKBlendMode.Overlay, SKBlendMode.Darken,
        SKBlendMode.Lighten, SKBlendMode.ColorDodge, SKBlendMode.ColorBurn, SKBlendMode.HardLight,
        SKBlendMode.SoftLight, SKBlendMode.Difference, SKBlendMode.Exclusion, SKBlendMode.Hue,
        SKBlendMode.Saturation, SKBlendMode.Color, SKBlendMode.Luminosity, SKBlendMode.DstOver, SKBlendMode.DstIn,
        SKBlendMode.DstOut, SKBlendMode.SrcIn, SKBlendMode.SrcOut, SKBlendMode.SrcATop, SKBlendMode.DstATop,
        SKBlendMode.Xor, SKBlendMode.Src, SKBlendMode.Plus,
    };

    private static SKBlendMode ModeOf(byte blend) => blend < Modes.Length ? Modes[blend] : SKBlendMode.SrcOver;

    // The composite operations that change what is outside the shape too:
    // Skia composites where the shape is only, so these are drawn on a layer
    // of their own, which is composited whole (as Chrome draws them):
    // destination-in, source-in, source-out, destination-atop, copy.
    private static bool Unbounded(byte blend) => blend is 17 or 19 or 20 or 22 or 24;

    private static SKColor ColorOf(uint c) => new((byte)(c >> 24), (byte)(c >> 16), (byte)(c >> 8), (byte)c);

    private static SKMatrix MatrixOf(double* m) =>
        new((float)m[0], (float)m[2], (float)m[4], (float)m[1], (float)m[3], (float)m[5], 0, 0, 1);

    private static SKPath PathOf(Native.CanvasPath* p, byte rule)
    {
        var path = new SKPath();
        double* c = p->Coords;
        for (nuint i = 0; i < p->VerbCount; i++)
        {
            switch (p->Verbs[i])
            {
                case Native.VerbMove:
                    path.MoveTo((float)c[0], (float)c[1]);
                    c += 2;
                    break;
                case Native.VerbLine:
                    path.LineTo((float)c[0], (float)c[1]);
                    c += 2;
                    break;
                case Native.VerbQuad:
                    path.QuadTo((float)c[0], (float)c[1], (float)c[2], (float)c[3]);
                    c += 4;
                    break;
                case Native.VerbCubic:
                    path.CubicTo((float)c[0], (float)c[1], (float)c[2], (float)c[3], (float)c[4], (float)c[5]);
                    c += 6;
                    break;
                default:
                    path.Close();
                    break;
            }
        }
        path.FillType = rule == Native.FillEvenOdd ? SKPathFillType.EvenOdd : SKPathFillType.Winding;
        return path;
    }

    /// <summary>A bitmap as an SKImage: its pixels copied, and kept by its id.</summary>
    private SKImage? ImageOf(Native.CanvasBitmap* b, out bool owned)
    {
        owned = false;
        if (b->Id != 0 && _images.TryGetValue(b->Id, out var kept))
        {
            return kept;
        }
        var info = new SKImageInfo(b->Width, b->Height, SKColorType.Rgba8888, SKAlphaType.Premul);
        var image = SKImage.FromPixelCopy(info, (IntPtr)b->Pixels, 4 * b->Width);
        if (image != null && b->Id != 0)
        {
            _images[b->Id] = image;
        }
        else
        {
            owned = image != null; // of this call alone: the caller's to dispose
        }
        return image;
    }

    /// <summary>The shader of a paint, or null for one that paints nothing.</summary>
    private SKShader? ShaderOf(Native.CanvasPaint* paint)
    {
        if (paint->Kind == Native.PaintPattern)
        {
            if (paint->Image == null || paint->Image->Width <= 0 || paint->Image->Height <= 0)
            {
                return null;
            }
            var image = ImageOf(paint->Image, out bool owned);
            if (image == null)
            {
                return null;
            }
            // along an axis it does not repeat on, a pattern shows in its
            // cell (the caller clips to it), with its edge up to the edge
            bool rx = paint->Repeat is Native.Repeat or Native.RepeatX;
            bool ry = paint->Repeat is Native.Repeat or Native.RepeatY;
            // a pattern is interpolated between its pixels whatever the
            // smoothing of images is, as the rasterizer draws it
            var shader = image.ToShader(rx ? SKShaderTileMode.Repeat : SKShaderTileMode.Clamp,
                ry ? SKShaderTileMode.Repeat : SKShaderTileMode.Clamp, new SKSamplingOptions(SKFilterMode.Linear),
                MatrixOf(paint->Matrix));
            if (owned)
            {
                image.Dispose();
            }
            return shader;
        }
        int n = checked((int)paint->StopCount);
        if (n == 0)
        {
            return null;
        }
        // the stops in the order of their places, as a canvas keeps them
        var order = Enumerable.Range(0, n).ToArray();
        var places = new float[n];
        for (int i = 0; i < n; i++)
        {
            float at = paint->Stops[i].Offset;
            places[i] = at > 0 ? (at < 1 ? at : 1) : 0; // (and what is no number is 0)
        }
        // (Array.Sort is not stable: stops at one place keep their order by their number)
        Array.Sort(order, (a, b) => places[a] != places[b] ? places[a].CompareTo(places[b]) : a.CompareTo(b));
        var colors = new SKColor[n];
        var sorted = new float[n];
        for (int i = 0; i < n; i++)
        {
            colors[i] = ColorOf(paint->Stops[order[i]].Color);
            sorted[i] = places[order[i]];
        }
        float* c = paint->Coords;
        switch (paint->Kind)
        {
            case Native.PaintLinear when paint->CoordCount >= 4:
                return SKShader.CreateLinearGradient(new SKPoint(c[0], c[1]), new SKPoint(c[2], c[3]), colors, sorted,
                    SKShaderTileMode.Clamp);
            case Native.PaintRadial when paint->CoordCount >= 6:
                return SKShader.CreateTwoPointConicalGradient(new SKPoint(c[0], c[1]), c[2], new SKPoint(c[3], c[4]),
                    c[5], colors, sorted, SKShaderTileMode.Clamp);
            case Native.PaintConic when paint->CoordCount >= 3:
                // a sweep starts on the x axis: turned to the start angle
                return SKShader.CreateSweepGradient(new SKPoint(c[1], c[2]), colors, sorted,
                    SKMatrix.CreateRotationDegrees((float)(c[0] * 180 / Math.PI), c[1], c[2]));
            default:
                return null;
        }
    }

    /// <summary>The paint of a style: its colour or shader, its alpha, and its shadow. null for a style that
    /// paints nothing.</summary>
    private SKPaint? PaintOf(Native.CanvasStyle* style, bool image = false)
    {
        var paint = new SKPaint { IsAntialias = true };
        if (!image && style->Paint != null)
        {
            var shader = ShaderOf(style->Paint);
            if (shader == null)
            {
                paint.Dispose();
                return null;
            }
            paint.Shader = shader;
            paint.ColorF = new SKColorF(1, 1, 1, style->Alpha);
        }
        else if (image)
        {
            // (how its pixels are sampled is given where it is drawn)
            paint.ColorF = new SKColorF(0, 0, 0, style->Alpha);
        }
        else
        {
            var c = ColorOf(style->Color);
            paint.ColorF = new SKColorF(c.Red / 255f, c.Green / 255f, c.Blue / 255f, c.Alpha / 255f * style->Alpha);
        }
        paint.ImageFilter = ShadowOf(style);
        return paint;
    }

    /// <summary>The shadow of a style as an image filter, or null.</summary>
    private SKImageFilter? ShadowOf(Native.CanvasStyle* style)
    {
        if ((style->ShadowColor & 0xff) == 0 || (style->ShadowBlur <= 0 && style->ShadowDx == 0 && style->ShadowDy == 0))
        {
            return null;
        }
        // A shadow is in the units of the page whatever the transform is,
        // and an image filter in those of what is drawn: the offset taken
        // back through the transform since the page began, the blur by its
        // scale.
        if (!_base.TryInvert(out var inverse))
        {
            return null;
        }
        var local = SKMatrix.Concat(inverse, _canvas!.TotalMatrix);
        if (!local.TryInvert(out var back))
        {
            return null;
        }
        var offset = back.MapVector((float)style->ShadowDx, (float)style->ShadowDy);
        double det = Math.Abs((double)local.ScaleX * local.ScaleY - (double)local.SkewX * local.SkewY);
        float sigma = (float)(style->ShadowBlur / 2 / (det > 0 ? Math.Sqrt(det) : 1));
        return SKImageFilter.CreateDropShadow(offset.X, offset.Y, sigma, sigma, ColorOf(style->ShadowColor));
    }

    /// <summary>Clips to the band, or the cell, a pattern that does not repeat along both axes shows in.</summary>
    private void ClipBand(Native.CanvasPaint* paint)
    {
        if (paint == null || paint->Kind != Native.PaintPattern || paint->Repeat == Native.Repeat || paint->Image == null)
        {
            return;
        }
        float w = paint->Image->Width, h = paint->Image->Height;
        const float far = 1e7f;
        var band = paint->Repeat == Native.RepeatX ? new SKRect(-far, 0, far, h)
            : paint->Repeat == Native.RepeatY ? new SKRect(0, -far, w, far)
            : new SKRect(0, 0, w, h);
        using var path = new SKPath();
        path.AddRect(band);
        path.Transform(MatrixOf(paint->Matrix));
        _canvas!.ClipPath(path, SKClipOperation.Intersect, true);
    }

    /// <summary>Draws a path with a paint that has its style: within the band of a pattern, and on a layer of
    /// its own for a composite operation that reaches past the shape.</summary>
    private void Draw(SKPath path, SKPaint paint, Native.CanvasStyle* style)
    {
        var canvas = _canvas!;
        bool layered = Unbounded(style->Blend);
        canvas.Save();
        if (layered)
        {
            using var whole = new SKPaint { BlendMode = ModeOf(style->Blend) };
            canvas.SaveLayer(whole);
        }
        else
        {
            paint.BlendMode = ModeOf(style->Blend);
        }
        ClipBand(style->Paint);
        canvas.DrawPath(path, paint);
        if (layered)
        {
            canvas.Restore();
        }
        canvas.Restore();
    }

    // ---- The functions of the canvas ----

    private static BdfSkiaRenderer? Self(IntPtr ctx)
    {
        var self = (BdfSkiaRenderer)GCHandle.FromIntPtr(ctx).Target!;
        return self._failure == null ? self : null;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnSave(IntPtr ctx)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            self._canvas!.Save();
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnRestore(IntPtr ctx)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            self._canvas!.Restore();
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnTransform(IntPtr ctx, double* m)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            var matrix = MatrixOf(m);
            self._canvas!.Concat(in matrix);
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnClip(IntPtr ctx, Native.CanvasPath* path, byte rule)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            if (path->VerbCount == 0)
            {
                // nothing is left of the clip
                self._canvas!.ClipRect(SKRect.Empty);
                return;
            }
            using var p = PathOf(path, rule);
            self._canvas!.ClipPath(p, SKClipOperation.Intersect, true);
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnFill(IntPtr ctx, Native.CanvasPath* path, byte rule, Native.CanvasStyle* style)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            using var paint = self.PaintOf(style);
            if (paint == null)
            {
                return;
            }
            using var p = PathOf(path, rule);
            self.Draw(p, paint, style);
            paint.Shader?.Dispose();
            paint.ImageFilter?.Dispose();
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnStroke(IntPtr ctx, Native.CanvasPath* path, Native.CanvasLine* line, Native.CanvasStyle* style)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            if (!(line->Width > 0))
            {
                return; // (a line of no width is a hairline to Skia)
            }
            using var paint = self.PaintOf(style);
            if (paint == null)
            {
                return;
            }
            paint.Style = SKPaintStyle.Stroke;
            paint.StrokeWidth = (float)line->Width;
            paint.StrokeCap = line->Cap == 1 ? SKStrokeCap.Round : line->Cap == 2 ? SKStrokeCap.Square : SKStrokeCap.Butt;
            paint.StrokeJoin = line->Join == 1 ? SKStrokeJoin.Round : line->Join == 2 ? SKStrokeJoin.Bevel : SKStrokeJoin.Miter;
            paint.StrokeMiter = (float)line->Miter;
            if (line->Dash != null && line->DashCount >= 2)
            {
                var dashes = new float[checked((int)(line->DashCount & ~(nuint)1))];
                for (int i = 0; i < dashes.Length; i++)
                {
                    dashes[i] = line->Dash[i];
                }
                paint.PathEffect = SKPathEffect.CreateDash(dashes, (float)line->DashOffset);
            }
            using var p = PathOf(path, Native.FillNonZero);
            self.Draw(p, paint, style);
            paint.PathEffect?.Dispose();
            paint.Shader?.Dispose();
            paint.ImageFilter?.Dispose();
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnImage(IntPtr ctx, Native.CanvasBitmap* bitmap, double* src, double* dst, Native.CanvasStyle* style)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            if (bitmap->Width <= 0 || bitmap->Height <= 0)
            {
                return;
            }
            var image = self.ImageOf(bitmap, out bool owned);
            if (image == null)
            {
                return;
            }
            using var paint = self.PaintOf(style, image: true)!;
            paint.BlendMode = ModeOf(style->Blend);
            // interpolated between its pixels when the style smooths images
            var sampling = new SKSamplingOptions(style->Smoothing != 0 ? SKFilterMode.Linear : SKFilterMode.Nearest);
            self._canvas!.DrawImage(image,
                new SKRect((float)src[0], (float)src[1], (float)(src[0] + src[2]), (float)(src[1] + src[3])),
                new SKRect((float)dst[0], (float)dst[1], (float)(dst[0] + dst[2]), (float)(dst[1] + dst[3])),
                sampling, paint);
            paint.ImageFilter?.Dispose();
            if (owned)
            {
                image.Dispose();
            }
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnBeginLayer(IntPtr ctx, double* bounds, Native.CanvasStyle* style)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            // the layer is composited with the alpha, the blend and the shadow of the style
            using var whole = new SKPaint { BlendMode = ModeOf(style->Blend), ColorF = new SKColorF(0, 0, 0, style->Alpha) };
            whole.ImageFilter = self.ShadowOf(style);
            self._canvas!.SaveLayer(
                new SKRect((float)bounds[0], (float)bounds[1], (float)(bounds[0] + bounds[2]), (float)(bounds[1] + bounds[3])),
                whole);
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnEndLayer(IntPtr ctx)
    {
        if (Self(ctx) is not { } self)
        {
            return;
        }
        try
        {
            self._canvas!.Restore();
        }
        catch (Exception e)
        {
            self._failure = e;
        }
    }
}
