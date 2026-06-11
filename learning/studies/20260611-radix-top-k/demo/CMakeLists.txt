cmake_minimum_required(VERSION 3.16)

project(radix_top_k_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(radix_top_k src/radix_top_k.cpp)

target_compile_options(radix_top_k PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

if(APPLE)
  # Some macOS toolchains emit @rpath/libc++.1.dylib without adding a default LC_RPATH.
  target_link_options(radix_top_k PRIVATE -Wl,-rpath,/usr/lib)
endif()
