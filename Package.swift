// swift-tools-version:5.9
//
// libbdf as a Swift package: the C library built from its sources (CBDF), the
// Core Graphics adapter (CBDFCoreGraphics), and BDF, a small Swift layer over
// the two for opening a document and drawing its pages.
//
//   .package(url: "https://github.com/shibukawa/libbdf.git", from: "0.8.0")
//   .product(name: "BDF", package: "libbdf")
import PackageDescription

let package = Package(
    name: "libbdf",
    platforms: [.macOS(.v11), .iOS(.v14), .tvOS(.v14), .watchOS(.v7), .visionOS(.v1)],
    products: [
        .library(name: "BDF", targets: ["BDF"]),
        .library(name: "CBDF", targets: ["CBDF"]),
        .library(name: "CBDFCoreGraphics", targets: ["CBDFCoreGraphics"]),
    ],
    targets: [
        // The C library: the sources of src/ and the Brotli decoder WOFF2
        // fonts are unpacked with. Its arithmetic is not fused (the sources
        // of the rasterizer say so themselves, with a pragma).
        .target(
            name: "CBDF",
            path: ".",
            exclude: ["src/brotli_none.c"],
            sources: ["src", "third_party/brotli/common", "third_party/brotli/dec"],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("src"),
                .headerSearchPath("third_party/brotli/include"),
            ]
        ),
        .target(
            name: "CBDFCoreGraphics",
            dependencies: ["CBDF"],
            path: "adapters/coregraphics",
            exclude: ["compare_coregraphics.c"],
            sources: ["bdf_coregraphics.c"],
            publicHeadersPath: "include",
            linkerSettings: [.linkedFramework("CoreGraphics"), .linkedFramework("CoreFoundation")]
        ),
        .target(name: "BDF", dependencies: ["CBDF", "CBDFCoreGraphics"], path: "Sources/BDF"),
        .testTarget(name: "BDFTests", dependencies: ["BDF"], path: "Tests/BDFTests"),
    ],
    cLanguageStandard: .c11
)
