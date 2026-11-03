#!/bin/sh
# Builds the iOS and iPadOS app into an unsigned .ipa, for a release: build/ios-package/prismark-VERSION-ios-arm64-unsigned.ipa.
# Runs on a Mac, not in CI: the project is in the format of Xcode 27, which the CI machines don't have.
# Unsigned, so an iPhone or iPad won't install it as it is: a sideloading tool signs it with the user's own Apple ID.
# Copyright 2026 The Prismark Authors. Apache-2.0.
set -eu
cd "$(dirname "$0")/.."
version=$(sed -n 's/^project(prismark VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
out=build/ios-package

sh tools/build-ios.sh

rm -rf "$out"
xcodebuild -project apps/ios/Prismark/Prismark.xcodeproj -scheme Prismark -configuration Release \
    -destination generic/platform=iOS -derivedDataPath "$out/derived" -quiet \
    CODE_SIGNING_ALLOWED=NO MARKETING_VERSION="$version" build

app=$out/derived/Build/Products/Release-iphoneos/Prismark.app
mkdir "$out/Payload"
cp -R "$app" "$out/Payload/"
ipa=prismark-$version-ios-arm64-unsigned.ipa
(cd "$out" && zip -qry "$ipa" Payload)
echo "$out/$ipa"
