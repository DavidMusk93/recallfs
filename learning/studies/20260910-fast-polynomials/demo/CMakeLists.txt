cmake_minimum_required(VERSION 3.20)
project(fast_polynomials_demo C)

add_compile_options(-Wall -Wextra -Werror)

add_library(fast_polynomial STATIC src/fast_polynomial.c)
target_include_directories(
    fast_polynomial
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(fast_polynomial PUBLIC c_std_11)

add_executable(fast_polynomial_test tests/fast_polynomial_test.c)
target_link_libraries(fast_polynomial_test PRIVATE fast_polynomial m)

add_executable(fast_polynomial_benchmark src/benchmark.c)
target_link_libraries(fast_polynomial_benchmark PRIVATE fast_polynomial m)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
if(ipo_supported)
    set_property(
        TARGET fast_polynomial fast_polynomial_benchmark
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
else()
    message(STATUS "IPO/LTO unavailable: ${ipo_error}")
endif()

set(FIL_RUNNER "" CACHE FILEPATH "Optional path to the FIL-C executable runner")
if(FIL_RUNNER)
    set_property(
        TARGET fast_polynomial_test
        PROPERTY CROSSCOMPILING_EMULATOR "${FIL_RUNNER}"
    )
endif()

enable_testing()
add_test(NAME fast_polynomial_correctness COMMAND fast_polynomial_test)
