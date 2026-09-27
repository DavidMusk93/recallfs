cmake_minimum_required(VERSION 3.20)
project(lsm_deletion_map_demo C)

add_compile_options(-Wall -Wextra -Werror)

add_library(deletion_map OBJECT src/deletion_map.c)
target_include_directories(
    deletion_map
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(deletion_map PUBLIC c_std_11)

add_executable(deletion_map_demo src/main.c)
target_link_libraries(deletion_map_demo PRIVATE deletion_map)

add_executable(deletion_map_test tests/deletion_map_test.c)
target_link_libraries(deletion_map_test PRIVATE deletion_map)

set(FIL_RUNNER "" CACHE FILEPATH "Optional path to the FIL-C executable runner")
if(FIL_RUNNER)
    set_property(
        TARGET deletion_map_demo deletion_map_test
        PROPERTY CROSSCOMPILING_EMULATOR "${FIL_RUNNER}"
    )
endif()

enable_testing()
add_test(NAME deletion_map_correctness COMMAND deletion_map_test)
