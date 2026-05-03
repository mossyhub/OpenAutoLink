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

# ABI from first arg — if "all" or omitted, build both ABIs
TARGET_ABI="${1:-all}"
if [ "$TARGET_ABI" = "all" ]; then
    SCRIPT="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    echo "Building aasdk for all ABIs..."
    bash "$SCRIPT" arm64-v8a
    bash "$SCRIPT" x86_64
    exit $?
fi
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

# NDK version to download if no native Linux NDK is found
NDK_DOWNLOAD_VERSION="r28b"
NDK_DOWNLOAD_DIR="/opt/android-ndk-${NDK_DOWNLOAD_VERSION}"
NDK_DOWNLOAD_URL="https://dl.google.com/android/repository/android-ndk-${NDK_DOWNLOAD_VERSION}-linux.zip"

# Download Linux NDK to a native path (not NTFS)
ensure_linux_ndk() {
    if [ -f "$NDK_DOWNLOAD_DIR/build/cmake/android.toolchain.cmake" ]; then
        echo "Linux NDK already present: $NDK_DOWNLOAD_DIR"
        return
    fi
    # Ensure unzip is available
    if ! command -v unzip &>/dev/null; then
        echo "Installing unzip..."
        sudo apt-get update -qq && sudo apt-get install -y -qq unzip
    fi
    echo "Downloading Android NDK ${NDK_DOWNLOAD_VERSION} for Linux..."
    local zip="/tmp/android-ndk-${NDK_DOWNLOAD_VERSION}-linux.zip"
    [ -f "$zip" ] || curl -fSL "$NDK_DOWNLOAD_URL" -o "$zip"
    echo "Extracting NDK to /opt/ (this may take a minute)..."
    sudo unzip -q "$zip" -d /opt/
    [ -d "$NDK_DOWNLOAD_DIR" ] || sudo mv /opt/android-ndk-${NDK_DOWNLOAD_VERSION} "$NDK_DOWNLOAD_DIR" 2>/dev/null || true
    rm -f "$zip"
    echo "NDK installed: $NDK_DOWNLOAD_DIR"
}

# Detect NDK — Windows-mount NDK (/mnt/...) cannot be used in WSL
# because its toolchain binaries are Windows .exe files.
WIN_SDK_NDK=$(ls -d /mnt/c/Users/*/AppData/Local/Android/Sdk/ndk/*/ 2>/dev/null | sort -V | tail -1)
if [ -n "${ANDROID_NDK_HOME:-}" ] && [[ "$ANDROID_NDK_HOME" != /mnt/* ]]; then
    NDK_ROOT="$ANDROID_NDK_HOME"
elif [ -d "/opt/android-ndk-r28b" ]; then
    NDK_ROOT="/opt/android-ndk-r28b"
elif [ -n "$WIN_SDK_NDK" ]; then
    echo "Windows SDK NDK detected but cannot be used in WSL (Windows .exe binaries)."
    echo "Auto-downloading Linux NDK ${NDK_DOWNLOAD_VERSION}..."
    ensure_linux_ndk
    NDK_ROOT="$NDK_DOWNLOAD_DIR"
else
    echo "No NDK found. Auto-downloading Linux NDK ${NDK_DOWNLOAD_VERSION}..."
    ensure_linux_ndk
    NDK_ROOT="$NDK_DOWNLOAD_DIR"
fi
NDK_ROOT="${NDK_ROOT%/}"
echo "Using NDK: $NDK_ROOT"

# Ensure build tools are present
# Note: on Debian/Ubuntu ninja is packaged as 'ninja-build', not 'ninja'
MISSING_TOOLS=()
for t in cmake make g++; do
    command -v "$t" &>/dev/null || MISSING_TOOLS+=("$t")
done
if ! command -v ninja &>/dev/null && ! command -v ninja-build &>/dev/null; then
    MISSING_TOOLS+=("ninja-build")
fi
# libssl-dev needed for host (x86_64) aap_protobuf build
dpkg -s libssl-dev &>/dev/null 2>&1 || MISSING_TOOLS+=("libssl-dev")
if [ ${#MISSING_TOOLS[@]} -gt 0 ]; then
    echo "Installing missing build tools: ${MISSING_TOOLS[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y -qq "${MISSING_TOOLS[@]}"
fi
# Ensure 'ninja' resolves (Debian/Ubuntu installs as ninja-build)
if ! command -v ninja &>/dev/null && command -v ninja-build &>/dev/null; then
    sudo ln -sf "$(command -v ninja-build)" /usr/local/bin/ninja
fi

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

# Patch staged source for OpenSSL 3.x compatibility:
# ENGINE_cleanup() was removed in OpenSSL 3.0 but <openssl/engine.h> still exists,
# so the #ifdef HAVE_ENGINE_H guard passes while the function call fails to compile.
SSL_WRAPPER="$AASDK_NATIVE/src/Transport/SSLWrapper.cpp"
if grep -q 'ifdef HAVE_ENGINE_H' "$SSL_WRAPPER" && ! grep -q 'OPENSSL_VERSION_NUMBER < 0x30000000L' "$SSL_WRAPPER"; then
    echo "Patching SSLWrapper.cpp for OpenSSL 3.x..."
    sed -i 's/#ifdef HAVE_ENGINE_H/#if defined(HAVE_ENGINE_H) \&\& (OPENSSL_VERSION_NUMBER < 0x30000000L)/' "$SSL_WRAPPER"
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
# Findabsl.cmake — satisfies find_package(absl) after FetchContent_MakeAvailable(abseil)
# FetchContent_MakeAvailable in protobuf/CMakeLists.txt makes absl:: targets available,
# but find_package(absl) in the parent still needs this module to be found.
if(TARGET absl::base)
    set(absl_FOUND TRUE)
    set(ABSL_FOUND TRUE)
    if(NOT absl_VERSION)
        set(absl_VERSION "20240722.0")
    endif()
    message(STATUS "FindAbsl shim: FetchContent absl targets are available")
else()
    set(absl_FOUND FALSE)
    set(ABSL_FOUND FALSE)
    if(absl_FIND_REQUIRED)
        message(FATAL_ERROR "absl not found — Findabsl shim: FetchContent targets not yet available")
    endif()
endif()
SHIMEOF

# Build
BUILD_DIR="$WORK_DIR/build"

# Step 1: Build host-native aap_protobuf (x86_64).
# Purpose: get a working host protoc + pre-generated .pb.h files for the cross-compile.
# We do NOT build aasdk for host — it needs SSLWrapper.cpp which links against
# the Android ARM64 OpenSSL and will fail on the host compiler.
# Instead we use a host-only shim dir that omits the ARM64 FindOpenSSL override
# so CMake finds the system libssl-dev instead.
HOST_BUILD_DIR="$SHARED_DIR/host-build"
HOST_PROTOC="$HOST_BUILD_DIR/bin/protoc"
if [ ! -f "$HOST_PROTOC" ]; then
    echo ""
    echo "=== Building host aasdk (x86_64) — for protoc + proto generation ==="
    echo ""
    rm -rf "$HOST_BUILD_DIR"
    mkdir -p "$HOST_BUILD_DIR"
    cd "$HOST_BUILD_DIR"

    # Host shim dir: Boost + Absl shims — let CMake find system OpenSSL via libssl-dev
    HOST_SHIM_DIR="$SHARED_DIR/cmake-shims-host"
    mkdir -p "$HOST_SHIM_DIR"
    cp "$SHIM_DIR/FindBoost.cmake" "$HOST_SHIM_DIR/"
    cp "$SHIM_DIR/Findlibusb-1.0.cmake" "$HOST_SHIM_DIR/"
    cp "$SHIM_DIR/Findabsl.cmake" "$HOST_SHIM_DIR/"
    # Do NOT copy FindOpenSSL.cmake — let CMake find system libssl-dev

    cmake "$AASDK_NATIVE" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DBUILD_AASDK_STATIC=ON \
        -DSKIP_BUILD_PROTOBUF=OFF \
        -DSKIP_BUILD_ABSL=OFF \
        -DTARGET_ARCH="" \
        -DLIBUSB_1_LIBRARIES="$STUB_DIR/libusb_stub.c" \
        -DLIBUSB_1_INCLUDE_DIRS="$STUB_DIR" \
        -DCMAKE_MODULE_PATH="$HOST_SHIM_DIR" \
        -DOAL_BOOST_INCLUDE_DIR="$BOOST_NATIVE" \
        -DCMAKE_SKIP_INSTALL_RULES=ON \
        -DAASDK_TEST=OFF \
        2>&1

    # Only build aap_protobuf for host — this runs protoc and generates .pb.h files.
    # We do NOT build aasdk here (SSLWrapper.cpp requires ARM64 OpenSSL).
    cmake --build . --target aap_protobuf -j$(nproc) 2>&1
    echo ""
    echo "Host build complete:"
    ls -la "$HOST_PROTOC" 2>&1
fi

# Step 2: Cross-compile aasdk for Android.
echo ""
echo "=== Configuring aasdk for Android $TARGET_ABI (cross-compile) ==="
echo ""

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Put host protoc on PATH so cmake's find_program(protoc) finds it during
# cross-compile configure. protobuf v22+ uses find_program(protoc) when
# CMAKE_CROSSCOMPILING=TRUE (set by Android toolchain) instead of building ARM64 protoc.
export PATH="$(dirname "$HOST_PROTOC"):$PATH"

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
    -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
    -DProtobuf_PROTOC_EXECUTABLE="$HOST_PROTOC" \
    -DPROTOBUF_PROTOC_EXECUTABLE="$HOST_PROTOC" \
    -DCMAKE_SKIP_INSTALL_RULES=ON \
    -DAASDK_TEST=OFF \
    2>&1

# After configure: cmake has resolved generator expressions and baked the protoc
# path into the build files. aasdk's protobuf/CMakeLists.txt does:
#   set(PROTOBUF_PROTOC_EXECUTABLE "${protobuf_BINARY_DIR}/protoc")
# which resolves to _deps/protobuf-build/protoc.
# Place a host-wrapper there so the build system finds a runnable protoc.
CROSS_PROTOC_DIR="$BUILD_DIR/_deps/protobuf-build"
mkdir -p "$CROSS_PROTOC_DIR"
cat > "$CROSS_PROTOC_DIR/protoc" << WRAPPER
#!/bin/bash
exec "$HOST_PROTOC" "\$@"
WRAPPER
chmod +x "$CROSS_PROTOC_DIR/protoc"
echo "Injected host protoc wrapper: $CROSS_PROTOC_DIR/protoc -> $HOST_PROTOC"

# Belt-and-suspenders: also patch any build/cmake files that reference the ARM64 protoc path
# with the host protoc (covers cmake versions that resolve TARGET_FILE differently).
find "$BUILD_DIR" \( -name "*.cmake" -o -name "build.ninja" -o -name "Makefile" \) \
    -exec grep -lF "$CROSS_PROTOC_DIR/protoc" {} \; 2>/dev/null | while read f; do
    sed -i "s|$CROSS_PROTOC_DIR/protoc|$HOST_PROTOC|g" "$f"
    echo "  Patched protoc path in: $f"
done

echo ""
echo "=== Building aasdk ==="
echo ""

# cmake resolved $<TARGET_FILE:protobuf::protoc> to the literal string "protobuf::protoc"
# (IMPORTED target has no IMPORTED_LOCATION). The generated Makefiles exec that string
# as a shell command. On Linux, ':' is valid in filenames — create a wrapper with that
# exact name in a shim dir and put it first on PATH so /bin/sh finds it.
SHIM_BIN_DIR="$BUILD_DIR/.shim-bin"
mkdir -p "$SHIM_BIN_DIR"
cat > "$SHIM_BIN_DIR/protobuf::protoc" << WRAPPER
#!/bin/sh
exec "$HOST_PROTOC" "\$@"
WRAPPER
chmod +x "$SHIM_BIN_DIR/protobuf::protoc"
export PATH="$SHIM_BIN_DIR:$PATH"
echo "PATH shim: '$SHIM_BIN_DIR/protobuf::protoc' -> $HOST_PROTOC"

cmake --build . --target aasdk aap_protobuf -j$(nproc) 2>&1 || {
    echo ""
    echo "=== Build failed — retrying with verbose output to show actual error ==="
    echo ""
    cmake --build . --target aasdk aap_protobuf -j1 --verbose 2>&1 | tail -100
    exit 1
}

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

# Pack into tarball (on native ext4 — fast) then copy single file to NTFS
TARBALL="$WORK_DIR/aasdk-android-$TARGET_ABI.tar.gz"
(cd "$PACK_DIR" && tar czf "$TARBALL" lib include)

mkdir -p "$OUTPUT_DIR"
echo "Copying to NTFS..."
cp "$TARBALL" "$OUTPUT_DIR/../aasdk-android-$TARGET_ABI.tar.gz"
# --warning=no-timestamp suppresses harmless "Cannot utime" errors on NTFS mounts.
# || true because tar exits non-zero on utime failures even though files extract correctly.
(cd "$OUTPUT_DIR" && tar xzf "../aasdk-android-$TARGET_ABI.tar.gz" --warning=no-timestamp 2>/dev/null) || true
rm -f "$OUTPUT_DIR/../aasdk-android-$TARGET_ABI.tar.gz"

# Verify the key outputs actually landed
if [ ! -f "$OUTPUT_DIR/lib/libaasdk.a" ]; then
    echo "ERROR: libaasdk.a not found in $OUTPUT_DIR/lib/ after extraction"
    exit 1
fi

echo ""
echo "=== aasdk Android $TARGET_ABI build complete ==="
echo "Output: $OUTPUT_DIR/"
ls -la "$OUTPUT_DIR/lib/"
echo "Headers: $(find "$OUTPUT_DIR/include" -type f | wc -l) files"
