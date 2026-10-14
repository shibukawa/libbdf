// swift-tools-version:5.9
//
// A viewer of BDF documents for macOS with SwiftUI, over the Swift package
// of libbdf (its product BDF). A project of your own takes the package from
// its repository instead of from this checkout:
//
//   .package(url: "https://github.com/shibukawa/libbdf.git", from: "0.8.0")
//
//   swift run BDFViewer ../../samples/two-pages.bdf
import PackageDescription

let package = Package(
    name: "BDFViewer",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(name: "libbdf", path: "../.."),
    ],
    targets: [
        .executableTarget(
            name: "BDFViewer",
            dependencies: [.product(name: "BDF", package: "libbdf")]
        ),
    ]
)
