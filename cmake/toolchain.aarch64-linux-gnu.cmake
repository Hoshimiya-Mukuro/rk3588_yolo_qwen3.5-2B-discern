# CMake toolchain file for Rockchip RK3588/RK3568 (aarch64)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.aarch64-linux-gnu.cmake ...
#
# Override with -DTOOLCHAIN_PATH=... -DTOOLCHAIN_PREFIX=...

set(TOOLCHAIN_PATH "/opt/gcc-arm-15.2/bin" CACHE STRING "Cross-compiler directory")
set(TOOLCHAIN_PREFIX "aarch64-linux-gnu-" CACHE STRING "Cross-compiler prefix")

set(CMAKE_SYSTEM_NAME     Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ─── Compilers ─────────────────────────────────────────────────────
set(TOOLCHAIN_FULL "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}")

set(CMAKE_C_COMPILER    "${TOOLCHAIN_FULL}gcc")
set(CMAKE_CXX_COMPILER  "${TOOLCHAIN_FULL}g++")
set(CMAKE_AR            "${TOOLCHAIN_FULL}ar"   CACHE FILEPATH "Archiver")
set(CMAKE_STRIP         "${TOOLCHAIN_FULL}strip" CACHE FILEPATH "Strip")

# ─── Search Policy ─────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
