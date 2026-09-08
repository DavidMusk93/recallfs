cmake_minimum_required(VERSION 3.20)
project(allocator_provenance_demo C)

add_executable(
    allocator_provenance_demo
    src/allocator_demo.c
    tests/allocator_demo_test.c
)

target_include_directories(
    allocator_provenance_demo
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(allocator_provenance_demo PRIVATE c_std_11)
target_compile_options(
    allocator_provenance_demo
    PRIVATE
    -Wall
    -Wextra
    -Werror
)

set(FIL_RUNNER "" CACHE FILEPATH "Path to the FIL-C executable runner")
if(NOT FIL_RUNNER)
    message(FATAL_ERROR "FIL_RUNNER is required to run FIL-C executables")
endif()

enable_testing()
add_test(
    NAME allocator_provenance
    COMMAND "${FIL_RUNNER}" "$<TARGET_FILE:allocator_provenance_demo>"
)
