# swiftui-viewer

A viewer for macOS with SwiftUI, over libbdf's Swift package (`BDF`): a page is drawn with Core Graphics by `BDFRenderer.draw(view:page:in:)` inside a SwiftUI `Canvas`, so it stays sharp at any zoom and on any display.

```bash
swift run BDFViewer ../../samples/two-pages.bdf
swift run BDFViewer doc.bdf --snapshot page.png --page 2     # a page, without a window
```

⌘O opens a document; ← → turn the pages; ⌘0, ⌘− and ⌘+ zoom; the toolbar chooses the view. An encrypted document asks for its password (`BDFDocument.unlock(password:)`).

`Package.swift` takes libbdf from this checkout (`path: "../.."`); a project of your own takes it from the repository:

```swift
.package(url: "https://github.com/shibukawa/libbdf.git", from: "0.8.0")
.product(name: "BDF", package: "libbdf")
```

`withCGContext` of a SwiftUI `GraphicsContext` gives a context whose y goes down, as libbdf draws: the page needs no flip. In a context of your own whose y goes up (a bitmap context, a PDF), flip it first, as the comment of `BDFRenderer.draw` says.
