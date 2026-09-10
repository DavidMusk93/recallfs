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

    .globl rco_context_switch
    .type rco_context_switch,@function
rco_context_switch:
    .cfi_startproc
    endbr64

    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    stmxcsr 56(%rdi)
    fnstcw 60(%rdi)

    movq 8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    ldmxcsr 56(%rsi)
    fldcw 60(%rsi)
    movq 0(%rsi), %rsp

    cld
    ret
    .cfi_endproc
    .size rco_context_switch, .-rco_context_switch

    .section .note.GNU-stack,"",@progbits
