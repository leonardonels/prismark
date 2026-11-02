# Android ARM64 with the NDK's Clang (a recorded fallback, spec 5.1): wraps the
# NDK toolchain file. Set ANDROID_NDK (or ANDROID_NDK_HOME) to the NDK root.
# K1 and K1x are not built for Android (spec 5.3).

if(NOT DEFINED ENV{ANDROID_NDK} AND DEFINED ENV{ANDROID_NDK_HOME})
  set(ENV{ANDROID_NDK} $ENV{ANDROID_NDK_HOME})
endif()
if(NOT DEFINED ENV{ANDROID_NDK})
  message(FATAL_ERROR "set ANDROID_NDK to the Android NDK root")
endif()
set(ANDROID_ABI arm64-v8a CACHE STRING "")
set(ANDROID_PLATFORM android-28 CACHE STRING "") # getrandom, posix_spawn, sched_getcpu
set(ANDROID_STL c++_static CACHE STRING "")
# 16 KB-page devices (Android 15 and later) load only libraries aligned for them; the default since NDK r28.
set(ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON CACHE BOOL "")
include($ENV{ANDROID_NDK}/build/cmake/android.toolchain.cmake)
