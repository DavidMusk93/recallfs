#define _POSIX_C_SOURCE 200809L

#include "fast_time.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__clang__)
#define NO_VECTOR _Pragma("clang loop vectorize(disable)")
#define NOINLINE_USED __attribute__((noinline, used))
#elif defined(__GNUC__)
#define NO_VECTOR
#define NOINLINE_USED                                                     \
    __attribute__((noinline, used, optimize("no-tree-vectorize")))
#else
#define NO_VECTOR
#define NOINLINE_USED
#endif

#if defined(__GNUC__) || defined(__clang__)
#define KEEP_HMS_FIELDS(value)                                                  \
    __asm__ volatile(""                                                        \
                     : "+r"((value).hour), "+r"((value).minute),              \
                       "+r"((value).second))
#else
#define KEEP_HMS_FIELDS(value) ((void)(value))
#endif

enum {
    DEFAULT_COUNT = 8192,
    DEFAULT_ROUNDS = 128,
    DEFAULT_SAMPLES = 15,
};

static volatile uint64_t benchmark_sink;

typedef uint64_t (*benchmark_loop)(const uint32_t *, size_t, uint32_t);

typedef struct {
    const char *name;
    hms_converter converter;
    benchmark_loop latency;
    benchmark_loop throughput;
} benchmark_case;

static uint32_t xorshift32(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static uint64_t pack_hms(hms_time value) {
    return ((uint64_t)value.hour << 32) | ((uint64_t)value.minute << 16) |
           value.second;
}

#define DEFINE_BENCHMARK_LOOPS(label, converter)                                \
    NOINLINE_USED static uint64_t label##_latency(                              \
        const uint32_t *input, size_t count, uint32_t rounds) {                 \
        uint32_t carry = 0;                                                     \
        for (uint32_t round = 0; round < rounds; ++round) {                     \
            NO_VECTOR                                                          \
            for (size_t index = 0; index < count; ++index) {                    \
                uint32_t value = input[index] ^ carry;                          \
                hms_time result = converter(value);                             \
                KEEP_HMS_FIELDS(result);                                        \
                carry = result.hour ^ result.minute ^ result.second;            \
            }                                                                   \
        }                                                                       \
        return carry;                                                           \
    }                                                                           \
                                                                                \
    NOINLINE_USED static uint64_t label##_throughput(                           \
        const uint32_t *input, size_t count, uint32_t rounds) {                 \
        uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;                               \
        uint64_t a4 = 0, a5 = 0, a6 = 0, a7 = 0;                               \
        for (uint32_t round = 0; round < rounds; ++round) {                     \
            NO_VECTOR                                                          \
            for (size_t index = 0; index < count; index += 8) {                 \
                a0 += pack_hms(converter(input[index]));                        \
                a1 += pack_hms(converter(input[index + 1]));                    \
                a2 += pack_hms(converter(input[index + 2]));                    \
                a3 += pack_hms(converter(input[index + 3]));                    \
                a4 += pack_hms(converter(input[index + 4]));                    \
                a5 += pack_hms(converter(input[index + 5]));                    \
                a6 += pack_hms(converter(input[index + 6]));                    \
                a7 += pack_hms(converter(input[index + 7]));                    \
            }                                                                   \
        }                                                                       \
        return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;                         \
    }

DEFINE_BENCHMARK_LOOPS(traditional, hms_traditional)
DEFINE_BENCHMARK_LOOPS(parallel_div, hms_parallel_div)
DEFINE_BENCHMARK_LOOPS(parallel_fixed, hms_parallel_fixed)
DEFINE_BENCHMARK_LOOPS(hi_low, hms_hi_low)
DEFINE_BENCHMARK_LOOPS(base64, hms_base64)

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t lhs = *(const uint64_t *)left;
    uint64_t rhs = *(const uint64_t *)right;
    return (lhs > rhs) - (lhs < rhs);
}

static uint32_t parse_positive_u32(const char *flag, const char *text) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > UINT32_MAX) {
        fprintf(stderr, "invalid %s value: %s\n", flag, text);
        exit(2);
    }
    return (uint32_t)value;
}

static hms_time reference_hms(uint32_t day_second) {
    hms_time result = {
        .hour = day_second / 3600U,
        .minute = (day_second % 3600U) / 60U,
        .second = day_second % 60U,
    };
    return result;
}

static int equal_hms(hms_time left, hms_time right) {
    return left.hour == right.hour && left.minute == right.minute &&
           left.second == right.second;
}

static int validate_inputs(const benchmark_case *cases, size_t case_count,
                           const uint32_t *input, size_t count) {
    for (size_t case_index = 0; case_index < case_count; ++case_index) {
        for (size_t input_index = 0; input_index < count; ++input_index) {
            hms_time expected = reference_hms(input[input_index]);
            hms_time actual = cases[case_index].converter(input[input_index]);
            if (!equal_hms(expected, actual)) {
                fprintf(stderr, "validation failed for %s at input %" PRIu32
                                "\n",
                        cases[case_index].name, input[input_index]);
                return 0;
            }
        }
    }
    return 1;
}

static void run_case(const char *mode, const char *name, benchmark_loop loop,
                     const uint32_t *input, size_t count, uint32_t rounds,
                     uint32_t samples) {
    uint64_t *elapsed = malloc((size_t)samples * sizeof(*elapsed));
    if (elapsed == NULL) {
        fputs("failed to allocate samples\n", stderr);
        exit(2);
    }

    benchmark_sink += loop(input, count, 1) + UINT64_C(0x9e3779b97f4a7c15);
    for (uint32_t sample = 0; sample < samples; ++sample) {
        uint64_t begin = monotonic_nanoseconds();
        uint64_t checksum = loop(input, count, rounds);
        uint64_t end = monotonic_nanoseconds();
        elapsed[sample] = end - begin;
        benchmark_sink += checksum + sample + UINT64_C(0x9e3779b97f4a7c15);
    }

    qsort(elapsed, samples, sizeof(*elapsed), compare_u64);
    double operations = (double)count * (double)rounds;
    uint32_t p95_index = (uint32_t)(((uint64_t)(samples - 1U) * 95U) / 100U);
    printf("%s,%s,%.6f,%.6f,%.6f\n", mode, name,
           (double)elapsed[samples / 2U] / operations,
           (double)elapsed[0] / operations,
           (double)elapsed[p95_index] / operations);
    free(elapsed);
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--count N] [--rounds N] [--samples N]"
            " [--mode all|latency|throughput]\n",
            program);
}

int main(int argc, char **argv) {
    uint32_t count = DEFAULT_COUNT;
    uint32_t rounds = DEFAULT_ROUNDS;
    uint32_t samples = DEFAULT_SAMPLES;
    const char *mode = "all";

    for (int index = 1; index < argc; ++index) {
        if (index + 1 >= argc) {
            print_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[index], "--count") == 0) {
            count = parse_positive_u32("--count", argv[++index]);
        } else if (strcmp(argv[index], "--rounds") == 0) {
            rounds = parse_positive_u32("--rounds", argv[++index]);
        } else if (strcmp(argv[index], "--samples") == 0) {
            samples = parse_positive_u32("--samples", argv[++index]);
        } else if (strcmp(argv[index], "--mode") == 0) {
            mode = argv[++index];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (count < 8U || count % 8U != 0U) {
        fputs("--count must be a positive multiple of 8\n", stderr);
        return 2;
    }
    if (strcmp(mode, "all") != 0 && strcmp(mode, "latency") != 0 &&
        strcmp(mode, "throughput") != 0) {
        print_usage(argv[0]);
        return 2;
    }

    uint32_t *input = malloc((size_t)count * sizeof(*input));
    if (input == NULL) {
        fputs("failed to allocate input\n", stderr);
        return 2;
    }
    uint32_t state = UINT32_C(0xdeadbeef);
    for (uint32_t index = 0; index < count; ++index) {
        input[index] = xorshift32(&state) % 86400U;
    }

    static const benchmark_case cases[] = {
        {"traditional", hms_traditional, traditional_latency,
         traditional_throughput},
        {"parallel_div", hms_parallel_div, parallel_div_latency,
         parallel_div_throughput},
        {"parallel_fixed", hms_parallel_fixed, parallel_fixed_latency,
         parallel_fixed_throughput},
        {"hi_low", hms_hi_low, hi_low_latency, hi_low_throughput},
        {"base64", hms_base64, base64_latency, base64_throughput},
    };
    size_t case_count = sizeof(cases) / sizeof(cases[0]);

    if (!validate_inputs(cases, case_count, input, count)) {
        free(input);
        return 1;
    }

    printf("# count=%" PRIu32 ",rounds=%" PRIu32 ",samples=%" PRIu32 "\n",
           count, rounds, samples);
    puts("mode,algorithm,median_ns_per_value,min_ns_per_value,p95_ns_per_value");

    for (size_t index = 0; index < case_count; ++index) {
        if (strcmp(mode, "all") == 0 || strcmp(mode, "latency") == 0) {
            run_case("latency", cases[index].name, cases[index].latency, input,
                     count, rounds, samples);
        }
        if (strcmp(mode, "all") == 0 || strcmp(mode, "throughput") == 0) {
            run_case("throughput", cases[index].name, cases[index].throughput,
                     input, count, rounds, samples);
        }
    }

    printf("# sink=%" PRIu64 "\n", benchmark_sink);
    free(input);
    return 0;
}
