#!/bin/bash
# Build aasdk + protobuf + abseil as static libraries for Android.
# Runs entirely on native WSL filesystem for performance, then copies
# the built .a files + generated headers to the NTFS output dir.
#
# Usage:
#   ./scripts/build-aasdk-android.sh              # ARM64 (default)
#   ./scripts/build-aasdk-android.sh arm64-v8a    # ARM64
#   ./scripts/build-aasdk-android.sh x86_64       # x86_64
#
# Output:
#   app/src/main/cpp/third_party/aasdk/<ABI>/
#     lib/libaasdk.a
#     lib/libaap_protobuf.a
#     lib/libprotobuf.a       (+ abseil libs)
#     include/                 (generated protobuf headers)

set -euo pipefail

# ABI from first arg, default arm64-v8a
TARGET_ABI="${1:-arm64-v8a}"
case "$TARGET_ABI" in
    arm64-v8a|x86_64) ;;
    *)
        echo "ERROR: Unsupported ABI '$TARGET_ABI'. Use arm64-v8a or x86_64."
        exit 1
        ;;
esac
echo "Building aasdk for ABI: $TARGET_ABI"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
AASDK_SOURCE="$REPO_ROOT/external/opencardev-aasdk"
OUTPUT_DIR="$REPO_ROOT/app/src/main/cpp/third_party/aasdk/$TARGET_ABI"
OPENSSL_DIR="$REPO_ROOT/app/src/main/cpp/third_party/openssl/$TARGET_ABI"
BOOST_DIR="$REPO_ROOT/app/src/main/cpp/third_party/boost/include"

# Work entirely on native ext4 for speed
WORK_DIR="/tmp/oal-aasdk-android-build/$TARGET_ABI"
# Shared resources (ABI-independent) go in parent dir
SHARED_DIR="/tmp/oal-aasdk-android-build/shared"

# Detect NDK
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    NDK_ROOT="$ANDROID_NDK_HOME"
elif [ -d "/opt/android-ndk-r28b" ]; then
    NDK_ROOT="/opt/android-ndk-r28b"
else
    echo "ERROR: Set ANDROID_NDK_HOME or install NDK at /opt/android-ndk-r28b"
    exit 1
fi
echo "Using NDK: $NDK_ROOT"

# CMake 4.x removed compat with cmake_minimum_required(VERSION <3.5).
# aasdk and some FetchContent sub-projects still declare older minimums.
# Export the policy override so it propagates into all sub-builds. See issue #11.
export CMAKE_POLICY_VERSION_MINIMUM=3.5

ANDROID_API=32
TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "ERROR: NDK toolchain file not found: $TOOLCHAIN_FILE"
    exit 1
fi

# Check dependencies
if [ ! -f "$OPENSSL_DIR/lib/libssl.a" ]; then
    echo "ERROR: OpenSSL not built. Run: scripts/build-openssl-android.sh"
    exit 1
fi
if [ ! -f "$BOOST_DIR/boost/asio.hpp" ]; then
    echo "ERROR: Boost headers not set up. Run: scripts/setup-ndk-deps.sh"
    exit 1
fi

# Copy aasdk source to native fs (avoid NTFS during build)
AASDK_NATIVE="$SHARED_DIR/aasdk-src"
if [ ! -f "$AASDK_NATIVE/CMakeLists.txt" ]; then
    echo "Staging aasdk source on native fs..."
    rm -rf "$AASDK_NATIVE"
    mkdir -p "$AASDK_NATIVE"
    # Use tar to avoid the slow per-file NTFS reads
    (cd "$AASDK_SOURCE" && tar cf - --exclude='.git' .) | (cd "$AASDK_NATIVE" && tar xf -)
else
    echo "aasdk source already staged"
fi

# Also copy Boost + OpenSSL to native fs for the build
BOOST_NATIVE="$SHARED_DIR/boost-include"
if [ ! -f "$BOOST_NATIVE/boost/asio.hpp" ]; then
    echo "Copying Boost headers to native fs..."
    rm -rf "$BOOST_NATIVE"
    mkdir -p "$BOOST_NATIVE"
    (cd "$BOOST_DIR" && tar cf - boost) | (cd "$BOOST_NATIVE" && tar xf -)
fi

OPENSSL_NATIVE="$WORK_DIR/openssl"
if [ ! -f "$OPENSSL_NATIVE/lib/libssl.a" ]; then
    echo "Copying OpenSSL to native fs..."
    rm -rf "$OPENSSL_NATIVE"
    mkdir -p "$OPENSSL_NATIVE"
    cp -r "$OPENSSL_DIR/lib" "$OPENSSL_DIR/include" "$OPENSSL_NATIVE/"
fi

# Create libusb stub on native fs
STUB_DIR="$SHARED_DIR/stubs"
mkdir -p "$STUB_DIR"
cp "$REPO_ROOT/app/src/main/cpp/stubs/libusb.h" "$STUB_DIR/"
cp "$REPO_ROOT/app/src/main/cpp/stubs/libusb_stub.c" "$STUB_DIR/"

# Create cmake shim dir for find_package overrides
SHIM_DIR="$SHARED_DIR/cmake-shims"
mkdir -p "$SHIM_DIR"

cat > "$SHIM_DIR/FindBoost.cmake" << 'SHIMEOF'
set(Boost_FOUND TRUE)
set(Boost_INCLUDE_DIRS "${OAL_BOOST_INCLUDE_DIR}")
set(Boost_LIBRARIES "")
set(Boost_VERSION "1.83.0")
set(Boost_SYSTEM_FOUND TRUE)
set(Boost_LOG_FOUND TRUE)
set(Boost_LOG_SETUP_FOUND TRUE)
include_directories(SYSTEM "${OAL_BOOST_INCLUDE_DIR}")
message(STATUS "FindBoost shim: ${OAL_BOOST_INCLUDE_DIR}")
SHIMEOF

cat > "$SHIM_DIR/FindOpenSSL.cmake" << 'SHIMEOF'
set(OPENSSL_FOUND TRUE)
set(OpenSSL_FOUND TRUE)
set(OPENSSL_INCLUDE_DIR "${OAL_OPENSSL_DIR}/include")
add_library(_oal_ssl STATIC IMPORTED)
set_target_properties(_oal_ssl PROPERTIES IMPORTED_LOCATION "${OAL_OPENSSL_DIR}/lib/libssl.a")
add_library(_oal_crypto STATIC IMPORTED)
set_target_properties(_oal_crypto PROPERTIES IMPORTED_LOCATION "${OAL_OPENSSL_DIR}/lib/libcrypto.a")
set(OPENSSL_LIBRARIES _oal_ssl _oal_crypto)
include_directories(SYSTEM "${OAL_OPENSSL_DIR}/include")
message(STATUS "FindOpenSSL shim: ${OAL_OPENSSL_DIR}")
SHIMEOF

cat > "$SHIM_DIR/Findlibusb-1.0.cmake" << 'SHIMEOF'
set(LIBUSB_1_FOUND TRUE)
set(libusb-1.0_FOUND TRUE)
message(STATUS "Findlibusb shim: using stub")
SHIMEOF

cat > "$SHIM_DIR/Findabsl.cmake" << 'SHIMEOF'
if(TARGET absl::base)
    set(absl_FOUND TRUE)
    message(STATUS "Findabsl shim: FetchContent targets available")
else()
    message(FATAL_ERROR "Findabsl shim: no targets")
endif()
SHIMEOF

# Build
BUILD_DIR="$WORK_DIR/build"

# Step 1: Build host-native aasdk (x86_64).
# This gives us a working host protoc AND pre-generated protobuf .pb.h/.pb.cc files.
# Shared across ABIs — only built once.
HOST_BUILD_DIR="$SHARED_DIR/host-build"
HOST_PROTOC="$HOST_BUILD_DIR/bin/protoc"
if [ ! -f "$HOST_PROTOC" ]; then
    echo ""
    echo "=== Building host aasdk (x86_64) — for protoc + proto generation ==="
    echo ""
    rm -rf "$HOST_BUILD_DIR"
    mkdir -p "$HOST_BUILD_DIR"
    cd "$HOST_BUILD_DIR"

    cmake "$AASDK_NATIVE" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DBUILD_AASDK_STATIC=ON \
        -DSKIP_BUILD_PROTOBUF=OFF \
        -DSKIP_BUILD_ABSL=OFF \
        -DTARGET_ARCH="" \
        -DLIBUSB_1_LIBRARIES="$STUB_DIR/libusb_stub.c" \
        -DLIBUSB_1_INCLUDE_DIRS="$STUB_DIR" \
        -DCMAKE_MODULE_PATH="$SHIM_DIR" \
        -DOAL_BOOST_INCLUDE_DIR="$BOOST_NATIVE" \
        -DOAL_OPENSSL_DIR="$OPENSSL_NATIVE" \
        -DCMAKE_SKIP_INSTALL_RULES=ON \
        -DAASDK_TEST=OFF \
        2>&1

    # Build everything for host — aasdk + aap_protobuf + protobuf + abseil
    cmake --build . --target aasdk aap_protobuf -j$(nproc) 2>&1
    echo ""
    echo "Host build complete:"
    ls -la "$HOST_PROTOC" 2>&1
    find "$HOST_BUILD_DIR" -name "libaasdk.a" 2>&1
fi

# Step 2: Cross-compile aasdk for Android.
echo ""
echo "=== Configuring aasdk for Android $TARGET_ABI (cross-compile) ==="
echo ""

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$AASDK_NATIVE" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI=$TARGET_ABI \
    -DANDROID_PLATFORM=android-$ANDROID_API \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_MODULE_PATH="$SHIM_DIR" \
    -DBUILD_AASDK_STATIC=ON \
    -DSKIP_BUILD_PROTOBUF=OFF \
    -DSKIP_BUILD_ABSL=OFF \
    -DTARGET_ARCH="" \
    -DLIBUSB_1_LIBRARIES="$STUB_DIR/libusb_stub.c" \
    -DLIBUSB_1_INCLUDE_DIRS="$STUB_DIR" \
    -DOAL_BOOST_INCLUDE_DIR="$BOOST_NATIVE" \
    -DOAL_OPENSSL_DIR="$OPENSSL_NATIVE" \
    -DCMAKE_SKIP_INSTALL_RULES=ON \
    -DAASDK_TEST=OFF \
    2>&1

# After configure: the build system created a protobuf::protoc target pointing
# to the ARM64 protoc binary which can't run on x86_64. Replace it with
# the host-built protoc. Also create a wrapper script that protobuf_generate
# can find by the target name.
echo "Injecting host protoc into cross-compile build..."
# The shim must use an absolute path to the HOST protoc,
# not $BUILD_DIR/bin/protoc which gets overwritten by the ARM64 build.
mkdir -p "$BUILD_DIR/.protoc_shim"
cat > "$BUILD_DIR/.protoc_shim/protobuf::protoc" << SHIMEOF
#!/bin/bash
exec "$HOST_PROTOC" "\$@"
SHIMEOF
chmod +x "$BUILD_DIR/.protoc_shim/protobuf::protoc"
export PATH="$BUILD_DIR/.protoc_shim:$PATH"

echo ""
echo "=== Building aasdk ==="
echo ""

cmake --build . --target aasdk aap_protobuf -j$(nproc) 2>&1

# Build protobuf runtime and abseil (needed for linking)
echo ""
echo "=== Building protobuf + abseil dependencies ==="
echo ""
cmake --build . --target protobuf-lite -j$(nproc) 2>&1 || true
cmake --build . --target libprotobuf -j$(nproc) 2>&1 || true

# Build all abseil targets
ABSL_TARGETS=$(cmake --build . --target help 2>&1 | grep -oP 'absl_\w+' | sort -u || true)
if [ -n "$ABSL_TARGETS" ]; then
    for target in $ABSL_TARGETS; do
        cmake --build . --target "$target" -j$(nproc) 2>&1 || true
    done
fi

echo ""
echo "=== Packaging output ==="
echo ""

# Collect built libraries
PACK_DIR="$WORK_DIR/output"
rm -rf "$PACK_DIR"
mkdir -p "$PACK_DIR/lib" "$PACK_DIR/include"

# aasdk static lib
find "$BUILD_DIR" -name "libaasdk.a" -exec cp {} "$PACK_DIR/lib/" \;

# protobuf libs (may be in different locations)
find "$BUILD_DIR" -name "libaap_protobuf.a" -exec cp {} "$PACK_DIR/lib/" \;
find "$BUILD_DIR" -name "libprotobuf.a" -exec cp {} "$PACK_DIR/lib/" \; 2>/dev/null || true
find "$BUILD_DIR" -name "libprotobuf-lite.a" -exec cp {} "$PACK_DIR/lib/" \; 2>/dev/null || true

# abseil libs — search entire build tree
find "$BUILD_DIR" -name "libabsl_*.a" -exec cp {} "$PACK_DIR/lib/" \; 2>/dev/null || true

# utf8_range (protobuf dependency)
find "$BUILD_DIR" -name "libutf8_range.a" -o -name "libutf8_validity.a" | while read f; do cp "$f" "$PACK_DIR/lib/"; done 2>/dev/null || true

# Generated protobuf headers (aap_protobuf .pb.h files)
if [ -d "$BUILD_DIR/protobuf" ]; then
    cp -r "$BUILD_DIR/protobuf" "$PACK_DIR/include/"
fi

# Protobuf runtime headers (google/protobuf/*.h — needed to compile against aasdk)
if [ -d "$BUILD_DIR/_deps/protobuf-src/src/google" ]; then
    cp -r "$BUILD_DIR/_deps/protobuf-src/src/google" "$PACK_DIR/include/"
fi

# Abseil headers (absl/*.h — needed by protobuf headers)
if [ -d "$BUILD_DIR/_deps/abseil-src/absl" ]; then
    cp -r "$BUILD_DIR/_deps/abseil-src/absl" "$PACK_DIR/include/"
fi

# Generated Version.hpp
if [ -f "$BUILD_DIR/include/aasdk/Version.hpp" ]; then
    mkdir -p "$PACK_DIR/include/aasdk"
    cp "$BUILD_DIR/include/aasdk/Version.hpp" "$PACK_DIR/include/aasdk/"
fi

echo "Built libraries:"
ls -la "$PACK_DIR/lib/"

# Pack into tarball and copy to NTFS
TARBALL="$WORK_DIR/aasdk-android-$TARGET_ABI.tar.gz"
(cd "$PACK_DIR" && tar czf "$TARBALL" lib include)

mkdir -p "$OUTPUT_DIR"
echo "Copying to NTFS..."
cp "$TARBALL" "$OUTPUT_DIR/../aasdk-android-$TARGET_ABI.tar.gz"
(cd "$OUTPUT_DIR" && tar xzf "../aasdk-android-$TARGET_ABI.tar.gz")
rm -f "$OUTPUT_DIR/../aasdk-android-$TARGET_ABI.tar.gz"

echo ""
echo "=== aasdk Android $TARGET_ABI build complete ==="
echo "Output: $OUTPUT_DIR/"
ls -la "$OUTPUT_DIR/lib/"
echo "Headers: $(find "$OUTPUT_DIR/include" -type f | wc -l) files"
