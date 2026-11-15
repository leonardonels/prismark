#!/usr/bin/env bash
# Builds the compiler the compile tests time, the same way for every package: Clang and LLVM 19.1.7 as static
# libraries for "Compiling code" (K1, linked into the runner) and, from the same libraries, the clang-19 and
# llvm-ar programs "Full software build" (K1x) runs. Then the package scripts build K1 in and ship the programs
# with K1_LLVM=PREFIX.
#
#   tools/build-llvm.sh TARGET PREFIX [WORK_DIR]
#
#   TARGET    linux-x86_64 | linux-aarch64 | macos-arm64 | windows-x86_64 (cross-built on Linux)
#   PREFIX    where to install, e.g. build/llvm-19-linux-x86_64
#   WORK_DIR  sources and build trees (default: build/llvm-work-TARGET); about 8 GB while building
#
# The builder is Clang 19.1.7 on every target, checked before anything is built:
#   linux-*         clang-19 and clang++-19 from apt.llvm.org (build Linux packages on Ubuntu 22.04)
#   macos-arm64     Homebrew's llvm@19, or LLVM19_BIN pointing at another 19.1.7 bin folder
#   windows-x86_64  llvm-mingw 20250114 in LLVM_MINGW, as for the app (and cc/c++ for LLVM's host tablegen)
#
# What is the same on every target, so the compiler is the same program wherever K1 and K1x run:
#   - the LLVM 19.1.7 source tarball, checked by SHA-256
#   - libc++ and libc++abi 19.1.7 from that tarball, static and hidden (hermetic): no OS's C++ library is used
#   - Release, no assertions, the AArch64 target only, Clang only, no zlib/zstd/libxml2/libedit
#   - built for the baseline ISA of every Prismark module (cmake/tiers.cmake): x86-64-v2 or armv8.2-a
#   - everything linked statically: no libLLVM or libclang-cpp shared library
# The memory allocator is each OS's own: the compile tests count it as part of the platform (docs/measuring.md).
#
# PREFIX/prismark-compiler.json records the recipe; its "id" covers this script and the source, not the target,
# so packages for different systems built from one recipe carry the same id. Takes 1-3 hours.
set -euo pipefail
[ -z "${GITHUB_ACTIONS:-}" ] || trap 'echo "::error file=tools/build-llvm.sh,line=$LINENO::failed: $BASH_COMMAND"' ERR

[ $# -ge 2 ] || { sed -n 2,28p "$0"; exit 2; }
TARGET=$1
mkdir -p "$2"
PREFIX=$(cd "$2" && pwd)
SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=${3:-$SRC/build/llvm-work-$TARGET}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

VERSION=19.1.7
URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/llvm-project-$VERSION.src.tar.xz
SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501

sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
jobs() { getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu; }

# ---- the target: builder, ISA, platform options ----
COMMON=() RT_EXTRA=()
case $TARGET in
  linux-x86_64 | linux-aarch64)
    [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = "${TARGET#linux-}" ] || { echo "$TARGET is built on $TARGET"; exit 2; }
    CC=clang-19 CXX=clang++-19
    COMMON=(-DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DLLVM_USE_LINKER=lld)
    UNWINDER=ON ;;
  macos-arm64)
    [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || { echo "$TARGET is built on Apple silicon"; exit 2; }
    B=${LLVM19_BIN:-$(brew --prefix llvm@19 2>/dev/null)/bin}
    CC=$B/clang CXX=$B/clang++
    # The oldest macOS the app runs on (cmake/toolchains/macos-clang.cmake); the system's libunwind.
    COMMON=(-DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_OSX_ARCHITECTURES=arm64
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)")
    UNWINDER=OFF ;;
  windows-x86_64)
    [ -n "${LLVM_MINGW:-}" ] && [ -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" ] ||
      { echo "LLVM_MINGW: llvm-mingw 20250114 is needed"; exit 2; }
    LLVM_MINGW=$(cd "$LLVM_MINGW" && pwd)
    CC=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang CXX=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++
    COMMON=(-DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/windows-cross-llvm-mingw.cmake" -DLLVM_MINGW="$LLVM_MINGW")
    # MinGW has no libgcc here: compiler-rt's builtins, as llvm-mingw itself is built.
    RT_EXTRA=(-DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBUNWIND_USE_COMPILER_RT=ON
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON)
    UNWINDER=ON ;;
  *) echo "unknown target $TARGET"; sed -n 9,9p "$0"; exit 2 ;;
esac
case $TARGET in *x86_64) MARCH=x86-64-v2 ;; *) MARCH=armv8.2-a ;; esac

got=$("$CC" --version | sed -n 's/.*clang version \([0-9.]*\).*/\1/p' | head -1)
[ "$got" = "$VERSION" ] || { echo "the builder must be Clang $VERSION; $CC is ${got:-not Clang}"; exit 2; }

# ---- the source ----
TARBALL=$WORK/llvm-project-$VERSION.src.tar.xz
if [ ! -f "$TARBALL" ] || [ "$(sha256 "$TARBALL")" != "$SHA256" ]; then
  curl -sSfL --retry 5 -o "$TARBALL.part" "$URL"
  mv "$TARBALL.part" "$TARBALL"
fi
[ "$(sha256 "$TARBALL")" = "$SHA256" ] || { echo "LLVM source hash mismatch"; exit 1; }
LLVM_SRC=$WORK/llvm-project-$VERSION.src
[ -d "$LLVM_SRC" ] || tar xf "$TARBALL" -C "$WORK"

# ---- 1. libc++, libc++abi (and libunwind), static and hidden, into PREFIX/runtime ----
RT=$PREFIX/runtime
RUNTIMES="libcxx;libcxxabi"
[ $UNWINDER = ON ] && RUNTIMES="$RUNTIMES;libunwind"
cmake -S "$LLVM_SRC/runtimes" -B "$WORK/runtimes" -G Ninja "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$RT" -DCMAKE_C_FLAGS="-march=$MARCH" -DCMAKE_CXX_FLAGS="-march=$MARCH" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DLLVM_ENABLE_RUNTIMES="$RUNTIMES" -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
  -DLIBCXX_HERMETIC_STATIC_LIBRARY=ON -DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON -DLIBUNWIND_HIDE_SYMBOLS=ON \
  -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXXABI_USE_LLVM_UNWINDER=$UNWINDER -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
  -DLIBCXX_INCLUDE_DOCS=OFF -DLIBUNWIND_INCLUDE_DOCS=OFF ${RT_EXTRA[@]+"${RT_EXTRA[@]}"}
cmake --build "$WORK/runtimes" --target install -j "$(jobs)"

# Every C++ file of the compiler is built against that libc++ and linked with it, nothing else.
RT_LIBS="$RT/lib/libc++.a $RT/lib/libc++abi.a"
[ $UNWINDER = ON ] && RT_LIBS="$RT_LIBS $RT/lib/libunwind.a"
CXX_FLAGS="-march=$MARCH -nostdinc++ -isystem $RT/include/c++/v1"
LINK_FLAGS="-nostdlib++"

# ---- 2. Clang and LLVM: static libraries, the clang-19 and llvm-ar programs ----
cmake -S "$LLVM_SRC/llvm" -B "$WORK/llvm" -G Ninja "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_C_FLAGS="-march=$MARCH" -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="$LINK_FLAGS" -DCMAKE_SHARED_LINKER_FLAGS="$LINK_FLAGS" \
  -DCMAKE_MODULE_LINKER_FLAGS="$LINK_FLAGS" -DCMAKE_CXX_STANDARD_LIBRARIES="$RT_LIBS" \
  $( [ "$TARGET" = windows-x86_64 ] && echo -DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu ) \
  -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_PROJECTS=clang -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF -DCLANG_LINK_CLANG_DYLIB=OFF \
  -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DCLANG_BUILD_TOOLS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_PLUGINS=OFF \
  -DLLVM_ENABLE_LTO=OFF -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
  $( [ "$TARGET" = windows-x86_64 ] && echo "-DCROSS_TOOLCHAIN_FLAGS_NATIVE=-DCMAKE_C_COMPILER=cc;-DCMAKE_CXX_COMPILER=c++;-DCMAKE_TOOLCHAIN_FILE=" )
# The tools are left out of "all" (LLVM_BUILD_TOOLS=OFF) but still defined: built here by name, from the same
# static libraries the install below provides.
cmake --build "$WORK/llvm" --target install clang llvm-ar clang-resource-headers -j "$(jobs)"
EXE=; [ "$TARGET" = windows-x86_64 ] && EXE=.exe
mkdir -p "$PREFIX/bin" "$PREFIX/lib/clang/19"
# The driver is bin/clang-19 with a clang link on Linux and macOS, but only bin/clang.exe on Windows: copy
# whichever it is under the one name the packages use. (${A[@]+...} above: macOS's bash 3.2 calls an empty
# array unbound under set -u.)
cp "$WORK/llvm/bin/clang$EXE" "$PREFIX/bin/clang-19$EXE"
cp "$WORK/llvm/bin/llvm-ar$EXE" "$PREFIX/bin/"
rm -rf "$PREFIX/lib/clang/19/include"
cp -R "$WORK/llvm/lib/clang/19/include" "$PREFIX/lib/clang/19/"

# The programs must need nothing from LLVM or a C++ library at run time.
case $TARGET in
  linux-*) bad=$(readelf -d "$PREFIX/bin/clang-19" | grep -E 'NEEDED.*(LLVM|clang|c\+\+|stdc\+\+)' || true) ;;
  macos-*) bad=$(otool -L "$PREFIX/bin/clang-19" | tail -n +2 | grep -vE '^\s*/(usr/lib/libSystem|System/)' || true) ;;
  windows-*) bad=$("$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$PREFIX/bin/clang-19.exe" |
               grep -iE 'Name: (libc\+\+|libunwind|libLLVM|libclang)' || true) ;;
esac
[ -z "$bad" ] || { echo "clang-19 is not self-contained:"; echo "$bad"; exit 1; }

# ---- 3. the record ----
RECIPE=$(sha256 "$0")
ID=$(printf 'prismark-compiler/1 %s %s\n' "$RECIPE" "$SHA256" | { sha256sum 2>/dev/null || shasum -a 256; } | cut -d' ' -f1)
cat > "$PREFIX/prismark-compiler.json" <<EOF
{
  "schema": "prismark-compiler/1",
  "id": "$ID",
  "version": "$VERSION",
  "target": "$TARGET",
  "recipe_sha256": "$RECIPE",
  "llvm_source_sha256": "$SHA256",
  "builder": "clang $got",
  "march": "$MARCH",
  "cxx_library": "libc++ $VERSION static",
  "allocator": "system"
}
EOF
cat "$PREFIX/prismark-compiler.json"
