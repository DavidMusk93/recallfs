#include "rco.h"
#include "rco_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(RCO_CACS)
#error "the CACS ABI test must be compiled with RCO_CACS"
#endif

#if !defined(RCO_EXPECT_PRESERVE_NONE)
#error "the CACS ABI test requires an expected preserve_none mode"
#elif RCO_EXPECT_PRESERVE_NONE && !defined(RCO_CACS_PRESERVE_NONE)
#error "the preserve_none CACS target did not propagate its public definition"
#elif !RCO_EXPECT_PRESERVE_NONE && defined(RCO_CACS_PRESERVE_NONE)
#error "the plain CACS target unexpectedly enabled preserve_none"
#endif

_Static_assert(sizeof(struct rco_context) == 64, "matched CACS context");
_Static_assert(RCO_CONTEXT_BOOTSTRAP_WORDS == 2, "CACS bootstrap words");

static void check(int condition, const char *expression, int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, __FILE__,
                line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static uint32_t read_mxcsr(void)
{
    uint32_t value;
    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

static void write_mxcsr(uint32_t value)
{
    __asm__ volatile("ldmxcsr %0" : : "m"(value));
}

static uint16_t read_x87_control(void)
{
    uint16_t value;
    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
}

static void write_x87_control(uint16_t value)
{
    __asm__ volatile("fldcw %0" : : "m"(value));
}

struct fp_case {
    uint32_t mxcsr;
    uint16_t x87_control;
    uint32_t observed_mxcsr;
    uint16_t observed_x87_control;
};

static int fp_worker(void *argument)
{
    struct fp_case *test_case = argument;
    write_mxcsr(test_case->mxcsr);
    write_x87_control(test_case->x87_control);
    CHECK(rco_yield() == 0);
    test_case->observed_mxcsr = read_mxcsr();
    test_case->observed_x87_control = read_x87_control();
    return 0;
}

struct bootstrap_case {
    uintptr_t entry_rsp_mod_16;
};

__attribute__((naked, noinline)) static int
bootstrap_worker(void *argument __attribute__((unused)))
{
    __asm__ volatile(
        "movq %rsp, %rax\n\t"
        "andq $15, %rax\n\t"
        "movq %rax, (%rdi)\n\t"
        "xorl %eax, %eax\n\t"
        "retq");
}

int main(void)
{
    uint32_t original_mxcsr = read_mxcsr();
    uint16_t original_x87_control = read_x87_control();
    struct fp_case first = {
        .mxcsr = (original_mxcsr & ~0x6000u) | 0x2000u,
        .x87_control =
            (uint16_t)((original_x87_control & ~0x0c00u) | 0x0800u),
    };
    struct fp_case second = {
        .mxcsr = (original_mxcsr & ~0x6000u) | 0x4000u,
        .x87_control =
            (uint16_t)((original_x87_control & ~0x0c00u) | 0x0400u),
    };
    struct bootstrap_case bootstrap = {0};

    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, fp_worker, &first, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, fp_worker, &second, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, bootstrap_worker, &bootstrap, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    CHECK(first.observed_mxcsr == first.mxcsr);
    CHECK(first.observed_x87_control == first.x87_control);
    CHECK(second.observed_mxcsr == second.mxcsr);
    CHECK(second.observed_x87_control == second.x87_control);
    CHECK(read_mxcsr() == original_mxcsr);
    CHECK(read_x87_control() == original_x87_control);
    CHECK(bootstrap.entry_rsp_mod_16 == 8);
    CHECK(rco_runtime_destroy(runtime) == 0);

    puts("rco CACS ABI tests passed");
    return EXIT_SUCCESS;
}
