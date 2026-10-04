// swift-tools-version:5.9
// Swift wrapper over the Prismark C ABI for iOS/iPadOS (and macOS).
//
// Build the static libraries first (cmake --preset ios-arm64 && cmake --build --preset ios-arm64),
// combine them and package them as Prismark.xcframework next to this file:
//   cd build/ios-arm64/Release-iphoneos
//   libtool -static -o libprismark_all.a libprismark.a libpmk_kernels_baseline.a libpmk_stats.a libpmk_result.a
//   xcodebuild -create-xcframework -library libprismark_all.a -headers ../../../include \
//     -output ../../../bindings/swift/Prismark.xcframework
import PackageDescription

let package = Package(
    name: "Prismark",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [.library(name: "Prismark", targets: ["Prismark"])],
    targets: [
        .binaryTarget(name: "PrismarkCore", path: "Prismark.xcframework"),
        .target(name: "CPrismark", dependencies: ["PrismarkCore"], path: "Sources/CPrismark"),
        .target(name: "Prismark", dependencies: ["CPrismark"], path: "Sources/Prismark"),
    ]
)
