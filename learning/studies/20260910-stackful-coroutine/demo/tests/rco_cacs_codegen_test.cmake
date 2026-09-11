if(DEFINED RCO_NEGATIVE_NORMAL_LINK AND RCO_NEGATIVE_NORMAL_LINK)
    foreach(required IN ITEMS RCO_NORMAL_COMPILER RCO_SOURCE_DIR
                              RCO_BINARY_DIR RCO_COMPATIBLE_LIBRARY
                              RCO_LIBRARY RCO_NM)
        if(NOT DEFINED ${required})
            message(FATAL_ERROR "missing ${required}")
        endif()
    endforeach()

    set(normal_compiler_command "${RCO_NORMAL_COMPILER}")
    if(DEFINED RCO_NORMAL_COMPILER_ARG1 AND
       NOT RCO_NORMAL_COMPILER_ARG1 STREQUAL "")
        string(STRIP "${RCO_NORMAL_COMPILER_ARG1}"
               normal_compiler_arg1)
        list(APPEND normal_compiler_command "${normal_compiler_arg1}")
    endif()

    set(normal_object "${RCO_BINARY_DIR}/rco-cacs-normal-caller.o")
    execute_process(
        COMMAND
            ${normal_compiler_command}
            -DRCO_EXPECT_PRESERVE_NONE=0
            -DRCO_NORMAL_CALLER_LINK_PROBE=1
            -std=c11
            -O2
            -I${RCO_SOURCE_DIR}/include
            -c
            ${RCO_SOURCE_DIR}/tests/rco_cacs_codegen_probe.c
            -o
            ${normal_object}
        RESULT_VARIABLE normal_compile_result
        OUTPUT_VARIABLE normal_compile_stdout
        ERROR_VARIABLE normal_compile_stderr
    )
    if(NOT normal_compile_result EQUAL 0)
        message(FATAL_ERROR
                "failed to compile normal-ABI caller:\n"
                "${normal_compile_stdout}${normal_compile_stderr}")
    endif()

    execute_process(
        COMMAND "${RCO_NM}" -u "${normal_object}"
        RESULT_VARIABLE normal_nm_result
        OUTPUT_VARIABLE normal_undefined_symbols
        ERROR_VARIABLE normal_nm_error
    )
    if(NOT normal_nm_result EQUAL 0)
        message(FATAL_ERROR "nm failed for normal caller:\n${normal_nm_error}")
    endif()
    foreach(symbol IN ITEMS rco_yield rco_wait_fd rco_sleep_ms
                            rco_preempt_point rco_preempt_disable
                            rco_preempt_enable rco_preempt_pending)
        if(NOT normal_undefined_symbols MATCHES
           "[ \t]${symbol}(\n|$)")
            message(FATAL_ERROR
                    "normal caller does not reference ${symbol}")
        endif()
    endforeach()

    set(compatible_executable
        "${RCO_BINARY_DIR}/rco-cacs-normal-caller-control")
    execute_process(
        COMMAND
            ${normal_compiler_command}
            -flto
            ${normal_object}
            "${RCO_COMPATIBLE_LIBRARY}"
            -o
            ${compatible_executable}
        RESULT_VARIABLE compatible_link_result
        OUTPUT_VARIABLE compatible_link_stdout
        ERROR_VARIABLE compatible_link_stderr
    )
    if(NOT compatible_link_result EQUAL 0)
        message(FATAL_ERROR
                "normal caller failed to link against the compatible "
                "CACS archive:\n"
                "${compatible_link_stdout}${compatible_link_stderr}")
    endif()

    set(normal_executable
        "${RCO_BINARY_DIR}/rco-cacs-normal-caller-must-not-link")
    execute_process(
        COMMAND
            ${normal_compiler_command}
            -flto
            ${normal_object}
            "${RCO_LIBRARY}"
            -o
            ${normal_executable}
        RESULT_VARIABLE normal_link_result
        OUTPUT_VARIABLE normal_link_stdout
        ERROR_VARIABLE normal_link_stderr
    )
    if(normal_link_result EQUAL 0)
        message(FATAL_ERROR
                "normal-ABI caller linked against preserve-none archive")
    endif()
    foreach(symbol IN ITEMS rco_yield rco_wait_fd rco_sleep_ms
                            rco_preempt_point rco_preempt_enable)
        if(NOT normal_link_stderr MATCHES "${symbol}")
            message(FATAL_ERROR
                    "negative link did not reject missing ${symbol}:\n"
                    "${normal_link_stdout}${normal_link_stderr}")
        endif()
    endforeach()

    message(STATUS
            "preserve-none archive rejected the normal-ABI caller")
    return()
endif()

foreach(required IN ITEMS RCO_COMPILER RCO_SOURCE_DIR RCO_BINARY_DIR
                          RCO_EXECUTABLE RCO_LIBRARY_TARGET
                          RCO_SYSV_LIBRARY_TARGET RCO_OBJDUMP RCO_NM
                          RCO_EXPECT_PRESERVE_NONE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "missing ${required}")
    endif()
endforeach()

set(compiler_command "${RCO_COMPILER}")
if(DEFINED RCO_COMPILER_ARG1 AND NOT RCO_COMPILER_ARG1 STREQUAL "")
    string(STRIP "${RCO_COMPILER_ARG1}" compiler_arg1)
    list(APPEND compiler_command "${compiler_arg1}")
endif()

set(definitions -DRCO_CACS=1
                -DRCO_EXPECT_PRESERVE_NONE=${RCO_EXPECT_PRESERVE_NONE})
if(RCO_EXPECT_PRESERVE_NONE)
    list(APPEND definitions -DRCO_CACS_PRESERVE_NONE=1)
endif()

set(runtime_ir "${RCO_BINARY_DIR}/${RCO_LIBRARY_TARGET}-runtime.ll")
execute_process(
    COMMAND
        ${compiler_command}
        ${definitions}
        -std=c11
        -O2
        -march=native
        -fno-omit-frame-pointer
        -mno-red-zone
        -fcf-protection=branch
        -I${RCO_SOURCE_DIR}/include
        -I${RCO_SOURCE_DIR}/src
        -S
        -emit-llvm
        ${RCO_SOURCE_DIR}/src/rco.c
        -o
        ${runtime_ir}
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
            "failed to emit CACS runtime IR:\n${compile_stdout}${compile_stderr}")
endif()

file(READ "${runtime_ir}" runtime_llvm)
if(runtime_llvm MATCHES "@rco_context_switch")
    message(FATAL_ERROR "CACS runtime still references the full SysV switch")
endif()
foreach(clobber IN ITEMS rax rbx rcx rdx r8 r9 r10 r11 r12 r13 r14 r15
                         xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 xmm6 xmm7 xmm8 xmm9
                         xmm10 xmm11 xmm12 xmm13 xmm14 xmm15 cc memory)
    if(NOT runtime_llvm MATCHES "~\\{${clobber}\\}")
        message(FATAL_ERROR "CACS inline switch omits ${clobber} clobber")
    endif()
endforeach()
if(runtime_llvm MATCHES "avx512f")
    foreach(clobber IN ITEMS xmm16 xmm17 xmm18 xmm19 xmm20 xmm21 xmm22 xmm23
                             xmm24 xmm25 xmm26 xmm27 xmm28 xmm29 xmm30 xmm31
                             k0 k1 k2 k3 k4 k5 k6 k7)
        if(NOT runtime_llvm MATCHES "~\\{${clobber}\\}")
            message(FATAL_ERROR
                    "native AVX-512 switch omits ${clobber} clobber")
        endif()
    endforeach()
endif()
foreach(operand IN ITEMS di si)
    if(NOT runtime_llvm MATCHES "[+=]\\{${operand}\\}")
        message(FATAL_ERROR
                "CACS inline switch does not expose ${operand} as read-write")
    endif()
endforeach()

foreach(instruction IN ITEMS stmxcsr fnstcw ldmxcsr fldcw endbr64)
    if(NOT runtime_llvm MATCHES "${instruction}")
        message(FATAL_ERROR
                "CACS inline switch omits ${instruction} from generated IR")
    endif()
endforeach()

set(probe_ir "${RCO_BINARY_DIR}/${RCO_LIBRARY_TARGET}-caller.ll")
execute_process(
    COMMAND
        ${compiler_command}
        ${definitions}
        -std=c11
        -O2
        -I${RCO_SOURCE_DIR}/include
        -I${RCO_SOURCE_DIR}/src
        -S
        -emit-llvm
        ${RCO_SOURCE_DIR}/tests/rco_cacs_codegen_probe.c
        -o
        ${probe_ir}
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
)
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR
            "failed to emit CACS caller IR:\n${probe_stdout}${probe_stderr}")
endif()

file(READ "${probe_ir}" probe_llvm)
if(RCO_EXPECT_PRESERVE_NONE)
    set(suspension_suffix "_cacs_preserve_none")
else()
    set(suspension_suffix "")
endif()
foreach(api IN ITEMS rco_yield rco_wait_fd rco_sleep_ms
                     rco_preempt_point rco_preempt_enable)
    set(symbol "${api}${suspension_suffix}")
    if(RCO_EXPECT_PRESERVE_NONE)
        if(NOT runtime_llvm MATCHES
           "define[^\n]*preserve_nonecc[^\n]*@${symbol}\\(")
            message(FATAL_ERROR
                    "${symbol} definition does not use preserve_none")
        endif()
    elseif(runtime_llvm MATCHES
           "define[^\n]*preserve_nonecc[^\n]*@${symbol}\\(")
        message(FATAL_ERROR "${symbol} unexpectedly uses preserve_none")
    endif()
endforeach()
if(RCO_EXPECT_PRESERVE_NONE)
    foreach(api IN ITEMS rco_yield rco_preempt_point rco_preempt_enable)
        if(NOT probe_llvm MATCHES
           "call preserve_nonecc i32 @${api}_cacs_preserve_none\\(")
            message(FATAL_ERROR
                    "the external ${api} caller does not use preserve_none")
        endif()
    endforeach()
elseif(runtime_llvm MATCHES "preserve_nonecc" OR
       probe_llvm MATCHES "preserve_nonecc")
    message(FATAL_ERROR "plain CACS unexpectedly uses preserve_none")
endif()

set(symbol_object
    "${RCO_BINARY_DIR}/${RCO_LIBRARY_TARGET}-runtime-symbols.o")
execute_process(
    COMMAND
        ${compiler_command}
        ${definitions}
        -std=c11
        -O2
        -fno-lto
        -mno-red-zone
        -fcf-protection=branch
        -I${RCO_SOURCE_DIR}/include
        -I${RCO_SOURCE_DIR}/src
        -c
        ${RCO_SOURCE_DIR}/src/rco.c
        -o
        ${symbol_object}
    RESULT_VARIABLE symbol_compile_result
    OUTPUT_VARIABLE symbol_compile_stdout
    ERROR_VARIABLE symbol_compile_stderr
)
if(NOT symbol_compile_result EQUAL 0)
    message(FATAL_ERROR
            "failed to compile runtime symbol probe:\n"
            "${symbol_compile_stdout}${symbol_compile_stderr}")
endif()

execute_process(
    COMMAND "${RCO_NM}" -g --defined-only "${symbol_object}"
    RESULT_VARIABLE symbol_nm_result
    OUTPUT_VARIABLE runtime_symbols
    ERROR_VARIABLE symbol_nm_error
)
if(NOT symbol_nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed for runtime symbol probe:\n"
                        "${symbol_nm_error}")
endif()
foreach(api IN ITEMS rco_yield rco_wait_fd rco_sleep_ms
                     rco_preempt_point rco_preempt_enable)
    set(symbol "${api}${suspension_suffix}")
    if(NOT runtime_symbols MATCHES "[ \t]${symbol}(\n|$)")
        message(FATAL_ERROR "CACS object does not define ${symbol}")
    endif()
    if(RCO_EXPECT_PRESERVE_NONE AND
       runtime_symbols MATCHES "[ \t]${api}(\n|$)")
        message(FATAL_ERROR
                "preserve-none object exposes normal-ABI ${api}")
    endif()
endforeach()

execute_process(
    COMMAND "${RCO_NM}" -A "${RCO_EXECUTABLE}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed:\n${nm_error}")
endif()
if(nm_output MATCHES "[ \t]rco_context_switch(\n|$)")
    message(FATAL_ERROR "CACS library exports the full SysV switch")
endif()

set(raw_probe_object
    "${RCO_BINARY_DIR}/${RCO_LIBRARY_TARGET}-raw-switch-probe.o")
execute_process(
    COMMAND
        ${compiler_command}
        ${definitions}
        -DRCO_RAW_SWITCH_PROBE_ONLY=1
        -std=c11
        -O2
        -march=native
        -fno-omit-frame-pointer
        -mno-red-zone
        -fcf-protection=branch
        -I${RCO_SOURCE_DIR}/include
        -I${RCO_SOURCE_DIR}/src
        -c
        ${RCO_SOURCE_DIR}/tests/rco_cacs_codegen_probe.c
        -o
        ${raw_probe_object}
    RESULT_VARIABLE raw_probe_result
    OUTPUT_VARIABLE raw_probe_stdout
    ERROR_VARIABLE raw_probe_stderr
)
if(NOT raw_probe_result EQUAL 0)
    message(FATAL_ERROR
            "failed to compile raw switch probe:\n"
            "${raw_probe_stdout}${raw_probe_stderr}")
endif()

execute_process(
    COMMAND
        "${RCO_OBJDUMP}"
        -dr
        "${raw_probe_object}"
    RESULT_VARIABLE raw_objdump_result
    OUTPUT_VARIABLE raw_switch_disassembly
    ERROR_VARIABLE raw_objdump_error
)
if(NOT raw_objdump_result EQUAL 0)
    message(FATAL_ERROR "raw probe objdump failed:\n${raw_objdump_error}")
endif()
if(NOT raw_switch_disassembly MATCHES
   "<rco_cacs_raw_switch_probe>:")
    message(FATAL_ERROR "raw switch probe function is missing")
endif()
foreach(instruction IN ITEMS stmxcsr fnstcw ldmxcsr fldcw endbr64)
    if(NOT raw_switch_disassembly MATCHES "${instruction}")
        message(FATAL_ERROR
                "raw CACS switch machine code omits ${instruction}")
    endif()
endforeach()

set(compile_commands_path "${RCO_BINARY_DIR}/compile_commands.json")
if(NOT EXISTS "${compile_commands_path}")
    message(FATAL_ERROR "compile_commands.json is required for ABI checks")
endif()
file(READ "${compile_commands_path}" compile_commands)
foreach(target IN ITEMS ${RCO_LIBRARY_TARGET} ${RCO_SYSV_LIBRARY_TARGET})
    string(REGEX MATCH
           "\\{[^{}]*CMakeFiles/${target}\\.dir/src/rco\\.c\\.o[^{}]*\\}"
           runtime_compile_command
           "${compile_commands}")
    if(runtime_compile_command STREQUAL "")
        message(FATAL_ERROR
                "could not find ${target} rco.c compile command")
    endif()
    foreach(option IN ITEMS -mno-red-zone -fcf-protection=branch)
        if(NOT runtime_compile_command MATCHES "${option}")
            message(FATAL_ERROR
                    "${target} is missing required option ${option}")
        endif()
    endforeach()
endforeach()

message(STATUS "${RCO_LIBRARY_TARGET} CACS codegen contract passed")
