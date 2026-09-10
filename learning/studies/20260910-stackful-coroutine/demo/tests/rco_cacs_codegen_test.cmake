foreach(required IN ITEMS RCO_COMPILER RCO_SOURCE_DIR RCO_BINARY_DIR
                          RCO_LIBRARY RCO_LIBRARY_TARGET RCO_OBJDUMP RCO_NM
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
    foreach(symbol IN ITEMS rco_yield rco_wait_fd rco_sleep_ms)
        if(NOT runtime_llvm MATCHES
           "define[^\n]*preserve_nonecc[^\n]*@${symbol}\\(")
            message(FATAL_ERROR
                    "${symbol} definition does not use preserve_none")
        endif()
    endforeach()
    if(NOT probe_llvm MATCHES
       "call preserve_nonecc i32 @rco_yield\\(")
        message(FATAL_ERROR
                "the external rco_yield caller does not use preserve_none")
    endif()
elseif(runtime_llvm MATCHES "preserve_nonecc" OR
       probe_llvm MATCHES "preserve_nonecc")
    message(FATAL_ERROR "plain CACS unexpectedly uses preserve_none")
endif()

execute_process(
    COMMAND "${RCO_NM}" -A "${RCO_LIBRARY}"
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

execute_process(
    COMMAND "${RCO_OBJDUMP}" -dr "${RCO_LIBRARY}"
    RESULT_VARIABLE objdump_result
    OUTPUT_VARIABLE disassembly
    ERROR_VARIABLE objdump_error
)
if(NOT objdump_result EQUAL 0)
    message(FATAL_ERROR "objdump failed:\n${objdump_error}")
endif()
foreach(instruction IN ITEMS stmxcsr fnstcw ldmxcsr fldcw endbr64)
    if(NOT disassembly MATCHES "${instruction}")
        message(FATAL_ERROR
                "CACS machine code omits ${instruction}")
    endif()
endforeach()

set(compile_commands_path "${RCO_BINARY_DIR}/compile_commands.json")
if(NOT EXISTS "${compile_commands_path}")
    message(FATAL_ERROR "compile_commands.json is required for ABI checks")
endif()
file(READ "${compile_commands_path}" compile_commands)
string(REGEX MATCH
       "\\{[^{}]*CMakeFiles/${RCO_LIBRARY_TARGET}\\.dir/src/rco\\.c\\.o[^{}]*\\}"
       runtime_compile_command
       "${compile_commands}")
if(runtime_compile_command STREQUAL "")
    message(FATAL_ERROR
            "could not find ${RCO_LIBRARY_TARGET} rco.c compile command")
endif()
foreach(option IN ITEMS -mno-red-zone -fcf-protection=branch)
    if(NOT runtime_compile_command MATCHES "${option}")
        message(FATAL_ERROR
                "${RCO_LIBRARY_TARGET} is missing required option ${option}")
    endif()
endforeach()

message(STATUS "${RCO_LIBRARY_TARGET} CACS codegen contract passed")
