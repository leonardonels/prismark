#!/usr/bin/env bash
# Builds Prismark for macOS (Apple silicon or x86-64, the machine's own) and packs Prismark.app into a disk image:
# open it and drag the app to Applications. The app holds the runner (Contents/MacOS/prismark) and Qt; the
# command line is the runner itself.
#
#   tools/package-macos.sh QT [OUT_DIR]
#
#   QT    Qt 6 for macOS, e.g. .../Qt/6.10.3/macos (aqt install-qt mac desktop 6.10.3 clang_64 -O Qt; 6.8 does not
#         link with the macOS 26 SDK, which has no AGL)
#
# Clang 19 comes from Homebrew (brew install llvm@19 ninja), as for the macos-clang preset. The app runs on the
# oldest macOS both the build (13.0) and the Qt it bundles support. The compile tests are not packed yet:
# "Compiling code" is not built in, and "Full software build" needs the one-time setup and Homebrew's build tools.
# The app is signed ad hoc, not notarised. The build goes to build/macos-bundle, or to $PRISMARK_BUILD_DIR when set.
set -euo pipefail

[ $# -ge 1 ] || { sed -n 2,14p "$0"; exit 2; }
QT=$(cd "$1" && pwd)
SRC=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "${2:-$SRC/build}"
OUT=$(cd "${2:-$SRC/build}" && pwd)
BUILD=${PRISMARK_BUILD_DIR:-$SRC/build/macos-bundle}
case $(uname -m) in arm64) ARCH=arm64 ARCH_NAME="Apple silicon" ;; x86_64) ARCH=x86_64 ARCH_NAME="Intel" ;;
  *) echo "no package for $(uname -m)"; exit 2 ;; esac
[ -x "$QT/bin/macdeployqt" ] || { echo "$QT/bin/macdeployqt not found: QT must be a Qt 6 for macOS"; exit 2; }

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/macos-clang.cmake" \
  -DCMAKE_PREFIX_PATH="$QT" -DPRISMARK_GUI=ON -DPRISMARK_K1=OFF -DBUILD_TESTING=OFF
cmake --build "$BUILD"
grep -q 'Qt6' "$BUILD/CMakeCache.txt" || { echo "the desktop app was not built with Qt 6 from $QT"; exit 1; }

VERSION=$(sed -n 's/^project(prismark VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
NAME=prismark-$VERSION-macos-$ARCH
STAGE=$OUT/$NAME
APP=$STAGE/Prismark.app
rm -rf "$STAGE" "$OUT/$NAME.dmg"
mkdir -p "$STAGE"
cp -R "$BUILD"/prismark-gui.app "$APP"
cp "$BUILD"/prismark "$APP"/Contents/MacOS/   # the runner, beside the app's executable where the app looks for it

# The icon, drawn by the app from its own vectors; name, version and identifier in Info.plist.
PRISMARK_GUI_ICONSET="$BUILD/Prismark.iconset" QT_QPA_PLATFORM=offscreen "$APP"/Contents/MacOS/prismark-gui
mkdir -p "$APP"/Contents/Resources
iconutil -c icns "$BUILD/Prismark.iconset" -o "$APP"/Contents/Resources/Prismark.icns
PLIST=$APP/Contents/Info.plist
for kv in "CFBundleIconFile Prismark" "CFBundleName Prismark" "CFBundleDisplayName Prismark" \
          "CFBundleShortVersionString $VERSION" "CFBundleVersion $VERSION" "LSMinimumSystemVersion 13.0"; do
  /usr/libexec/PlistBuddy -c "Delete :${kv%% *}" "$PLIST" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :${kv%% *} string ${kv#* }" "$PLIST"
done
/usr/libexec/PlistBuddy -c "Delete :NSHighResolutionCapable" "$PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$PLIST"

# Qt's frameworks and plugins into the app, and every program's search paths pointed at them; then an ad-hoc
# signature over the whole bundle (Apple silicon runs no unsigned code).
"$QT"/bin/macdeployqt "$APP" -executable="$APP"/Contents/MacOS/prismark -always-overwrite
codesign --force --deep --sign - "$APP"

# Check: nothing any program or library in the app needs comes from outside it, except macOS itself.
bad=0
while read -r f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  while read -r lib; do
    case $lib in /System/*|/usr/lib/*|@rpath/*|@executable_path/*|@loader_path/*) ;;
      *) echo "not from the package: $lib (needed by ${f#"$APP"/})"; bad=1 ;; esac
  done < <(otool -L "$f" | grep -v ':$' | awk '{print $1}' | grep -vxF "$(otool -D "$f" | grep -v ':$')")
done < <(find "$APP"/Contents -type f \( -perm -u+x -o -name '*.dylib' \) ; find "$APP"/Contents/Frameworks -type f -path '*/Versions/A/*' -not -name '*.plist' 2>/dev/null)
[ $bad = 0 ] || { echo "package incomplete"; exit 1; }
"$APP"/Contents/MacOS/prismark --version > /dev/null

cp "$SRC"/LICENSE "$STAGE"/
ln -s /Applications "$STAGE"/Applications
cat > "$STAGE"/README.txt <<TXT
Prismark $VERSION for macOS ($ARCH_NAME)

Drag Prismark to Applications and open it. It runs on macOS 13 or later.

The app is not notarised by Apple. The first time, open it with a right-click (or
Control-click) on Prismark and "Open", then confirm; after that it opens normally.
If macOS says the app is damaged, run once in Terminal:
  xattr -dr com.apple.quarantine /Applications/Prismark.app

The command line is the runner inside the app:
  /Applications/Prismark.app/Contents/MacOS/prismark help

Results are saved in ~/Library/Application Support/prismark/results, shared by the
app and the command line.

The compile tests are not built in yet. "Compiling code" is unavailable; "Full
software build" works after a one-time setup in the app, which needs Clang 19,
CMake and Ninja from Homebrew: brew install llvm@19 cmake ninja
TXT

hdiutil create -volname "Prismark $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$OUT/$NAME.dmg" > /dev/null
echo "$OUT/$NAME.dmg"
