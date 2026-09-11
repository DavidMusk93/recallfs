cmake_minimum_required(VERSION 3.20)
project(btree VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BTREE_BUILD_TESTS "Build B+ tree regression tests" ON)
option(BTREE_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

add_library(btree STATIC
    src/btree.c
    src/btree_crc32c.c
)
add_library(btree::btree ALIAS btree)

add_library(btree_storage_memory STATIC
    src/btree_storage_memory.c
)
add_library(btree::memory ALIAS btree_storage_memory)
set_target_properties(btree_storage_memory PROPERTIES EXPORT_NAME memory)

add_library(btree_storage_file STATIC
    src/btree_storage_file.c
)
add_library(btree::file ALIAS btree_storage_file)
set_target_properties(btree_storage_file PROPERTIES EXPORT_NAME file)

set(btree_library_targets
    btree
    btree_storage_memory
    btree_storage_file
)

if(BTREE_BUILD_TESTS AND UNIX)
    add_library(btree_storage_file_test_hooks STATIC
        src/btree_storage_file.c
    )
    target_compile_definitions(
        btree_storage_file_test_hooks
        PRIVATE BTREE_ENABLE_TEST_HOOKS=1
    )
    list(APPEND btree_library_targets btree_storage_file_test_hooks)
endif()

foreach(target IN LISTS btree_library_targets)
    target_compile_features(${target} PUBLIC c_std_11)
    target_include_directories(
        ${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
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
endforeach()

if(UNIX)
    foreach(target IN LISTS btree_library_targets)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
        if(APPLE)
            target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
        endif()
    endforeach()
endif()

target_link_libraries(btree_storage_file PUBLIC btree)

if(TARGET btree_storage_file_test_hooks)
    target_link_libraries(btree_storage_file_test_hooks PUBLIC btree)
endif()

if(BTREE_ENABLE_SANITIZERS)
    foreach(target IN LISTS btree_library_targets)
        target_compile_options(
            ${target}
            PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        if(UNIX)
            target_compile_definitions(
                ${target}
                PRIVATE _POSIX_C_SOURCE=200809L
            )
        endif()
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

if(BTREE_BUILD_TESTS)
    enable_testing()

    function(btree_configure_test target)
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
        if(BTREE_ENABLE_SANITIZERS)
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

    function(btree_add_test target source)
        add_executable(${target} ${source})
        target_link_libraries(
            ${target}
            PRIVATE btree_storage_memory btree_storage_file
        )
        btree_configure_test(${target})
    endfunction()

    btree_add_test(btree_model_test tests/btree_model_test.c)
    btree_add_test(btree_generic_test tests/btree_generic_test.c)
    btree_add_test(btree_contract_test tests/btree_contract_test.c)
    btree_add_test(btree_backend_test tests/btree_backend_test.c)
    btree_add_test(btree_structure_test tests/btree_structure_test.c)
    btree_add_test(btree_persistence_test tests/btree_persistence_test.c)
    btree_add_test(btree_corruption_test tests/btree_corruption_test.c)

    if(UNIX)
        add_executable(btree_crash_test tests/btree_crash_test.c)
        target_link_libraries(
            btree_crash_test
            PRIVATE btree_storage_file_test_hooks
        )
        btree_configure_test(btree_crash_test)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
    TARGETS btree btree_storage_memory btree_storage_file
    EXPORT btreeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(
    FILES include/btree.h include/btree_backends.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    EXPORT btreeTargets
    NAMESPACE btree::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btree
)

configure_package_config_file(
    cmake/btreeConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/btreeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btree
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/btreeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/btreeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/btreeConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btree
)
