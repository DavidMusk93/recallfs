cmake_minimum_required(VERSION 3.20)
project(odt_example VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(odt 1.0 CONFIG REQUIRED)

add_executable(odt_example src/odt_example.c)
target_link_libraries(odt_example PRIVATE odt::odt)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        odt_example
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wsign-conversion
            -ffp-contract=off
    )
endif()
