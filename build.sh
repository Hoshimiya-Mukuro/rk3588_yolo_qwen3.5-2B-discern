#!/bin/bash
# build.sh — Build nc2rtsp for Rockchip (RK3588/RK3568)
#
# Usage:
#   ./build.sh                   # default: aarch64-linux-gnu- cross-compile
#   ./build.sh -c                # clean before build
#   ./build.sh -t native         # native compile (use host gcc/g++)
#   ./build.sh -t /opt/gcc-arm-15.2/bin/aarch64-linux-gnu-  # custom toolchain
#   ./build.sh -b cmake          # use CMake instead of Makefile
#   ./build.sh -m                # enable MPP at compile time (-DENABLE_MPP)
#   ./build.sh -d                # debug build (-O0 -g3)
#   ./build.sh -v                # verbose build output
#   ./build.sh -j 8              # parallel jobs
#   ./build.sh -h                # help

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ─── Defaults ───────────────────────────────────────────────────────
CROSS="/opt/gcc-arm-15.2/bin/aarch64-none-linux-gnu-"
BUILD_SYSTEM="make"
ENABLE_MPP=""
BUILD_TYPE="release"
JOBS=$(nproc 2>/dev/null || echo 4)
CLEAN_FIRST=0
VERBOSE=0

# ─── Help ───────────────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  -t <prefix>   Toolchain prefix (default: $CROSS)
                Use "-t native" for native host build.
  -b <system>   Build system: "make" (default) or "cmake"
  -m            Enable MPP at compile time (-DENABLE_MPP)
  -c            Clean build directory before building
  -d            Debug build (-O0 -g3)
  -v            Verbose build output
  -j <N>        Parallel jobs (default: $JOBS)
  -h            Show this help

Examples:
  $0                                    # cross-compile (default toolchain)
  $0 -t native                          # native build on device
  $0 -t /opt/gcc-arm-15.2/bin/aarch64-linux-gnu- -m  # cross + MPP
  $0 -c -d                              # clean debug build
  $0 -b cmake -j 8                      # CMake with 8 parallel jobs
EOF
    exit 0
}

# ─── Parse Args ─────────────────────────────────────────────────────
while getopts "t:b:mcdvj:h" opt; do
    case $opt in
        t) CROSS="$OPTARG" ;;
        b) BUILD_SYSTEM="$OPTARG" ;;
        m) ENABLE_MPP="1" ;;
        c) CLEAN_FIRST=1 ;;
        d) BUILD_TYPE="debug" ;;
        v) VERBOSE=1 ;;
        j) JOBS="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

[ "$CROSS" = "native" ] && CROSS=""
CROSS="${TOOLCHAIN:-$CROSS}"

# ─── Verify Toolchain ───────────────────────────────────────────────
if [ -n "$CROSS" ]; then
    if [ ! -x "${CROSS}gcc" ] && ! command -v "${CROSS}gcc" &>/dev/null; then
        echo "[ERROR] Cross-compiler not found: ${CROSS}gcc"
        echo "  Install it or use: $0 -t native"
        exit 1
    fi
    echo "[BUILD] Toolchain: ${CROSS}gcc"
    echo "[BUILD]           $(${CROSS}gcc --version | head -1)"
else
    echo "[BUILD] Toolchain: native (host gcc)"
fi

# ─── Clean ──────────────────────────────────────────────────────────
if [ $CLEAN_FIRST -eq 1 ]; then
    echo "[BUILD] Cleaning..."
    if [ "$BUILD_SYSTEM" = "cmake" ]; then
        rm -rf build
    else
        make clean CROSS="$CROSS" ${ENABLE_MPP:+ENABLE_MPP=$ENABLE_MPP}
    fi
fi

# ─── Build Options ──────────────────────────────────────────────────
MAKE_ARGS="CROSS=$CROSS"
[ -n "$ENABLE_MPP" ] && MAKE_ARGS="$MAKE_ARGS ENABLE_MPP=$ENABLE_MPP"
[ $VERBOSE -eq 1 ] && MAKE_ARGS="$MAKE_ARGS V=1"

CMAKE_OPTS=""
[ -n "$ENABLE_MPP" ] && CMAKE_OPTS="$CMAKE_OPTS -DENABLE_MPP=ON"
if [ -n "$CROSS" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_TOOLCHAIN_FILE=$SCRIPT_DIR/cmake/toolchain.aarch64-linux-gnu.cmake"
    CMAKE_OPTS="$CMAKE_OPTS -DTOOLCHAIN_PATH=$(dirname "$CROSS")"
    CMAKE_PREFIX=$(basename "$CROSS")
    CMAKE_OPTS="$CMAKE_OPTS -DTOOLCHAIN_PREFIX=$CMAKE_PREFIX"
fi
if [ "$BUILD_TYPE" = "debug" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=Debug"
else
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=Release"
fi

echo "[BUILD] Type:    $BUILD_TYPE"
echo "[BUILD] System:  $BUILD_SYSTEM"
echo "[BUILD] MPP:     ${ENABLE_MPP:-OFF}"
echo "[BUILD] Jobs:    $JOBS"
echo ""

# ─── Build ──────────────────────────────────────────────────────────
if [ "$BUILD_SYSTEM" = "cmake" ]; then
    mkdir -p build && cd build
    echo "[BUILD] Configuring CMake..."
    cmake .. $CMAKE_OPTS
    echo ""
    echo "[BUILD] Compiling..."
    cmake --build . --parallel "$JOBS" $([ $VERBOSE -eq 1 ] && echo "--verbose")
    echo ""
    if [ -f bin/nc2rtsp ]; then
        echo "[BUILD] ✓ Success: build/bin/nc2rtsp"
        file bin/nc2rtsp 2>/dev/null || true
        ls -lh bin/nc2rtsp
    elif [ -f nc2rtsp ]; then
        echo "[BUILD] ✓ Success: build/nc2rtsp"
        file nc2rtsp 2>/dev/null || true
        ls -lh nc2rtsp
    fi
else
    echo "[BUILD] Compiling..."
    make -j "$JOBS" $MAKE_ARGS
    echo ""
    if [ -f build/bin/nc2rtsp ]; then
        echo "[BUILD] ✓ Success: build/bin/nc2rtsp"
        file build/bin/nc2rtsp 2>/dev/null || true
        ls -lh build/bin/nc2rtsp
    fi
fi

# ─── Strip (release only) ──────────────────────────────────────────
if [ "$BUILD_TYPE" = "release" ] && [ "$BUILD_SYSTEM" = "make" ]; then
    echo ""
    echo "[BUILD] Stripping..."
    make strip CROSS="$CROSS"
    ls -lh build/bin/nc2rtsp 2>/dev/null || true
fi

echo ""
echo "[BUILD] Done."
