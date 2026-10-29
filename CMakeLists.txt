cmake_minimum_required(VERSION 3.25)
project(prismark VERSION 0.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Prismark needs Clang (clang-cl on Windows); the kernels rely on GNU C extensions")
endif()
# clang-cl would read -Wall as -Weverything; on Windows use the GNU-style clang driver (see the toolchain file).
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- platform ----
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(PRISMARK_PAL linux)
elseif(APPLE)
  set(PRISMARK_PAL darwin)
  # Every binary is built at one ISA level (cmake/tiers.cmake): a universal build would give the x86-64 slice the
  # ARM64 -march, and an x86-64 build on Apple silicon would measure Rosetta, not the CPU.
  list(LENGTH CMAKE_OSX_ARCHITECTURES _narch)
  if(_narch GREATER 1)
    message(FATAL_ERROR "Prismark builds one architecture at a time (CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})")
  endif()
  if(_narch EQUAL 1)
    set(PRISMARK_TARGET_ARCH ${CMAKE_OSX_ARCHITECTURES})
  else()
    set(PRISMARK_TARGET_ARCH ${CMAKE_SYSTEM_PROCESSOR})
  endif()
  if(NOT IOS)
    execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE _host_arm64 OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_host_arm64 STREQUAL "1" AND NOT PRISMARK_TARGET_ARCH STREQUAL "arm64")
      message(FATAL_ERROR "Apple silicon host but building ${PRISMARK_TARGET_ARCH} (Rosetta): "
        "configure with -DCMAKE_OSX_ARCHITECTURES=arm64 (or use the macos-clang preset), and an arm64 CMake")
    endif()
  endif()
elseif(WIN32)
  set(PRISMARK_PAL windows)
else()
  message(FATAL_ERROR "No platform layer for ${CMAKE_SYSTEM_NAME}")
endif()
# Desktop platforms build K1/K1x; phones do not offer them, iPads offer K1 only (spec 5.3, cmake/k1.cmake).
if(ANDROID OR IOS)
  set(PRISMARK_DESKTOP OFF)
else()
  set(PRISMARK_DESKTOP ON)
endif()

option(PRISMARK_PINNED_TOOLCHAIN "Configured with the pinned toolchain" OFF)
if(PRISMARK_PINNED_TOOLCHAIN)
  string(REGEX MATCH "^[0-9]+" _clang_major "${CMAKE_C_COMPILER_VERSION}")
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT _clang_major STREQUAL PRISMARK_CLANG_VERSION)
    message(FATAL_ERROR "Pinned toolchain expects Clang ${PRISMARK_CLANG_VERSION}, "
      "got ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
  endif()
else()
  message(WARNING "Not using the pinned toolchain (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}); "
    "fine for development, but results are not comparable to official builds.")
endif()

include(cmake/tiers.cmake)
include(cmake/third_party.cmake)

execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  OUTPUT_VARIABLE PRISMARK_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PRISMARK_GIT_SHA)
  set(PRISMARK_GIT_SHA unknown)
else()
  # Built from a commit plus changes: the result must not claim the commit alone (checked at configure time).
  execute_process(COMMAND git diff --quiet HEAD -- WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} RESULT_VARIABLE _dirty
    ERROR_QUIET)
  if(_dirty EQUAL 1)
    string(APPEND PRISMARK_GIT_SHA "-dirty")
  endif()
endif()

find_package(Threads REQUIRED)

# ---- kernels ----
prismark_add_kernel(k7_chase SOURCES kernels/k7_chase/k7.c)
prismark_add_kernel(k9_calib SOURCES kernels/k9_calib/k9.c)
prismark_add_kernel(k2_render SOURCES kernels/k2_render/k2.c)
prismark_add_kernel(k3_compress SOURCES kernels/k3_compress/k3.c ${PRISMARK_ZSTD_SOURCES}
  INCLUDE ${PRISMARK_ZSTD_INCLUDE} DEFINES ${PRISMARK_ZSTD_DEFINES})
prismark_add_kernel(k4_image SOURCES kernels/k4_image/k4.c INCLUDE ${PRISMARK_STB_DIR})
prismark_add_kernel(k5_json SOURCES kernels/k5_json/k5.c)
prismark_add_kernel(k6_lua SOURCES kernels/k6_lua/k6.c kernels/k6_lua/lua_one.c
  INCLUDE ${PRISMARK_LUA_INCLUDE} DEFINES ${PRISMARK_LUA_DEFINES})
prismark_add_kernel(k11_stream SOURCES kernels/k11_stream/k11.c)
prismark_add_kernel(k12_vector SOURCES kernels/k12_vector/k12.c)
include(cmake/k1.cmake)
prismark_kernel_tables()
if(PRISMARK_HAVE_K1)
  target_compile_definitions(pmk_kernels_baseline PRIVATE PMK_HAVE_K1)
endif()

# ---- support libraries ----
add_library(pmk_result STATIC src/result/buf.c src/result/json.c src/result/jparse.c)
target_include_directories(pmk_result PUBLIC src/result)

add_library(pmk_stats STATIC src/stats/stats.c src/stats/steady.c)
target_include_directories(pmk_stats PUBLIC src/stats)
if(UNIX)
  target_link_libraries(pmk_stats PUBLIC m)
endif()

if(PRISMARK_PAL STREQUAL "linux")
  set(PRISMARK_PAL_SOURCES pal/linux/pal_linux.c pal/linux/sysfs.c pal/posix/pal_posix.c)
  set(PRISMARK_PAL_LIBS ${CMAKE_DL_LIBS})
elseif(PRISMARK_PAL STREQUAL "darwin")
  set(PRISMARK_PAL_SOURCES pal/darwin/pal_darwin.c pal/darwin/sensors.c pal/posix/pal_posix.c)
  if(IOS)
    set(PRISMARK_PAL_LIBS "")
  else()
    set(PRISMARK_PAL_LIBS "-framework CoreGraphics" "-framework CoreFoundation" "-framework IOKit")
  endif()
else()
  set(PRISMARK_PAL_SOURCES pal/windows/pal_windows.c)
  set(PRISMARK_PAL_LIBS powrprof bcrypt winmm user32 pdh setupapi)
endif()

# ---- core library: engine, analysis, PAL ----
# Built at the baseline ISA, as the kernels are.

add_library(prismark
  src/engine/runner.c
  src/engine/workload.c
  src/engine/mode_cold_burst.c
  src/engine/mode_periodic.c
  src/engine/mode_throughput.c
  src/engine/mode_stress.c
  src/engine/k1x.c
  src/engine/checksums.c
  src/engine/describe.c
  src/stats/analysis.c
  src/stats/analysis_tp.c
  src/stats/compare.c
  src/stats/headline.c
  ${PRISMARK_PAL_SOURCES})
string(REPLACE "\\" "\\\\" _sources_esc "${PRISMARK_SOURCES_JSON}")
string(REPLACE "\"" "\\\"" _sources_esc "${_sources_esc}")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/pmk_build_info.h
  CONTENT "/* Generated by CMake. */\n#define PMK_SOURCES_JSON \"${_sources_esc}\"\n#define PMK_K1_LLVM_VERSION \"${PRISMARK_K1_LLVM_VERSION}\"\n")
target_include_directories(prismark PUBLIC include PRIVATE src/engine pal ${CMAKE_BINARY_DIR}/generated)
target_compile_options(prismark PRIVATE -march=${PRISMARK_TIER_baseline_MARCH} -mtune=generic)
target_compile_definitions(prismark PRIVATE
  PMK_VERSION="${PROJECT_VERSION}" PMK_GIT_SHA="${PRISMARK_GIT_SHA}")
target_link_libraries(prismark PRIVATE pmk_kernels_baseline pmk_stats pmk_result ${PRISMARK_PAL_LIBS}
  Threads::Threads)

# ---- applications ----
if(IOS)
  return() # iOS: the static libraries above are wrapped by bindings/swift
endif()
add_executable(prismark_cli apps/cli/main.c)
set_target_properties(prismark_cli PROPERTIES OUTPUT_NAME prismark)
target_link_libraries(prismark_cli PRIVATE prismark Threads::Threads)
if(PRISMARK_HAVE_K1)
  set_target_properties(prismark_cli PROPERTIES LINKER_LANGUAGE CXX)
endif()
if(WIN32)
  # Compiling code runs Clang in this process, and large units recurse deeper than the 1 MB stack Windows gives
  # by default (the clang program itself compiles on a thread with more). 8 MB, as Linux gives; threads the
  # runner starts get the same.
  if(MINGW)
    target_link_options(prismark_cli PRIVATE -Wl,--stack,8388608)
  else()
    target_link_options(prismark_cli PRIVATE -Wl,/STACK:8388608)
  endif()
endif()

install(TARGETS prismark_cli)
# Reference systems for "prismark compare RUN REFERENCE": installed beside the program, and read from the source
# tree when it runs from the build folder.
file(GLOB _cli_references ${PROJECT_SOURCE_DIR}/references/*.json)
install(FILES ${_cli_references} DESTINATION share/prismark/references)
target_compile_definitions(prismark_cli PRIVATE PRISMARK_REFERENCES_DIR="${PROJECT_SOURCE_DIR}/references")

include(apps/gui/gui.cmake)

# Android: the JNI bridge as a shared library for the app.
if(ANDROID)
  add_library(prismark_jni SHARED bindings/android/prismark_jni.c)
  target_link_libraries(prismark_jni PRIVATE prismark log)
endif()

# ---- tests ----
include(CTest)
if(BUILD_TESTING)
  add_executable(test_core tests/test_core.c)
  target_include_directories(test_core PRIVATE ${PRISMARK_ZSTD_INCLUDE} src/engine pal)
  target_link_libraries(test_core PRIVATE prismark pmk_stats pmk_result pmk_kernels_baseline ${PRISMARK_PAL_LIBS})
  if(PRISMARK_HAVE_K1)
    set_target_properties(test_core PROPERTIES LINKER_LANGUAGE CXX)
  endif()
  add_test(NAME core COMMAND test_core)
  # The command line: options, messages, exit status, one short run (tests/test_cli.py, about 15 s).
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME cli COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tests/test_cli.py
             $<TARGET_FILE:prismark_cli> ${PROJECT_SOURCE_DIR})
    set_tests_properties(cli PROPERTIES TIMEOUT 300)
  endif()
endif()
