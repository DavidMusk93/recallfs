    .text

    .globl rco_context_capture_fp
    .type rco_context_capture_fp,@function
rco_context_capture_fp:
    .cfi_startproc
    endbr64
    stmxcsr 56(%rdi)
    fnstcw 60(%rdi)
    ret
    .cfi_endproc
    .size rco_context_capture_fp, .-rco_context_capture_fp

    .section .note.GNU-stack,"",@progbits
