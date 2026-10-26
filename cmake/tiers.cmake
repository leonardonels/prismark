# ISA level. Every kernel and the harness are compiled once, with only this level's -march flag, so every
# host of an architecture runs the same instructions:
#
#   x86-64-v2 on x86-64, armv8.2-a on ARM64
#
# Kernels are built without FP contraction, so their floating-point operations, and with them their outputs,
# are bit-identical on every host and compiler (the checksum gate compares x86-64 against ARM64).

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  set(PRISMARK_ISA x86_64)
  set(PRISMARK_TIER_baseline_MARCH "x86-64-v2")
  set(PRISMARK_TIER_baseline_LEVEL "x86-64-v2")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(PRISMARK_ISA aarch64)
  set(PRISMARK_TIER_baseline_MARCH "armv8.2-a")
  set(PRISMARK_TIER_baseline_LEVEL "armv8.2-a")
else()
  message(FATAL_ERROR "Prismark supports x86-64 and ARM64 (got ${CMAKE_SYSTEM_PROCESSOR})")
endif()

# Kernel flags: no FP contraction (bit-identical outputs), no fast math.
set(PRISMARK_KERNEL_FLAGS -ffp-contract=off -fno-fast-math)

function(_prismark_tier_flags target tier)
  set(march "${PRISMARK_TIER_${tier}_MARCH}")
  target_compile_options(${target} PRIVATE -march=${march} -mtune=generic ${PRISMARK_KERNEL_FLAGS})
  target_compile_definitions(${target} PRIVATE
    PMK_TIER=${tier} PMK_TIER_KIND="baseline" PMK_TIER_LEVEL="${PRISMARK_TIER_${tier}_LEVEL}"
    PMK_TIER_MARCH="-march=${march}")
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/kernels ${PROJECT_SOURCE_DIR}/src/stats)
endfunction()

# prismark_add_kernel(<name> SOURCES <files>... [INCLUDE <dirs>...] [DEFINES <defs>...] [LIBS <libs>...])
# registers a kernel; prismark_kernel_tables() then builds the kernel table from the registered kernels.
function(prismark_add_kernel name)
  cmake_parse_arguments(K "" "" "SOURCES;INCLUDE;DEFINES;LIBS" ${ARGN})
  set_property(GLOBAL APPEND PROPERTY PRISMARK_KERNELS ${name})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_SOURCES ${K_SOURCES})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_INCLUDE ${K_INCLUDE})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_DEFINES ${K_DEFINES})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_LIBS ${K_LIBS})
endfunction()

# Builds pmk_kernels_baseline, the static kernel table linked into the core.
function(prismark_kernel_tables)
  get_property(kernels GLOBAL PROPERTY PRISMARK_KERNELS)
  set(target pmk_kernels_baseline)
  add_library(${target} STATIC ${PROJECT_SOURCE_DIR}/kernels/table.c)
  target_include_directories(${target} INTERFACE ${PROJECT_SOURCE_DIR}/kernels)
  _prismark_tier_flags(${target} baseline)
  foreach(k IN LISTS kernels)
    get_property(srcs GLOBAL PROPERTY PRISMARK_KERNEL_${k}_SOURCES)
    get_property(incs GLOBAL PROPERTY PRISMARK_KERNEL_${k}_INCLUDE)
    get_property(defs GLOBAL PROPERTY PRISMARK_KERNEL_${k}_DEFINES)
    get_property(libs GLOBAL PROPERTY PRISMARK_KERNEL_${k}_LIBS)
    target_sources(${target} PRIVATE ${srcs})
    target_include_directories(${target} PRIVATE ${incs})
    target_compile_definitions(${target} PRIVATE ${defs})
    target_link_libraries(${target} PRIVATE ${libs})
  endforeach()
  if(UNIX)
    target_link_libraries(${target} PRIVATE m)
  endif()
endfunction()
