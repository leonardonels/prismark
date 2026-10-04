# Pinned upstream LLVM/Clang on macOS (Apple silicon or x86-64), from Homebrew:
#   brew install llvm@19 ninja cmake
# Apple Clang is a fallback only (spec 5.1): configure with the `dev` preset to
# use it; its version is recorded and results are unverified.

set(PRISMARK_CLANG_VERSION 19 CACHE STRING "Pinned upstream Clang major version")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PRISMARK_CLANG_VERSION)
find_program(PRISMARK_CLANG_C NAMES clang
  HINTS /opt/homebrew/opt/llvm@${PRISMARK_CLANG_VERSION}/bin /usr/local/opt/llvm@${PRISMARK_CLANG_VERSION}/bin
  NO_DEFAULT_PATH NO_CACHE)
find_program(PRISMARK_CLANG_CXX NAMES clang++
  HINTS /opt/homebrew/opt/llvm@${PRISMARK_CLANG_VERSION}/bin /usr/local/opt/llvm@${PRISMARK_CLANG_VERSION}/bin
  NO_DEFAULT_PATH NO_CACHE)
if(NOT PRISMARK_CLANG_C OR NOT PRISMARK_CLANG_CXX)
  message(FATAL_ERROR "upstream clang ${PRISMARK_CLANG_VERSION} not found: brew install llvm@${PRISMARK_CLANG_VERSION}, "
    "or use the `dev` preset (Apple Clang, unverified results)")
endif()
set(CMAKE_C_COMPILER ${PRISMARK_CLANG_C})
set(CMAKE_CXX_COMPILER ${PRISMARK_CLANG_CXX})
execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE _sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_OSX_SYSROOT ${_sdk})
set(PRISMARK_PINNED_TOOLCHAIN ON CACHE BOOL "Configured with the pinned toolchain" FORCE)
