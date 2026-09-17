cmake_minimum_required(VERSION 3.20)
project(ringzero_hashing_benchmark VERSION 0.2.0 LANGUAGES C)

option(RZ_BUILD_BENCHMARK "Build the selector benchmark" ON)
option(RZ_BUILD_TOOLS "Build packet-path validation tools" ON)

function(rz_enable_warnings target)
    target_compile_options(
        ${target}
        PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wshadow
            -Wstrict-prototypes
            -fno-omit-frame-pointer
    )
endfunction()

add_library(ringzero_hashing STATIC src/hashing.c)
add_library(ringzero::hashing ALIAS ringzero_hashing)
target_compile_features(ringzero_hashing PUBLIC c_std_11)
target_include_directories(
    ringzero_hashing
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
rz_enable_warnings(ringzero_hashing)

include(CTest)
if(BUILD_TESTING)
    add_executable(ringzero_hashing_test tests/test_hashing.c)
    target_link_libraries(ringzero_hashing_test PRIVATE ringzero::hashing)
    rz_enable_warnings(ringzero_hashing_test)
    add_test(NAME ringzero_hashing_properties COMMAND ringzero_hashing_test)
endif()

if(RZ_BUILD_TOOLS)
    add_executable(ringzero_udp_sequence tools/udp_sequence.c)
    rz_enable_warnings(ringzero_udp_sequence)
    if(BUILD_TESTING)
        add_test(NAME ringzero_udp_sequence_self_test
                 COMMAND ringzero_udp_sequence self-test)
        add_test(
            NAME ringzero_udp_sequence_rejects_negative_pace
            COMMAND ringzero_udp_sequence send 127.0.0.1 9 1 -1
        )
        set_tests_properties(
            ringzero_udp_sequence_rejects_negative_pace
            PROPERTIES WILL_FAIL TRUE
        )
        add_test(
            NAME ringzero_udp_sequence_rejects_negative_timeout
            COMMAND ringzero_udp_sequence receive 127.0.0.1 9 1 -1
        )
        set_tests_properties(
            ringzero_udp_sequence_rejects_negative_timeout
            PROPERTIES WILL_FAIL TRUE
        )
    endif()
endif()

if(RZ_BUILD_BENCHMARK)
    add_executable(ringzero_hashing_compare src/compare.c)
    target_link_libraries(ringzero_hashing_compare PRIVATE ringzero::hashing m)
    rz_enable_warnings(ringzero_hashing_compare)
    if(BUILD_TESTING)
        add_test(
            NAME ringzero_hashing_compare_smoke
            COMMAND
                ringzero_hashing_compare
                --keys 10000
                --lookup-keys 1000
                --samples 3
                --backends 8
                --table-size 1031
                --vnodes 32
                --order-offset 0
        )
        set_tests_properties(
            ringzero_hashing_compare_smoke
            PROPERTIES PASS_REGULAR_EXPRESSION "# sink=[1-9][0-9]*"
        )
        add_test(
            NAME ringzero_hashing_compare_rejects_bad_offset
            COMMAND ringzero_hashing_compare --order-offset 5
        )
        set_tests_properties(
            ringzero_hashing_compare_rejects_bad_offset
            PROPERTIES WILL_FAIL TRUE
        )
        add_test(
            NAME ringzero_hashing_compare_rejects_size_overflow
            COMMAND
                ringzero_hashing_compare
                --lookup-keys 18446744073709551615
        )
        set_tests_properties(
            ringzero_hashing_compare_rejects_size_overflow
            PROPERTIES WILL_FAIL TRUE
        )
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
    if(ipo_supported)
        set_property(
            TARGET ringzero_hashing ringzero_hashing_compare
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        )
    else()
        message(STATUS "IPO/LTO unavailable: ${ipo_error}")
    endif()
endif()
