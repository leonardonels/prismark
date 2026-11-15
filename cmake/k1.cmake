# K1 — in-process compile with Clang/LLVM linked as a library. Offered on desktops and iPads, not on
# phones (the engine leaves it out of their test set). iOS/iPadOS builds need an LLVM built for iOS,
# given with PRISMARK_K1_LLVM_DIR; the host's LLVM is never used there.
#
# Needs the Clang and LLVM CMake packages. Packages use Prismark's own build
# (tools/build-llvm.sh: one recipe for every system, static, its own libc++,
# the baseline ISA of spec 3.3), whose id every result records; a distribution
# LLVM (libclang-19-dev and llvm-19-dev from apt.llvm.org, Homebrew) also
# works, and its results say they were not timed with that build.

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
    # Prismark's own compiler build (tools/build-llvm.sh) says so in prismark-compiler.json: static libraries
    # and its own static libc++, the same on every system. Any other LLVM still builds K1, recorded as such.
    set(_k1_record ${_k1_prefix}/prismark-compiler.json)
    set(_k1_opts)
    llvm_map_components_to_libnames(_k1_llvm support core aarch64codegen aarch64asmparser aarch64desc aarch64info)
    if(EXISTS ${_k1_record})
      file(READ ${_k1_record} _k1_json)
      string(JSON PRISMARK_K1_COMPILER_ID GET ${_k1_json} id)
      set(_rt ${_k1_prefix}/runtime)
      set(_k1_libs clangFrontend clangCodeGen clangDriver clangBasic ${_k1_llvm} -nostdlib++
        ${_rt}/lib/libc++.a ${_rt}/lib/libc++abi.a)
      if(EXISTS ${_rt}/lib/libunwind.a)
        list(APPEND _k1_libs ${_rt}/lib/libunwind.a)
      endif()
      # Its headers, not the system's: k1.cpp must agree with the library it links.
      list(APPEND _k1_opts -nostdinc++ -isystem ${_rt}/include/c++/v1)
    elseif(TARGET clang-cpp AND TARGET LLVM)
      set(PRISMARK_K1_COMPILER_ID "")
      set(_k1_libs clang-cpp LLVM)
    else()
      set(PRISMARK_K1_COMPILER_ID "")
      set(_k1_libs clangFrontend clangCodeGen clangDriver clangBasic ${_k1_llvm})
    endif()
    set(_k1_defs ${LLVM_DEFINITIONS})
    separate_arguments(_k1_defs)
    if(NOT LLVM_ENABLE_RTTI)
      list(APPEND _k1_opts -fno-rtti)
    endif()
    set_source_files_properties(kernels/k1_compile/k1.cpp PROPERTIES COMPILE_OPTIONS "${_k1_opts}")
    prismark_add_kernel(k1_compile SOURCES kernels/k1_compile/k1.cpp
      INCLUDE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS} LIBS ${_k1_libs})
    set(PRISMARK_HAVE_K1 ON)
    set(PRISMARK_K1_LLVM_VERSION ${LLVM_PACKAGE_VERSION})
    if(PRISMARK_K1_COMPILER_ID)
      message(STATUS "K1: in-process Clang ${LLVM_PACKAGE_VERSION}, Prismark's build ${PRISMARK_K1_COMPILER_ID}")
    else()
      message(STATUS "K1: in-process Clang ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR} (not Prismark's build)")
    endif()
  elseif(PRISMARK_K1 STREQUAL "ON")
    message(FATAL_ERROR "PRISMARK_K1=ON but the Clang development package was not found")
  else()
    message(STATUS "K1: Clang development package not found; K1 will be reported as unavailable")
  endif()
endif()
