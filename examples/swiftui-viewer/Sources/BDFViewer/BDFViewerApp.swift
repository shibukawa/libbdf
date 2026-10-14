// A viewer of BDF documents with SwiftUI: a page is drawn with Core Graphics
// (BDFRenderer.draw, libbdf's Core Graphics adapter) in a SwiftUI Canvas,
// so it stays sharp at any zoom and on any display.
//
//   swift run BDFViewer doc.bdf
//   swift run BDFViewer doc.bdf --snapshot page.png [--page N]   (a page, without a window)
import AppKit
import BDF
import SwiftUI
import UniformTypeIdentifiers

@main
struct BDFViewerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = ViewerModel()

    var body: some Scene {
        WindowGroup("BDF") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 480, minHeight: 360)
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open…") { model.choose() }
                    .keyboardShortcut("o")
            }
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        let args = CommandLine.arguments
        if args.count > 3, let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count {
            let page = args.firstIndex(of: "--page").flatMap { $0 + 1 < args.count ? Int(args[$0 + 1]) : nil } ?? 1
            exit(Snapshot.write(document: URL(fileURLWithPath: args[1]), page: page - 1,
                                to: URL(fileURLWithPath: args[i + 1])) ? 0 : 1)
        }
        // Run from a terminal (swift run) the program is no app bundle: make
        // it an app with a window and a menu, in front.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

// MARK: - The document

@MainActor
final class ViewerModel: ObservableObject {
    @Published private(set) var document: BDFDocument?
    @Published private(set) var renderer: BDFRenderer?
    @Published private(set) var name = ""
    @Published var view = 0
    @Published var page = 0
    @Published var zoom: CGFloat = 0 // 0: fit the window
    @Published var askPassword = false
    @Published var message: String?

    init() {
        let args = CommandLine.arguments
        if args.count > 1, !args[1].hasPrefix("-") {
            open(URL(fileURLWithPath: args[1]))
        }
    }

    /// The views that have pages: fixed and flow views, and the strips of a
    /// scroll view. (A sheet is drawn tile by tile: see sdl-viewer.)
    var views: [(index: Int, view: BDFDocument.View)] {
        guard let document, !document.isLocked else { return [] }
        return document.views.enumerated()
            .filter { $0.element.pageCount > 0 && $0.element.kind != .sheet }
            .map { (index: $0.offset, view: $0.element) }
    }

    var pageCount: Int { views.first { $0.index == view }?.view.pageCount ?? 0 }

    var pageSize: CGSize? { document?.pageSize(view: view, page: page) }

    func choose() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "bdf") ?? .data]
        if panel.runModal() == .OK, let url = panel.url {
            open(url)
        }
    }

    func open(_ url: URL) {
        do {
            let document = try BDFDocument(contentsOf: url)
            self.document = document
            renderer = nil
            name = url.lastPathComponent
            if document.isLocked {
                askPassword = true // an encrypted document: its views come with the password
            } else {
                try start()
            }
        } catch {
            message = "\(url.lastPathComponent): \(error)"
        }
    }

    func unlock(_ password: String) {
        do {
            try document?.unlock(password: password)
            askPassword = false
            try start()
        } catch let error as BDFError {
            if case .password = error.code {
                message = "The password does not open the document."
            } else {
                message = "\(error)"
            }
        } catch {
            message = "\(error)"
        }
    }

    private func start() throws {
        guard let document else { return }
        renderer = try BDFRenderer(document: document, fonts: Self.systemFonts())
        view = views.first?.index ?? 0
        page = 0
        zoom = 0
    }

    func step(_ by: Int) {
        page = min(max(page + by, 0), max(pageCount - 1, 0))
    }

    /// Fonts for text that names its font (documents made from HTML,
    /// Markdown or EPUB): libbdf reads no font directories, the application
    /// says which fonts there are.
    static func systemFonts() -> BDFFonts? {
        guard let fonts = try? BDFFonts() else { return nil }
        for path in ["/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Times.ttc",
                     "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Courier.ttc",
                     "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/ヒラギノ明朝 ProN.ttc",
                     "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc"] {
            try? fonts.add(contentsOf: URL(fileURLWithPath: path))
        }
        return fonts.count > 0 ? fonts : nil
    }
}

// MARK: - Views

struct ContentView: View {
    @EnvironmentObject private var model: ViewerModel
    @State private var password = ""

    var body: some View {
        Group {
            if let renderer = model.renderer, let size = model.pageSize {
                GeometryReader { geometry in
                    let fit = min((geometry.size.width - 24) / size.width, (geometry.size.height - 24) / size.height)
                    let zoom = model.zoom > 0 ? model.zoom : max(fit, 0.05)
                    ScrollView([.horizontal, .vertical]) {
                        PageCanvas(renderer: renderer, view: model.view, page: model.page, size: size)
                            .frame(width: size.width * zoom, height: size.height * zoom)
                            .shadow(radius: 3)
                            .padding(12)
                            .frame(minWidth: geometry.size.width, minHeight: geometry.size.height)
                    }
                    .background(Color(white: 0.34))
                }
            } else {
                Text(model.document == nil ? "Open a BDF document with ⌘O" : "")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .navigationTitle(model.name.isEmpty ? "BDF" : model.name)
        .toolbar {
            ToolbarItemGroup {
                if model.views.count > 1 {
                    Picker("View", selection: $model.view) {
                        ForEach(model.views, id: \.index) { Text($0.view.title.isEmpty ? $0.view.id : $0.view.title).tag($0.index) }
                    }
                    .onChange(of: model.view) { _ in model.page = 0 }
                }
                Button { model.step(-1) } label: { Image(systemName: "chevron.left") }
                    .keyboardShortcut(.leftArrow, modifiers: [])
                    .disabled(model.page == 0)
                if model.pageCount > 0 {
                    Text("\(model.page + 1) / \(model.pageCount)")
                        .monospacedDigit()
                }
                Button { model.step(1) } label: { Image(systemName: "chevron.right") }
                    .keyboardShortcut(.rightArrow, modifiers: [])
                    .disabled(model.page + 1 >= model.pageCount)
                Button { model.zoom = 0 } label: { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
                    .keyboardShortcut("0")
                Button { model.zoom = max((model.zoom > 0 ? model.zoom : 1) * 0.8, 0.05) } label: { Image(systemName: "minus.magnifyingglass") }
                    .keyboardShortcut("-")
                Button { model.zoom = min((model.zoom > 0 ? model.zoom : 1) * 1.25, 32) } label: { Image(systemName: "plus.magnifyingglass") }
                    .keyboardShortcut("+")
            }
        }
        .sheet(isPresented: $model.askPassword) {
            VStack(alignment: .leading, spacing: 12) {
                Text("\(model.name) is encrypted.")
                SecureField("Password", text: $password)
                    .onSubmit { model.unlock(password) }
                HStack {
                    Spacer()
                    Button("Cancel") { model.askPassword = false }
                    Button("Open") { model.unlock(password) }
                        .keyboardShortcut(.defaultAction)
                }
            }
            .padding()
            .frame(width: 320)
        }
        .alert(model.message ?? "", isPresented: Binding(get: { model.message != nil }, set: { if !$0 { model.message = nil } })) {
            Button("OK") { model.message = nil }
        }
    }
}

/// A page drawn by libbdf with Core Graphics, scaled to the size it is shown at.
struct PageCanvas: View {
    let renderer: BDFRenderer
    let view: Int
    let page: Int
    let size: CGSize

    var body: some View {
        Canvas { context, canvasSize in
            // the paper is the caller's
            context.fill(Path(CGRect(origin: .zero, size: canvasSize)), with: .color(.white))
            context.withCGContext { cg in
                // y goes down here, as libbdf draws; a unit of a page is a point
                cg.scaleBy(x: canvasSize.width / size.width, y: canvasSize.height / size.height)
                do {
                    try renderer.draw(view: view, page: page, in: cg)
                } catch {
                    NSLog("page %d: %@", page + 1, "\(error)")
                }
            }
        }
    }
}

// MARK: - A page without a window

/// A page of a document drawn by the same view the window shows,
/// through SwiftUI's ImageRenderer, into a PNG file: to see what the viewer
/// draws without a person at the screen.
@MainActor
enum Snapshot {
    static func write(document url: URL, page: Int, to out: URL) -> Bool {
        do {
            let document = try BDFDocument(contentsOf: url)
            if document.isLocked {
                try document.unlock(password: ProcessInfo.processInfo.environment["BDF_PASSWORD"] ?? "")
            }
            let renderer = try BDFRenderer(document: document, fonts: ViewerModel.systemFonts())
            guard let view = document.views.firstIndex(where: { $0.pageCount > 0 && $0.kind != .sheet }),
                  let size = document.pageSize(view: view, page: page) else {
                FileHandle.standardError.write(Data("\(url.lastPathComponent): no view with pages\n".utf8))
                return false
            }
            let image = ImageRenderer(content: PageCanvas(renderer: renderer, view: view, page: page, size: size)
                .frame(width: size.width, height: size.height))
            image.scale = 2
            guard let cg = image.cgImage,
                  let file = CGImageDestinationCreateWithURL(out as CFURL, UTType.png.identifier as CFString, 1, nil) else {
                return false
            }
            CGImageDestinationAddImage(file, cg, nil)
            guard CGImageDestinationFinalize(file) else { return false }
            print("\(out.path): \(cg.width) × \(cg.height)")
            return true
        } catch {
            FileHandle.standardError.write(Data("\(url.lastPathComponent): \(error)\n".utf8))
            return false
        }
    }
}
