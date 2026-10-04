# Third-party kernel sources, fetched at pinned versions and verified by
# SHA-256. The hashes are compiled into the core and recorded in every result.
#
# Offline builds: point FETCHCONTENT_SOURCE_DIR_PMK_ZSTD / _PMK_LUA at unpacked
# copies, and PRISMARK_STB_DIR at a directory holding the two stb headers.

include(FetchContent)

set(PRISMARK_ZSTD_VERSION 1.5.6)
set(PRISMARK_ZSTD_SHA256 8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1)
set(PRISMARK_LUA_VERSION 5.4.7)
set(PRISMARK_LUA_SHA256 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)
set(PRISMARK_STB_COMMIT 2c980bb59875b0d32144a71867fbdebb2f77cd20)
set(PRISMARK_STB_IMAGE_SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
set(PRISMARK_STB_WRITE_SHA256 cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05)

# SOURCE_SUBDIR points at a directory without a CMakeLists.txt, so the
# sources are only downloaded; Prismark compiles them itself, once per tier.
FetchContent_Declare(pmk_zstd
  URL https://github.com/facebook/zstd/releases/download/v${PRISMARK_ZSTD_VERSION}/zstd-${PRISMARK_ZSTD_VERSION}.tar.gz
  URL_HASH SHA256=${PRISMARK_ZSTD_SHA256}
  SOURCE_SUBDIR prismark-sources-only
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(pmk_lua
  URL https://www.lua.org/ftp/lua-${PRISMARK_LUA_VERSION}.tar.gz
  URL_HASH SHA256=${PRISMARK_LUA_SHA256}
  SOURCE_SUBDIR prismark-sources-only
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pmk_zstd pmk_lua)

set(PRISMARK_STB_DIR "${CMAKE_BINARY_DIR}/_deps/stb" CACHE PATH "Directory with stb_image.h and stb_image_write.h")
foreach(f IN ITEMS stb_image.h:${PRISMARK_STB_IMAGE_SHA256} stb_image_write.h:${PRISMARK_STB_WRITE_SHA256})
  string(REPLACE ":" ";" pair ${f})
  list(GET pair 0 name)
  list(GET pair 1 hash)
  file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/${PRISMARK_STB_COMMIT}/${name}
    ${PRISMARK_STB_DIR}/${name} EXPECTED_HASH SHA256=${hash} STATUS st)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Could not fetch ${name} (stb ${PRISMARK_STB_COMMIT}): ${st}")
  endif()
endforeach()

set(_zstd ${pmk_zstd_SOURCE_DIR}/lib)
set(PRISMARK_ZSTD_SOURCES
  ${_zstd}/common/debug.c ${_zstd}/common/entropy_common.c ${_zstd}/common/error_private.c
  ${_zstd}/common/fse_decompress.c ${_zstd}/common/xxhash.c ${_zstd}/common/zstd_common.c
  ${_zstd}/compress/fse_compress.c ${_zstd}/compress/hist.c ${_zstd}/compress/huf_compress.c
  ${_zstd}/compress/zstd_compress.c ${_zstd}/compress/zstd_compress_literals.c
  ${_zstd}/compress/zstd_compress_sequences.c ${_zstd}/compress/zstd_compress_superblock.c
  ${_zstd}/compress/zstd_double_fast.c ${_zstd}/compress/zstd_fast.c ${_zstd}/compress/zstd_lazy.c
  ${_zstd}/compress/zstd_ldm.c ${_zstd}/compress/zstd_opt.c
  # Decompression is only used by the unit tests to verify round trips.
  ${_zstd}/decompress/huf_decompress.c ${_zstd}/decompress/zstd_ddict.c
  ${_zstd}/decompress/zstd_decompress.c ${_zstd}/decompress/zstd_decompress_block.c)
set(PRISMARK_ZSTD_INCLUDE ${_zstd} ${_zstd}/common)
# No assembly, no runtime BMI2 dispatch and no intrinsics: only the compiler's code generation varies per tier.
set(PRISMARK_ZSTD_DEFINES ZSTD_DISABLE_ASM DYNAMIC_BMI2=0 ZSTD_NO_INTRINSICS ZSTD_LEGACY_SUPPORT=0 DEBUGLEVEL=0)
set_source_files_properties(${PRISMARK_ZSTD_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

set(PRISMARK_LUA_INCLUDE ${pmk_lua_SOURCE_DIR}/src)
if(WIN32)
  set(PRISMARK_LUA_DEFINES "")
else()
  set(PRISMARK_LUA_DEFINES LUA_USE_POSIX)
endif()

# Recorded in every result (harness.sources).
set(PRISMARK_SOURCES_JSON
  "{\"zstd\":{\"version\":\"${PRISMARK_ZSTD_VERSION}\",\"sha256\":\"${PRISMARK_ZSTD_SHA256}\"},\
\"lua\":{\"version\":\"${PRISMARK_LUA_VERSION}\",\"sha256\":\"${PRISMARK_LUA_SHA256}\"},\
\"stb\":{\"commit\":\"${PRISMARK_STB_COMMIT}\",\"stb_image_sha256\":\"${PRISMARK_STB_IMAGE_SHA256}\",\
\"stb_image_write_sha256\":\"${PRISMARK_STB_WRITE_SHA256}\"}}")
