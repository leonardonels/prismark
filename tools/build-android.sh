#!/usr/bin/env bash
# Builds Prismark for Android (apps/android): the core, every kernel and the JNI bridge with the android-arm64
# CMake preset (NDK Clang), the pinned fonts the app shows (docs/design.md), and the APK with Gradle.
#
#   tools/build-android.sh [debug|release]      (default: debug, installable as is)
#
# Needs ANDROID_NDK (or ANDROID_NDK_HOME), the Android SDK (ANDROID_HOME, or sdk.dir in apps/android/local.properties),
# CMake 3.25 or later, Ninja and a JDK 17 or later. The APK is copied to build/android/.
#
# Copyright 2026 The Prismark Authors. Apache-2.0.
set -euo pipefail

type=${1:-debug}
case $type in debug | release) ;; *) echo "usage: $0 [debug|release]" >&2; exit 2 ;; esac
root=$(cd "$(dirname "$0")/.." && pwd)
app=$root/apps/android/app/src/main
cd "$root"

# 1. The native library. Everything links statically into libprismark_jni.so; the preset also builds the
#    command line for Android (build/android-arm64/prismark), which runs from `adb shell`.
cmake --preset android-arm64
cmake --build --preset android-arm64
mkdir -p "$app/jniLibs/arm64-v8a"
cp build/android-arm64/libprismark_jni.so "$app/jniLibs/arm64-v8a/"

# 2. Fonts, the same files and hashes as the desktop app (apps/gui/gui.cmake). Without them the app uses the
#    system fonts, so a failed download only warns.
fonts=$app/assets/fonts
mkdir -p "$fonts"
fetch() { # url file sha256
  [ -f "$fonts/$2" ] && echo "$3  $fonts/$2" | sha256sum -c --status && return 0
  if curl -sSfL --retry 3 -o "$fonts/$2.part" "$1" && echo "$3  $fonts/$2.part" | sha256sum -c --status; then
    mv "$fonts/$2.part" "$fonts/$2"
  else
    rm -f "$fonts/$2.part"
    echo "warning: $2 not fetched; the app uses the system font instead" >&2
  fi
}
inter=https://raw.githubusercontent.com/rsms/inter/v3.19/docs/font-files
gf=https://raw.githubusercontent.com/google/fonts/6157dfaa640ca5738b4d1a7070fe389c3036d7fc/ofl
fetch $inter/Inter-Regular.otf Inter-Regular.otf a7e791e8f5a0fb02b65663f7fca73e1d1ca9543f772ad480cbd76f4e3fe3f8cc
fetch $inter/Inter-Medium.otf Inter-Medium.otf 99dab2bdcb613c4c8264000a94351d1227f74dc95a86d1249493aeee0c0179c4
fetch $inter/Inter-SemiBold.otf Inter-SemiBold.otf 8c1990b6012254ea2b487161697d107357dd0ee55811cfd91c8c11227bbef457
fetch $inter/Inter-Bold.otf Inter-Bold.otf 1e9dfd6a6e33ac63a8fe3b4ed7ae0df9eac2d0b25e444e8e7daef4ac77943fc1
fetch "$gf/saira/Saira%5Bwdth,wght%5D.ttf" Saira.ttf 9d050fc5a01c85f74c4257c207d50b55d1e40c37308c642f974a2c5003231dde
fetch $gf/ibmplexmono/IBMPlexMono-Regular.ttf IBMPlexMono-Regular.ttf 6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46
fetch $gf/ibmplexmono/IBMPlexMono-Medium.ttf IBMPlexMono-Medium.ttf a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755
fetch $gf/ibmplexmono/IBMPlexMono-SemiBold.ttf IBMPlexMono-SemiBold.ttf d3c38e55c78f5b0f28009fddba4834ec503278936a5986032424c9bd2d23aa46

# 3. The APK. A release build is signed when PRISMARK_ANDROID_KEYSTORE is set (docs/packaging.md), else unsigned;
#    a debug build is signed with the local debug key, so one built elsewhere must be uninstalled before another installs over it.
task=assemble${type^}
(cd apps/android && ./gradlew --no-daemon "$task")
version=$(sed -n 's/^project(prismark VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
mkdir -p build/android
src=$(ls apps/android/app/build/outputs/apk/"$type"/*.apk | head -1)
out=build/android/prismark-$version-android-arm64-$type.apk
cp "$src" "$out"
echo "$out"
