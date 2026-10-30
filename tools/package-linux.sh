#!/usr/bin/env bash
# Builds Prismark for Linux (x86-64 or ARM64, the machine's own) and packs it for installing: a .deb (Ubuntu,
# Debian) and an .rpm (Fedora), the same files in both. Everything goes to /opt/prismark, with prismark and
# prismark-gui in /usr/bin and the app in the desktop's menu. The folder holds the desktop app, the runner, Qt
# and every library they need that a desktop distribution may lack, and the compile tests with everything they
# need: Clang built into the runner (Compiling code), and for Full software build the prepared snapshot (k1x/)
# and the build tools (tools/: Clang 19, CMake, Ninja). The packages depend only on what every desktop has
# (graphics, X11 and Wayland, fonts, D-Bus, GLib), so one build serves both families.
#
#   K1X_SNAPSHOT=DIR tools/package-linux.sh QT [OUT_DIR]
#
#   QT            Qt 6 for Linux, e.g. .../Qt/6.8.3/gcc_64 (aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O Qt)
#   K1X_SNAPSHOT  a prepared compile-test snapshot (tools/k1x/prepare.py), shipped as it is
#
# Clang and LLVM 19 come from the build machine (/usr/lib/llvm-19, apt.llvm.org), or PRISMARK_K1_LLVM_DIR.
# Build on the oldest distribution it should run on (glibc is the system's): Ubuntu 22.04 covers 22.04 and later,
# and every Fedora still supported. Needs dpkg-deb and rpmbuild (apt install rpm). The build tools are
# downloaded at pinned versions and checked by SHA-256.
#
# PRISMARK_COMPILE_TESTS=0 leaves the compile tests out (no LLVM or K1X_SNAPSHOT needed; the app lists them
# as unavailable). The build goes to build/linux-bundle, or to $PRISMARK_BUILD_DIR when set.
set -euo pipefail

[ $# -ge 1 ] || { sed -n 2,22p "$0"; exit 2; }
QT=$(realpath "$1")
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$SRC/build}")
BUILD=$(realpath -m "${PRISMARK_BUILD_DIR:-$SRC/build/linux-bundle}")
COMPILE=${PRISMARK_COMPILE_TESTS:-1}
LLVM=${PRISMARK_K1_LLVM_DIR:-/usr/lib/llvm-19}
# The package is for the machine it is built on; the pinned build tools are the same versions for both.
case $(uname -m) in
  x86_64)
    ARCH=x86_64 ARCH_NAME=x86-64 DEB_ARCH=amd64 NINJA_ZIP=ninja-linux.zip
    CMAKE_SHA=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
    NINJA_SHA=6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255
    PATCHELF_SHA=ce84f2447fb7a8679e58bc54a20dc2b01b37b5802e12c57eece772a6f14bf3f0 ;;
  aarch64)
    ARCH=aarch64 ARCH_NAME=ARM64 DEB_ARCH=arm64 NINJA_ZIP=ninja-linux-aarch64.zip
    CMAKE_SHA=b4cc788d63112b2749b40627e719eb5d3b8ed8f00c36d77189f4019cfe64bc9e
    NINJA_SHA=5c25c6570b0155e95fce5918cb95f1ad9870df5768653afe128db822301a05a1
    PATCHELF_SHA=ae13e2effe077e829be759182396b931d8f85cfb9cfe9d49385516ea367ef7b2 ;;
  *) echo "no package for $(uname -m)"; exit 2 ;;
esac
command -v dpkg-deb > /dev/null && command -v rpmbuild > /dev/null || { echo "dpkg-deb and rpmbuild are needed"; exit 2; }

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
DEB=prismark_${VERSION}_$DEB_ARCH.deb
RPM=prismark-$VERSION-1.$ARCH.rpm
STAGE=$BUILD/stage/opt/prismark   # the installed folder, as it will be on the system
rm -rf "$BUILD/stage" "$OUT/$DEB" "$OUT/$RPM"
mkdir -p "$OUT" "$STAGE"/lib "$STAGE"/plugins

cp "$BUILD"/prismark "$BUILD"/prismark-gui "$STAGE"/
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
  tar -xzf "$(fetch https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-$ARCH.tar.gz \
    $CMAKE_SHA cmake-3.31.6-linux-$ARCH.tar.gz)" -C "$T"
  unzip -q "$(fetch https://github.com/ninja-build/ninja/releases/download/v1.12.1/$NINJA_ZIP \
    $NINJA_SHA "${NINJA_ZIP%.zip}-1.12.1.zip")" -d "$T"
  cp "$T"/ninja "$STAGE"/tools/bin/
  cp "$T"/cmake-3.31.6-linux-$ARCH/bin/cmake "$STAGE"/tools/bin/
  cp -r "$T"/cmake-3.31.6-linux-$ARCH/share/cmake-3.31 "$STAGE"/tools/share/
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
SYSTEM='^(linux-vdso|ld-linux-x86-64|ld-linux-aarch64|libc|libm|libdl|libpthread|librt|libresolv|libutil|libstdc\+\+|libgcc_s|libGL|libGLX|libGLdispatch|libEGL|libdrm|libgbm|libX11|libX11-xcb|libXext|libXrender|libXi|libXfixes|libXau|libXdmcp|libxcb|libxcb-(glx|shm|render|randr|xfixes|shape|sync|xkb|xinput|dri2|dri3|present)|libxkbcommon|libxkbcommon-x11|libwayland-(client|cursor|egl|server)|libdbus-1|libfontconfig|libfreetype|libexpat|libuuid|libglib-2\.0|libgthread-2\.0|libgobject-2\.0|libgio-2\.0|libgmodule-2\.0|libsystemd|libudev|libcap|libgcrypt|libgpg-error|liblz4|libselinux|libpcre2-8|libpcre|libmount|libblkid|libffi)\.so'
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
tar -xzf "$(fetch https://github.com/NixOS/patchelf/releases/download/0.18.0/patchelf-0.18.0-$ARCH.tar.gz \
  $PATCHELF_SHA patchelf-0.18.0-$ARCH.tar.gz)" -C "$BUILD" ./bin/patchelf
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

# What someone who installed it needs to know.
if [ "$COMPILE" != 0 ]; then
  COMPILE_TXT='The compile tests ("Compiling code", "Full software build") are included: Clang is
built into prismark, and the folders k1x (the sources they compile) and tools
(Clang, CMake and Ninja for the full build) are used from /opt/prismark.'
else
  COMPILE_TXT='Not included: "Compiling code" and "Full software build" (the compile tests); the app
lists them as unavailable.'
fi
cat > "$STAGE"/README.txt <<TXT
Prismark $VERSION for Linux $ARCH_NAME

Start Prismark from the desktop's menu, or prismark-gui in a terminal. prismark is
the command line (prismark help). Both are in /opt/prismark, with everything they
use, and linked from /usr/bin. Runs on Ubuntu 22.04 or later, Debian 12 or later,
and Fedora.

Results are saved in ~/.local/share/prismark/results, shared by the app and the
command line; removing the package leaves them.

Power is read from the CPU's energy counter, which only root may read on most
distributions. To let the app read it until the next reboot:
  sudo chmod a+r /sys/class/powercap/intel-rapl:0/energy_uj

$COMPILE_TXT
TXT

# ---- the packages: /opt/prismark, the commands in /usr/bin, the menu entry and its icon ----
ROOT=$BUILD/stage
mkdir -p "$ROOT"/usr/bin "$ROOT"/usr/share/applications "$ROOT"/usr/share/icons/hicolor/scalable/apps \
  "$ROOT"/usr/share/doc/prismark
ln -s /opt/prismark/prismark "$ROOT"/usr/bin/prismark
ln -s /opt/prismark/prismark-gui "$ROOT"/usr/bin/prismark-gui
cp "$SRC"/apps/gui/prismark.desktop "$ROOT"/usr/share/applications/
cp "$SRC"/apps/gui/icons/prismark-app.svg "$ROOT"/usr/share/icons/hicolor/scalable/apps/prismark.svg
cp "$SRC"/LICENSE "$ROOT"/usr/share/doc/prismark/copyright
SUMMARY="Vendor-neutral CPU benchmark: per-test results, no composite score"
DESCRIPTION="Prismark measures a processor with real workloads (3D rendering, compression, compiling code
and more), each ranked on its own against reference systems. Desktop app and command line."

# The system libraries the package uses: every one a program or library in it asks for that it does not carry.
SYSLIBS=$(find "$STAGE" -path "$STAGE/k1x" -prune -o -path "$STAGE/tools/share" -prune -o -type f \
            \( -name '*.so*' -o -perm -u+x \) -print |
          while read -r f; do if file -b "$f" | grep -q '^ELF'; then echo "$f"; fi; done |
          while read -r f; do readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; done | sort -u |
          while read -r n; do if [ ! -e "$STAGE/lib/$n" ] && ! [[ $n =~ ^(ld-linux|linux-vdso) ]]; then echo "$n"; fi; done)

# .deb: each library's package on the build system. Ubuntu 24.04 and Debian 13 renamed some (libglib2.0-0t64),
# so each may also come under its new name.
pkgs=$(for n in $SYSLIBS; do
          p=$(dpkg -S "/$n" 2> /dev/null | sed -n '1s/[:,].*//p')
          [ -n "$p" ] || { echo "no package on this system has $n" >&2; exit 1; }
          echo "$p"
        done | sort -u) || exit 1
deps="libc6 (>= $(ldd --version | sed -n '1s/.* //p'))"
for p in $pkgs; do
  case $p in libc6) ;; lib*t64|[!l]*) deps="$deps, $p" ;; *) deps="$deps, $p | ${p}t64" ;; esac
done
mkdir -p "$ROOT"/DEBIAN
cat > "$ROOT"/DEBIAN/control <<CTL
Package: prismark
Version: $VERSION
Architecture: $DEB_ARCH
Maintainer: The Prismark Authors
Installed-Size: $(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
Depends: $deps
Section: utils
Priority: optional
Homepage: https://github.com/leonardonels/prismark
Description: $SUMMARY
$(echo "$DESCRIPTION" | sed 's/^/ /')
CTL
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$OUT/$DEB" > /dev/null
rm -rf "$ROOT"/DEBIAN

# .rpm: the same files. The libraries by soname, as Fedora's own packages ask for them; nothing in /opt/prismark
# is offered to other packages, and the files go in as built (no stripping, no build-id links).
RPMTOP=$BUILD/rpmbuild
rm -rf "$RPMTOP" && mkdir -p "$RPMTOP"/SPECS
{
  echo "Name: prismark"
  echo "Version: $VERSION"
  echo "Release: 1"
  echo "Summary: $SUMMARY"
  echo "License: Apache-2.0"
  echo "URL: https://github.com/leonardonels/prismark"
  echo "AutoReqProv: no"
  for n in $SYSLIBS; do echo "Requires: $n()(64bit)"; done
  echo "%description"
  echo "$DESCRIPTION"
  echo "%install"
  echo "mkdir -p %{buildroot} && cp -a '$ROOT'/. %{buildroot}/"
  echo "%files"
  echo "/opt/prismark"
  echo "/usr/bin/prismark"
  echo "/usr/bin/prismark-gui"
  echo "/usr/share/applications/prismark.desktop"
  echo "/usr/share/icons/hicolor/scalable/apps/prismark.svg"
  echo "/usr/share/doc/prismark"
} > "$RPMTOP"/SPECS/prismark.spec
rpmbuild -bb --target "$ARCH" --define "_topdir $RPMTOP" --define "_build_id_links none" \
  --define "debug_package %{nil}" --define "__os_install_post %{nil}" --define "__arch_install_post %{nil}" \
  --define "_binary_payload w6.xzdio" "$RPMTOP"/SPECS/prismark.spec > /dev/null
cp "$RPMTOP"/RPMS/"$ARCH"/"$RPM" "$OUT/"
echo "$OUT/$DEB"
echo "$OUT/$RPM"
