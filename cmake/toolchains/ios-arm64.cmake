# iOS / iPadOS ARM64: static libraries only (no loadable tier modules, no CLI),
# wrapped by bindings/swift. K1 and K1x are not built (spec 5.3).
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0)
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
set(PRISMARK_MAX_TIERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
