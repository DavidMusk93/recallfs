cmake_minimum_required(VERSION 3.20)
project(odt VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ODT_BUILD_TESTS "Build odt contract tests" ON)

add_library(
    odt STATIC
    src/odt.c
    src/odt_exact.c
    src/odt_geometry.c
)
add_library(odt::odt ALIAS odt)

target_compile_features(odt PUBLIC c_std_11)
if(NOT MSVC)
    target_link_libraries(odt PRIVATE m)
endif()
target_include_directories(
    odt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

function(odt_enable_warnings target)
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
                -ffp-contract=off
        )
    endif()
endfunction()

odt_enable_warnings(odt)

if(ODT_BUILD_TESTS)
    enable_testing()

    add_executable(
        odt_api_test
        tests/odt_api_test.c
        tests/odt_test_support.c
    )
    target_include_directories(
        odt_api_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(odt_api_test PRIVATE odt)
    odt_enable_warnings(odt_api_test)
    add_test(NAME odt_api_test COMMAND odt_api_test)

    add_executable(
        odt_geometry_test
        tests/odt_geometry_test.c
        tests/odt_test_support.c
    )
    target_include_directories(
        odt_geometry_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(odt_geometry_test PRIVATE odt)
    odt_enable_warnings(odt_geometry_test)
    add_test(NAME odt_geometry_test COMMAND odt_geometry_test)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
    TARGETS odt
    EXPORT odtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/odt.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT odtTargets
    NAMESPACE odt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/odt
)

configure_package_config_file(
    cmake/odtConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/odtConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/odt
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/odtConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/odtConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/odtConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/odt
)
