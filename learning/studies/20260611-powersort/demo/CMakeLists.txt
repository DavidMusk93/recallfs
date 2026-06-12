cmake_minimum_required(VERSION 3.16)

project(powersort_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(powersort_demo src/powersort_demo.cpp)

target_compile_options(powersort_demo PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror=return-type
)

if(APPLE)
  # Some macOS toolchains emit @rpath/libc++.1.dylib without adding a default LC_RPATH.
  target_link_options(powersort_demo PRIVATE -Wl,-rpath,/usr/lib)
endif()
