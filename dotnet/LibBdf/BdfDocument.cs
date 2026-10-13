using System.Runtime.InteropServices;

namespace LibBdf;

/// <summary>What libbdf could not do, with what it said.</summary>
public sealed class BdfException : Exception
{
    /// <summary>The status of libbdf (BDF_ERR_*).</summary>
    public int Status { get; }

    internal unsafe BdfException(int status, Native.Error error)
        : base(Native.Text(error.Message) is { Length: > 0 } said ? said : $"libbdf: status {status}")
    {
        Status = status;
    }

    internal static unsafe void Check(int status, Native.Error error)
    {
        if (status != Native.Ok)
        {
            throw new BdfException(status, error);
        }
    }
}

public enum BdfViewKind
{
    Unknown = 0,
    Fixed = 1,
    Flow = 2,
    Sheet = 3,
    Scroll = 4,
}

/// <summary>A view of a document: pages, a flow, a sheet or a scroll.</summary>
public sealed record BdfView(string Id, BdfViewKind Kind, string Title, int PageCount, bool RightToLeft);

/// <summary>A BDF document in the single-file form.</summary>
public sealed unsafe class BdfDocument : IDisposable
{
    private IntPtr _doc;
    private IntPtr _bytes; // the document reads from them while it lives

    /// <summary>Opens a document from the bytes of its file.</summary>
    public BdfDocument(ReadOnlySpan<byte> data)
    {
        _bytes = Marshal.AllocHGlobal(Math.Max(data.Length, 1));
        data.CopyTo(new Span<byte>((void*)_bytes, data.Length));
        IntPtr doc;
        Native.Error error;
        int status = Native.bdf_doc_open_memory((byte*)_bytes, (nuint)data.Length, &doc, &error);
        if (status != Native.Ok)
        {
            Marshal.FreeHGlobal(_bytes);
            _bytes = IntPtr.Zero;
            GC.SuppressFinalize(this);
            throw new BdfException(status, error);
        }
        _doc = doc;
    }

    public static BdfDocument Open(string path) => new(File.ReadAllBytes(path));

    /// <summary>The version of the native library.</summary>
    public static string NativeVersion => Native.Text(Native.bdf_version());

    internal IntPtr Handle => _doc != IntPtr.Zero ? _doc : throw new ObjectDisposedException(nameof(BdfDocument));

    /// <summary>Whether the document is encrypted and not unlocked yet: it has no views then.</summary>
    public bool IsLocked => Native.bdf_doc_locked(Handle) != 0;

    /// <summary>Unlocks an encrypted document with its password.</summary>
    public void Unlock(string password)
    {
        byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(password);
        Native.Error error;
        int status;
        fixed (byte* p = utf8)
        {
            // (a pointer to something even for no bytes)
            byte none = 0;
            status = Native.bdf_doc_unlock(Handle, utf8.Length > 0 ? p : &none, (nuint)utf8.Length, &error);
        }
        BdfException.Check(status, error);
    }

    public IReadOnlyList<BdfView> Views
    {
        get
        {
            var views = new List<BdfView>();
            nuint n = Native.bdf_doc_view_count(Handle);
            for (nuint i = 0; i < n; i++)
            {
                Native.View v;
                if (Native.bdf_doc_view(Handle, i, &v) != Native.Ok)
                {
                    continue;
                }
                var kind = v.KindCode is >= 1 and <= 4 ? (BdfViewKind)v.KindCode : BdfViewKind.Unknown;
                views.Add(new BdfView(Native.Text(v.Id, v.IdLen), kind, Native.Text(v.Title, v.TitleLen),
                    checked((int)v.PageCount), v.Rtl != 0));
            }
            return views;
        }
    }

    /// <summary>The size of a page of a view, in units of the page (points), or null for a page there is not.</summary>
    public (float Width, float Height)? PageSize(int page, int view = 0)
    {
        if (page < 0 || view < 0)
        {
            return null;
        }
        Native.Page p;
        return Native.bdf_doc_page(Handle, (nuint)view, (nuint)page, &p) == Native.Ok ? (p.Width, p.Height) : null;
    }

    public void Dispose()
    {
        if (_doc != IntPtr.Zero)
        {
            Native.bdf_doc_free(_doc);
            _doc = IntPtr.Zero;
        }
        if (_bytes != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(_bytes);
            _bytes = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
    }

    ~BdfDocument() => Dispose();
}

/// <summary>A set of fonts for the text that names its font: the application says which fonts there are.</summary>
public sealed unsafe class BdfFonts : IDisposable
{
    private IntPtr _fonts;

    public BdfFonts()
    {
        IntPtr fonts;
        Native.Error error;
        BdfException.Check(Native.bdf_fonts_new(&fonts, &error), error);
        _fonts = fonts;
    }

    internal IntPtr Handle => _fonts != IntPtr.Zero ? _fonts : throw new ObjectDisposedException(nameof(BdfFonts));

    /// <summary>Adds a font file that is in memory (TrueType, OpenType, or a collection).</summary>
    public void Add(ReadOnlySpan<byte> data)
    {
        Native.Error error;
        int status;
        fixed (byte* p = data)
        {
            byte none = 0;
            status = Native.bdf_fonts_add(Handle, data.Length > 0 ? p : &none, (nuint)data.Length, &error);
        }
        BdfException.Check(status, error);
    }

    /// <summary>
    /// Adds a font file by its path. libbdf reads the names of its faces now and its glyphs when a renderer
    /// first draws with one, so the file stays where it is while the set is used, and a large font costs
    /// no memory until it is drawn with.
    /// </summary>
    public void AddFile(string path)
    {
        string full = Path.GetFullPath(path);
        if (!File.Exists(full))
        {
            throw new FileNotFoundException("the font file is not there", full);
        }
        // UTF-8 with a NUL after it: what libbdf takes a path as on every system
        byte[] utf8 = new byte[System.Text.Encoding.UTF8.GetByteCount(full) + 1];
        System.Text.Encoding.UTF8.GetBytes(full, 0, full.Length, utf8, 0);
        Native.Error error;
        int status;
        fixed (byte* p = utf8)
        {
            status = Native.bdf_fonts_add_file(Handle, p, &error);
        }
        BdfException.Check(status, error);
    }

    public int Count => checked((int)Native.bdf_fonts_count(Handle));

    public void Dispose()
    {
        if (_fonts != IntPtr.Zero)
        {
            Native.bdf_fonts_free(_fonts);
            _fonts = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
    }

    ~BdfFonts() => Dispose();
}

/// <summary>A page as libbdf's rasterizer drew it: premultiplied RGBA, four bytes a pixel, rows from the top.</summary>
public sealed record BdfImage(int Width, int Height, byte[] Pixels);

/// <summary>What draws the pages of a document. It keeps what it decoded from one drawing to the next; use it
/// from one thread at a time. It keeps its document and its fonts alive.</summary>
public sealed unsafe class BdfRenderer : IDisposable
{
    private IntPtr _renderer;

    public BdfDocument Document { get; }
    public BdfFonts? Fonts { get; }

    public BdfRenderer(BdfDocument document, BdfFonts? fonts = null)
    {
        Document = document;
        Fonts = fonts;
        var options = new Native.RenderOptions { Fonts = fonts?.Handle ?? IntPtr.Zero };
        IntPtr renderer;
        Native.Error error;
        BdfException.Check(Native.bdf_renderer_new(document.Handle, &options, &renderer, &error), error);
        _renderer = renderer;
    }

    /// <summary>The renderer of the C API, for what plays a page through a canvas (LibBdf.SkiaSharp).</summary>
    public IntPtr Handle => _renderer != IntPtr.Zero ? _renderer : throw new ObjectDisposedException(nameof(BdfRenderer));

    /// <summary>A page drawn by libbdf's own rasterizer, on white: the same pixels on every platform.
    /// scale is pixels a unit of the page.</summary>
    public BdfImage Render(int page, int view = 0, double scale = 1)
    {
        if (page < 0 || view < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(page));
        }
        Native.Image image;
        Native.Error error;
        BdfException.Check(Native.bdf_render_page(Handle, (nuint)view, (nuint)page, scale, &image, &error), error);
        try
        {
            var pixels = new byte[4L * image.Width * image.Height];
            new ReadOnlySpan<byte>(image.Pixels, pixels.Length).CopyTo(pixels);
            return new BdfImage(image.Width, image.Height, pixels);
        }
        finally
        {
            Native.bdf_image_free(&image);
        }
    }

    /// <summary>What could not be drawn as the viewer would draw it, each said once.</summary>
    public IReadOnlyList<string> Warnings
    {
        get
        {
            var warnings = new List<string>();
            nuint n = Native.bdf_renderer_warning_count(Handle);
            for (nuint i = 0; i < n; i++)
            {
                warnings.Add(Native.Text(Native.bdf_renderer_warning(Handle, i)));
            }
            return warnings;
        }
    }

    public void Dispose()
    {
        if (_renderer != IntPtr.Zero)
        {
            Native.bdf_renderer_free(_renderer);
            _renderer = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
        GC.KeepAlive(Document);
        GC.KeepAlive(Fonts);
    }

    ~BdfRenderer() => Dispose();
}
