cmake_minimum_required(VERSION 3.20)
project(rbt_package_consumer LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(rbt 0.2 CONFIG REQUIRED)

add_executable(rbt_package_consumer main.c)
target_link_libraries(
    rbt_package_consumer
    PRIVATE rbt::rbt rbt::memory rbt::file
)
