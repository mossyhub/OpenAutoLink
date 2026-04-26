cmake_minimum_required(VERSION 3.22.1)
project(openautolink-jni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# NDK paths
# --------------------------------------------------------------------------
# OpenSSL — prebuilt for Android via prefab or manual build.
# We use BoringSSL from Android NDK's built-in instead of maintaining our own
# OpenSSL builds. aasdk's SSLWrapper uses OpenSSL API which BoringSSL supports.
# --------------------------------------------------------------------------

# aasdk source tree (submodule at repo root)
set(AASDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../external/opencardev-aasdk")
get_filename_component(AASDK_SOURCE_DIR "${AASDK_SOURCE_DIR}" ABSOLUTE)

message(STATUS "OAL JNI: aasdk source = ${AASDK_SOURCE_DIR}")
message(STATUS "OAL JNI: ANDROID_ABI = ${ANDROID_ABI}")
message(STATUS "OAL JNI: ANDROID_NDK = ${ANDROID_NDK}")

# --------------------------------------------------------------------------
# Third-party: Boost (headers-only subset for Asio)
# We use Boost.Asio header-only mode (BOOST_ASIO_HEADER_ONLY) to avoid
# needing to compile Boost libraries for Android.
# --------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    boost_headers
    URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# We only need headers — disable all Boost library builds
set(BOOST_INCLUDE_LIBRARIES asio system CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)

# --------------------------------------------------------------------------
# Third-party: OpenSSL for Android
# Use prebuilt OpenSSL from the build-openssl-android.sh script output,
# or from a prefab AAR. For now, find_package with hints.
# --------------------------------------------------------------------------
set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl/${ANDROID_ABI}" CACHE PATH "")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include" CACHE PATH "")

# If prebuilt OpenSSL exists, use it; otherwise we'll need to build it
if(EXISTS "${OPENSSL_ROOT_DIR}/lib/libssl.a")
    message(STATUS "OAL JNI: Using prebuilt OpenSSL from ${OPENSSL_ROOT_DIR}")
    add_library(openssl_ssl STATIC IMPORTED)
    set_target_properties(openssl_ssl PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_ROOT_DIR}/lib/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
    add_library(openssl_crypto STATIC IMPORTED)
    set_target_properties(openssl_crypto PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_ROOT_DIR}/lib/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
    set(OPENSSL_FOUND TRUE)
    set(OPENSSL_LIBRARIES openssl_ssl openssl_crypto)
else()
    message(STATUS "OAL JNI: Prebuilt OpenSSL not found at ${OPENSSL_ROOT_DIR}")
    message(STATUS "OAL JNI: Run scripts/build-openssl-android.sh first")
    # Use find_package as fallback
    find_package(OpenSSL QUIET)
    if(NOT OPENSSL_FOUND)
        message(FATAL_ERROR "OpenSSL not found. Build it with: scripts/build-openssl-android.sh")
    endif()
endif()

# --------------------------------------------------------------------------
# Third-party: libusb stub
# aasdk links libusb but we don't use USB on Android (we use Nearby transport).
# Provide a stub to satisfy the linker.
# --------------------------------------------------------------------------
add_library(usb_stub STATIC "${CMAKE_CURRENT_SOURCE_DIR}/stubs/libusb_stub.c")
target_include_directories(usb_stub PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stubs")

# --------------------------------------------------------------------------
# aasdk protobuf library (aap_protobuf)
# --------------------------------------------------------------------------
# aasdk's protobuf subdirectory needs protoc for the host platform.
# NDK builds use the host protoc (not cross-compiled).
# We let aasdk's FetchContent handle protobuf + abseil download.
set(SKIP_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(SKIP_BUILD_ABSL OFF CACHE BOOL "" FORCE)
set(BUILD_AASDK_STATIC ON CACHE BOOL "" FORCE)

# Override aasdk's libusb requirement — we provide a stub
set(LIBUSB_1_LIBRARIES usb_stub CACHE STRING "" FORCE)
set(LIBUSB_1_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/stubs" CACHE STRING "" FORCE)

# Disable aasdk's compiler override (we use NDK compilers)
set(TARGET_ARCH "" CACHE STRING "" FORCE)

# Build aasdk as static library for Android
add_subdirectory("${AASDK_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/aasdk_build" EXCLUDE_FROM_ALL)

# --------------------------------------------------------------------------
# Boost — fetch and configure (header-only for Asio)
# --------------------------------------------------------------------------
FetchContent_MakeAvailable(boost_headers)

# --------------------------------------------------------------------------
# JNI bridge library
# --------------------------------------------------------------------------
add_library(openautolink-jni SHARED
    aasdk_jni.cpp
    jni_transport.cpp
    jni_session.cpp
    jni_service_catalog.cpp
)

target_include_directories(openautolink-jni PRIVATE
    "${AASDK_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/aasdk_build"
    "${CMAKE_CURRENT_BINARY_DIR}/aasdk_build/include"
    "${CMAKE_CURRENT_BINARY_DIR}/aasdk_build/protobuf"
    "${OPENSSL_INCLUDE_DIR}"
)

# Link against aasdk (static), protobuf, OpenSSL, Boost, Android log
target_link_libraries(openautolink-jni PRIVATE
    aasdk
    aap_protobuf
    ${OPENSSL_LIBRARIES}
    usb_stub
    log          # Android __android_log_print
    android      # ANativeWindow, etc.
)

# If Boost targets are available from FetchContent, link them
if(TARGET Boost::system)
    target_link_libraries(openautolink-jni PRIVATE Boost::system)
endif()
if(TARGET Boost::asio)
    target_link_libraries(openautolink-jni PRIVATE Boost::asio)
endif()

target_compile_definitions(openautolink-jni PRIVATE
    BOOST_ASIO_HAS_STD_INVOKE_RESULT=1
    BOOST_ALL_NO_LIB=1
)

# Strip in release for smaller APK
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(openautolink-jni PRIVATE -s)
endif()
