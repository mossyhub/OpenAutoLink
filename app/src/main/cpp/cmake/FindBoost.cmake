# FindBoost.cmake shim for Android NDK build.
# aasdk calls find_package(Boost REQUIRED COMPONENTS system log_setup log).
# We provide pre-extracted Boost headers (header-only for asio/system).
# With DISABLE_MODERN_LOGGING=ON, Boost.Log is not actually used.

# Find our pre-extracted headers
set(_boost_inc "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/include")
if(NOT EXISTS "${_boost_inc}/boost/asio.hpp")
    # Try relative to the shim file location
    get_filename_component(_boost_inc "${CMAKE_CURRENT_LIST_DIR}/../third_party/boost/include" ABSOLUTE)
endif()

if(EXISTS "${_boost_inc}/boost/asio.hpp")
    set(Boost_FOUND TRUE)
    set(Boost_INCLUDE_DIRS "${_boost_inc}")
    set(Boost_LIBRARIES "")
    set(Boost_VERSION "1.83.0")
    set(Boost_VERSION_MAJOR 1)
    set(Boost_VERSION_MINOR 83)
    # Boost.System is header-only since 1.69
    set(Boost_SYSTEM_FOUND TRUE)
    # Mark log components as found (not actually used with DISABLE_MODERN_LOGGING)
    set(Boost_LOG_FOUND TRUE)
    set(Boost_LOG_SETUP_FOUND TRUE)
    include_directories(SYSTEM "${_boost_inc}")
    message(STATUS "FindBoost shim: using pre-extracted headers from ${_boost_inc}")
else()
    message(FATAL_ERROR "FindBoost shim: Boost headers not found at ${_boost_inc}")
endif()
