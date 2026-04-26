# FindOpenSSL.cmake shim for Android NDK build.
# aasdk calls find_package(OpenSSL REQUIRED).
# We provide OpenSSL via prebuilt static libraries — this shim maps
# our imported targets to the variables aasdk expects.

if(TARGET openssl_ssl AND TARGET openssl_crypto)
    set(OPENSSL_FOUND TRUE)
    set(OpenSSL_FOUND TRUE)
    set(OPENSSL_LIBRARIES openssl_ssl openssl_crypto)
    # Get include dir from target property
    get_target_property(_ssl_inc openssl_ssl INTERFACE_INCLUDE_DIRECTORIES)
    set(OPENSSL_INCLUDE_DIR "${_ssl_inc}")
    message(STATUS "FindOpenSSL shim: using prebuilt Android OpenSSL")
else()
    message(FATAL_ERROR "FindOpenSSL shim: openssl_ssl/openssl_crypto targets not defined — build OpenSSL first")
endif()
