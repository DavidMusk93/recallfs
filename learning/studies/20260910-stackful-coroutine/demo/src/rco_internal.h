#ifndef RCO_INTERNAL_H
#define RCO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define RCO_CONTEXT_BOOTSTRAP_WORDS 2

struct rco_context {
    uintptr_t rsp;
#if defined(RCO_CACS)
    uintptr_t rbp;
    uintptr_t reserved_gprs[5];
    uint32_t mxcsr;
    uint16_t x87_control;
    uint16_t reserved;
#else
    uintptr_t rbx;
    uintptr_t rbp;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
    uint32_t mxcsr;
    uint16_t x87_control;
    uint16_t reserved;
#endif
};

_Static_assert(offsetof(struct rco_context, rsp) == 0, "rsp ABI offset");
#if defined(RCO_CACS)
_Static_assert(offsetof(struct rco_context, rbp) == 8, "rbp ABI offset");
_Static_assert(offsetof(struct rco_context, mxcsr) == 56, "mxcsr ABI offset");
_Static_assert(offsetof(struct rco_context, x87_control) == 60,
               "x87 control ABI offset");
_Static_assert(sizeof(struct rco_context) == 64, "CACS context ABI size");
#else
_Static_assert(offsetof(struct rco_context, rbx) == 8, "rbx ABI offset");
_Static_assert(offsetof(struct rco_context, rbp) == 16, "rbp ABI offset");
_Static_assert(offsetof(struct rco_context, r12) == 24, "r12 ABI offset");
_Static_assert(offsetof(struct rco_context, r13) == 32, "r13 ABI offset");
_Static_assert(offsetof(struct rco_context, r14) == 40, "r14 ABI offset");
_Static_assert(offsetof(struct rco_context, r15) == 48, "r15 ABI offset");
_Static_assert(offsetof(struct rco_context, mxcsr) == 56, "mxcsr ABI offset");
_Static_assert(offsetof(struct rco_context, x87_control) == 60,
               "x87 control ABI offset");
_Static_assert(sizeof(struct rco_context) == 64, "context ABI size");
#endif

void rco_context_capture_fp(struct rco_context *context);

#if defined(RCO_CACS)
#if !defined(__clang__) || __clang_major__ < 21
#error "RCO_CACS requires Clang 21 or newer"
#endif

#if defined(__AVX512F__)
#define RCO_CACS_HIGH_XMM_CLOBBERS                                      \
    , "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", \
        "xmm23", "xmm24", "xmm25", "xmm26", "xmm27", "xmm28",         \
        "xmm29", "xmm30", "xmm31"
#else
#define RCO_CACS_HIGH_XMM_CLOBBERS
#endif

/*
 * rdi/rsi are read-write operands, so Clang also treats them as clobbered.
 * rsp/rbp are omitted from the clobber list because each resumed context gets
 * its own saved values back before control reaches the local label.
 */
static __attribute__((always_inline)) inline void
rco_context_switch(struct rco_context *from, const struct rco_context *to)
{
    __asm__ volatile(
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "movq %%rsp, 0(%0)\n\t"
        "movq %%rbp, 8(%0)\n\t"
        "stmxcsr 56(%0)\n\t"
        "fnstcw 60(%0)\n\t"
        "ldmxcsr 56(%1)\n\t"
        "fldcw 60(%1)\n\t"
        "movq 8(%1), %%rbp\n\t"
        "movq 0(%1), %%rsp\n\t"
        "popq %%rax\n\t"
        "cld\n\t"
        "jmpq *%%rax\n\t"
        "1: endbr64"
        : "+D"(from), "+S"(to)
        :
        : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12",
          "r13", "r14", "r15", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
          "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
          "xmm12", "xmm13", "xmm14", "xmm15"
              RCO_CACS_HIGH_XMM_CLOBBERS,
          "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",
          "st(7)", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6",
          "mm7", "cc", "memory");
}
#else
void rco_context_switch(struct rco_context *from,
                        const struct rco_context *to);
#endif

#endif
