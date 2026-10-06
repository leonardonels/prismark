# Cross-compiling for Windows x86-64 from Linux with llvm-mingw (Clang 19, MinGW ABI, UCRT).
#
# Get llvm-mingw 20250114 (LLVM 19.1.7) from https://github.com/mstorsjo/llvm-mingw/releases
# and, for the desktop app, Qt for Windows built with llvm-mingw plus a host Qt of the same
# version for moc/rcc (e.g. `aqt install-qt windows desktop 6.8.3 win64_llvm_mingw` and
# `aqt install-qt linux desktop 6.8.3 linux_gcc_64`). Then:
#
#   cmake -B build/windows-cross -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-cross-llvm-mingw.cmake \
#     -DLLVM_MINGW=/path/to/llvm-mingw -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/llvm-mingw_64 \
#     -DQT_HOST_PATH=/path/to/Qt/6.8.3/gcc_64
#
# tools/package-windows.sh does all of this and zips a portable folder.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PRISMARK_CLANG_VERSION 19 CACHE STRING "Pinned upstream Clang major version")
set(LLVM_MINGW "$ENV{LLVM_MINGW}" CACHE PATH "llvm-mingw installation")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PRISMARK_CLANG_VERSION LLVM_MINGW)
if(NOT LLVM_MINGW)
  message(FATAL_ERROR "Set LLVM_MINGW to the llvm-mingw installation")
endif()

set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(PRISMARK_PINNED_TOOLCHAIN ON CACHE BOOL "Configured with the pinned toolchain" FORCE)
