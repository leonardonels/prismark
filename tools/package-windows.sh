#!/usr/bin/env bash
# Cross-builds Prismark for Windows x86-64 on Linux and zips a portable folder: unzip and run prismark-gui.exe,
# nothing to install. It holds the desktop app, the runner, Qt and the C++ runtime, and the
# compile tests with everything they need: Clang built into the runner (Compiling code), and for Full software
# build the prepared snapshot (k1x\) and the build tools (tools\: Clang 19, CMake, Ninja).
#
#   K1_LLVM=DIR K1X_SNAPSHOT=DIR tools/package-windows.sh LLVM_MINGW QT_WINDOWS QT_HOST [OUT_DIR]
#
#   LLVM_MINGW    llvm-mingw 20250114 (LLVM 19.1.7), ucrt-ubuntu-x86_64 release
#   QT_WINDOWS    Qt for Windows built with llvm-mingw, e.g. .../Qt/6.8.3/llvm-mingw_64
#   QT_HOST       host Qt of the same version, for moc and rcc, e.g. .../Qt/6.8.3/gcc_64
#   K1_LLVM       Clang and LLVM 19 libraries for Windows, from tools/build-llvm-windows.sh
#   K1X_SNAPSHOT  a prepared compile-test snapshot (tools/k1x/prepare.py), shipped as it is
#
# Both Qts install with aqtinstall:
#   aqt install-qt windows desktop 6.8.3 win64_llvm_mingw -O Qt
#   aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O Qt
# The Windows build tools are downloaded at pinned versions and checked by SHA-256.
#
# PRISMARK_COMPILE_TESTS=0 leaves the compile tests out (no K1_LLVM or K1X_SNAPSHOT needed; the app lists them
# as unavailable). The build goes to build/windows-cross, or to $PRISMARK_BUILD_DIR when set.
set -euo pipefail

[ $# -ge 3 ] || { sed -n 2,22p "$0"; exit 2; }
LLVM_MINGW=$(realpath "$1")
QT_WIN=$(realpath "$2")
QT_HOST=$(realpath "$3")
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${4:-$SRC/build}")
BUILD=$(realpath -m "${PRISMARK_BUILD_DIR:-$SRC/build/windows-cross}")
COMPILE=${PRISMARK_COMPILE_TESTS:-1}

K1_ARGS=(-DPRISMARK_K1=OFF)
if [ "$COMPILE" != 0 ]; then
  [ -n "${K1_LLVM:-}" ] && [ -f "$K1_LLVM/lib/cmake/clang/ClangConfig.cmake" ] ||
    { echo "K1_LLVM: Clang and LLVM 19 for Windows are needed (tools/build-llvm-windows.sh), or PRISMARK_COMPILE_TESTS=0"; exit 2; }
  [ -n "${K1X_SNAPSHOT:-}" ] && [ -f "$K1X_SNAPSHOT/manifest.json" ] ||
    { echo "K1X_SNAPSHOT: a prepared snapshot is needed (tools/k1x/prepare.py), or PRISMARK_COMPILE_TESTS=0"; exit 2; }
  K1_LLVM=$(realpath "$K1_LLVM")
  K1X_SNAPSHOT=$(realpath "$K1X_SNAPSHOT")
  K1_ARGS=(-DPRISMARK_K1=ON -DPRISMARK_K1_LLVM_DIR="$K1_LLVM")
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/windows-cross-llvm-mingw.cmake" \
  -DLLVM_MINGW="$LLVM_MINGW" -DCMAKE_PREFIX_PATH="$QT_WIN" -DQT_HOST_PATH="$QT_HOST" \
  -DPRISMARK_GUI=ON "${K1_ARGS[@]}" -DBUILD_TESTING=OFF
cmake --build "$BUILD"

VERSION=$(sed -n 's/^project(prismark VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
NAME=prismark-$VERSION-windows-x86_64
STAGE=$OUT/$NAME
rm -rf "$STAGE" "$OUT/$NAME.zip"
mkdir -p "$STAGE"/{platforms,styles,imageformats,iconengines}

cp "$BUILD"/prismark.exe "$BUILD"/prismark-gui.exe "$STAGE"/
for m in Core Gui Widgets Svg; do cp "$QT_WIN/bin/Qt6$m.dll" "$STAGE"/; done
cp "$QT_WIN"/plugins/platforms/qwindows.dll "$STAGE"/platforms/
cp "$QT_WIN"/plugins/styles/qmodernwindowsstyle.dll "$STAGE"/styles/ 2>/dev/null || true
cp "$QT_WIN"/plugins/imageformats/qsvg.dll "$STAGE"/imageformats/
cp "$QT_WIN"/plugins/iconengines/qsvgicon.dll "$STAGE"/iconengines/
# The C++ runtime the binaries were linked against (newer than Qt's own copy, and compatible with it).
cp "$LLVM_MINGW"/x86_64-w64-mingw32/bin/{libc++,libunwind}.dll "$STAGE"/
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
  unzip -q "$(fetch https://github.com/mstorsjo/llvm-mingw/releases/download/20250114/llvm-mingw-20250114-ucrt-x86_64.zip \
    9395d95c089611e6cd728c9a07219ffc5ac706e0c719833a2f7e64a706a81541 llvm-mingw-20250114-ucrt-x86_64.zip)" -d "$T"
  unzip -q "$(fetch https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip \
    d163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836 cmake-3.31.6-windows-x86_64.zip)" -d "$T"
  unzip -q "$(fetch https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip \
    f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a ninja-win-1.12.1.zip)" -d "$T"
  # The snapshot's toolchain asks for clang-19 and clang++-19: the real compiler (clang.exe in llvm-mingw is a
  # wrapper for MinGW targets), under both names; the driver takes its mode from its name. llvm-ar for CMake's
  # compiler check; the measured build itself only compiles.
  L=$T/llvm-mingw-20250114-ucrt-x86_64/bin
  cp "$L"/clang-19.exe "$STAGE"/tools/bin/clang-19.exe
  cp "$L"/clang-19.exe "$STAGE"/tools/bin/clang++-19.exe
  cp "$L"/{llvm-ar,llvm-ranlib}.exe "$L"/{libclang-cpp,libLLVM-19,libc++,libunwind}.dll "$STAGE"/tools/bin/
  cp "$T"/ninja.exe "$STAGE"/tools/bin/
  cp "$T"/cmake-3.31.6-windows-x86_64/bin/cmake.exe "$STAGE"/tools/bin/
  cp -r "$T"/cmake-3.31.6-windows-x86_64/share/cmake-3.31 "$STAGE"/tools/share/
fi

# What someone who unzips it needs to know, with Windows line ends.
if [ "$COMPILE" != 0 ]; then
  COMPILE_TXT='The compile tests ("Compiling code", "Full software build") are included: Clang is
built into prismark.exe, and the folders k1x (the sources they compile) and tools
(Clang, CMake and Ninja for the full build) are used from here.'
else
  COMPILE_TXT='Not included: "Compiling code" and "Full software build" (the compile tests); the app
lists them as unavailable.'
fi
sed 's/$/\r/' > "$STAGE"/README.txt <<TXT
Prismark $VERSION for Windows x86-64, portable

Unzip anywhere and start prismark-gui.exe (the desktop app). Nothing is installed and
nothing else is needed: everything it uses is in this folder. Needs 64-bit Windows 10
or later.

prismark.exe is the command line (prismark help). The desktop app starts it for
every run, so keep the folder together.

Results are saved in %LOCALAPPDATA%\\prismark\\results, shared by the app and the
command line.

$COMPILE_TXT
TXT

# Every DLL a binary imports must be beside it (Windows looks in the folder of the program that loads it: the
# app's folder for the app, its plugins and the runner; tools\bin for the build tools) or be part of Windows.
OBJDUMP=$LLVM_MINGW/bin/llvm-objdump
missing=0
while read -r bin; do
  case $bin in "$STAGE"/tools/*) dir=$STAGE/tools/bin ;; *) dir=$STAGE ;; esac
  while read -r dll; do
    if [ ! -e "$dir/$dll" ] && ! [[ ${dll,,} =~ ^(api-ms-win-|kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|ws2_32|winmm|powrprof|pdh|bcrypt|bcryptprimitives|comdlg32|imm32|dwmapi|uxtheme|version|netapi32|userenv|shlwapi|setupapi|cfgmgr32|d3d11|d3d12|dxgi|dcomp|d3d9|opengl32|authz|mpr|wtsapi32|crypt32|secur32|ntdll|dwrite|winspool|shcore|comctl32|rpcrt4|iphlpapi|propsys|runtimeobject|msvcrt|psapi|dbghelp) ]]; then
      echo "missing: $dll (needed by ${bin#"$STAGE"/})"; missing=1
    fi
  done < <("$OBJDUMP" -p "$bin" | sed -n 's/^ *DLL Name: //p' | sort -u)
done < <(find "$STAGE" -name '*.exe' -o -name '*.dll')
[ $missing -eq 0 ] || { echo "package incomplete"; exit 1; }

(cd "$OUT" && rm -f "$NAME.zip" && zip -qr9 "$NAME.zip" "$NAME")
echo "$OUT/$NAME.zip"
