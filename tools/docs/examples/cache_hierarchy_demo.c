#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
    PROBE_GRANULE_BYTES = 64,
    MIN_PROBE_BYTES = 4 * 1024,
    MAX_PROBE_BYTES = 64 * 1024 * 1024,
    MIN_PROBE_STEPS = 1 * 1024 * 1024,
    MAX_PROBE_STEPS = 8 * 1024 * 1024,
    MATRIX_SIDE = 2048,
    LOCALITY_ROUNDS = 3
};

struct cache_level
{
    double hit_cycles;
    double conditional_miss_rate;
};

struct probe_node
{
    uint32_t next;
    unsigned char padding[PROBE_GRANULE_BYTES - sizeof(uint32_t)];
};

_Static_assert(sizeof(struct probe_node) == PROBE_GRANULE_BYTES,
               "probe node must have the configured granularity");

static volatile uint64_t observation_sink;
static uint64_t random_state = UINT64_C(0x6a09e667f3bcc909);

static void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime");
    return (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
}

static uint32_t next_random_u32(void)
{
    uint64_t value = random_state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    random_state = value;
    return (uint32_t)(value >> 32);
}

static double hierarchy_amat(const struct cache_level *levels,
                             size_t level_count,
                             double memory_cycles)
{
    double tail = memory_cycles;
    size_t level = level_count;

    while (level > 0)
    {
        level--;
        tail = levels[level].hit_cycles
               + levels[level].conditional_miss_rate * tail;
    }
    return tail;
}

static void demonstrate_amat(void)
{
    const struct cache_level hierarchy[] = {
        {.hit_cycles = 1.0, .conditional_miss_rate = 0.05},
        {.hit_cycles = 4.0, .conditional_miss_rate = 0.20},
        {.hit_cycles = 20.0, .conditional_miss_rate = 0.20}};
    const struct cache_level l1_only[] = {
        {.hit_cycles = 1.0, .conditional_miss_rate = 0.05}};
    const double memory_cycles = 200.0;
    double global_memory_miss = 1.0;
    size_t level;

    for (level = 0; level < sizeof(hierarchy) / sizeof(hierarchy[0]);
         level++)
    {
        global_memory_miss *= hierarchy[level].conditional_miss_rate;
    }

    puts("[1] Recursive AMAT model (illustrative inputs, not measurements)");
    printf("  L1 -> DRAM:            %.2f cycles/access\n",
           hierarchy_amat(l1_only, 1, memory_cycles));
    printf("  L1 -> L2 -> L3 -> DRAM: %.2f cycles/access\n",
           hierarchy_amat(hierarchy,
                          sizeof(hierarchy) / sizeof(hierarchy[0]),
                          memory_cycles));
    printf("  global DRAM miss rate:  %.3f%%\n",
           global_memory_miss * 100.0);
}

static void build_random_cycle(struct probe_node *nodes,
                               uint32_t *order,
                               size_t count)
{
    size_t index;

    assert(count > 1);
    assert(count <= UINT32_MAX);

    for (index = 0; index < count; index++)
        order[index] = (uint32_t)index;

    for (index = count - 1; index > 0; index--)
    {
        size_t other = (size_t)next_random_u32() % (index + 1);
        uint32_t temporary = order[index];

        order[index] = order[other];
        order[other] = temporary;
    }

    for (index = 0; index < count; index++)
        nodes[order[index]].next = order[(index + 1) % count];
}

static size_t probe_step_count(size_t node_count)
{
    size_t steps = node_count * 8;

    if (steps < MIN_PROBE_STEPS)
        return MIN_PROBE_STEPS;
    if (steps > MAX_PROBE_STEPS)
        return MAX_PROBE_STEPS;
    return steps;
}

static double measure_dependent_loads(const struct probe_node *nodes,
                                      size_t node_count,
                                      size_t steps)
{
    uint32_t current = 0;
    size_t step;
    double started;
    double elapsed;

    for (step = 0; step < node_count; step++)
        current = nodes[current].next;

    started = monotonic_seconds();
    for (step = 0; step < steps; step++)
        current = nodes[current].next;
    elapsed = monotonic_seconds() - started;

    observation_sink ^= current;
    return elapsed * 1.0e9 / (double)steps;
}

static void demonstrate_working_set_knees(void)
{
    size_t bytes;

    puts("[2] Random dependent-load sweep");
    puts("  working-set KiB | ns/dependent-load");

    for (bytes = MIN_PROBE_BYTES; bytes <= MAX_PROBE_BYTES; bytes *= 2)
    {
        size_t node_count = bytes / sizeof(struct probe_node);
        size_t steps = probe_step_count(node_count);
        struct probe_node *nodes = calloc(node_count, sizeof(*nodes));
        uint32_t *order = malloc(node_count * sizeof(*order));
        double nanoseconds;

        if (nodes == NULL || order == NULL)
            fail("allocate pointer-chase working set");

        build_random_cycle(nodes, order, node_count);
        nanoseconds = measure_dependent_loads(nodes, node_count, steps);
        printf("  %15zu | %17.2f\n", bytes / 1024, nanoseconds);

        free(order);
        free(nodes);
    }
}

static uint64_t sum_rows(const uint64_t *matrix, size_t side)
{
    uint64_t sum = 0;
    size_t row;

    for (row = 0; row < side; row++)
    {
        size_t column;

        for (column = 0; column < side; column++)
            sum += matrix[row * side + column];
    }
    return sum;
}

static uint64_t sum_columns(const uint64_t *matrix, size_t side)
{
    uint64_t sum = 0;
    size_t column;

    for (column = 0; column < side; column++)
    {
        size_t row;

        for (row = 0; row < side; row++)
            sum += matrix[row * side + column];
    }
    return sum;
}

static double measure_sum(uint64_t (*sum)(const uint64_t *, size_t),
                          const uint64_t *matrix,
                          size_t side,
                          uint64_t *result)
{
    double started = monotonic_seconds();

    *result = sum(matrix, side);
    return monotonic_seconds() - started;
}

static void demonstrate_spatial_locality(void)
{
    const size_t element_count = (size_t)MATRIX_SIDE * MATRIX_SIDE;
    const size_t bytes = element_count * sizeof(uint64_t);
    uint64_t *matrix = malloc(bytes);
    uint64_t expected = 0;
    double best_row_seconds = 1.0e100;
    double best_column_seconds = 1.0e100;
    size_t index;
    int round;

    if (matrix == NULL)
        fail("allocate locality matrix");

    for (index = 0; index < element_count; index++)
    {
        matrix[index] = (uint64_t)(index % 251U);
        expected += matrix[index];
    }

    for (round = 0; round < LOCALITY_ROUNDS; round++)
    {
        uint64_t row_sum;
        uint64_t column_sum;
        double row_seconds;
        double column_seconds;

        if ((round & 1) == 0)
        {
            row_seconds =
                measure_sum(sum_rows, matrix, MATRIX_SIDE, &row_sum);
            column_seconds =
                measure_sum(sum_columns, matrix, MATRIX_SIDE, &column_sum);
        }
        else
        {
            column_seconds =
                measure_sum(sum_columns, matrix, MATRIX_SIDE, &column_sum);
            row_seconds =
                measure_sum(sum_rows, matrix, MATRIX_SIDE, &row_sum);
        }

        assert(row_sum == expected);
        assert(column_sum == expected);
        if (row_seconds < best_row_seconds)
            best_row_seconds = row_seconds;
        if (column_seconds < best_column_seconds)
            best_column_seconds = column_seconds;
        observation_sink ^= row_sum ^ column_sum;
    }

    puts("[3] C row-major layout");
    printf("  matrix:       %d x %d (%zu MiB)\n",
           MATRIX_SIDE,
           MATRIX_SIDE,
           bytes / (1024 * 1024));
    printf("  row-major:    %.3f ms\n", best_row_seconds * 1.0e3);
    printf("  column-major: %.3f ms\n", best_column_seconds * 1.0e3);
    printf("  ratio:        %.2fx\n",
           best_column_seconds / best_row_seconds);

    free(matrix);
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s {model|latency|locality|all}\n",
            program);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "model") == 0)
        demonstrate_amat();
    else if (strcmp(argv[1], "latency") == 0)
        demonstrate_working_set_knees();
    else if (strcmp(argv[1], "locality") == 0)
        demonstrate_spatial_locality();
    else if (strcmp(argv[1], "all") == 0)
    {
        demonstrate_amat();
        demonstrate_working_set_knees();
        demonstrate_spatial_locality();
    }
    else
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
