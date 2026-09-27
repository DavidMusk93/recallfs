cmake_minimum_required(VERSION 3.20)

project(lock_free_ring_buffer_study LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(SPSC_CACHE_LINE_SIZE "64" CACHE STRING "Target destructive cache-line size")
set(FIL_RUNNER "" CACHE FILEPATH "Optional runner for FIL-C executables")

find_package(Threads REQUIRED)

add_library(spsc_ring OBJECT src/spsc_ring.c)
target_include_directories(spsc_ring PUBLIC include)
target_compile_definitions(
    spsc_ring
    PUBLIC
    SPSC_CACHE_LINE_SIZE=${SPSC_CACHE_LINE_SIZE}
)
target_link_libraries(spsc_ring PUBLIC Threads::Threads)

add_executable(spsc_ring_test tests/spsc_ring_test.c)
target_link_libraries(spsc_ring_test PRIVATE spsc_ring)

add_executable(spsc_ring_benchmark src/benchmark.c)
target_link_libraries(spsc_ring_benchmark PRIVATE spsc_ring)

foreach(target IN ITEMS spsc_ring spsc_ring_test spsc_ring_benchmark)
  target_compile_options(
      ${target}
      PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
      -Wconversion
      -Wshadow
      -Wstrict-prototypes
  )
endforeach()

if(FIL_RUNNER)
  set_property(
      TARGET spsc_ring_test spsc_ring_benchmark
      PROPERTY CROSSCOMPILING_EMULATOR "${FIL_RUNNER}"
  )
endif()

enable_testing()
add_test(NAME spsc_ring_correctness COMMAND spsc_ring_test)
add_test(NAME spsc_ring_benchmark_self_check
         COMMAND spsc_ring_benchmark --self-check)
