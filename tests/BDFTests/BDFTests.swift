// Tests of the Swift layer over libbdf, on samples/two-pages.bdf: two pages of
// 100 by 80; on the first a red rectangle from (10, 10) to (40, 30) and a blue
// one at half its alpha from (30, 20) to (60, 50); on the second a green
// circle around (50, 40).
import CoreGraphics
import Foundation
import XCTest

@testable import BDF

/// samples/two-pages.bdf in base64 (go run -C go ./genvectors ../tests sample
/// writes the file).
private let twoPages = [
    "YmRmAAEAAAAgAAAAAAAAAPYBAAAAAAAAAAAAAAAAAAB7ImJkZiI6MSwib3BzZXQiOjEsInVuaXQiOiJwdCIsIm1ldGEiOnsi",
    "Z2VuZXJhdG9yIjoiYmRmLWdvLzAuMSJ9LCJ2aWV3cyI6W3siaWQiOiJwYWdlcyIsImtpbmQiOiJmaXhlZCIsInRpdGxlIjoi",
    "UGFnZXMiLCJwYWdlcyI6W3sidyI6MTAwLCJoIjo4MCwibGF5ZXJzIjpbeyJyb2xlIjoiYm9keSIsIm9iaiI6ImJkZmQ5MmE2",
    "Mjk4MTg3YTQ3MjI4YmJkZDNmNDkwNTU1In1dfSx7InciOjEwMCwiaCI6ODAsImxheWVycyI6W3sicm9sZSI6ImJvZHkiLCJv",
    "YmoiOiI0OWY3YjM0NmY4YWY1M2IyNTJiMzZmOTIxMzYzYTZjMyJ9XX1dfV0sInBhcnRzIjpbeyJoIjoiYmRmZDkyYTYyOTgx",
    "ODdhNDcyMjhiYmRkM2Y0OTA1NTUiLCJ0Ijoib2JqIiwiZW5jIjoiaWRlbnRpdHkiLCJsZW4iOjc1LCJzaXplIjo3NSwib2Zm",
    "IjowfSx7ImgiOiI0OWY3YjM0NmY4YWY1M2IyNTJiMzZmOTIxMzYzYTZjMyIsInQiOiJvYmoiLCJlbmMiOiJpZGVudGl0eSIs",
    "ImxlbiI6NzQsInNpemUiOjc0LCJvZmYiOjc1fV19Qk9CSgEAAAAAAAAAAAAAAAAAyEIAAKBCAAAAAAAALBD/AAD/IAAAIEEA",
    "ACBBAADwQQAAoEEQgP8AACAAAPBBAACgQQAA8EEAAPBBQk9CSgEAAAAAAAAAAAAAAAAAyEIAAKBCAAEAAQYAAEhCAAAgQgAA",
    "yEEAAMhBAAAAAAAAAADbD8lAAAAAAAAAAAAIEP8AoAAiAAA=",
].joined()

final class BDFTests: XCTestCase {
    private func document() throws -> BDFDocument {
        try BDFDocument(data: XCTUnwrap(Data(base64Encoded: twoPages)))
    }

    /// The colour of a pixel of an image, as 0xRRGGBBAA.
    private func pixels(of image: CGImage) throws -> (Int, Int) -> UInt32 {
        let data = try XCTUnwrap(image.dataProvider?.data) as Data
        let row = image.bytesPerRow
        return { x, y in
            let i = y * row + 4 * x
            return UInt32(data[i]) << 24 | UInt32(data[i + 1]) << 16 | UInt32(data[i + 2]) << 8 | UInt32(data[i + 3])
        }
    }

    func testViewsAndPages() throws {
        let doc = try document()
        XCTAssertFalse(doc.isLocked)
        let views = doc.views
        XCTAssertEqual(views.count, 1)
        XCTAssertEqual(views[0].id, "pages")
        XCTAssertEqual(views[0].kind, .fixed)
        XCTAssertEqual(views[0].title, "Pages")
        XCTAssertEqual(views[0].pageCount, 2)
        XCTAssertEqual(doc.pageSize(page: 0), CGSize(width: 100, height: 80))
        XCTAssertNil(doc.pageSize(page: 2))
        XCTAssertNil(doc.pageSize(view: 1, page: 0))
    }

    func testWhatIsNoDocument() {
        XCTAssertThrowsError(try BDFDocument(data: Data("not a document".utf8))) { error in
            XCTAssertFalse(String(describing: error).isEmpty)
        }
        XCTAssertThrowsError(try BDFDocument(data: Data()))
    }

    func testRasterizer() throws {
        let renderer = try BDFRenderer(document: document())
        let image = try renderer.image(page: 0, scale: 2)
        XCTAssertEqual(image.width, 200)
        XCTAssertEqual(image.height, 160)
        let at = try pixels(of: image)
        XCTAssertEqual(at(10, 10), 0xffff_ffff) // the paper
        XCTAssertEqual(at(30, 30), 0xff00_00ff) // red
        XCTAssertEqual(at(100, 80), 0x7f7f_ffff) // blue at half its alpha, on the paper
        XCTAssertEqual(at(70, 50), 0x7f00_80ff) // and on the red
        let second = try pixels(of: renderer.image(page: 1))
        XCTAssertEqual(second(50, 40), 0x00a0_00ff)
        XCTAssertEqual(second(5, 5), 0xffff_ffff)
        XCTAssertThrowsError(try renderer.image(page: 2))
        XCTAssertEqual(renderer.warnings, [])
    }

    func testCoreGraphics() throws {
        let renderer = try BDFRenderer(document: document())
        let width = 200, height = 160
        let context = try XCTUnwrap(CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 4 * width,
                                               space: CGColorSpaceCreateDeviceRGB(),
                                               bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        context.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        // the page's origin at the top left, y down, two pixels a unit
        context.translateBy(x: 0, y: CGFloat(height))
        context.scaleBy(x: 2, y: -2)
        try renderer.draw(page: 0, in: context)
        let at = try pixels(of: XCTUnwrap(context.makeImage()))
        XCTAssertEqual(at(10, 10), 0xffff_ffff)
        XCTAssertEqual(at(30, 30), 0xff00_00ff)
        // (Core Graphics rounds a half its own way: within one of the rasterizer)
        let mixed = at(100, 80)
        XCTAssertLessThanOrEqual(abs(Int(mixed >> 24) - 0x7f), 1)
        XCTAssertEqual(mixed & 0xffff, 0xffff)
        XCTAssertThrowsError(try renderer.draw(page: 2, in: context))
    }

    func testFonts() throws {
        let fonts = try BDFFonts()
        XCTAssertEqual(fonts.count, 0)
        XCTAssertThrowsError(try fonts.add(Data("not a font".utf8)))
        XCTAssertThrowsError(try fonts.add(contentsOf: URL(fileURLWithPath: "/nonexistent/font.ttf")))
        // a renderer keeps its fonts and its document alive
        let renderer = try BDFRenderer(document: document(), fonts: fonts)
        XCTAssertNotNil(renderer.fonts)
        XCTAssertEqual(try renderer.image(page: 0).width, 100)
    }
}
