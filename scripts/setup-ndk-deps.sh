#!/bin/bash
# Download and extract Boost headers for Android NDK build.
# Only headers needed — Boost.Asio and Boost.System are header-only.
#
# Usage:
#   ./scripts/setup-ndk-deps.sh
#
# Output:
#   app/src/main/cpp/third_party/boost/include/boost/

set -euo pipefail

BOOST_VERSION="1.83.0"
BOOST_VERSION_UNDERSCORE="${BOOST_VERSION//./_}"
BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="$REPO_ROOT/app/src/main/cpp/third_party/boost/include"
BUILD_DIR="/tmp/boost-android-setup"

if [ -f "$OUTPUT_DIR/boost/asio.hpp" ]; then
    echo "Boost headers already present at $OUTPUT_DIR"
    exit 0
fi

# Method 1: Copy from system-installed Boost (fastest on WSL)
if [ -f "/usr/include/boost/asio.hpp" ]; then
    echo "Copying Boost headers from system (/usr/include/boost)..."
    mkdir -p "$OUTPUT_DIR"
    cp -r /usr/include/boost "$OUTPUT_DIR/"
    echo "Done."
else
    # Method 2: Download and extract from tarball
    BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    if [ ! -f "boost_${BOOST_VERSION_UNDERSCORE}.tar.gz" ]; then
        echo "Downloading Boost ${BOOST_VERSION}..."
        curl -fSL "$BOOST_URL" -o "boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
    fi

    echo "Extracting Boost headers (this takes a while on NTFS)..."
    mkdir -p "$OUTPUT_DIR"
    # Extract to native Linux fs first, then copy to NTFS (much faster)
    local TMPEXT="/tmp/boost_extract_$$"
    mkdir -p "$TMPEXT"
    tar xzf "boost_${BOOST_VERSION_UNDERSCORE}.tar.gz" \
        "boost_${BOOST_VERSION_UNDERSCORE}/boost" \
        --strip-components=1 \
        -C "$TMPEXT"
    cp -r "$TMPEXT/boost" "$OUTPUT_DIR/"
    rm -rf "$TMPEXT"
fi

echo ""
echo "Boost ${BOOST_VERSION} headers extracted to:"
echo "  $OUTPUT_DIR/boost/"
echo ""
ls "$OUTPUT_DIR/boost/asio.hpp" && echo "✓ Boost.Asio found"
ls "$OUTPUT_DIR/boost/system/error_code.hpp" && echo "✓ Boost.System found"
