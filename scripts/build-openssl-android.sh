#!/bin/bash
# Build OpenSSL 3.x for Android (ARM64 or x86_64).
# Run from WSL or Linux — produces static libraries for the NDK build.
#
# Usage:
#   ./scripts/build-openssl-android.sh              # ARM64 (default)
#   ./scripts/build-openssl-android.sh arm64-v8a    # ARM64
#   ./scripts/build-openssl-android.sh x86_64       # x86_64
#
# Output:
#   app/src/main/cpp/third_party/openssl/<ABI>/
#     include/openssl/*.h
#     lib/libssl.a
#     lib/libcrypto.a

set -euo pipefail

# ABI from first arg, default arm64-v8a
TARGET_ABI="${1:-arm64-v8a}"
case "$TARGET_ABI" in
    arm64-v8a)
        OPENSSL_TARGET="android-arm64"
        ;;
    x86_64)
        OPENSSL_TARGET="android-x86_64"
        ;;
    *)
        echo "ERROR: Unsupported ABI '$TARGET_ABI'. Use arm64-v8a or x86_64."
        exit 1
        ;;
esac
echo "Building OpenSSL for ABI: $TARGET_ABI (target: $OPENSSL_TARGET)"

OPENSSL_VERSION="3.3.2"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"

# Detect NDK — prefer ANDROID_NDK_HOME, then common locations
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    NDK_ROOT="$ANDROID_NDK_HOME"
elif [ -d "/opt/android-ndk-r28b" ]; then
    NDK_ROOT="/opt/android-ndk-r28b"
elif [ -d "$HOME/Android/Sdk/ndk" ]; then
    NDK_ROOT=$(ls -d "$HOME/Android/Sdk/ndk"/*/ 2>/dev/null | sort -V | tail -1)
else
    echo "ERROR: Set ANDROID_NDK_HOME or install NDK"
    exit 1
fi
NDK_ROOT="${NDK_ROOT%/}"
echo "Using NDK: $NDK_ROOT"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="/tmp/openssl-android-build/$TARGET_ABI"
OUTPUT_DIR="$REPO_ROOT/app/src/main/cpp/third_party/openssl/$TARGET_ABI"

ANDROID_API=32
TOOLCHAIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download if not cached
if [ ! -f "openssl-${OPENSSL_VERSION}.tar.gz" ]; then
    echo "Downloading OpenSSL ${OPENSSL_VERSION}..."
    curl -fSL "$OPENSSL_URL" -o "openssl-${OPENSSL_VERSION}.tar.gz"
fi

# Extract
rm -rf "openssl-${OPENSSL_VERSION}"
tar xzf "openssl-${OPENSSL_VERSION}.tar.gz"
cd "openssl-${OPENSSL_VERSION}"

# Configure for Android ARM64
export ANDROID_NDK_ROOT="$NDK_ROOT"
export PATH="$TOOLCHAIN/bin:$PATH"

./Configure $OPENSSL_TARGET \
    -D__ANDROID_API__=$ANDROID_API \
    --prefix="$OUTPUT_DIR" \
    --openssldir="$OUTPUT_DIR/ssl" \
    no-shared \
    no-tests \
    no-ui-console \
    no-stdio \
    no-engine \
    no-async \
    -fPIC

# Build (static libs only)
make -j$(nproc) build_libs

# Install headers and libs
mkdir -p "$OUTPUT_DIR/lib" "$OUTPUT_DIR/include"
cp libssl.a libcrypto.a "$OUTPUT_DIR/lib/"
cp -r include/openssl "$OUTPUT_DIR/include/"
# Also copy generated headers
cp -r include/openssl/* "$OUTPUT_DIR/include/openssl/" 2>/dev/null || true

echo ""
echo "OpenSSL ${OPENSSL_VERSION} built for Android ARM64:"
echo "  Headers: $OUTPUT_DIR/include/openssl/"
echo "  Libs:    $OUTPUT_DIR/lib/libssl.a"
echo "           $OUTPUT_DIR/lib/libcrypto.a"
echo ""
ls -la "$OUTPUT_DIR/lib/"
