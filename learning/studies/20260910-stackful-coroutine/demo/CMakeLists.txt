cmake_minimum_required(VERSION 3.20)
project(rco VERSION 0.2.0 LANGUAGES C ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "rco currently supports Linux only")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "rco currently supports System V AMD64 only")
endif()

option(RCO_BUILD_TESTS "Build native correctness tests" ON)
option(RCO_BUILD_BENCHMARKS "Build context-switch benchmarks" ON)
option(RCO_BUILD_EXAMPLES "Build production-oriented examples" ON)
option(RCO_BUILD_CACS "Build experimental Clang 21 CACS variants" OFF)

find_package(Threads REQUIRED)

function(rco_enable_c_warnings target)
    target_compile_options(
        ${target}
        PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-fno-omit-frame-pointer>
    )
endfunction()

function(rco_configure_library target)
    target_compile_features(${target} PUBLIC c_std_11)
    target_link_libraries(${target} PUBLIC Threads::Threads rt)
    target_include_directories(
        ${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    rco_enable_c_warnings(${target})
    target_compile_options(
        ${target}
        PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-fcf-protection=branch>
    )
endfunction()

if(RCO_BUILD_CACS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_library(rco
    src/rco.c
    src/rco_pool.c
    src/rco_context_x86_64.S
)
add_library(rco::rco ALIAS rco)
rco_configure_library(rco)

if(RCO_BUILD_CACS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       CMAKE_C_COMPILER_VERSION VERSION_LESS 21)
        message(FATAL_ERROR
                "RCO_BUILD_CACS requires Zig cc/Clang 21 or newer")
    endif()

    include(CheckCSourceCompiles)
    check_c_source_compiles(
        "
        #if !defined(__has_attribute) || !__has_attribute(preserve_none)
        #error preserve_none is unavailable
        #endif
        __attribute__((preserve_none)) static int probe(void) { return 0; }
        int main(void) { return probe(); }
        "
        RCO_COMPILER_HAS_PRESERVE_NONE
    )
    if(NOT RCO_COMPILER_HAS_PRESERVE_NONE)
        message(FATAL_ERROR
                "RCO_BUILD_CACS requires compiler preserve_none support")
    endif()

    function(rco_enable_cacs_comparison_flags target)
        target_compile_options(
            ${target}
            PUBLIC
                $<$<COMPILE_LANGUAGE:C>:-mno-red-zone;-fcf-protection=branch>
        )
    endfunction()

    rco_enable_cacs_comparison_flags(rco)

    function(rco_add_cacs_library target)
        add_library(
            ${target}
            src/rco.c
            src/rco_pool.c
            src/rco_context_cacs_x86_64.S
        )
        rco_configure_library(${target})
        target_compile_definitions(${target} PUBLIC RCO_CACS=1)
        rco_enable_cacs_comparison_flags(${target})
    endfunction()

    rco_add_cacs_library(rco_cacs)
    add_library(rco::cacs ALIAS rco_cacs)

    rco_add_cacs_library(rco_cacs_preserve_none)
    add_library(rco::cacs_preserve_none ALIAS rco_cacs_preserve_none)
    target_compile_definitions(
        rco_cacs_preserve_none
        PUBLIC
            RCO_CACS_PRESERVE_NONE=1
    )
endif()

if(RCO_BUILD_TESTS)
    enable_testing()

    function(rco_add_local_state_test target test_name library)
        add_executable(${target} tests/rco_local_state_test.c)
        target_link_libraries(${target} PRIVATE ${library} Threads::Threads)
        rco_enable_c_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    function(rco_add_preempt_test target test_name library)
        add_executable(${target} tests/rco_preempt_test.c)
        target_link_libraries(${target} PRIVATE ${library} Threads::Threads)
        rco_enable_c_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    function(rco_add_pool_test target test_name library)
        add_executable(${target} tests/rco_pool_test.c)
        target_link_libraries(${target} PRIVATE ${library} Threads::Threads)
        rco_enable_c_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 15)
    endfunction()

    add_executable(rco_test tests/rco_test.c)
    target_link_libraries(rco_test PRIVATE rco m)
    rco_enable_c_warnings(rco_test)
    add_test(NAME rco_native COMMAND rco_test)

    rco_add_pool_test(
        rco_pool_test_sysv
        rco_pool_sysv
        rco
    )

    rco_add_local_state_test(
        rco_local_state_test
        rco_local_state
        rco
    )

    rco_add_preempt_test(
        rco_preempt_test
        rco_preempt_sysv
        rco
    )

    add_executable(rco_guard_test tests/rco_guard_test.c)
    target_link_libraries(rco_guard_test PRIVATE rco)
    rco_enable_c_warnings(rco_guard_test)
    add_test(NAME rco_guard_page COMMAND rco_guard_test)

    if(RCO_BUILD_CACS)
        function(rco_add_cacs_tests suffix library expect_preserve_none)
            add_executable(rco_test_${suffix} tests/rco_test.c)
            target_link_libraries(rco_test_${suffix} PRIVATE ${library} m)
            rco_enable_c_warnings(rco_test_${suffix})
            add_test(NAME rco_native_${suffix} COMMAND rco_test_${suffix})

            rco_add_local_state_test(
                rco_local_state_test_${suffix}
                rco_local_state_${suffix}
                ${library}
            )

            rco_add_preempt_test(
                rco_preempt_test_${suffix}
                rco_preempt_${suffix}
                ${library}
            )

            add_executable(rco_guard_test_${suffix} tests/rco_guard_test.c)
            target_link_libraries(rco_guard_test_${suffix} PRIVATE ${library})
            rco_enable_c_warnings(rco_guard_test_${suffix})
            add_test(NAME rco_guard_page_${suffix}
                     COMMAND rco_guard_test_${suffix})

            add_executable(rco_cacs_test_${suffix} tests/rco_cacs_test.c)
            target_link_libraries(rco_cacs_test_${suffix} PRIVATE ${library})
            target_include_directories(
                rco_cacs_test_${suffix}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(
                rco_cacs_test_${suffix}
                PRIVATE RCO_EXPECT_PRESERVE_NONE=${expect_preserve_none}
            )
            rco_enable_c_warnings(rco_cacs_test_${suffix})
            add_test(NAME rco_cacs_abi_${suffix}
                     COMMAND rco_cacs_test_${suffix})

            add_test(
                NAME rco_cacs_codegen_${suffix}
                COMMAND
                    ${CMAKE_COMMAND}
                    -DRCO_COMPILER=${CMAKE_C_COMPILER}
                    -DRCO_COMPILER_ARG1=${CMAKE_C_COMPILER_ARG1}
                    -DRCO_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                    -DRCO_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                    -DRCO_EXECUTABLE=$<TARGET_FILE:rco_cacs_test_${suffix}>
                    -DRCO_LIBRARY_TARGET=${library}
                    -DRCO_SYSV_LIBRARY_TARGET=rco
                    -DRCO_OBJDUMP=${CMAKE_OBJDUMP}
                    -DRCO_NM=${CMAKE_NM}
                    -DRCO_EXPECT_PRESERVE_NONE=${expect_preserve_none}
                    -P
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rco_cacs_codegen_test.cmake
            )
        endfunction()

        rco_add_cacs_tests(cacs rco_cacs 0)
        rco_add_cacs_tests(cacs_preserve_none rco_cacs_preserve_none 1)

        rco_add_pool_test(
            rco_pool_test_cacs
            rco_pool_cacs
            rco_cacs
        )
        rco_add_pool_test(
            rco_pool_test_cacs_preserve_none
            rco_pool_cacs_preserve_none
            rco_cacs_preserve_none
        )

        add_test(
            NAME rco_cacs_preserve_none_rejects_normal_caller
            COMMAND
                ${CMAKE_COMMAND}
                -DRCO_NEGATIVE_NORMAL_LINK=ON
                -DRCO_NORMAL_COMPILER=${CMAKE_C_COMPILER}
                -DRCO_NORMAL_COMPILER_ARG1=${CMAKE_C_COMPILER_ARG1}
                -DRCO_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DRCO_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DRCO_COMPATIBLE_LIBRARY=$<TARGET_FILE:rco_cacs>
                -DRCO_LIBRARY=$<TARGET_FILE:rco_cacs_preserve_none>
                -DRCO_NM=${CMAKE_NM}
                -P
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/rco_cacs_codegen_test.cmake
        )
    endif()
endif()

if(RCO_BUILD_BENCHMARKS)
    add_executable(rco_bench bench/rco_bench.c)
    target_link_libraries(rco_bench PRIVATE rco)
    rco_enable_c_warnings(rco_bench)

    function(rco_add_high_concurrency_bench target library)
        add_executable(
            ${target}
            bench/rco_high_concurrency_bench.c
        )
        target_link_libraries(${target} PRIVATE ${library})
        rco_enable_c_warnings(${target})
    endfunction()

    rco_add_high_concurrency_bench(rco_high_concurrency_bench_sysv rco)

    if(RCO_BUILD_CACS)
        add_executable(rco_bench_cacs bench/rco_bench.c)
        target_link_libraries(rco_bench_cacs PRIVATE rco_cacs)
        rco_enable_c_warnings(rco_bench_cacs)

        add_executable(rco_bench_cacs_preserve_none bench/rco_bench.c)
        target_link_libraries(
            rco_bench_cacs_preserve_none
            PRIVATE rco_cacs_preserve_none
        )
        rco_enable_c_warnings(rco_bench_cacs_preserve_none)

        rco_add_high_concurrency_bench(
            rco_high_concurrency_bench_cacs
            rco_cacs
        )
        rco_add_high_concurrency_bench(
            rco_high_concurrency_bench_cacs_preserve_none
            rco_cacs_preserve_none
        )
    endif()

    if(RCO_BUILD_TESTS AND RCO_BUILD_CACS)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(
            RCO_HIGH_CONCURRENCY_TEST_COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rco_high_concurrency_bench_test.py
            --harness
            ${CMAKE_CURRENT_SOURCE_DIR}/bench/rco_high_concurrency_bench.py
            --sysv
            $<TARGET_FILE:rco_high_concurrency_bench_sysv>
            --cacs
            $<TARGET_FILE:rco_high_concurrency_bench_cacs>
            --cacs-preserve-none
            $<TARGET_FILE:rco_high_concurrency_bench_cacs_preserve_none>
        )

        foreach(
            suite
            IN ITEMS arguments oracle csv timeout integration
        )
            add_test(
                NAME rco_high_concurrency_${suite}
                COMMAND ${RCO_HIGH_CONCURRENCY_TEST_COMMAND} ${suite}
            )
        endforeach()
        set_tests_properties(
            rco_high_concurrency_arguments
            rco_high_concurrency_csv
            PROPERTIES TIMEOUT 10
        )
        set_tests_properties(
            rco_high_concurrency_oracle
            rco_high_concurrency_timeout
            PROPERTIES TIMEOUT 15
        )
        set_tests_properties(
            rco_high_concurrency_integration
            PROPERTIES TIMEOUT 30
        )
    endif()
endif()

if(RCO_BUILD_EXAMPLES)
    add_executable(rco_l4_forwarder examples/l4_forwarder.c)
    target_link_libraries(rco_l4_forwarder PRIVATE rco)
    rco_enable_c_warnings(rco_l4_forwarder)

    add_executable(rco_l4_forwarder_epoll examples/l4_forwarder_epoll.c)
    target_compile_features(rco_l4_forwarder_epoll PRIVATE c_std_11)
    rco_enable_c_warnings(rco_l4_forwarder_epoll)

    if(RCO_BUILD_CACS)
        add_executable(rco_l4_forwarder_cacs examples/l4_forwarder.c)
        target_link_libraries(rco_l4_forwarder_cacs PRIVATE rco_cacs)
        rco_enable_c_warnings(rco_l4_forwarder_cacs)

        add_executable(
            rco_l4_forwarder_cacs_preserve_none
            examples/l4_forwarder.c
        )
        target_link_libraries(
            rco_l4_forwarder_cacs_preserve_none
            PRIVATE rco_cacs_preserve_none
        )
        rco_enable_c_warnings(rco_l4_forwarder_cacs_preserve_none)
    endif()

    if(RCO_BUILD_TESTS)
        add_executable(
            rco_l4_epoll_test
            tests/l4_forwarder_epoll_test.c
        )
        target_compile_features(rco_l4_epoll_test PRIVATE c_std_11)
        rco_enable_c_warnings(rco_l4_epoll_test)
        add_test(NAME epoll_l4_state_machine COMMAND rco_l4_epoll_test)
        add_test(
            NAME l4_bench_process_resources
            COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_bench_process_resources_test.sh
        )
        add_test(
            NAME l4_bench_harness
            COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_bench_harness_test.sh
        )
        set_tests_properties(l4_bench_harness PROPERTIES TIMEOUT 30)

        add_executable(rco_l4_wait_test tests/l4_forwarder_test.c)
        target_link_libraries(rco_l4_wait_test PRIVATE rco)
        rco_enable_c_warnings(rco_l4_wait_test)
        add_test(NAME rco_l4_wait_failure COMMAND rco_l4_wait_test)

        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(
            NAME rco_l4_e2e
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_e2e.py
                $<TARGET_FILE:rco_l4_forwarder>
                coroutine-sysv
        )
        add_test(
            NAME epoll_l4_e2e
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_e2e.py
                $<TARGET_FILE:rco_l4_forwarder_epoll>
                epoll
        )
        if(RCO_BUILD_CACS)
            add_test(
                NAME cacs_l4_e2e
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_e2e.py
                    $<TARGET_FILE:rco_l4_forwarder_cacs>
                    cacs
            )
            add_test(
                NAME cacs_preserve_none_l4_e2e
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_e2e.py
                    $<TARGET_FILE:rco_l4_forwarder_cacs_preserve_none>
                    cacs-preserve-none
            )
        endif()
        add_test(
            NAME epoll_l4_rlimit
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/l4_epoll_resource_limit.py
                $<TARGET_FILE:rco_l4_forwarder_epoll>
        )
        set_tests_properties(rco_l4_e2e epoll_l4_e2e PROPERTIES TIMEOUT 15)
        if(RCO_BUILD_CACS)
            set_tests_properties(
                cacs_l4_e2e
                cacs_preserve_none_l4_e2e
                PROPERTIES TIMEOUT 15
            )
        endif()
        set_tests_properties(
            epoll_l4_rlimit
            PROPERTIES
                SKIP_RETURN_CODE 77
                TIMEOUT 10
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS rco ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/rco.h include/rco_local.h include/rco_pool.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
