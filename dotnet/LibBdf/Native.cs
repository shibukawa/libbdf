// The C API of libbdf as .NET sees it: what the classes of this assembly
// call, and the structures of the canvas a page is played through
// (include/bdf/bdf.h has what each of them means).
using System.Runtime.InteropServices;

namespace LibBdf;

/// <summary>The functions and structures of libbdf, as they are in C.</summary>
public static unsafe class Native
{
    private const string Lib = "bdf";

    public const int Ok = 0;
    public const int ErrFormat = 1;
    public const int ErrArg = 2;
    public const int ErrNoMemory = 4;

    [StructLayout(LayoutKind.Sequential)]
    public struct Error
    {
        public int Status;
        public nuint Offset;
        public byte* Message;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct View
    {
        public byte* Id;
        public nuint IdLen;
        public byte* Kind;
        public nuint KindLen;
        public int KindCode;
        public byte* Title;
        public nuint TitleLen;
        public byte* Direction;
        public nuint DirectionLen;
        public int Rtl;
        public byte* TextIndex;
        public nuint TextIndexLen;
        public nuint PageCount;
        public int HasContinuous;
        public float Gap;
        public float Tile;
        public float TileStated;
        public nuint ColumnRuns;
        public nuint RowRuns;
        public int HasFreeze;
        public long FreezeColumns;
        public long FreezeRows;
        public int Gridlines;
        public nuint TileCount;
        public byte* TilesRef;
        public nuint TilesRefLen;
        public int HasPlay;
        public byte* Seq;
        public nuint SeqLen;
        public byte* Audio;
        public nuint AudioLen;
        public byte* AudioType;
        public nuint AudioTypeLen;
        public byte* Cues;
        public nuint CuesLen;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Page
    {
        public float Width;
        public float Height;
        public int HasBody;
        public fixed float Body[4];
        public nuint LayerCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Image
    {
        public int Width;
        public int Height;
        public int X;
        public int Y;
        public byte* Pixels;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RenderOptions
    {
        public int HasBackground;
        public uint Background;
        public IntPtr Fonts;
    }

    // ---- The canvas (bdf_canvas and what its functions are given) ----

    public const byte VerbMove = 0, VerbLine = 1, VerbQuad = 2, VerbCubic = 3, VerbClose = 4;
    public const byte FillNonZero = 0, FillEvenOdd = 1;
    public const byte PaintLinear = 0, PaintRadial = 1, PaintConic = 2, PaintPattern = 3;
    public const byte Repeat = 0, RepeatX = 1, RepeatY = 2, NoRepeat = 3;

    [StructLayout(LayoutKind.Sequential)]
    public struct CanvasPath
    {
        public byte* Verbs;
        public nuint VerbCount;
        public double* Coords;
        public nuint CoordCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CanvasBitmap
    {
        public ulong Id;
        public int Width;
        public int Height;
        public byte* Pixels;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Stop
    {
        public float Offset;
        public uint Color;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CanvasPaint
    {
        public byte Kind;
        public float* Coords;
        public nuint CoordCount;
        public Stop* Stops;
        public nuint StopCount;
        public CanvasBitmap* Image;
        public byte Repeat;
        public fixed double Matrix[6];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CanvasStyle
    {
        public uint Color;
        public CanvasPaint* Paint;
        public float Alpha;
        public byte Blend;
        public uint ShadowColor;
        public double ShadowBlur;
        public double ShadowDx;
        public double ShadowDy;
        public int Smoothing;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CanvasLine
    {
        public double Width;
        public byte Cap;
        public byte Join;
        public double Miter;
        public float* Dash;
        public nuint DashCount;
        public double DashOffset;
    }

    /// <summary>bdf_canvas: the functions a page is played through. Text is
    /// left as null: libbdf then gives the outlines of the glyphs.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Canvas
    {
        public IntPtr Ctx;
        public delegate* unmanaged[Cdecl]<IntPtr, void> Save;
        public delegate* unmanaged[Cdecl]<IntPtr, void> Restore;
        public delegate* unmanaged[Cdecl]<IntPtr, double*, void> Transform;
        public delegate* unmanaged[Cdecl]<IntPtr, CanvasPath*, byte, void> Clip;
        public delegate* unmanaged[Cdecl]<IntPtr, CanvasPath*, byte, CanvasStyle*, void> Fill;
        public delegate* unmanaged[Cdecl]<IntPtr, CanvasPath*, CanvasLine*, CanvasStyle*, void> Stroke;
        public delegate* unmanaged[Cdecl]<IntPtr, CanvasBitmap*, double*, double*, CanvasStyle*, void> Image;
        public delegate* unmanaged[Cdecl]<IntPtr, double*, CanvasStyle*, void> BeginLayer;
        public delegate* unmanaged[Cdecl]<IntPtr, void> EndLayer;
        public IntPtr Text;
    }

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern byte* bdf_version();

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_doc_open_memory(byte* data, nuint len, IntPtr* doc, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void bdf_doc_free(IntPtr doc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_doc_locked(IntPtr doc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_doc_unlock(IntPtr doc, byte* password, nuint len, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern nuint bdf_doc_view_count(IntPtr doc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_doc_view(IntPtr doc, nuint i, View* view);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_doc_page(IntPtr doc, nuint view, nuint page, Page* outPage);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_fonts_new(IntPtr* fonts, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void bdf_fonts_free(IntPtr fonts);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_fonts_add(IntPtr fonts, byte* data, nuint len, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_fonts_add_file(IntPtr fonts, byte* path, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern nuint bdf_fonts_count(IntPtr fonts);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_renderer_new(IntPtr doc, RenderOptions* options, IntPtr* renderer, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void bdf_renderer_free(IntPtr renderer);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_render_page(IntPtr renderer, nuint view, nuint page, double scale, Image* image, Error* err);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void bdf_image_free(Image* image);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern nuint bdf_renderer_warning_count(IntPtr renderer);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern byte* bdf_renderer_warning(IntPtr renderer, nuint i);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int bdf_play_page(IntPtr renderer, nuint view, nuint page, double scale, Canvas* canvas, Error* err);

    /// <summary>The text libbdf gave with n bytes at p.</summary>
    public static string Text(byte* p, nuint n) =>
        p == null || n == 0 ? "" : System.Text.Encoding.UTF8.GetString(p, checked((int)n));

    /// <summary>The text of a string that ends with a zero.</summary>
    public static string Text(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((IntPtr)p) ?? "";
}
