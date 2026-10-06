# K1 — in-process compile with Clang/LLVM linked as a library. Offered on desktops and iPads, not on
# phones (the engine leaves it out of their test set). iOS/iPadOS builds need an LLVM built for iOS,
# given with PRISMARK_K1_LLVM_DIR; the host's LLVM is never used there.
#
# Needs the Clang and LLVM CMake packages (e.g. libclang-19-dev and
# llvm-19-dev from apt.llvm.org). For official results LLVM itself is built
# at the baseline ISA (spec 3.3); a distribution build records its origin.
# K1 lives only in the baseline tier: it is not part of ISA uplift.

set(PRISMARK_K1 AUTO CACHE STRING "Build K1 (in-process Clang): AUTO, ON or OFF")
set_property(CACHE PRISMARK_K1 PROPERTY STRINGS AUTO ON OFF)
set(PRISMARK_K1_LLVM_DIR "" CACHE PATH "Prefix of the LLVM/Clang installation used by K1")
# K1's compiler is part of the workload: it must be the version the snapshot is prepared with
# (tools/k1x/prepare.py uses clang-19), whatever compiler builds Prismark itself.
set(PRISMARK_K1_LLVM_MAJOR 19)
set(PRISMARK_HAVE_K1 OFF)

if((PRISMARK_DESKTOP OR (IOS AND PRISMARK_K1_LLVM_DIR)) AND NOT PRISMARK_K1 STREQUAL "OFF")
  # Probe for the headers first: distributions often ship the Clang CMake package without them,
  # and a partial package fails inside find_package.
  if(IOS)
    set(_k1_prefixes ${PRISMARK_K1_LLVM_DIR})
  else()
    # Debian/Ubuntu (apt.llvm.org), Fedora (clang19-devel, llvm19-devel), Homebrew.
    set(_k1_prefixes ${PRISMARK_K1_LLVM_DIR} /usr/lib/llvm-${PRISMARK_K1_LLVM_MAJOR}
      /usr/lib64/llvm${PRISMARK_K1_LLVM_MAJOR} /usr/lib/llvm${PRISMARK_K1_LLVM_MAJOR}
      /opt/homebrew/opt/llvm@${PRISMARK_K1_LLVM_MAJOR} /usr/local/opt/llvm@${PRISMARK_K1_LLVM_MAJOR})
  endif()
  find_path(PRISMARK_K1_INCLUDE clang/Frontend/CompilerInstance.h PATHS ${_k1_prefixes} PATH_SUFFIXES include
    NO_DEFAULT_PATH)
  if(PRISMARK_K1_INCLUDE)
    get_filename_component(_k1_prefix ${PRISMARK_K1_INCLUDE} DIRECTORY)
    find_package(Clang CONFIG QUIET PATHS ${_k1_prefix}/lib/cmake/clang NO_DEFAULT_PATH)
  endif()
  if(PRISMARK_K1_INCLUDE AND Clang_FOUND AND NOT LLVM_VERSION_MAJOR VERSION_EQUAL PRISMARK_K1_LLVM_MAJOR)
    message(WARNING "K1 needs LLVM ${PRISMARK_K1_LLVM_MAJOR}, found ${LLVM_PACKAGE_VERSION}; K1 is not built")
    set(Clang_FOUND OFF)
  endif()
  if(PRISMARK_K1_INCLUDE AND Clang_FOUND)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(TARGET clang-cpp AND TARGET LLVM)
      set(_k1_libs clang-cpp LLVM)
    else()
      llvm_map_components_to_libnames(_k1_llvm support core aarch64codegen aarch64asmparser aarch64desc aarch64info)
      set(_k1_libs clangFrontend clangCodeGen clangDriver clangBasic ${_k1_llvm})
    endif()
    set(_k1_defs ${LLVM_DEFINITIONS})
    separate_arguments(_k1_defs)
    if(NOT LLVM_ENABLE_RTTI)
      set_source_files_properties(kernels/k1_compile/k1.cpp PROPERTIES COMPILE_OPTIONS "-fno-rtti")
    endif()
    prismark_add_kernel(k1_compile BASELINE_ONLY SOURCES kernels/k1_compile/k1.cpp
      INCLUDE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS} LIBS ${_k1_libs})
    set(PRISMARK_HAVE_K1 ON)
    set(PRISMARK_K1_LLVM_VERSION ${LLVM_PACKAGE_VERSION})
    message(STATUS "K1: in-process Clang ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
  elseif(PRISMARK_K1 STREQUAL "ON")
    message(FATAL_ERROR "PRISMARK_K1=ON but the Clang development package was not found")
  else()
    message(STATUS "K1: Clang development package not found; K1 will be reported as unavailable")
  endif()
endif()
