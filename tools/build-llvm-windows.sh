#!/usr/bin/env bash
# Builds the Clang and LLVM 19 libraries that "Compiling code" (K1) links into the Windows runner, cross-compiled
# on Linux with llvm-mingw, and installs them (static libraries, headers, CMake packages) to PREFIX. Then
# tools/package-windows.sh builds K1 in with K1_LLVM=PREFIX.
#
#   tools/build-llvm-windows.sh LLVM_MINGW PREFIX [WORK_DIR]
#
#   LLVM_MINGW  llvm-mingw 20250114 (LLVM 19.1.7), ucrt-ubuntu-x86_64 release: the same toolchain as the app
#   PREFIX      where to install, e.g. build/llvm-19-windows
#   WORK_DIR    sources and build tree (default: build/llvm-windows-work); about 6 GB while building
#
# Only what K1 needs: Clang's libraries and LLVM's, with the AArch64 target K1 compiles for, no tools, no tests.
# Built for x86-64-v2 like every baseline module (spec 3.3), so the compiler K1 measures runs the same
# instructions on every x86-64 machine. LLVM builds its own host tablegen tools first; that needs a host C and C++
# compiler (cc, c++). Takes 30-90 minutes.
set -euo pipefail

[ $# -ge 2 ] || { sed -n 2,16p "$0"; exit 2; }
LLVM_MINGW=$(realpath "$1")
PREFIX=$(realpath -m "$2")
SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(realpath -m "${3:-$SRC/build/llvm-windows-work}")

VERSION=19.1.7
URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/llvm-project-$VERSION.src.tar.xz
SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501

mkdir -p "$WORK"
TARBALL=$WORK/llvm-project-$VERSION.src.tar.xz
if [ ! -f "$TARBALL" ] || [ "$(sha256sum "$TARBALL" | cut -d' ' -f1)" != "$SHA256" ]; then
  curl -sSfL -o "$TARBALL.part" "$URL"
  mv "$TARBALL.part" "$TARBALL"
fi
[ "$(sha256sum "$TARBALL" | cut -d' ' -f1)" = "$SHA256" ] || { echo "LLVM source hash mismatch"; exit 1; }
if [ ! -d "$WORK/llvm-project-$VERSION.src" ]; then
  tar xf "$TARBALL" -C "$WORK"
fi

cmake -S "$WORK/llvm-project-$VERSION.src/llvm" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/windows-cross-llvm-mingw.cmake" -DLLVM_MINGW="$LLVM_MINGW" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
  -DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DCLANG_BUILD_TOOLS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BINDINGS=OFF \
  -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=cc;-DCMAKE_CXX_COMPILER=c++;-DCMAKE_TOOLCHAIN_FILE="
cmake --build "$WORK/build" --target install
echo "$PREFIX"
