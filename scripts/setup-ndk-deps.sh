#!/bin/bash
# Download and extract Boost headers for Android NDK build.
# Only headers needed — Boost.Asio and Boost.System are header-only.
#
# Strategy: Do all heavy I/O on native WSL ext4 filesystem, then copy
# a single tarball to NTFS and extract with Windows tar (avoids the
# catastrophic performance of writing 25K+ small files across WSL→NTFS).
#
# Usage:
#   ./scripts/setup-ndk-deps.sh
#
# Output:
#   app/src/main/cpp/third_party/boost/include/boost/

set -euo pipefail

BOOST_VERSION="1.83.0"
BOOST_VERSION_UNDERSCORE="${BOOST_VERSION//./_}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="$REPO_ROOT/app/src/main/cpp/third_party/boost/include"
WORK_DIR="/tmp/oal-ndk-deps"

if [ -f "$OUTPUT_DIR/boost/asio.hpp" ]; then
    echo "Boost headers already present at $OUTPUT_DIR"
    exit 0
fi

mkdir -p "$WORK_DIR"

# Step 1: Get boost headers onto native ext4 (fast)
BOOST_STAGING="$WORK_DIR/boost_include"
if [ ! -f "$BOOST_STAGING/boost/asio.hpp" ]; then
    rm -rf "$BOOST_STAGING"
    mkdir -p "$BOOST_STAGING"

    # Always download the pinned Boost version. Do NOT use system /usr/include/boost
    # — distros (e.g. Arch) ship Boost >=1.74 which removed boost::asio::io_service,
    # required by aasdk. See issue #10.
    BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
    TARBALL="$WORK_DIR/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
    if [ ! -f "$TARBALL" ]; then
        echo "Downloading Boost ${BOOST_VERSION}..."
        curl -fSL "$BOOST_URL" -o "$TARBALL"
    fi
    echo "Extracting Boost headers..."
    # Extract boost headers to temp dir and move (avoids tar --strip-components
    # + member filter interaction that silently extracts nothing on some tar versions)
    EXTRACT_TMP="$WORK_DIR/boost_extract"
    rm -rf "$EXTRACT_TMP"
    mkdir -p "$EXTRACT_TMP"
    tar xzf "$TARBALL" -C "$EXTRACT_TMP" "boost_${BOOST_VERSION_UNDERSCORE}/boost"
    mv "$EXTRACT_TMP/boost_${BOOST_VERSION_UNDERSCORE}/boost" "$BOOST_STAGING/"
    rm -rf "$EXTRACT_TMP"
    echo "Boost headers staged: $(find "$BOOST_STAGING/boost" -type f | wc -l) files"
fi

# Step 2: Pack into a single tarball (one big file = fast NTFS write)
PACK="$WORK_DIR/boost_headers.tar.gz"
if [ ! -f "$PACK" ] || [ "$BOOST_STAGING/boost/asio.hpp" -nt "$PACK" ]; then
    echo "Packing boost headers into single tarball..."
    tar czf "$PACK" -C "$BOOST_STAGING" boost
fi

# Step 3: Copy single tarball to NTFS (fast — one file)
NTFS_PACK="$REPO_ROOT/app/src/main/cpp/third_party/boost/boost_headers.tar.gz"
mkdir -p "$(dirname "$NTFS_PACK")"
echo "Copying tarball to NTFS..."
cp "$PACK" "$NTFS_PACK"

# Step 4: Extract on NTFS (use tar — much faster than cp for many files)
echo "Extracting on NTFS..."
mkdir -p "$OUTPUT_DIR"
tar xzf "$NTFS_PACK" -C "$OUTPUT_DIR"
rm -f "$NTFS_PACK"

# Verify
if [ -f "$OUTPUT_DIR/boost/asio.hpp" ]; then
    echo ""
    echo "Boost ${BOOST_VERSION} headers ready at:"
    echo "  $OUTPUT_DIR/boost/"
    echo "  $(find "$OUTPUT_DIR/boost" -type f | wc -l) header files"
else
    echo "ERROR: Boost headers not found after extraction"
    exit 1
fi

echo ""
echo "Boost ${BOOST_VERSION} headers extracted to:"
echo "  $OUTPUT_DIR/boost/"
echo ""
ls "$OUTPUT_DIR/boost/asio.hpp" && echo "✓ Boost.Asio found"
ls "$OUTPUT_DIR/boost/system/error_code.hpp" && echo "✓ Boost.System found"
