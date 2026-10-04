# Pinned upstream LLVM/Clang on Windows (x86-64 or ARM64): the GNU-style clang
# driver targeting the MSVC ABI (not clang-cl, which reads -Wall as -Weverything).
#
# Install the LLVM release (https://github.com/llvm/llvm-project/releases) and
# Ninja; build from a "Developer PowerShell" so the Windows SDK libraries are found.

set(PRISMARK_CLANG_VERSION 19 CACHE STRING "Pinned upstream Clang major version")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PRISMARK_CLANG_VERSION)
find_program(PRISMARK_CLANG_C NAMES clang HINTS "$ENV{ProgramFiles}/LLVM/bin" NO_CACHE REQUIRED)
find_program(PRISMARK_CLANG_CXX NAMES clang++ HINTS "$ENV{ProgramFiles}/LLVM/bin" NO_CACHE REQUIRED)
find_program(PRISMARK_LLVM_RC NAMES llvm-rc HINTS "$ENV{ProgramFiles}/LLVM/bin" NO_CACHE)
set(CMAKE_C_COMPILER ${PRISMARK_CLANG_C})
set(CMAKE_CXX_COMPILER ${PRISMARK_CLANG_CXX})
if(PRISMARK_LLVM_RC)
  set(CMAKE_RC_COMPILER ${PRISMARK_LLVM_RC})
endif()
set(PRISMARK_PINNED_TOOLCHAIN ON CACHE BOOL "Configured with the pinned toolchain" FORCE)
