#!/bin/bash
# build-headless.sh — Build the pi-aa-openauto-headless binary
# Works on any SBC (CM5, VIM4, etc.) using standard /opt/pi-aa/ paths.
#
# Usage:
#   sudo ./build-headless.sh              # full clean build
#   sudo ./build-headless.sh --rebuild    # incremental rebuild only
#   sudo ./build-headless.sh --jobs 2     # limit parallel jobs (default: nproc)
#
# Paths:
#   Source:   /opt/pi-aa/external/opencardev-aasdk/
#   Headless: /opt/pi-aa/openauto-headless/
#   Build:    /opt/pi-aa/build/headless/
#   Output:   /opt/pi-aa/bin/pi-aa-openauto-headless
set -eu

PI_AA_ROOT="/opt/pi-aa"
AASDK_SRC="${PI_AA_ROOT}/external/opencardev-aasdk"
HEADLESS_SRC="${PI_AA_ROOT}/openauto-headless"
BUILD_DIR="${PI_AA_ROOT}/build/headless"
OUTPUT_BIN="${PI_AA_ROOT}/bin/pi-aa-openauto-headless"

JOBS=$(nproc)
REBUILD_ONLY=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild) REBUILD_ONLY=true; shift ;;
        --jobs)    JOBS="$2"; shift 2 ;;
        *)         echo "Unknown arg: $1"; exit 1 ;;
    esac
done

echo "=== pi-aa headless build ==="
echo "  AASDK source:   ${AASDK_SRC}"
echo "  Headless source: ${HEADLESS_SRC}"
echo "  Build dir:       ${BUILD_DIR}"
echo "  Output binary:   ${OUTPUT_BIN}"
echo "  Parallel jobs:   ${JOBS}"

# Verify source dirs exist
if [ ! -f "${AASDK_SRC}/CMakeLists.txt" ]; then
    echo "ERROR: aasdk source not found at ${AASDK_SRC}"
    echo "  Sync from Windows: scp -r external/opencardev-aasdk USER@HOST:${AASDK_SRC}"
    exit 1
fi
if [ ! -f "${HEADLESS_SRC}/CMakeLists.txt" ]; then
    echo "ERROR: headless source not found at ${HEADLESS_SRC}"
    exit 1
fi

mkdir -p "${BUILD_DIR}" "${PI_AA_ROOT}/bin"

if [ "${REBUILD_ONLY}" = false ]; then
    echo "--- Clean configure ---"
    rm -rf "${BUILD_DIR}"/*
    cd "${BUILD_DIR}"
    cmake "${HEADLESS_SRC}" \
        -DPI_AA_ENABLE_AASDK_LIVE=ON \
        -DPI_AA_AASDK_SOURCE_DIR="${AASDK_SRC}" \
        -DSKIP_BUILD_PROTOBUF=ON \
        -DSKIP_BUILD_ABSL=ON \
        -DCMAKE_BUILD_TYPE=Release
fi

echo "--- Building (${JOBS} jobs) ---"
cd "${BUILD_DIR}"
make -j"${JOBS}" 2>&1

if [ -f "${BUILD_DIR}/pi-aa-openauto-headless" ]; then
    cp "${BUILD_DIR}/pi-aa-openauto-headless" "${OUTPUT_BIN}"
    echo "=== SUCCESS: ${OUTPUT_BIN} ==="
    ls -la "${OUTPUT_BIN}"
else
    echo "=== BUILD FAILED ==="
    exit 1
fi
