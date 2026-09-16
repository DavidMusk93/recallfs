cmake_minimum_required(VERSION 3.20)
project(odt_example VERSION 1.0.0 LANGUAGES C)

include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ODT_BENCHMARK_DISABLE_OBSERVABLE_SINK "Build the benchmark DCE negative probe" OFF)

find_package(odt 1.0 CONFIG REQUIRED)

add_executable(odt_example src/odt_example.c)
target_link_libraries(odt_example PRIVATE odt::odt)

add_executable(odt_benchmark src/benchmark.c)
target_link_libraries(odt_benchmark PRIVATE odt::odt)
if(ODT_BENCHMARK_DISABLE_OBSERVABLE_SINK)
    target_compile_definitions(odt_benchmark PRIVATE ODT_BENCHMARK_DISABLE_OBSERVABLE_SINK)
endif()
if(BUILD_TESTING)
    add_test(NAME odt_benchmark_self_check COMMAND odt_benchmark --self-check)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    foreach(target IN ITEMS odt_example odt_benchmark)
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
                -Wconversion
                -Wsign-conversion
                -ffp-contract=off
        )
    endforeach()
endif()
