cmake_minimum_required(VERSION 3.20)
project(rbt VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RBT_BUILD_TESTS "Build rbt contract tests" ON)
option(RBT_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

add_library(rbt STATIC
    src/rbt.c
    src/rbt_crc32c.c
    src/rbt_page_updates.c
)
add_library(rbt::rbt ALIAS rbt)

add_library(rbt_storage_memory STATIC
    src/rbt_storage_memory.c
)
add_library(rbt::memory ALIAS rbt_storage_memory)
set_target_properties(rbt_storage_memory PROPERTIES EXPORT_NAME memory)

add_library(rbt_storage_file STATIC
    src/rbt_storage_file.c
)
add_library(rbt::file ALIAS rbt_storage_file)
set_target_properties(rbt_storage_file PROPERTIES EXPORT_NAME file)

set(rbt_library_targets
    rbt
    rbt_storage_memory
    rbt_storage_file
)

if(RBT_BUILD_TESTS AND UNIX)
    add_library(rbt_storage_file_test_hooks STATIC
        src/rbt_storage_file.c
    )
    target_compile_definitions(
        rbt_storage_file_test_hooks
        PRIVATE RBT_ENABLE_TEST_HOOKS=1
    )
    list(APPEND rbt_library_targets rbt_storage_file_test_hooks)
endif()

function(rbt_enable_warnings target)
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
endfunction()

foreach(target IN LISTS rbt_library_targets)
    target_compile_features(${target} PUBLIC c_std_11)
    target_include_directories(
        ${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    rbt_enable_warnings(${target})
endforeach()

if(UNIX)
    foreach(target IN LISTS rbt_library_targets)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
        if(APPLE)
            target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
        endif()
    endforeach()
endif()

target_link_libraries(rbt_storage_memory PUBLIC rbt)
target_link_libraries(rbt_storage_file PUBLIC rbt)

if(TARGET rbt_storage_file_test_hooks)
    target_link_libraries(rbt_storage_file_test_hooks PUBLIC rbt)
endif()

if(RBT_ENABLE_SANITIZERS)
    foreach(target IN LISTS rbt_library_targets)
        target_compile_options(
            ${target}
            PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

if(RBT_BUILD_TESTS)
    enable_testing()

    function(rbt_configure_test target)
        rbt_enable_warnings(${target})
        if(RBT_ENABLE_SANITIZERS)
            target_compile_options(
                ${target}
                PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
            )
            target_link_options(
                ${target}
                PRIVATE -fsanitize=address,undefined
            )
        endif()
        if(APPLE)
            target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
        endif()
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    function(rbt_add_test target source)
        add_executable(${target} ${source})
        target_link_libraries(
            ${target}
            PRIVATE rbt_storage_memory rbt_storage_file
        )
        rbt_configure_test(${target})
    endfunction()

    rbt_add_test(rbt_schema_test tests/rbt_schema_test.c)
    rbt_add_test(rbt_overflow_test tests/rbt_overflow_test.c)
    rbt_add_test(rbt_locality_test tests/rbt_locality_test.c)
    rbt_add_test(rbt_structure_test tests/rbt_structure_test.c)
    rbt_add_test(rbt_model_test tests/rbt_model_test.c)

    if(UNIX)
        rbt_add_test(rbt_backend_test tests/rbt_backend_test.c)
        rbt_add_test(rbt_corruption_test tests/rbt_corruption_test.c)

        add_executable(rbt_crash_test tests/rbt_crash_test.c)
        target_link_libraries(
            rbt_crash_test
            PRIVATE rbt_storage_file_test_hooks
        )
        rbt_configure_test(rbt_crash_test)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
    TARGETS rbt rbt_storage_memory rbt_storage_file
    EXPORT rbtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(
    FILES include/rbt.h include/rbt_backends.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    EXPORT rbtTargets
    NAMESPACE rbt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt
)

configure_package_config_file(
    cmake/rbtConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/rbtConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rbtConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ExactVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rbtConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/rbtConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rbt
)
