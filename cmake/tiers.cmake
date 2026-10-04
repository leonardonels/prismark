# ISA tiers. Every kernel is compiled once per tier, with only that tier's
# -march flag, into that tier's library. The core reaches a tier only through
# its pmk_kernels table, so tiers cannot mix inside one measurement.
#
#   baseline   x86-64-v2 / armv8.2-a; static library linked into the core
#   max levels x86-64-v3, x86-64-v4 / armv8.2-a+dotprod+fp16, armv9-a+sve2;
#              loadable modules (hidden visibility, one exported entry
#              point), prebuilt at these fixed levels and selected at
#              runtime by what the CPU supports
#
# Kernels are built without FP contraction, so every tier performs the same
# floating-point operations and produces bit-identical outputs; ISA uplift
# then measures only how well the same source maps onto the wider ISA.

include(CheckCCompilerFlag)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  set(PRISMARK_ISA x86_64)
  set(PRISMARK_TIER_baseline_MARCH "x86-64-v2")
  set(PRISMARK_TIER_baseline_LEVEL "x86-64-v2")
  set(_max_tiers v3 v4)
  set(PRISMARK_TIER_v3_MARCH "x86-64-v3")
  set(PRISMARK_TIER_v3_LEVEL "x86-64-v3")
  set(PRISMARK_TIER_v4_MARCH "x86-64-v4")
  set(PRISMARK_TIER_v4_LEVEL "x86-64-v4")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(PRISMARK_ISA aarch64)
  set(PRISMARK_TIER_baseline_MARCH "armv8.2-a")
  set(PRISMARK_TIER_baseline_LEVEL "armv8.2-a")
  set(_max_tiers dotprod sve2)
  set(PRISMARK_TIER_dotprod_MARCH "armv8.2-a+dotprod+fp16")
  set(PRISMARK_TIER_dotprod_LEVEL "armv8.2-a+dotprod+fp16")
  set(PRISMARK_TIER_sve2_MARCH "armv9-a+sve2")
  set(PRISMARK_TIER_sve2_LEVEL "armv9-a+sve2")
else()
  message(FATAL_ERROR "Prismark supports x86-64 and ARM64 (got ${CMAKE_SYSTEM_PROCESSOR})")
endif()

option(PRISMARK_MAX_TIERS "Build the max-level ISA tiers as loadable modules" ON)
# Static-only platforms (iOS) cannot load modules; the max tiers are then off.
if(IOS)
  set(PRISMARK_MAX_TIERS OFF)
endif()

set(PRISMARK_TIERS baseline)
if(PRISMARK_MAX_TIERS)
  foreach(t IN LISTS _max_tiers)
    check_c_compiler_flag("-march=${PRISMARK_TIER_${t}_MARCH}" _has_march_${t})
    if(_has_march_${t})
      list(APPEND PRISMARK_TIERS ${t})
    else()
      message(WARNING "Compiler cannot target ${PRISMARK_TIER_${t}_MARCH}; tier ${t} not built")
    endif()
  endforeach()
endif()
set(PRISMARK_MAX_TIER_LIST ${PRISMARK_TIERS})
list(REMOVE_ITEM PRISMARK_MAX_TIER_LIST baseline)

# Kernel flags shared by every tier: no FP contraction (bit-identical outputs), no fast math.
set(PRISMARK_KERNEL_FLAGS -ffp-contract=off -fno-fast-math)

function(_prismark_tier_flags target tier)
  set(march "${PRISMARK_TIER_${tier}_MARCH}")
  target_compile_options(${target} PRIVATE -march=${march} -mtune=generic ${PRISMARK_KERNEL_FLAGS})
  if(tier STREQUAL "baseline")
    set(kind baseline)
  else()
    set(kind max)
  endif()
  target_compile_definitions(${target} PRIVATE
    PMK_TIER=${tier} PMK_TIER_KIND="${kind}" PMK_TIER_LEVEL="${PRISMARK_TIER_${tier}_LEVEL}"
    PMK_TIER_MARCH="-march=${march}")
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/kernels ${PROJECT_SOURCE_DIR}/src/stats)
endfunction()

# prismark_add_kernel(<name> SOURCES <files>... [INCLUDE <dirs>...] [DEFINES <defs>...]
#                     [BASELINE_ONLY] [LIBS <libs>...])
# registers a kernel; prismark_kernel_tables() then builds every tier from the registered kernels.
function(prismark_add_kernel name)
  cmake_parse_arguments(K "BASELINE_ONLY" "" "SOURCES;INCLUDE;DEFINES;LIBS" ${ARGN})
  set_property(GLOBAL APPEND PROPERTY PRISMARK_KERNELS ${name})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_SOURCES ${K_SOURCES})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_INCLUDE ${K_INCLUDE})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_DEFINES ${K_DEFINES})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_LIBS ${K_LIBS})
  set_property(GLOBAL PROPERTY PRISMARK_KERNEL_${name}_BASELINE_ONLY ${K_BASELINE_ONLY})
endfunction()

# Builds pmk_kernels_baseline (static) and prismark-kernels-<tier> (module) for each max tier.
function(prismark_kernel_tables)
  get_property(kernels GLOBAL PROPERTY PRISMARK_KERNELS)
  foreach(tier IN LISTS PRISMARK_TIERS)
    if(tier STREQUAL "baseline")
      set(target pmk_kernels_baseline)
      add_library(${target} STATIC ${PROJECT_SOURCE_DIR}/kernels/table.c)
      target_include_directories(${target} INTERFACE ${PROJECT_SOURCE_DIR}/kernels)
    else()
      set(target prismark-kernels-${tier})
      add_library(${target} MODULE ${PROJECT_SOURCE_DIR}/kernels/table.c)
      if(ANDROID)
        set(_prefix lib) # APKs only package lib*.so
      else()
        set(_prefix "")
      endif()
      set_target_properties(${target} PROPERTIES PREFIX "${_prefix}" C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
      target_compile_definitions(${target} PRIVATE PMK_TIER_MODULE)
      if(NOT WIN32 AND NOT APPLE)
        target_link_options(${target} PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,defs)
      endif()
    endif()
    _prismark_tier_flags(${target} ${tier})
    foreach(k IN LISTS kernels)
      get_property(only GLOBAL PROPERTY PRISMARK_KERNEL_${k}_BASELINE_ONLY)
      if(only AND NOT tier STREQUAL "baseline")
        continue()
      endif()
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
  endforeach()
  set(PRISMARK_MODULE_TARGETS "")
  foreach(t IN LISTS PRISMARK_MAX_TIER_LIST)
    list(APPEND PRISMARK_MODULE_TARGETS prismark-kernels-${t})
  endforeach()
  set(PRISMARK_MODULE_TARGETS ${PRISMARK_MODULE_TARGETS} PARENT_SCOPE)
endfunction()
