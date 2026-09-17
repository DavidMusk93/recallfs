#define _POSIX_C_SOURCE 200809L

#include "hashing.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    DEFAULT_KEYS = 1000000,
    DEFAULT_LOOKUP_KEYS = 500000,
    DEFAULT_SAMPLES = 7,
    DEFAULT_BACKENDS = 32,
    DEFAULT_TABLE_SIZE = 4099,
    DEFAULT_VIRTUAL_NODES = 256,
};

typedef struct {
    uint64_t keys;
    size_t lookup_keys;
    size_t samples;
    size_t backend_count;
    size_t table_size;
    uint32_t virtual_nodes;
    size_t order_offset;
} benchmark_config;

typedef struct {
    double max_over_average;
    double min_over_average;
    double coefficient_of_variation;
} distribution_result;

typedef struct {
    const char *name;
    rz_hash_kind kind;
} algorithm_descriptor;

static const algorithm_descriptor algorithms[] = {
    {.name = "modulo", .kind = RZ_HASH_MODULO},
    {.name = "ring", .kind = RZ_HASH_RING},
    {.name = "rendezvous", .kind = RZ_HASH_RENDEZVOUS},
    {.name = "jump", .kind = RZ_HASH_JUMP},
    {.name = "maglev", .kind = RZ_HASH_MAGLEV},
};

enum { ALGORITHM_COUNT = sizeof(algorithms) / sizeof(algorithms[0]) };

static volatile uint64_t benchmark_sink;

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t lhs = *(const uint64_t *)left;
    uint64_t rhs = *(const uint64_t *)right;
    return (lhs > rhs) - (lhs < rhs);
}

static void *allocate_items(size_t count, size_t element_size, const char *label) {
    if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size) {
        fprintf(stderr, "%s allocation size overflow\n", label);
        exit(2);
    }
    void *memory = malloc(count * element_size);
    if (memory == NULL) {
        fprintf(stderr, "%s allocation failed\n", label);
        exit(2);
    }
    return memory;
}

static void *allocate_zeroed_items(size_t count, size_t element_size,
                                   const char *label) {
    if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size) {
        fprintf(stderr, "%s allocation size overflow\n", label);
        exit(2);
    }
    void *memory = calloc(count, element_size);
    if (memory == NULL) {
        fprintf(stderr, "%s allocation failed\n", label);
        exit(2);
    }
    return memory;
}

static uint64_t parse_u64(const char *flag, const char *text, bool allow_zero) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || (!allow_zero && value == 0)) {
        fprintf(stderr, "invalid %s value: %s\n", flag, text);
        exit(2);
    }
    return (uint64_t)value;
}

static size_t parse_size(const char *flag, const char *text) {
    uint64_t value = parse_u64(flag, text, false);
    if (value > SIZE_MAX) {
        fprintf(stderr, "%s exceeds size_t: %s\n", flag, text);
        exit(2);
    }
    return (size_t)value;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--keys N] [--lookup-keys N] [--samples N]"
            " [--backends N] [--table-size N] [--vnodes N]"
            " [--order-offset N]\n",
            program);
}

static benchmark_config parse_config(int argc, char **argv) {
    benchmark_config config = {
        .keys = DEFAULT_KEYS,
        .lookup_keys = DEFAULT_LOOKUP_KEYS,
        .samples = DEFAULT_SAMPLES,
        .backend_count = DEFAULT_BACKENDS,
        .table_size = DEFAULT_TABLE_SIZE,
        .virtual_nodes = DEFAULT_VIRTUAL_NODES,
        .order_offset = 0,
    };

    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            print_usage(argv[0]);
            exit(2);
        }
        if (strcmp(argv[index], "--keys") == 0) {
            config.keys = parse_u64("--keys", argv[index + 1], false);
        } else if (strcmp(argv[index], "--lookup-keys") == 0) {
            config.lookup_keys = parse_size("--lookup-keys", argv[index + 1]);
        } else if (strcmp(argv[index], "--samples") == 0) {
            config.samples = parse_size("--samples", argv[index + 1]);
        } else if (strcmp(argv[index], "--backends") == 0) {
            config.backend_count = parse_size("--backends", argv[index + 1]);
        } else if (strcmp(argv[index], "--table-size") == 0) {
            config.table_size = parse_size("--table-size", argv[index + 1]);
        } else if (strcmp(argv[index], "--vnodes") == 0) {
            uint64_t value = parse_u64("--vnodes", argv[index + 1], false);
            if (value > UINT32_MAX) {
                fputs("--vnodes exceeds uint32_t\n", stderr);
                exit(2);
            }
            config.virtual_nodes = (uint32_t)value;
        } else if (strcmp(argv[index], "--order-offset") == 0) {
            uint64_t value = parse_u64("--order-offset", argv[index + 1], true);
            if (value > SIZE_MAX) {
                fputs("--order-offset exceeds size_t\n", stderr);
                exit(2);
            }
            config.order_offset = (size_t)value;
        } else {
            print_usage(argv[0]);
            exit(2);
        }
    }

    if (config.backend_count < 2 || config.backend_count > INT32_MAX ||
        config.backend_count > UINT32_MAX || config.order_offset >= ALGORITHM_COUNT) {
        fputs("invalid backend count or order offset\n", stderr);
        exit(2);
    }
    return config;
}

static rz_selector *build_selector(rz_hash_kind kind, const rz_backend_id *backends,
                                   size_t backend_count,
                                   const benchmark_config *config) {
    const rz_selector_config selector_config = {
        .kind = kind,
        .backends = backends,
        .backend_count = backend_count,
        .maglev_table_size = config->table_size,
        .ring_virtual_nodes = config->virtual_nodes,
    };
    rz_selector *selector = NULL;
    int status = rz_selector_create(&selector_config, &selector);
    if (status != 0) {
        fprintf(stderr, "selector build failed: %s\n", strerror(status));
        exit(2);
    }
    return selector;
}

static size_t backend_position(const rz_backend_id *backends, size_t backend_count,
                               rz_backend_id selected) {
    for (size_t index = 0; index < backend_count; ++index) {
        if (backends[index] == selected) {
            return index;
        }
    }
    fprintf(stderr, "selector returned unknown backend %" PRIu32 "\n", selected);
    exit(1);
}

static distribution_result measure_distribution(const rz_selector *selector,
                                                const rz_backend_id *backends,
                                                size_t backend_count,
                                                uint64_t key_count) {
    size_t *counts =
        allocate_zeroed_items(backend_count, sizeof(*counts), "distribution");
    for (uint64_t key = 0; key < key_count; ++key) {
        rz_backend_id selected = rz_selector_select(selector, rz_hash_key(key));
        ++counts[backend_position(backends, backend_count, selected)];
    }

    double average = (double)key_count / (double)backend_count;
    size_t minimum = counts[0];
    size_t maximum = counts[0];
    double squared_error = 0.0;
    for (size_t index = 0; index < backend_count; ++index) {
        if (counts[index] < minimum) {
            minimum = counts[index];
        }
        if (counts[index] > maximum) {
            maximum = counts[index];
        }
        double delta = (double)counts[index] - average;
        squared_error += delta * delta;
    }
    free(counts);

    return (distribution_result){
        .max_over_average = (double)maximum / average,
        .min_over_average = (double)minimum / average,
        .coefficient_of_variation =
            sqrt(squared_error / (double)backend_count) / average,
    };
}

static double measure_churn(const rz_selector *before, const rz_selector *after,
                            uint64_t key_count) {
    uint64_t changed = 0;
    for (uint64_t key = 0; key < key_count; ++key) {
        uint64_t hash = rz_hash_key(key);
        changed += rz_selector_select(before, hash) != rz_selector_select(after, hash);
    }
    return (double)changed / (double)key_count;
}

static uint64_t rotate_left(uint64_t value, unsigned shift) {
    return (value << shift) | (value >> (64U - shift));
}

static uint64_t lookup_checksum(const rz_selector *selector, const uint64_t *key_hashes,
                                size_t key_count) {
    uint64_t sums[4] = {0, 0, 0, 0};
    size_t index = 0;
    for (; index + 4 <= key_count; index += 4) {
        for (size_t lane = 0; lane < 4; ++lane) {
            rz_backend_id backend =
                rz_selector_select(selector, key_hashes[index + lane]);
            sums[lane] +=
                ((uint64_t)backend + 1) * (key_hashes[index + lane] | UINT64_C(1));
        }
    }
    for (; index < key_count; ++index) {
        rz_backend_id backend = rz_selector_select(selector, key_hashes[index]);
        sums[0] += ((uint64_t)backend + 1) * (key_hashes[index] | UINT64_C(1));
    }
    return sums[0] ^ rotate_left(sums[1], 13) ^ rotate_left(sums[2], 29) ^
           rotate_left(sums[3], 47);
}

static double measure_lookup(const rz_selector *selector, const uint64_t *key_hashes,
                             size_t key_count, size_t samples, uint64_t *checksum_out) {
    uint64_t *elapsed = allocate_items(samples, sizeof(*elapsed), "lookup sample");
    uint64_t expected = lookup_checksum(selector, key_hashes, key_count);
    for (size_t sample = 0; sample < samples; ++sample) {
        uint64_t begin = monotonic_nanoseconds();
        uint64_t checksum = lookup_checksum(selector, key_hashes, key_count);
        uint64_t end = monotonic_nanoseconds();
        if (checksum != expected) {
            fputs("lookup checksum changed between samples\n", stderr);
            exit(1);
        }
        elapsed[sample] = end - begin;
        benchmark_sink += checksum + sample + UINT64_C(0x9e3779b97f4a7c15);
    }
    qsort(elapsed, samples, sizeof(*elapsed), compare_u64);
    double nanoseconds = (double)elapsed[samples / 2] / (double)key_count;
    free(elapsed);
    *checksum_out = expected;
    return nanoseconds;
}

static uint64_t selector_fingerprint(const rz_selector *selector) {
    uint64_t checksum = rz_selector_memory_bytes(selector);
    for (uint64_t key = 0; key < 1024; ++key) {
        checksum = checksum * UINT64_C(0x9e3779b97f4a7c15) +
                   rz_selector_select(selector, rz_hash_key(key));
    }
    return checksum;
}

static double measure_build(rz_hash_kind kind, const rz_backend_id *backends,
                            size_t backend_count, const benchmark_config *config) {
    uint64_t *elapsed =
        allocate_items(config->samples, sizeof(*elapsed), "build sample");
    for (size_t sample = 0; sample < config->samples; ++sample) {
        uint64_t begin = monotonic_nanoseconds();
        rz_selector *selector = build_selector(kind, backends, backend_count, config);
        uint64_t end = monotonic_nanoseconds();
        elapsed[sample] = end - begin;
        benchmark_sink += selector_fingerprint(selector) + sample;
        rz_selector_destroy(selector);
    }
    qsort(elapsed, config->samples, sizeof(*elapsed), compare_u64);
    double microseconds = (double)elapsed[config->samples / 2] / 1000.0;
    free(elapsed);
    return microseconds;
}

int main(int argc, char **argv) {
    benchmark_config config = parse_config(argc, argv);
    rz_backend_id *base =
        allocate_items(config.backend_count, sizeof(*base), "base backend");
    rz_backend_id *added =
        allocate_items(config.backend_count + 1, sizeof(*added), "added backend");
    rz_backend_id *removed =
        allocate_items(config.backend_count - 1, sizeof(*removed), "removed backend");
    uint64_t *key_hashes =
        allocate_items(config.lookup_keys, sizeof(*key_hashes), "key hash");

    for (size_t index = 0; index < config.backend_count; ++index) {
        base[index] = (rz_backend_id)index;
        added[index] = (rz_backend_id)index;
    }
    added[config.backend_count] = (rz_backend_id)config.backend_count;
    size_t removed_id = config.backend_count / 2;
    for (size_t source = 0, destination = 0; source < config.backend_count; ++source) {
        if (source != removed_id) {
            removed[destination++] = (rz_backend_id)source;
        }
    }
    for (size_t index = 0; index < config.lookup_keys; ++index) {
        key_hashes[index] = rz_hash_key(index);
    }

    printf("# keys=%" PRIu64 " lookup_keys=%zu samples=%zu backends=%zu"
           " table_size=%zu vnodes=%" PRIu32 " removed_id=%zu order_offset=%zu\n",
           config.keys, config.lookup_keys, config.samples, config.backend_count,
           config.table_size, config.virtual_nodes, removed_id, config.order_offset);
    puts("algorithm,max_over_avg,min_over_avg,cv,add_churn,"
         "remove_middle_churn,lookup_ns,build_us,memory_bytes,checksum");

    for (size_t ordinal = 0; ordinal < ALGORITHM_COUNT; ++ordinal) {
        size_t index = (ordinal + config.order_offset) % ALGORITHM_COUNT;
        const algorithm_descriptor *algorithm = &algorithms[index];
        rz_hash_kind kind = algorithm->kind;
        rz_selector *selector =
            build_selector(kind, base, config.backend_count, &config);
        rz_selector *after_add =
            build_selector(kind, added, config.backend_count + 1, &config);
        rz_selector *after_remove =
            build_selector(kind, removed, config.backend_count - 1, &config);
        distribution_result distribution =
            measure_distribution(selector, base, config.backend_count, config.keys);
        double add_churn = measure_churn(selector, after_add, config.keys);
        double remove_churn = measure_churn(selector, after_remove, config.keys);
        uint64_t checksum = 0;
        double lookup_ns = measure_lookup(selector, key_hashes, config.lookup_keys,
                                          config.samples, &checksum);
        double build_us = measure_build(kind, base, config.backend_count, &config);

        printf("%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f,%zu,%" PRIu64 "\n",
               algorithm->name, distribution.max_over_average,
               distribution.min_over_average, distribution.coefficient_of_variation,
               add_churn, remove_churn, lookup_ns, build_us,
               rz_selector_memory_bytes(selector), checksum);

        rz_selector_destroy(after_remove);
        rz_selector_destroy(after_add);
        rz_selector_destroy(selector);
    }

    printf("# sink=%" PRIu64 "\n", benchmark_sink);
    free(key_hashes);
    free(removed);
    free(added);
    free(base);
    return 0;
}
