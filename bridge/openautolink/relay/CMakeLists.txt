cmake_minimum_required(VERSION 3.16)
project(openautolink_relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(openautolink-relay src/main.cpp)
target_compile_options(openautolink-relay PRIVATE -Wall -Wextra -Wpedantic)
