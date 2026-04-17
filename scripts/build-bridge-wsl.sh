#!/bin/bash
# build-bridge-wsl.sh — Cross-compile the relay binary in WSL for ARM64.
#
# The relay is a single-file C++ binary with zero external dependencies.
# Much simpler than the old headless build (no aasdk, OpenSSL, Protobuf, Boost).
#
# Usage (from WSL or invoked by deploy-bridge.ps1):
#   bash scripts/build-bridge-wsl.sh
#   bash scripts/build-bridge-wsl.sh clean    # full rebuild
set -eu

# Where the repo lives on the Windows side (may be /mnt/d/...)
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"

# Fast native WSL filesystem paths
WSL_SRC="$HOME/openautolink-relay-src"
WSL_BUILD="$HOME/openautolink-relay-build"

# Output goes back to the Windows filesystem for deploy-bridge.ps1
OUTPUT_DIR="${REPO_ROOT}/build-bridge-arm64"

if [ "${1:-}" = "clean" ]; then
    echo ">>> Clean build requested"
    rm -rf "$WSL_BUILD"
fi

# ── Sync relay source into WSL native filesystem ─────────────────────
echo "=== Cross-compiling relay for ARM64 ==="
echo "  Syncing relay source to WSL filesystem..."

mkdir -p "$WSL_SRC"
rsync -a --checksum --delete "$REPO_ROOT/bridge/openautolink/relay/" "$WSL_SRC/"

echo "  Source: ${WSL_SRC}"
echo "  Build:  ${WSL_BUILD}"
echo ""

mkdir -p "$WSL_BUILD"

# CMake toolchain file for cross-compilation
TOOLCHAIN="${WSL_BUILD}/aarch64-toolchain.cmake"
cat > "$TOOLCHAIN" << 'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cd "$WSL_BUILD"

if [ ! -f CMakeCache.txt ]; then
    echo ">>> Configuring CMake..."
    cmake "$WSL_SRC" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release
    echo ""
fi

echo ">>> Building..."
cmake --build . --target openautolink-relay -j$(nproc)

echo ""
echo ">>> Stripping binary..."
aarch64-linux-gnu-strip -o openautolink-relay-stripped openautolink-relay

ls -lh openautolink-relay-stripped

# ── Copy result back to Windows filesystem ───────────────────────────
echo ""
echo ">>> Copying binary to ${OUTPUT_DIR}/"
mkdir -p "$OUTPUT_DIR"
cp openautolink-relay-stripped "$OUTPUT_DIR/"

echo ""
echo "=== Build complete ==="
echo "  Binary: ${OUTPUT_DIR}/openautolink-relay-stripped"
echo "  Deploy: scripts/deploy-bridge.ps1  (from PowerShell)"
