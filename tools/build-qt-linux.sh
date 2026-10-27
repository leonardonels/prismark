#!/usr/bin/env bash
# Builds Qt 6.8.3 for Linux from source and installs it to PREFIX, for tools/package-linux.sh where Qt's own
# binaries are built for a newer system than the package should run on (Qt's ARM64 Linux build needs glibc 2.38,
# so it does not run on Ubuntu 22.04 or JetPack 6).
#
#   tools/build-qt-linux.sh PREFIX [WORK_DIR]
#
#   PREFIX    where to install, e.g. build/qt-6.8.3; then tools/package-linux.sh PREFIX
#   WORK_DIR  sources and build trees (default: build/qt-linux-work); about 3 GB while building
#
# Only what the app uses: qtbase (Widgets, D-Bus, the X11 platform, the desktop portal theme, the compose and
# IBus input methods), qtsvg and qtwayland's client side, with Qt's own copies of the image and text libraries.
# Needs a C++17 compiler (cc, c++), CMake 3.25 or later, Ninja, and the X11, Wayland, OpenGL, fontconfig and
# D-Bus development packages (the linux-portable job in .github/workflows/ci.yml lists them for Ubuntu).
# Takes 30-90 minutes.
set -euo pipefail

[ $# -ge 1 ] || { sed -n 2,16p "$0"; exit 2; }
PREFIX=$(realpath -m "$1")
SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(realpath -m "${2:-$SRC/build/qt-linux-work}")

VERSION=6.8.3
URL=https://download.qt.io/archive/qt/${VERSION%.*}/$VERSION/submodules
MODULES=(
  qtbase:56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80
  qtsvg:35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438
  qtwayland:20fe385887d21190165a3180c17dcfc8b9a0e1da4ec76865b6334bdc709994b0
)

mkdir -p "$WORK"
for m in "${MODULES[@]}"; do
  name=${m%%:*} sha=${m#*:}
  f=$WORK/$name-everywhere-src-$VERSION.tar.xz
  if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$sha" ]; then
    curl -sSfL -o "$f.part" "$URL/$name-everywhere-src-$VERSION.tar.xz" && mv "$f.part" "$f"
  fi
  [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$sha" ] || { echo "$name: source hash mismatch"; exit 1; }
  [ -d "$WORK/$name-everywhere-src-$VERSION" ] || tar xf "$f" -C "$WORK"
done

mkdir -p "$WORK/build-qtbase"
(cd "$WORK/build-qtbase" && "$WORK/qtbase-everywhere-src-$VERSION/configure" -prefix "$PREFIX" -release \
  -opensource -confirm-license -nomake examples -nomake tests -no-icu -no-feature-sql \
  -xcb -xkbcommon -opengl desktop -dbus-linked -fontconfig -system-freetype -system-zlib \
  -qt-libpng -qt-libjpeg -qt-harfbuzz -qt-pcre)
cmake --build "$WORK/build-qtbase" --parallel
cmake --install "$WORK/build-qtbase"

for name in qtsvg qtwayland; do
  mkdir -p "$WORK/build-$name"
  (cd "$WORK/build-$name" && "$PREFIX/bin/qt-configure-module" "$WORK/$name-everywhere-src-$VERSION")
  cmake --build "$WORK/build-$name" --parallel
  cmake --install "$WORK/build-$name"
done

# What tools/package-linux.sh copies must be there.
for p in platforms/libqxcb.so platforms/libqwayland-egl.so platforms/libqwayland-generic.so \
  imageformats/libqsvg.so iconengines/libqsvgicon.so platformthemes/libqxdgdesktopportal.so \
  platforminputcontexts/libcomposeplatforminputcontextplugin.so platforminputcontexts/libibusplatforminputcontextplugin.so; do
  [ -e "$PREFIX/plugins/$p" ] || { echo "Qt built without plugins/$p"; exit 1; }
done
echo "Qt $VERSION installed in $PREFIX"
