cmake_minimum_required(VERSION 3.20)
project(rco VERSION 0.1.0 LANGUAGES C ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "rco currently supports Linux only")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "rco currently supports System V AMD64 only")
endif()

option(RCO_BUILD_TESTS "Build native correctness tests" ON)
option(RCO_BUILD_BENCHMARKS "Build context-switch benchmarks" ON)

add_library(rco
    src/rco.c
    src/rco_context_x86_64.S
)
add_library(rco::rco ALIAS rco)

target_compile_features(rco PUBLIC c_std_11)
target_include_directories(
    rco
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(
    rco
    PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-fno-omit-frame-pointer>
        $<$<COMPILE_LANGUAGE:ASM>:-fcf-protection=branch>
)

if(RCO_BUILD_TESTS)
    enable_testing()

    add_executable(rco_test tests/rco_test.c)
    target_link_libraries(rco_test PRIVATE rco m)
    target_compile_options(
        rco_test
        PRIVATE
            -Wall
            -Wextra
            -Werror
            -fno-omit-frame-pointer
    )
    add_test(NAME rco_native COMMAND rco_test)

    add_executable(rco_guard_test tests/rco_guard_test.c)
    target_link_libraries(rco_guard_test PRIVATE rco)
    target_compile_options(
        rco_guard_test
        PRIVATE
            -Wall
            -Wextra
            -Werror
            -fno-omit-frame-pointer
    )
    add_test(NAME rco_guard_page COMMAND rco_guard_test)
endif()

if(RCO_BUILD_BENCHMARKS)
    add_executable(rco_bench bench/rco_bench.c)
    target_link_libraries(rco_bench PRIVATE rco)
    target_compile_options(
        rco_bench
        PRIVATE
            -Wall
            -Wextra
            -Werror
            -fno-omit-frame-pointer
    )
endif()

include(GNUInstallDirs)
install(TARGETS rco ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/rco.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
