#!/usr/bin/env bash
# Builds Prismark for macOS (Apple silicon or x86-64, the machine's own) and packs Prismark.app into a disk image:
# open it and drag the app to Applications. Nothing else to install: the app holds the runner
# (Contents/MacOS/prismark, also the command line), Qt, and the compile tests with everything they need: Clang
# built into the runner (Compiling code), and for Full software build the prepared snapshot (Resources/k1x) and
# the build tools (Resources/tools: Clang 19, CMake, Ninja).
#
#   K1_LLVM=DIR K1X_SNAPSHOT=DIR tools/package-macos.sh QT [OUT_DIR]
#
#   QT            Qt 6 for macOS, e.g. .../Qt/6.10.3/macos (aqt install-qt mac desktop 6.10.3 clang_64 -O Qt; 6.8
#                 does not link with the macOS 26 SDK, which has no AGL)
#   K1_LLVM       Clang and LLVM 19 for macOS, built in and shipped: LLVM's own release (LLVM-19.1.7-macOS-ARM64)
#                 runs on older macOS than Homebrew's llvm@19, which is built for the machine's own release
#   K1X_SNAPSHOT  a prepared compile-test snapshot (tools/k1x/prepare.py), shipped as it is
#
# The programs are compiled with Homebrew's Clang 19 (brew install llvm@19 ninja), as for the macos-clang preset.
# The app's least macOS is the newest any program or library in it needs, written to Info.plist. The build tools
# are downloaded at pinned versions and checked by SHA-256. The app is signed ad hoc, not notarised.
#
# PRISMARK_COMPILE_TESTS=0 leaves the compile tests out (no K1_LLVM or K1X_SNAPSHOT needed; the app lists them
# as unavailable). The build goes to build/macos-bundle, or to $PRISMARK_BUILD_DIR when set.
set -euo pipefail

[ $# -ge 1 ] || { sed -n 2,21p "$0"; exit 2; }
QT=$(cd "$1" && pwd)
SRC=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "${2:-$SRC/build}"
OUT=$(cd "${2:-$SRC/build}" && pwd)
BUILD=${PRISMARK_BUILD_DIR:-$SRC/build/macos-bundle}
COMPILE=${PRISMARK_COMPILE_TESTS:-1}
case $(uname -m) in
  arm64) ARCH=arm64 ARCH_NAME="Apple silicon" ;;
  x86_64) ARCH=x86_64 ARCH_NAME="Intel" ;;
  *) echo "no package for $(uname -m)"; exit 2 ;;
esac
[ -x "$QT/bin/macdeployqt" ] || { echo "$QT/bin/macdeployqt not found: QT must be a Qt 6 for macOS"; exit 2; }

K1_ARGS=(-DPRISMARK_K1=OFF)
if [ "$COMPILE" != 0 ]; then
  [ -n "${K1_LLVM:-}" ] && [ -f "$K1_LLVM/lib/cmake/clang/ClangConfig.cmake" ] ||
    { echo "K1_LLVM: Clang and LLVM 19 for macOS are needed (LLVM's release, unpacked), or PRISMARK_COMPILE_TESTS=0"; exit 2; }
  [ -n "${K1X_SNAPSHOT:-}" ] && [ -f "$K1X_SNAPSHOT/manifest.json" ] ||
    { echo "K1X_SNAPSHOT: a prepared snapshot is needed (tools/k1x/prepare.py), or PRISMARK_COMPILE_TESTS=0"; exit 2; }
  K1_LLVM=$(cd "$K1_LLVM" && pwd)
  K1X_SNAPSHOT=$(cd "$K1X_SNAPSHOT" && pwd)
  K1_ARGS=(-DPRISMARK_K1=ON -DPRISMARK_K1_LLVM_DIR="$K1_LLVM")
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/macos-clang.cmake" \
  -DCMAKE_PREFIX_PATH="$QT" -DPRISMARK_GUI=ON "${K1_ARGS[@]}" -DBUILD_TESTING=OFF
cmake --build "$BUILD"
grep -q 'Qt6' "$BUILD/CMakeCache.txt" || { echo "the desktop app was not built with Qt 6 from $QT"; exit 1; }

VERSION=$(sed -n 's/^project(prismark VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
NAME=prismark-$VERSION-macos-$ARCH
STAGE=$OUT/$NAME
APP=$STAGE/Prismark.app
RES=$APP/Contents/Resources
rm -rf "$STAGE" "$OUT/$NAME.dmg"
mkdir -p "$STAGE"
cp -R "$BUILD"/prismark-gui.app "$APP"   # with the runner and the icon (apps/gui/gui.cmake)
PLIST=$APP/Contents/Info.plist
plist() { /usr/libexec/PlistBuddy -c "Delete :$1" "$PLIST" 2>/dev/null || true; /usr/libexec/PlistBuddy -c "Add :$1 $2 $3" "$PLIST"; }
plist CFBundleDisplayName string Prismark
plist NSHighResolutionCapable bool true

# Qt's frameworks and plugins, and every library the runner needs, into Contents/Frameworks.
"$QT"/bin/macdeployqt "$APP" -executable="$APP"/Contents/MacOS/prismark -always-overwrite
FW=$APP/Contents/Frameworks
# Plain libraries are also loaded by the tools in Resources: they find each other beside themselves, not
# beside the app's executable.
for f in "$FW"/*.dylib; do
  [ -e "$f" ] || continue
  for dep in $(otool -L "$f" | awk 'NR > 1 {print $1}' | grep '^@executable_path/../Frameworks/[^/]*\.dylib$' || true); do
    install_name_tool -change "$dep" "@loader_path/${dep##*/}" "$f" 2> /dev/null
  done
done
# ---- the compile tests' snapshot and build tools, in Resources (code outside MacOS, Frameworks and PlugIns is
# signed one file at a time below) ----
fetch() { # url sha256 file: downloads once into $BUILD/downloads, checked
  local f=$BUILD/downloads/$3
  mkdir -p "$BUILD/downloads"
  if [ ! -f "$f" ] || [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" != "$2" ]; then curl -sSfL -o "$f.part" "$1" && mv "$f.part" "$f"; fi
  [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$2" ] || { echo "$3: hash mismatch"; exit 1; }
  echo "$f"
}
if [ "$COMPILE" != 0 ]; then
  cp -R "$K1X_SNAPSHOT" "$RES"/k1x
  rm -rf "$RES"/k1x.work
  T=$BUILD/tools-unpacked
  rm -rf "$T" && mkdir -p "$T" "$RES"/tools/bin "$RES"/tools/share
  tar -xzf "$(fetch https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-macos-universal.tar.gz \
    330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d cmake-3.31.6-macos-universal.tar.gz)" -C "$T"
  unzip -q "$(fetch https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-mac.zip \
    89a287444b5b3e98f88a945afa50ce937b8ffd1dcc59c555ad9b1baf855298c9 ninja-mac-1.12.1.zip)" -d "$T"
  cp "$T"/ninja "$RES"/tools/bin/
  cp "$T"/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake "$RES"/tools/bin/
  cp -R "$T"/cmake-3.31.6-macos-universal/CMake.app/Contents/share/cmake-3.31 "$RES"/tools/share/
  # The snapshot's toolchain asks for clang-19 and clang++-19, and CMake's compiler check for llvm-ar; the
  # measured build itself only compiles. The real programs are in tools/llvm/bin and take their mode from the
  # name they are started by, so tools/bin holds links to them under each name; Clang finds its headers in
  # tools/llvm/lib/clang/19.
  mkdir -p "$RES"/tools/llvm/bin "$RES"/tools/llvm/lib/clang/19
  cp -L "$K1_LLVM"/bin/clang-19 "$K1_LLVM"/bin/llvm-ar "$RES"/tools/llvm/bin/
  cp -R "$K1_LLVM"/lib/clang/19/include "$RES"/tools/llvm/lib/clang/19/
  for t in clang-19:clang-19 clang++-19:clang-19 llvm-ar:llvm-ar llvm-ranlib:llvm-ar; do
    ln -s ../llvm/bin/"${t#*:}" "$RES"/tools/bin/"${t%%:*}"
  done
fi

# The tools in Resources: libraries they load from outside macOS come into Contents/Frameworks, found through
# their rpath.
if [ "$COMPILE" != 0 ]; then
  for f in "$RES"/tools/llvm/bin/*; do
    for dep in $(otool -L "$f" | awk 'NR > 1 {print $1}' | grep -v '^/System/\|^/usr/lib/' || true); do
      lib=${dep##*/}
      [ -e "$FW/$lib" ] || cp -L "$K1_LLVM/lib/$lib" "$FW/$lib"
      install_name_tool -change "$dep" "@rpath/$lib" "$f" 2> /dev/null
    done
    for rp in $(otool -l "$f" | awk '/LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}'); do
      install_name_tool -delete_rpath "$rp" "$f"
    done
    install_name_tool -add_rpath @loader_path/../../../../Frameworks "$f"
  done
fi

# Ad-hoc signatures (Apple silicon runs no unsigned code): each program in Resources on its own, then the bundle.
find "$RES" -type f -perm -u+x | while read -r f; do
  if file -b "$f" | grep -q 'Mach-O'; then codesign --force --sign - "$f"; fi
done
codesign --force --deep --sign - "$APP"

# Check: nothing any program or library in the app needs comes from outside it, except macOS itself; and the
# least macOS the app runs on, the newest any of them asks for.
bad=0
minos=13.0
while read -r f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  while read -r lib; do
    case $lib in /System/*|/usr/lib/*|@rpath/*|@executable_path/*|@loader_path/*) ;;
      *) echo "not from the package: $lib (needed by ${f#"$APP"/})"; bad=1 ;; esac
  done < <(otool -L "$f" | grep -v ':$' | awk '{print $1}' | grep -vxF "$(otool -D "$f" | grep -v ':$')")
  for v in $(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b = 1} b && $1 == "minos" {print $2; b = 0}'); do
    minos=$(printf '%s\n%s\n' "$minos" "$v" | sort -V | tail -1)
  done
done < <(find "$APP"/Contents -type f \( -perm -u+x -o -name '*.dylib' \)
         find "$FW" -type f -path '*/Versions/A/*' -not -name '*.plist' 2> /dev/null)
[ $bad = 0 ] || { echo "package incomplete"; exit 1; }
plist LSMinimumSystemVersion string "$minos"
codesign --force --deep --sign - "$APP"   # Info.plist changed
"$APP"/Contents/MacOS/prismark --version > /dev/null
if [ "$COMPILE" != 0 ]; then
  "$RES"/tools/bin/clang++-19 --version > /dev/null
  "$RES"/tools/bin/cmake --version > /dev/null
fi
echo "least macOS: $minos"

if [ "$COMPILE" != 0 ]; then
  COMPILE_TXT='The compile tests ("Compiling code", "Full software build") are included, with
everything they need: nothing to install, no Homebrew or Xcode.'
else
  COMPILE_TXT='Not included: "Compiling code" and "Full software build" (the compile tests); the app
lists them as unavailable.'
fi
cp "$SRC"/LICENSE "$STAGE"/
cat > "$STAGE"/README.txt <<TXT
Prismark $VERSION for macOS ($ARCH_NAME)

Drag Prismark to Applications and open it. It runs on macOS $minos or later.

The app is not notarised by Apple, so macOS may stop it the first time. Then open
System Settings > Privacy & Security and click "Open Anyway" (on macOS 14, a
right-click on Prismark and "Open" also works); after that it opens normally.
If macOS says the app is damaged, run once in Terminal:
  xattr -dr com.apple.quarantine /Applications/Prismark.app

The command line is the runner inside the app:
  /Applications/Prismark.app/Contents/MacOS/prismark help

Results are saved in ~/Library/Application Support/prismark/results, shared by the
app and the command line.

$COMPILE_TXT
TXT

# The disk image opens on a window that shows the install: the app, an arrow, the Applications folder. The app
# draws the background (apps/gui/main.cpp); dmgbuild lays the window out without the Finder, so this also works
# on a CI machine. Pinned, in its own environment. The volume keeps the standard drive icon: the app's own icon is
# for the app, not for the disk it comes on.
env PRISMARK_GUI_DMG_BACKGROUND="$BUILD"/dmg-background QT_QPA_PLATFORM=offscreen \
  "$BUILD"/prismark-gui.app/Contents/MacOS/prismark-gui
tiffutil -cathidpicheck "$BUILD"/dmg-background.png "$BUILD"/dmg-background@2x.png -out "$BUILD"/dmg-background.tiff \
  > /dev/null
[ -x "$BUILD"/dmgbuild/bin/dmgbuild ] || { python3 -m venv "$BUILD"/dmgbuild && "$BUILD"/dmgbuild/bin/pip install -q dmgbuild==1.6.7; }
cat > "$BUILD"/dmg-settings.py <<'PY'
import os.path
stage = defines["stage"]
files = [os.path.join(stage, n) for n in ("Prismark.app", "README.txt", "LICENSE")]
symlinks = {"Applications": "/Applications"}
background = defines["background"]
format = "UDZO"
filesystem = "HFS+"
window_rect = ((200, 140), (740, 560))
default_view = "icon-view"
show_toolbar = show_pathbar = show_sidebar = show_status_bar = show_tab_view = False
icon_size = 100
text_size = 13
icon_locations = {"Prismark.app": (200, 230), "Applications": (540, 230), "README.txt": (520, 440), "LICENSE": (640, 440)}
PY
"$BUILD"/dmgbuild/bin/dmgbuild -s "$BUILD"/dmg-settings.py -D stage="$STAGE" -D background="$BUILD"/dmg-background.tiff \
  "Prismark $VERSION" "$OUT/$NAME.dmg" > /dev/null
echo "$OUT/$NAME.dmg"
