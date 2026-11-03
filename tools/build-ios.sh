#!/bin/sh
# Prepares what the iOS app links and bundles: the core for iPhone and for the simulator, the fonts, the references.
# Copyright 2026 The Prismark Authors. Apache-2.0.
set -eu
cd "$(dirname "$0")/.."
app=apps/ios/Prismark/Prismark
out=build/ios-core

for sdk in iphoneos iphonesimulator; do
    cmake -S . -B "build/ios-$sdk" -G Xcode \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios-arm64.cmake -DCMAKE_OSX_SYSROOT="$sdk"
    cmake --build "build/ios-$sdk" --config Release -- -sdk "$sdk" -quiet CODE_SIGNING_ALLOWED=NO
    mkdir -p "$out/$sdk"
    (cd "build/ios-$sdk/Release-$sdk" && libtool -static -o libprismark_all.a \
        libprismark.a libpmk_kernels_baseline.a libpmk_stats.a libpmk_result.a)
    cp "build/ios-$sdk/Release-$sdk/libprismark_all.a" "$out/$sdk/"
done

mkdir -p "$app/Fonts"
fetch() { # url file sha256
    [ -f "$app/Fonts/$2" ] && echo "$3  $app/Fonts/$2" | shasum -a 256 -c --status && return 0
    curl -sSfL --retry 3 -o "$app/Fonts/$2" "$1"
    echo "$3  $app/Fonts/$2" | shasum -a 256 -c --status
}
gf=https://raw.githubusercontent.com/google/fonts/6157dfaa640ca5738b4d1a7070fe389c3036d7fc/ofl
fetch "$gf/saira/Saira%5Bwdth,wght%5D.ttf" Saira.ttf 9d050fc5a01c85f74c4257c207d50b55d1e40c37308c642f974a2c5003231dde
fetch "$gf/archivo/Archivo%5Bwdth,wght%5D.ttf" Archivo.ttf 0e094a7d3c7c4c25cf1310c4b30014f1dae9332220b1c2c88f4fa996f0b05053
fetch "$gf/ibmplexmono/IBMPlexMono-Regular.ttf" IBMPlexMono-Regular.ttf 6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46
fetch "$gf/ibmplexmono/IBMPlexMono-SemiBold.ttf" IBMPlexMono-SemiBold.ttf d3c38e55c78f5b0f28009fddba4834ec503278936a5986032424c9bd2d23aa46

python3 tools/site_data.py "$out/data.json"
cp "$out/references.json" "$app/references.json"
echo "Ready. Open apps/ios/Prismark/Prismark.xcodeproj and press Cmd+R."
