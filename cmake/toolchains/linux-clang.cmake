# Pinned upstream LLVM/Clang on Linux (x86-64 and ARM64).
#
# Install on Ubuntu from apt.llvm.org:
#   wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19
#
# The top-level CMakeLists checks the compiler major version against
# PRISMARK_CLANG_VERSION; results record the full compiler version.

set(PRISMARK_CLANG_VERSION 19 CACHE STRING "Pinned upstream Clang major version")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PRISMARK_CLANG_VERSION)

find_program(PRISMARK_CLANG_C NAMES clang-${PRISMARK_CLANG_VERSION}
  HINTS /usr/lib/llvm-${PRISMARK_CLANG_VERSION}/bin NO_CACHE)
find_program(PRISMARK_CLANG_CXX NAMES clang++-${PRISMARK_CLANG_VERSION}
  HINTS /usr/lib/llvm-${PRISMARK_CLANG_VERSION}/bin NO_CACHE)
if(NOT PRISMARK_CLANG_C OR NOT PRISMARK_CLANG_CXX)
  message(FATAL_ERROR
    "clang-${PRISMARK_CLANG_VERSION} not found. Install it with "
    "`wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh ${PRISMARK_CLANG_VERSION}`, "
    "or configure with the `dev` preset to use any compiler (results are then unverified).")
endif()

set(CMAKE_C_COMPILER ${PRISMARK_CLANG_C})
set(CMAKE_CXX_COMPILER ${PRISMARK_CLANG_CXX})
set(PRISMARK_PINNED_TOOLCHAIN ON CACHE BOOL "Configured with the pinned toolchain" FORCE)
