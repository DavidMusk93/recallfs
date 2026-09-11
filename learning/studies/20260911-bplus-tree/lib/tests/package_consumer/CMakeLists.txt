cmake_minimum_required(VERSION 3.20)
project(btree_package_consumer LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(btree 1 CONFIG REQUIRED)

add_executable(btree_package_consumer main.c)
target_link_libraries(
    btree_package_consumer
    PRIVATE btree::btree btree::memory
)
