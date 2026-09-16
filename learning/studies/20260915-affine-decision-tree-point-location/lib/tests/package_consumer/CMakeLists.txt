cmake_minimum_required(VERSION 3.20)
project(odt_package_consumer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(odt 1.0 CONFIG REQUIRED)

add_executable(odt_consumer_c consumer.c)
target_link_libraries(odt_consumer_c PRIVATE odt::odt)

add_executable(odt_consumer_cpp consumer.cpp)
target_link_libraries(odt_consumer_cpp PRIVATE odt::odt)

foreach(target IN ITEMS odt_consumer_c odt_consumer_cpp)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
                -Wconversion
                -Wsign-conversion
        )
    endif()
endforeach()
