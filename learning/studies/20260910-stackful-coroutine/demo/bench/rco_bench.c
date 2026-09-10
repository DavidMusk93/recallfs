#define _GNU_SOURCE

#include "rco.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile uint64_t benchmark_sink;

struct worker_case {
    uint64_t iterations;
    uint64_t checksum;
};

static uint64_t now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int yield_worker(void *argument)
{
    struct worker_case *test_case = argument;
    uint64_t checksum = 0;
    for (uint64_t index = 0; index < test_case->iterations; ++index) {
        checksum += (index ^ rco_current_id()) + 1;
        if (rco_yield() != 0) {
            return -ECANCELED;
        }
    }
    test_case->checksum = checksum;
    return 0;
}

static uint64_t expected_worker_checksum(uint64_t iterations, uint64_t task_id)
{
    uint64_t checksum = 0;
    for (uint64_t index = 0; index < iterations; ++index) {
        checksum += (index ^ task_id) + 1;
    }
    return checksum;
}

static double run_coroutine_sample(uint64_t iterations)
{
    struct rco_config config = {
        .default_stack_size = 64 * 1024,
        .max_coroutines = 2,
        .max_fds = 16,
        .stack_cache_bytes = 0,
    };
    struct rco_runtime *runtime = NULL;
    struct worker_case first = {.iterations = iterations};
    struct worker_case second = {.iterations = iterations};
    uint64_t first_id = 0;
    uint64_t second_id = 0;
    if (rco_runtime_create(&config, &runtime) != 0 ||
        rco_spawn(runtime, 0, yield_worker, &first, &first_id) != 0 ||
        rco_spawn(runtime, 0, yield_worker, &second, &second_id) != 0) {
        fputs("failed to initialize coroutine benchmark\n", stderr);
        exit(EXIT_FAILURE);
    }

    uint64_t start = now_ns();
    int result = rco_runtime_run(runtime);
    uint64_t elapsed = now_ns() - start;
    struct rco_stats stats;
    if (rco_runtime_get_stats(runtime, &stats) != 0 || result != 0 ||
        first.checksum != expected_worker_checksum(iterations, first_id) ||
        second.checksum != expected_worker_checksum(iterations, second_id) ||
        stats.spawned != 2 || stats.completed != 2 || stats.active != 0 ||
        stats.context_switches != 4 * iterations + 4) {
        fputs("coroutine benchmark validation failed\n", stderr);
        exit(EXIT_FAILURE);
    }
    benchmark_sink ^= first.checksum ^ second.checksum;
    if (rco_runtime_destroy(runtime) != 0) {
        fputs("failed to destroy coroutine runtime\n", stderr);
        exit(EXIT_FAILURE);
    }
    return (double)elapsed / (double)(2 * iterations);
}

__attribute__((noinline)) static uint64_t call_step(uint64_t value)
{
    __asm__ volatile("" : "+r"(value) : : "memory");
    return value * 6364136223846793005ULL + 1442695040888963407ULL;
}

static double run_call_sample(uint64_t iterations)
{
    uint64_t value = 1;
    uint64_t start = now_ns();
    for (uint64_t index = 0; index < 2 * iterations; ++index) {
        value = call_step(value ^ index);
    }
    uint64_t elapsed = now_ns() - start;
    benchmark_sink ^= value;
    return (double)elapsed / (double)(2 * iterations);
}

static double run_sched_yield_sample(uint64_t iterations)
{
    uint64_t checksum = 0;
    uint64_t start = now_ns();
    for (uint64_t index = 0; index < 2 * iterations; ++index) {
        if (sched_yield() != 0) {
            perror("sched_yield");
            exit(EXIT_FAILURE);
        }
        checksum += index + 1;
    }
    uint64_t elapsed = now_ns() - start;
    benchmark_sink ^= checksum;
    return (double)elapsed / (double)(2 * iterations);
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static uint64_t parse_u64(const char *text, const char *name)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

int main(int argc, char **argv)
{
    uint64_t iterations = 1000000;
    size_t samples = 11;
    if (argc > 1) {
        iterations = parse_u64(argv[1], "iterations");
    }
    if (argc > 2) {
        uint64_t parsed = parse_u64(argv[2], "samples");
        if (parsed > 101) {
            fputs("samples must be <= 101\n", stderr);
            return EXIT_FAILURE;
        }
        samples = (size_t)parsed;
    }
    if (iterations > (UINT64_MAX - 4) / 4) {
        fputs("iterations are too large\n", stderr);
        return EXIT_FAILURE;
    }

    double *coroutine = calloc(samples, sizeof(*coroutine));
    double *call = calloc(samples, sizeof(*call));
    double *kernel_yield = calloc(samples, sizeof(*kernel_yield));
    if (coroutine == NULL || call == NULL || kernel_yield == NULL) {
        fputs("sample allocation failed\n", stderr);
        return EXIT_FAILURE;
    }

    (void)run_coroutine_sample(10000);
    (void)run_call_sample(10000);
    (void)run_sched_yield_sample(1000);
    for (size_t sample = 0; sample < samples; ++sample) {
        coroutine[sample] = run_coroutine_sample(iterations);
        call[sample] = run_call_sample(iterations);
        kernel_yield[sample] = run_sched_yield_sample(iterations);
    }
    qsort(coroutine, samples, sizeof(*coroutine), compare_double);
    qsort(call, samples, sizeof(*call), compare_double);
    qsort(kernel_yield, samples, sizeof(*kernel_yield), compare_double);

    size_t median = samples / 2;
    puts("operation,median_ns_per_operation");
    printf("rco_yield,%.3f\n", coroutine[median]);
    printf("function_call,%.3f\n", call[median]);
    printf("sched_yield,%.3f\n", kernel_yield[median]);
    printf("sink=%" PRIu64 "\n", benchmark_sink);

    free(kernel_yield);
    free(call);
    free(coroutine);
    return benchmark_sink == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
