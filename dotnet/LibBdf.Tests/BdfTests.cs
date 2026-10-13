// Tests of libbdf for .NET on samples/two-pages.bdf: two pages of 100 by 80;
// on the first a red rectangle from (10, 10) to (40, 30) and a blue one at
// half its alpha from (30, 20) to (60, 50); on the second a green circle
// around (50, 40). The native library is found as the system finds
// libraries (LD_LIBRARY_PATH in the CI).
using LibBdf;
using LibBdf.Skia;
using SkiaSharp;
using Xunit;

public class BdfTests
{
    private static BdfDocument Sample() => BdfDocument.Open(Path.Combine(AppContext.BaseDirectory, "two-pages.bdf"));

    // The colour of a pixel of premultiplied RGBA, as 0xRRGGBBAA.
    private static uint At(byte[] pixels, int width, int x, int y)
    {
        int i = 4 * (y * width + x);
        return (uint)pixels[i] << 24 | (uint)pixels[i + 1] << 16 | (uint)pixels[i + 2] << 8 | pixels[i + 3];
    }

    [Fact]
    public void ViewsAndPages()
    {
        using var doc = Sample();
        Assert.False(doc.IsLocked);
        Assert.Matches(@"^\d+\.\d+\.\d+$", BdfDocument.NativeVersion);
        var view = Assert.Single(doc.Views);
        Assert.Equal("pages", view.Id);
        Assert.Equal(BdfViewKind.Fixed, view.Kind);
        Assert.Equal("Pages", view.Title);
        Assert.Equal(2, view.PageCount);
        Assert.False(view.RightToLeft);
        Assert.Equal((100f, 80f), doc.PageSize(0));
        Assert.Null(doc.PageSize(2));
        Assert.Null(doc.PageSize(0, view: 1));
    }

    [Fact]
    public void WhatIsNoDocument()
    {
        var e = Assert.Throws<BdfException>(() => new BdfDocument("not a document"u8));
        Assert.NotEqual(0, e.Status);
        Assert.NotEmpty(e.Message);
        Assert.Throws<BdfException>(() => new BdfDocument(ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void Rasterizer()
    {
        using var doc = Sample();
        using var renderer = new BdfRenderer(doc);
        var image = renderer.Render(0, scale: 2);
        Assert.Equal(200, image.Width);
        Assert.Equal(160, image.Height);
        Assert.Equal(0xffffffffu, At(image.Pixels, 200, 10, 10)); // the paper
        Assert.Equal(0xff0000ffu, At(image.Pixels, 200, 30, 30)); // red
        Assert.Equal(0x7f7fffffu, At(image.Pixels, 200, 100, 80)); // blue at half its alpha, on the paper
        Assert.Equal(0x7f0080ffu, At(image.Pixels, 200, 70, 50)); // and on the red
        var second = renderer.Render(1);
        Assert.Equal(0x00a000ffu, At(second.Pixels, 100, 50, 40));
        Assert.Equal(0xffffffffu, At(second.Pixels, 100, 5, 5));
        Assert.Throws<BdfException>(() => renderer.Render(2));
        Assert.Empty(renderer.Warnings);
    }

    [Fact]
    public void Fonts()
    {
        using var fonts = new BdfFonts();
        Assert.Equal(0, fonts.Count);
        Assert.Throws<BdfException>(() => fonts.Add("not a font"u8));
        using var doc = Sample();
        using var renderer = new BdfRenderer(doc, fonts);
        Assert.Equal(100, renderer.Render(0).Width);
    }

    [Fact]
    public void SkiaSharp()
    {
        using var doc = Sample();
        using var renderer = new BdfRenderer(doc);
        using var skia = new BdfSkiaRenderer(renderer);
        const int width = 200, height = 160;
        using var bitmap = new SKBitmap(new SKImageInfo(width, height, SKColorType.Rgba8888, SKAlphaType.Premul));
        using (var canvas = new SKCanvas(bitmap))
        {
            canvas.Clear(SKColors.White);
            canvas.Scale(2);
            skia.DrawPage(canvas, 0);
            // the canvas is as it was
            Assert.Equal(2f, canvas.TotalMatrix.ScaleX);
            Assert.Throws<BdfException>(() => skia.DrawPage(canvas, 2));
        }
        byte[] pixels = bitmap.Bytes;
        Assert.Equal(0xffffffffu, At(pixels, width, 10, 10));
        Assert.Equal(0xff0000ffu, At(pixels, width, 30, 30));
        // (Skia rounds a half its own way: within two of the rasterizer)
        uint mixed = At(pixels, width, 100, 80);
        Assert.InRange((int)(mixed >> 24), 0x7d, 0x81);
        Assert.Equal(0xffffu, mixed & 0xffff);

        // the same picture as the rasterizer's, but for the edges
        var soft = renderer.Render(0, scale: 2);
        long sum = 0;
        for (int i = 0; i < pixels.Length; i++)
        {
            sum += Math.Abs(pixels[i] - soft.Pixels[i]);
        }
        Assert.True(sum / (double)pixels.Length < 1, $"the mean difference is {sum / (double)pixels.Length}");
    }
}
