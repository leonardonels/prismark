#!/usr/bin/env bash
# Builds Prismark for Linux x86-64 and packs a portable folder: unpack and run ./prismark-gui, nothing to
# install (a live USB session is enough). It holds the desktop app, the runner, the tier modules, Qt and every
# library they need that a desktop distribution may lack, and the compile tests with everything they need: Clang
# built into the runner (Compiling code), and for Full software build the prepared snapshot (k1x/) and the build
# tools (tools/: Clang 19, CMake, Ninja).
#
#   K1X_SNAPSHOT=DIR tools/package-linux.sh QT [OUT_DIR]
#
#   QT            Qt 6 for Linux, e.g. .../Qt/6.8.3/gcc_64 (aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O Qt)
#   K1X_SNAPSHOT  a prepared compile-test snapshot (tools/k1x/prepare.py), shipped as it is
#
# Clang and LLVM 19 come from the build machine (/usr/lib/llvm-19, apt.llvm.org), or PRISMARK_K1_LLVM_DIR.
# Build on the oldest distribution it should run on (glibc is the system's): Ubuntu 22.04 covers 22.04 and later.
# The build tools are downloaded at pinned versions and checked by SHA-256.
#
# PRISMARK_COMPILE_TESTS=0 leaves the compile tests out (no LLVM or K1X_SNAPSHOT needed; the app lists them
# as unavailable). The build goes to build/linux-bundle, or to $PRISMARK_BUILD_DIR when set.
set -euo pipefail

[ $# -ge 1 ] || { sed -n 2,19p "$0"; exit 2; }
QT=$(realpath "$1")
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$SRC/build}")
BUILD=$(realpath -m "${PRISMARK_BUILD_DIR:-$SRC/build/linux-bundle}")
COMPILE=${PRISMARK_COMPILE_TESTS:-1}
LLVM=${PRISMARK_K1_LLVM_DIR:-/usr/lib/llvm-19}

K1_ARGS=(-DPRISMARK_K1=OFF)
if [ "$COMPILE" != 0 ]; then
  [ -f "$LLVM/lib/cmake/clang/ClangConfig.cmake" ] ||
    { echo "Clang and LLVM 19 are needed in $LLVM (or PRISMARK_K1_LLVM_DIR), or PRISMARK_COMPILE_TESTS=0"; exit 2; }
  [ -n "${K1X_SNAPSHOT:-}" ] && [ -f "$K1X_SNAPSHOT/manifest.json" ] ||
    { echo "K1X_SNAPSHOT: a prepared snapshot is needed (tools/k1x/prepare.py), or PRISMARK_COMPILE_TESTS=0"; exit 2; }
  K1X_SNAPSHOT=$(realpath "$K1X_SNAPSHOT")
  K1_ARGS=(-DPRISMARK_K1=ON -DPRISMARK_K1_LLVM_DIR="$LLVM")
fi

# The programs find their libraries in lib/ beside them. DT_RPATH (not RUNPATH) also covers the libraries those
# load, so a bundled library without a search path of its own (libxml2 -> ICU) still finds its dependencies.
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/linux-clang.cmake" \
  -DCMAKE_PREFIX_PATH="$QT" -DPRISMARK_GUI=ON "${K1_ARGS[@]}" -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='$ORIGIN/lib' \
  -DCMAKE_EXE_LINKER_FLAGS=-Wl,--disable-new-dtags -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--disable-new-dtags
cmake --build "$BUILD"
grep -q 'Qt6' "$BUILD/CMakeCache.txt" || { echo "the desktop app was not built with Qt 6 from $QT"; exit 1; }

VERSION=$(sed -n 's/^project(prismark VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
NAME=prismark-$VERSION-linux-x86_64
STAGE=$OUT/$NAME
rm -rf "$STAGE" "$OUT/$NAME.tar.gz"
mkdir -p "$STAGE"/lib "$STAGE"/plugins

cp "$BUILD"/prismark "$BUILD"/prismark-gui "$BUILD"/prismark-kernels-*.so "$STAGE"/
# Qt's plugins: X11 and Wayland, the SVG icons, and the desktop's file dialogs and theme through the portal.
for d in platforms/libqxcb.so platforms/libqwayland-egl.so platforms/libqwayland-generic.so \
  wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client xcbglintegrations \
  imageformats/libqsvg.so iconengines/libqsvgicon.so platformthemes/libqxdgdesktopportal.so \
  platforminputcontexts/libcomposeplatforminputcontextplugin.so platforminputcontexts/libibusplatforminputcontextplugin.so; do
  mkdir -p "$STAGE/plugins/$(dirname "$d")"
  cp -rL "$QT/plugins/$d" "$STAGE/plugins/$(dirname "$d")/"
done
printf '[Paths]\nPrefix = .\nPlugins = plugins\n' > "$STAGE"/qt.conf
cp "$SRC"/LICENSE "$STAGE"/
mkdir -p "$STAGE"/references && cp "$SRC"/references/*.json "$STAGE"/references/   # for prismark compare

# ---- the compile tests' snapshot and build tools ----
fetch() { # url sha256 file: downloads once into $BUILD/downloads, checked
  local f=$BUILD/downloads/$3
  mkdir -p "$BUILD/downloads"
  if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$2" ]; then curl -sSfL -o "$f.part" "$1" && mv "$f.part" "$f"; fi
  [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$2" ] || { echo "$3: hash mismatch"; exit 1; }
  echo "$f"
}
if [ "$COMPILE" != 0 ]; then
  cp -r "$K1X_SNAPSHOT" "$STAGE"/k1x
  T=$BUILD/tools-unpacked
  rm -rf "$T" && mkdir -p "$T" "$STAGE"/tools/bin "$STAGE"/tools/share
  tar -xzf "$(fetch https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz \
    5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf cmake-3.31.6-linux-x86_64.tar.gz)" -C "$T"
  unzip -q "$(fetch https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip \
    6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255 ninja-linux-1.12.1.zip)" -d "$T"
  cp "$T"/ninja "$STAGE"/tools/bin/
  cp "$T"/cmake-3.31.6-linux-x86_64/bin/cmake "$STAGE"/tools/bin/
  cp -r "$T"/cmake-3.31.6-linux-x86_64/share/cmake-3.31 "$STAGE"/tools/share/
  # The snapshot's toolchain asks for clang-19 and clang++-19, and CMake's compiler check for llvm-ar; the
  # measured build itself only compiles. The real programs are in tools/llvm/bin and take their mode from the
  # name they are started by, so tools/bin holds links to them under each name. They find LLVM and Clang's
  # headers in ../lib (RUNPATH $ORIGIN/../lib, lib/clang/19): tools/llvm/lib is the package's lib/, so LLVM is
  # shipped once.
  mkdir -p "$STAGE"/tools/llvm/bin "$STAGE"/lib/clang/19
  cp -L "$LLVM"/bin/clang-19 "$LLVM"/bin/llvm-ar "$STAGE"/tools/llvm/bin/
  ln -s ../../lib "$STAGE"/tools/llvm/lib
  cp -r "$LLVM"/lib/clang/19/include "$STAGE"/lib/clang/19/
  for t in clang-19:clang-19 clang++-19:clang-19 llvm-ar:llvm-ar llvm-ranlib:llvm-ar; do
    ln -s ../llvm/bin/"${t#*:}" "$STAGE"/tools/bin/"${t%%:*}"
  done
fi

# Every library a program or plugin needs goes to lib/, except those every desktop Linux has (glibc, the C++
# runtime, graphics, X11 and Wayland, D-Bus, fonts, GLib). Repeated until nothing new is added.
SYSTEM='^(linux-vdso|ld-linux-x86-64|libc|libm|libdl|libpthread|librt|libresolv|libutil|libstdc\+\+|libgcc_s|libGL|libGLX|libGLdispatch|libEGL|libdrm|libgbm|libX11|libX11-xcb|libXext|libXrender|libXi|libXfixes|libXau|libXdmcp|libxcb|libxcb-(glx|shm|render|randr|xfixes|shape|sync|xkb|xinput|dri2|dri3|present)|libxkbcommon|libxkbcommon-x11|libwayland-(client|cursor|egl|server)|libdbus-1|libfontconfig|libfreetype|libexpat|libuuid|libglib-2\.0|libgthread-2\.0|libgobject-2\.0|libgio-2\.0|libgmodule-2\.0|libsystemd|libudev|libcap|libgcrypt|libgpg-error|liblz4|libselinux|libpcre2-8|libpcre|libmount|libblkid|libffi)\.so'
for round in 1 2 3 4 5 6; do
  added=0
  while read -r bin; do
    while read -r name path; do
      [[ $name =~ $SYSTEM ]] && continue
      [ -e "$STAGE/lib/$name" ] && continue
      [ "$path" = "not" ] && { echo "missing: $name (needed by ${bin#"$STAGE"/})"; exit 1; }
      cp -L "$path" "$STAGE/lib/$name" && added=1
    done < <(LD_LIBRARY_PATH="$STAGE/lib:$QT/lib" ldd "$bin" 2>/dev/null | awk '/=>/ {print $1, $3}')
  done < <(find "$STAGE" -path "$STAGE/k1x" -prune -o -path "$STAGE/tools/share" -prune -o -type f \
             \( -name '*.so*' -o -perm -u+x \) -print | while read -r f; do file -b "$f" | grep -q '^ELF' && echo "$f"; done)
  [ $added = 0 ] && break
done

# A bundled library without a search path of its own would look for its dependencies only in the system (where
# libxcb-cursor finds no libxcb-util, libxml2 no ICU 70): each gets RUNPATH $ORIGIN, its own folder.
tar -xzf "$(fetch https://github.com/NixOS/patchelf/releases/download/0.18.0/patchelf-0.18.0-x86_64.tar.gz \
  ce84f2447fb7a8679e58bc54a20dc2b01b37b5802e12c57eece772a6f14bf3f0 patchelf-0.18.0-x86_64.tar.gz)" -C "$BUILD" ./bin/patchelf
for f in "$STAGE"/lib/*.so*; do
  [ -n "$("$BUILD"/bin/patchelf --print-rpath "$f")" ] || "$BUILD"/bin/patchelf --set-rpath '$ORIGIN' "$f"
done
# Check: each file's own dependencies, except those left to the system, now resolve inside the package with no
# help from the environment (what a system library loads is its own business).
bad=0
while read -r bin; do
  needed=" $(readelf -d "$bin" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
  while read -r name path; do
    [[ $name =~ $SYSTEM ]] && continue
    [[ $needed == *" $name "* ]] || continue
    case $path in "$STAGE"/*) ;; *) echo "not from the package: $name => $path (needed by ${bin#"$STAGE"/})"; bad=1 ;; esac
  done < <(env -u LD_LIBRARY_PATH ldd "$bin" 2>/dev/null | awk '/=>/ {print $1, $3}' | while read -r n p; do
             echo "$n $( [ -e "$p" ] && realpath "$p" || echo "$p")"; done)
done < <(find "$STAGE" -path "$STAGE/k1x" -prune -o -path "$STAGE/tools/share" -prune -o -type f \
           \( -name '*.so*' -o -perm -u+x \) -print | while read -r f; do file -b "$f" | grep -q '^ELF' && echo "$f"; done)
[ $bad = 0 ] || { echo "package incomplete"; exit 1; }

# What someone who unpacks it needs to know.
if [ "$COMPILE" != 0 ]; then
  COMPILE_TXT='The compile tests ("Compiling code", "Full software build") are included: Clang is
built into prismark, and the folders k1x (the sources they compile) and tools
(Clang, CMake and Ninja for the full build) are used from here.'
else
  COMPILE_TXT='Not included: "Compiling code" and "Full software build" (the compile tests); the app
lists them as unavailable.'
fi
cat > "$STAGE"/README.txt <<TXT
Prismark $VERSION for Linux x86-64, portable

Unpack anywhere and start ./prismark-gui (the desktop app). Nothing is installed and
nothing else is needed: everything it uses is in this folder. Runs on Ubuntu 22.04 or
later, or another distribution of the same age or newer, including a live USB session.

./prismark is the command line (./prismark help). The desktop app starts it for
every run, so keep the folder together.

Results are saved in ~/.local/share/prismark/results, shared by the app and the
command line. In a live session they are lost at shutdown: copy them off first.

Power is read from the CPU's energy counter, which only root may read on most
distributions. To let the app read it until the next reboot:
  sudo chmod a+r /sys/class/powercap/intel-rapl:0/energy_uj

$COMPILE_TXT
TXT

(cd "$OUT" && tar -czf "$NAME.tar.gz" "$NAME")
echo "$OUT/$NAME.tar.gz"
