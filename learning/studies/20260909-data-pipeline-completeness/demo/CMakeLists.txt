cmake_minimum_required(VERSION 3.20)
project(data_pipeline_completeness_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(completeness_warnings INTERFACE)
target_compile_options(completeness_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

add_library(completeness_model
    src/completeness.cpp
)
target_include_directories(completeness_model PUBLIC include)
target_link_libraries(completeness_model PRIVATE completeness_warnings)

add_executable(completeness_tests
    tests/completeness_test.cpp
)
target_link_libraries(completeness_tests PRIVATE completeness_model completeness_warnings)

add_executable(completeness_demo
    src/main.cpp
)
target_link_libraries(completeness_demo PRIVATE completeness_model completeness_warnings)

if(ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    foreach(target completeness_model completeness_tests completeness_demo)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

enable_testing()
add_test(NAME completeness_model COMMAND completeness_tests)
