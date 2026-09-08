cmake_minimum_required(VERSION 3.20)
project(fast_time_of_day_demo C)

add_compile_options(-Wall -Wextra -Werror)

add_library(fast_time STATIC src/fast_time.c)
target_include_directories(
    fast_time
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(fast_time PUBLIC c_std_11)

add_executable(fast_time_test tests/fast_time_test.c)
target_link_libraries(fast_time_test PRIVATE fast_time)

add_executable(fast_time_benchmark src/benchmark.c)
target_link_libraries(fast_time_benchmark PRIVATE fast_time)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
if(ipo_supported)
    set_property(
        TARGET fast_time fast_time_benchmark
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
else()
    message(STATUS "IPO/LTO unavailable: ${ipo_error}")
endif()

set(FIL_RUNNER "" CACHE FILEPATH "Optional path to the FIL-C executable runner")
if(FIL_RUNNER)
    set_property(
        TARGET fast_time_test
        PROPERTY CROSSCOMPILING_EMULATOR "${FIL_RUNNER}"
    )
endif()

enable_testing()
add_test(NAME fast_time_correctness COMMAND fast_time_test)
