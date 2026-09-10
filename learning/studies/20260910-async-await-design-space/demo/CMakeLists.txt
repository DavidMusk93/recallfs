cmake_minimum_required(VERSION 3.20)

project(async_await_design_space_demo LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(
    mini_async
    STATIC
    src/mini_async.c
    src/async_examples.c
)
target_include_directories(mini_async PUBLIC include)

add_executable(async_design_demo src/main.c)
target_link_libraries(async_design_demo PRIVATE mini_async)

add_executable(mini_async_test tests/mini_async_test.c)
target_link_libraries(mini_async_test PRIVATE mini_async)

foreach(target IN ITEMS mini_async async_design_demo mini_async_test)
    target_compile_options(
        ${target}
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endforeach()

enable_testing()
add_test(NAME mini_async_semantics COMMAND mini_async_test)
