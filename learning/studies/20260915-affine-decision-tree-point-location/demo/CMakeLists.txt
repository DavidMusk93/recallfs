cmake_minimum_required(VERSION 3.20)
project(odt_point_location_demo C)

add_library(odt STATIC src/odt.c src/point_location_example.c)
target_include_directories(odt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(odt PUBLIC c_std_11)
target_compile_options(odt PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(point_location src/main.c)
target_link_libraries(point_location PRIVATE odt m)
target_compile_options(point_location PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(odt_test tests/odt_test.c)
target_link_libraries(odt_test PRIVATE odt m)
target_compile_options(odt_test PRIVATE -Wall -Wextra -Wpedantic -Werror)

set(FIL_RUNNER "" CACHE FILEPATH "Optional path to the FIL-C executable runner")
if(FIL_RUNNER)
    set_property(TARGET odt_test PROPERTY CROSSCOMPILING_EMULATOR "${FIL_RUNNER}")
endif()

enable_testing()
add_test(NAME odt_correctness COMMAND odt_test)
