#ifndef RCO_INTERNAL_H
#define RCO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

struct rco_context {
    uintptr_t rsp;
    uintptr_t rbx;
    uintptr_t rbp;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
    uint32_t mxcsr;
    uint16_t x87_control;
    uint16_t reserved;
};

_Static_assert(offsetof(struct rco_context, rsp) == 0, "rsp ABI offset");
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

void rco_context_capture_fp(struct rco_context *context);
void rco_context_switch(struct rco_context *from,
                        const struct rco_context *to);

#endif
