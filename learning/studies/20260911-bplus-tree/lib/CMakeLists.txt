cmake_minimum_required(VERSION 3.20)
project(bptree VERSION 0.1.0 LANGUAGES C)

option(BPTREE_BUILD_TESTS "Build B+ tree regression tests" ON)
option(BPTREE_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(BPTREE_ENABLE_TEST_HOOKS "Enable subprocess crash points" OFF)

add_library(bptree STATIC
    src/bptree.c
    src/bptree_crc32c.c
)
add_library(bptree::bptree ALIAS bptree)

add_library(bptree_storage_memory STATIC
    src/bptree_storage_memory.c
)
add_library(bptree::memory ALIAS bptree_storage_memory)

add_library(bptree_storage_file STATIC
    src/bptree_storage_file.c
)
add_library(bptree::file ALIAS bptree_storage_file)

foreach(target IN ITEMS bptree bptree_storage_memory bptree_storage_file)
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
    foreach(target IN ITEMS bptree bptree_storage_memory bptree_storage_file)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    endforeach()
endif()

target_link_libraries(bptree_storage_file PUBLIC bptree)

if(BPTREE_ENABLE_TEST_HOOKS)
    target_compile_definitions(
        bptree_storage_file
        PRIVATE BPTREE_ENABLE_TEST_HOOKS=1
    )
endif()

if(BPTREE_ENABLE_SANITIZERS)
    foreach(target IN ITEMS bptree bptree_storage_memory bptree_storage_file)
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

if(BPTREE_BUILD_TESTS)
    enable_testing()

    function(bptree_add_test target source)
        add_executable(${target} ${source})
        target_link_libraries(
            ${target}
            PRIVATE bptree_storage_memory bptree_storage_file
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
        if(BPTREE_ENABLE_SANITIZERS)
            target_compile_options(
                ${target}
                PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
            )
            target_link_options(
                ${target}
                PRIVATE -fsanitize=address,undefined
            )
        endif()
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    bptree_add_test(bptree_model_test tests/bptree_model_test.c)
    bptree_add_test(bptree_contract_test tests/bptree_contract_test.c)
    bptree_add_test(bptree_backend_test tests/bptree_backend_test.c)
    bptree_add_test(bptree_structure_test tests/bptree_structure_test.c)
    bptree_add_test(bptree_persistence_test tests/bptree_persistence_test.c)
    bptree_add_test(bptree_corruption_test tests/bptree_corruption_test.c)

    if(UNIX AND BPTREE_ENABLE_TEST_HOOKS)
        bptree_add_test(bptree_crash_test tests/bptree_crash_test.c)
    endif()
endif()

include(GNUInstallDirs)
install(
    TARGETS bptree bptree_storage_memory bptree_storage_file
    EXPORT bptreeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(
    FILES include/bptree.h include/bptree_backends.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    EXPORT bptreeTargets
    NAMESPACE bptree::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bptree
)
