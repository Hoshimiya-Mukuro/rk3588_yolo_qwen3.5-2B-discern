cmake_minimum_required(VERSION 3.10)
project(nc2rtsp VERSION 1.0 LANGUAGES C CXX)

# ─── Cross-Compilation ──────────────────────────────────────────────
# Load toolchain file via: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.aarch64-linux-gnu.cmake ...
# Or pass TOOLCHAIN_PREFIX directly:
#   cmake -DTOOLCHAIN_PREFIX=/opt/gcc-arm-15.2/bin/aarch64-linux-gnu- ...

if(DEFINED TOOLCHAIN_PREFIX)
    set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
    set(CMAKE_AR           "${TOOLCHAIN_PREFIX}ar")
    set(CMAKE_STRIP        "${TOOLCHAIN_PREFIX}strip")
endif()

# ─── C/C++ Standards ────────────────────────────────────────────────
set(CMAKE_C_STANDARD    11)
set(CMAKE_CXX_STANDARD  11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Compile Options ────────────────────────────────────────────────
option(ENABLE_MPP "Enable Rockchip MPP support at compile time" OFF)

set(COMMON_FLAGS "-Wall -Wextra -O2 -g")
if(ENABLE_MPP)
    list(APPEND COMMON_FLAGS "-DENABLE_MPP")
endif()

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS}")

# ─── Include Directories ────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include/live555
    ${CMAKE_SOURCE_DIR}/include/rockchip
    ${CMAKE_SOURCE_DIR}/include/rga
    ${CMAKE_SOURCE_DIR}/include/rknn
    ${CMAKE_SOURCE_DIR}/include/jpeg
)

# ─── Library Search Paths ───────────────────────────────────────────
link_directories(${CMAKE_SOURCE_DIR}/lib)

# ─── Source Files ───────────────────────────────────────────────────
set(C_SOURCES
    src/uvc.c
    src/ringbuf.c
    src/encode.c
    src/decode.c
    src/vlm_link.c
    src/image_drawing.c
)

set(CXX_SOURCES
    src/devicesource.cpp
    src/rtsp_s.cpp
    src/yolo.cc
    src/yolo_postprocess.cc
    main.cpp
)

# ─── Build Target ───────────────────────────────────────────────────
add_executable(nc2rtsp ${C_SOURCES} ${CXX_SOURCES})

set_source_files_properties(${C_SOURCES} PROPERTIES LANGUAGE C)

# ─── Link Libraries ─────────────────────────────────────────────────
target_link_libraries(nc2rtsp PRIVATE
    -Wl,--allow-multiple-definition
    liveMedia
    groupsock
    BasicUsageEnvironment
    UsageEnvironment
    rockchip_mpp
    turbojpeg
    rga
    rknnrt
    pthread
    rt
    drm
    dl
)

# ─── Install ────────────────────────────────────────────────────────
install(TARGETS nc2rtsp RUNTIME DESTINATION bin)

# ─── Build Info ─────────────────────────────────────────────────────
message(STATUS "Toolchain prefix: ${TOOLCHAIN_PREFIX}")
message(STATUS "C compiler:       ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler:     ${CMAKE_CXX_COMPILER}")
message(STATUS "ENABLE_MPP:       ${ENABLE_MPP}")
