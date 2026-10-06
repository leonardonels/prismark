#!/usr/bin/env bash
# Cross-builds Prismark for Windows x86-64 on Linux and zips a portable folder
# (desktop app, runner, tier modules, Qt and C++ runtime DLLs): unzip and run prismark-gui.exe.
#
#   tools/package-windows.sh LLVM_MINGW QT_WINDOWS QT_HOST [OUT_DIR]
#
#   LLVM_MINGW  llvm-mingw 20250114 (LLVM 19.1.7), ucrt-ubuntu-x86_64 release
#   QT_WINDOWS  Qt for Windows built with llvm-mingw, e.g. .../Qt/6.8.3/llvm-mingw_64
#   QT_HOST     host Qt of the same version, for moc and rcc, e.g. .../Qt/6.8.3/gcc_64
#
# Both Qts install with aqtinstall:
#   aqt install-qt windows desktop 6.8.3 win64_llvm_mingw -O Qt
#   aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O Qt
#
# K1 and K1x (Compiling code, Full software build) are not built: they need LLVM and Clang
# libraries built for Windows. The app lists them as unavailable.
set -euo pipefail

[ $# -ge 3 ] || { sed -n 2,16p "$0"; exit 2; }
LLVM_MINGW=$(realpath "$1")
QT_WIN=$(realpath "$2")
QT_HOST=$(realpath "$3")
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${4:-$SRC/build}")
BUILD=$SRC/build/windows-cross

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/windows-cross-llvm-mingw.cmake" \
  -DLLVM_MINGW="$LLVM_MINGW" -DCMAKE_PREFIX_PATH="$QT_WIN" -DQT_HOST_PATH="$QT_HOST" \
  -DPRISMARK_GUI=ON -DPRISMARK_K1=OFF -DBUILD_TESTING=OFF
cmake --build "$BUILD"

VERSION=$(sed -n 's/^project(prismark VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
NAME=prismark-$VERSION-windows-x86_64
STAGE=$OUT/$NAME
rm -rf "$STAGE" "$OUT/$NAME.zip"
mkdir -p "$STAGE"/{platforms,styles,imageformats,iconengines}

cp "$BUILD"/prismark.exe "$BUILD"/prismark-gui.exe "$BUILD"/prismark-kernels-*.dll "$STAGE"/
for m in Core Gui Widgets Svg; do cp "$QT_WIN/bin/Qt6$m.dll" "$STAGE"/; done
cp "$QT_WIN"/plugins/platforms/qwindows.dll "$STAGE"/platforms/
cp "$QT_WIN"/plugins/styles/qmodernwindowsstyle.dll "$STAGE"/styles/ 2>/dev/null || true
cp "$QT_WIN"/plugins/imageformats/qsvg.dll "$STAGE"/imageformats/
cp "$QT_WIN"/plugins/iconengines/qsvgicon.dll "$STAGE"/iconengines/
# The C++ runtime the binaries were linked against (newer than Qt's own copy, and compatible with it).
cp "$LLVM_MINGW"/x86_64-w64-mingw32/bin/{libc++,libunwind}.dll "$STAGE"/
cp "$SRC"/LICENSE "$STAGE"/

# Every DLL a binary imports must be in the folder or be part of Windows.
OBJDUMP=$LLVM_MINGW/bin/llvm-objdump
missing=0
while read -r dll; do
  if [ ! -e "$STAGE/$dll" ] && ! [[ ${dll,,} =~ ^(api-ms-win-|kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|ws2_32|winmm|powrprof|bcrypt|bcryptprimitives|comdlg32|imm32|dwmapi|uxtheme|version|netapi32|userenv|shlwapi|setupapi|cfgmgr32|d3d11|d3d12|dxgi|dcomp|d3d9|opengl32|authz|mpr|wtsapi32|crypt32|secur32|ntdll|dwrite|winspool|shcore|comctl32|rpcrt4|iphlpapi|propsys|runtimeobject|msvcrt) ]]; then
    echo "missing: $dll"; missing=1
  fi
done < <(find "$STAGE" -name '*.exe' -o -name '*.dll' | xargs -n1 "$OBJDUMP" -p | sed -n 's/^ *DLL Name: //p' | sort -u)
[ $missing -eq 0 ] || { echo "package incomplete"; exit 1; }

(cd "$OUT" && rm -f "$NAME.zip" && zip -qr9 "$NAME.zip" "$NAME")
echo "$OUT/$NAME.zip"
