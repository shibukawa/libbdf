// A small Swift layer over libbdf: a document, its views and pages, and its
// pages drawn into a CGContext or as a CGImage. The whole C API is there as
// well, in the module CBDF.
import CBDF
import CBDFCoreGraphics
import CoreGraphics
import Foundation

/// What libbdf could not do, with what it said.
public struct BDFError: Error, CustomStringConvertible {
    public enum Code: Sendable {
        case format, argument, tooLarge, noMemory, unsupported, password, other(Int32)
    }

    public let code: Code
    public let message: String

    public var description: String { message }

    init(_ status: bdf_status, _ error: bdf_error) {
        switch status {
        case BDF_ERR_FORMAT: code = .format
        case BDF_ERR_ARG: code = .argument
        case BDF_ERR_TOO_LARGE: code = .tooLarge
        case BDF_ERR_NOMEM: code = .noMemory
        case BDF_ERR_UNSUPPORTED: code = .unsupported
        case BDF_ERR_PASSWORD: code = .password
        default: code = .other(Int32(status.rawValue))
        }
        message = error.message.map { String(cString: $0) } ?? ""
    }
}

/// A set of fonts for the text that names its font (see `bdf_fonts`): the
/// application says which fonts there are.
public final class BDFFonts {
    let pointer: OpaquePointer

    public init() throws {
        var out: OpaquePointer?
        var error = bdf_error()
        let status = bdf_fonts_new(&out, &error)
        guard status == BDF_OK, let out else { throw BDFError(status, error) }
        pointer = out
    }

    deinit { bdf_fonts_free(pointer) }

    /// Adds a font file that is in memory (TrueType, OpenType, or a collection).
    public func add(_ data: Data) throws {
        var error = bdf_error()
        let status = data.withUnsafeBytes { bytes in
            bdf_fonts_add(pointer, bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count, &error)
        }
        guard status == BDF_OK else { throw BDFError(status, error) }
    }

    /// Adds a font file by its path; its glyphs are read when they are first drawn.
    public func add(contentsOf url: URL) throws {
        var error = bdf_error()
        let status = bdf_fonts_add_file(pointer, url.path, &error)
        guard status == BDF_OK else { throw BDFError(status, error) }
    }

    public var count: Int { bdf_fonts_count(pointer) }
}

/// A BDF document in the single-file form.
public final class BDFDocument {
    public enum ViewKind: Sendable { case fixed, flow, sheet, scroll, unknown }

    public struct View: Sendable {
        public let id: String
        public let kind: ViewKind
        public let title: String
        public let pageCount: Int
        public let rightToLeft: Bool
    }

    let pointer: OpaquePointer
    private let bytes: UnsafeMutableRawBufferPointer // the document reads from them while it lives

    /// Opens a document from the bytes of its file.
    public init(data: Data) throws {
        bytes = UnsafeMutableRawBufferPointer.allocate(byteCount: max(data.count, 1), alignment: 1)
        data.copyBytes(to: bytes)
        var out: OpaquePointer?
        var error = bdf_error()
        let status = bdf_doc_open_memory(bytes.bindMemory(to: UInt8.self).baseAddress, data.count, &out, &error)
        guard status == BDF_OK, let out else {
            bytes.deallocate()
            throw BDFError(status, error)
        }
        pointer = out
    }

    public convenience init(contentsOf url: URL) throws {
        try self.init(data: Data(contentsOf: url))
    }

    deinit {
        bdf_doc_free(pointer)
        bytes.deallocate()
    }

    /// Whether the document is encrypted and not unlocked yet: it has no views then.
    public var isLocked: Bool { bdf_doc_locked(pointer) != 0 }

    /// Unlocks an encrypted document with its password.
    public func unlock(password: String) throws {
        var error = bdf_error()
        let utf8 = Array(password.utf8)
        let status = utf8.withUnsafeBufferPointer { p in
            p.withMemoryRebound(to: CChar.self) { bdf_doc_unlock(pointer, $0.baseAddress, $0.count, &error) }
        }
        guard status == BDF_OK else { throw BDFError(status, error) }
    }

    public var views: [View] {
        (0..<bdf_doc_view_count(pointer)).compactMap { i in
            var v = bdf_view()
            guard bdf_doc_view(pointer, i, &v) == BDF_OK else { return nil }
            let kind: ViewKind
            switch v.kind_code {
            case BDF_VIEW_FIXED: kind = .fixed
            case BDF_VIEW_FLOW: kind = .flow
            case BDF_VIEW_SHEET: kind = .sheet
            case BDF_VIEW_SCROLL: kind = .scroll
            default: kind = .unknown
            }
            return View(id: BDFDocument.string(v.id, v.id_len), kind: kind, title: BDFDocument.string(v.title, v.title_len),
                        pageCount: v.npages, rightToLeft: v.rtl != 0)
        }
    }

    /// The size of a page of a view, in units of the page (points).
    public func pageSize(view: Int = 0, page: Int) -> CGSize? {
        var p = bdf_page()
        guard view >= 0, page >= 0, bdf_doc_page(pointer, view, page, &p) == BDF_OK else { return nil }
        return CGSize(width: CGFloat(p.w), height: CGFloat(p.h))
    }

    private static func string(_ p: UnsafePointer<CChar>?, _ n: Int) -> String {
        guard let p, n > 0 else { return "" }
        return String(decoding: UnsafeRawBufferPointer(start: p, count: n), as: UTF8.self)
    }
}

/// What draws the pages of a document. It keeps what it decoded of the
/// document from one drawing to the next; use it from one thread at a time.
public final class BDFRenderer {
    public let document: BDFDocument
    public let fonts: BDFFonts?
    private let pointer: OpaquePointer
    private let adapter: OpaquePointer

    public init(document: BDFDocument, fonts: BDFFonts? = nil) throws {
        self.document = document
        self.fonts = fonts
        var options = bdf_render_options()
        options.fonts = fonts?.pointer
        var out: OpaquePointer?
        var error = bdf_error()
        let status = bdf_renderer_new(document.pointer, &options, &out, &error)
        guard status == BDF_OK, let out else { throw BDFError(status, error) }
        guard let adapter = bdf_cg_new(out) else {
            bdf_renderer_free(out)
            throw BDFError(BDF_ERR_NOMEM, bdf_error())
        }
        pointer = out
        self.adapter = adapter
    }

    deinit {
        bdf_cg_free(adapter)
        bdf_renderer_free(pointer)
    }

    /// Draws a page with Core Graphics, in the coordinates of the context as
    /// they are: a unit of the page is a point, the page's origin at the
    /// origin, y down. (Flip a context whose y goes up first:
    /// `translateBy(x: 0, y: height)`, `scaleBy(x: 1, y: -1)`.) The paper is
    /// the caller's to fill.
    public func draw(view: Int = 0, page: Int, in context: CGContext) throws {
        var error = bdf_error()
        let status = bdf_cg_draw_page(adapter, view, page, context, &error)
        guard status == BDF_OK else { throw BDFError(status, error) }
    }

    /// A page as an image drawn by libbdf's own rasterizer, on white: the
    /// same pixels on every platform. `scale` is pixels a unit of the page.
    public func image(view: Int = 0, page: Int, scale: CGFloat = 1) throws -> CGImage {
        var img = bdf_image()
        var error = bdf_error()
        let status = bdf_render_page(pointer, view, page, Double(scale), &img, &error)
        guard status == BDF_OK else { throw BDFError(status, error) }
        defer { bdf_image_free(&img) }
        let count = 4 * Int(img.width) * Int(img.height)
        guard let provider = CGDataProvider(data: Data(bytes: img.pixels, count: count) as CFData),
              let image = CGImage(width: Int(img.width), height: Int(img.height), bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: 4 * Int(img.width), space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { throw BDFError(BDF_ERR_NOMEM, bdf_error()) }
        return image
    }

    /// What could not be drawn as the viewer would draw it, each said once.
    public var warnings: [String] {
        (0..<bdf_renderer_warning_count(pointer)).map { String(cString: bdf_renderer_warning(pointer, $0)) }
    }
}
