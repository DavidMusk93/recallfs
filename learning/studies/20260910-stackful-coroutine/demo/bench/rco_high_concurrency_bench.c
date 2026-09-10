#define _GNU_SOURCE

#include "rco.h"

#include <alloca.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define BENCH_SCHEMA "rco-high-concurrency-v2"
#define MAX_TASKS UINT64_C(16384)
#define MAX_STACK_BYTES (UINT64_C(512) * 1024)
#define MAX_TOTAL_STACK_BYTES (UINT64_C(2) * 1024 * 1024 * 1024)
#define MAX_TOUCHED_BYTES (UINT64_C(512) * 1024 * 1024)
#define MAX_TOTAL_YIELDS UINT64_C(20000000)
#define STACK_HEADROOM_BYTES UINT64_C(8192)

#if defined(RCO_CACS_PRESERVE_NONE)
#define BENCH_BACKEND_IDENTITY "cacs-preserve-none"
#elif defined(RCO_CACS)
#define BENCH_BACKEND_IDENTITY "cacs"
#else
#define BENCH_BACKEND_IDENTITY "sysv"
#endif

static volatile uint64_t benchmark_sink;

struct options {
    uint64_t tasks;
    uint64_t stack_bytes;
    uint64_t touch_bytes;
    uint64_t yields_per_task;
};

struct benchmark_state {
    struct options options;
    uint64_t page_size;
    volatile uint64_t ready;
    volatile uint64_t validated;
    volatile bool stop_claimed;
    volatile bool measurement_started;
    volatile uint64_t end_barrier_arrivals;
    volatile uint64_t barrier_yields;
    volatile bool measurement_ended;
    volatile int worker_error;
    struct rusage usage_start;
    struct rusage usage_end;
    uint64_t wall_start_ns;
    uint64_t wall_end_ns;
};

struct worker {
    struct benchmark_state *state;
    uint64_t task_id;
    uint64_t checksum;
};

struct usage_delta {
    uint64_t user_us;
    uint64_t system_us;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t voluntary_switches;
    uint64_t involuntary_switches;
};

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --tasks N --stack-bytes N --touch-bytes N "
            "--yields-per-task N\n",
            program);
}

static bool multiply_u64(uint64_t left, uint64_t right, uint64_t *out)
{
    if (left != 0 && right > UINT64_MAX / left) {
        return false;
    }
    *out = left * right;
    return true;
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *out)
{
    if (right > UINT64_MAX - left) {
        return false;
    }
    *out = left + right;
    return true;
}

static bool parse_u64(const char *text, uint64_t *out)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static int parse_options(int argc, char **argv, struct options *out)
{
    enum {
        SEEN_TASKS = 1u << 0,
        SEEN_STACK = 1u << 1,
        SEEN_TOUCH = 1u << 2,
        SEEN_YIELDS = 1u << 3,
        SEEN_ALL = SEEN_TASKS | SEEN_STACK | SEEN_TOUCH | SEEN_YIELDS,
    };
    unsigned seen = 0;
    struct options options = {0};

    if (argc != 9) {
        usage(argv[0]);
        return -EINVAL;
    }
    for (int index = 1; index < argc; index += 2) {
        const char *name = argv[index];
        uint64_t *destination = NULL;
        unsigned bit = 0;
        if (strcmp(name, "--tasks") == 0) {
            destination = &options.tasks;
            bit = SEEN_TASKS;
        } else if (strcmp(name, "--stack-bytes") == 0) {
            destination = &options.stack_bytes;
            bit = SEEN_STACK;
        } else if (strcmp(name, "--touch-bytes") == 0) {
            destination = &options.touch_bytes;
            bit = SEEN_TOUCH;
        } else if (strcmp(name, "--yields-per-task") == 0) {
            destination = &options.yields_per_task;
            bit = SEEN_YIELDS;
        } else {
            fprintf(stderr, "unknown option: %s\n", name);
            return -EINVAL;
        }
        if ((seen & bit) != 0) {
            fprintf(stderr, "duplicate option: %s\n", name);
            return -EINVAL;
        }
        if (!parse_u64(argv[index + 1], destination)) {
            fprintf(stderr, "invalid value for %s: %s\n", name,
                    argv[index + 1]);
            return -EINVAL;
        }
        seen |= bit;
    }
    if (seen != SEEN_ALL) {
        usage(argv[0]);
        return -EINVAL;
    }
    *out = options;
    return 0;
}

static int validate_options(const struct options *options, uint64_t page_size)
{
    uint64_t touched_total = 0;
    uint64_t stack_total = 0;
    uint64_t total_yields = 0;
    uint64_t stack_required = 0;

    if (options->tasks == 0 || options->tasks > MAX_TASKS) {
        fputs("tasks must be in [1, 16384]\n", stderr);
        return -EINVAL;
    }
    if (options->stack_bytes < RCO_STACK_SIZE_MIN ||
        options->stack_bytes > MAX_STACK_BYTES ||
        options->stack_bytes % page_size != 0) {
        fprintf(stderr,
                "stack-bytes must be page-aligned and in [%zu, 524288]\n",
                RCO_STACK_SIZE_MIN);
        return -EINVAL;
    }
    if (options->touch_bytes == 0 ||
        options->touch_bytes > MAX_STACK_BYTES ||
        options->touch_bytes % page_size != 0) {
        fputs("touch-bytes must be page-aligned and in [4096, 524288]\n",
              stderr);
        return -EINVAL;
    }
    if (!add_u64(options->touch_bytes, page_size, &stack_required) ||
        !add_u64(stack_required, STACK_HEADROOM_BYTES, &stack_required) ||
        stack_required > options->stack_bytes) {
        fputs("stack-bytes must leave room for aligned sentinels and frames\n",
              stderr);
        return -EINVAL;
    }
    if (!multiply_u64(options->tasks, options->stack_bytes, &stack_total) ||
        stack_total > MAX_TOTAL_STACK_BYTES) {
        fputs("total stack bytes must be <= 2147483648\n", stderr);
        return -EINVAL;
    }
    if (!multiply_u64(options->tasks, options->touch_bytes, &touched_total) ||
        touched_total > MAX_TOUCHED_BYTES) {
        fputs("total touched stack bytes must be <= 536870912\n", stderr);
        return -EINVAL;
    }
    if (options->yields_per_task == 0 ||
        !multiply_u64(options->tasks, options->yields_per_task,
                      &total_yields) ||
        total_yields > MAX_TOTAL_YIELDS) {
        fputs("total yields per sample must be in [1, 20000000]\n", stderr);
        return -EINVAL;
    }
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static uint8_t sentinel_value(uint64_t task_id, uint64_t page_index)
{
    return (uint8_t)((task_id * UINT64_C(131) +
                      page_index * UINT64_C(17) + UINT64_C(0x5a)) &
                     UINT64_C(0xff));
}

static uint64_t expected_checksum(const struct options *options,
                                  uint64_t page_size)
{
    uint64_t sentinels = options->touch_bytes / page_size;
    uint64_t checksum = 0;
    for (uint64_t task_id = 1; task_id <= options->tasks; ++task_id) {
        uint64_t sum = 0;
        for (uint64_t page = 0; page < sentinels; ++page) {
            sum += sentinel_value(task_id, page);
        }
        checksum += sum * (options->yields_per_task + 1);
    }
    return checksum;
}

static uint64_t validate_sentinels(volatile unsigned char *base,
                                   const struct worker *worker)
{
    const struct benchmark_state *state = worker->state;
    uint64_t sentinels =
        state->options.touch_bytes / state->page_size;
    uint64_t checksum = 0;
    for (uint64_t page = 0; page < sentinels; ++page) {
        uint8_t expected = sentinel_value(worker->task_id, page);
        uint8_t observed = base[page * state->page_size];
        if (observed != expected) {
            return UINT64_MAX;
        }
        checksum += observed;
    }
    return checksum;
}

static void record_worker_error(struct benchmark_state *state, int error)
{
    if (state->worker_error == 0) {
        state->worker_error = error;
    }
}

static void end_measurement(struct benchmark_state *state)
{
    state->wall_end_ns = now_ns();
    if (state->wall_end_ns == 0) {
        record_worker_error(state, -EIO);
    }
    if (getrusage(RUSAGE_SELF, &state->usage_end) != 0) {
        record_worker_error(state, -errno);
    }
    state->measurement_ended = true;
}

__attribute__((noinline)) static int high_concurrency_worker(void *argument)
{
    struct worker *worker = argument;
    struct benchmark_state *state = worker->state;
    size_t allocation_size =
        (size_t)(state->options.touch_bytes + state->page_size);
    unsigned char *raw = alloca(allocation_size);
    uintptr_t aligned_address =
        ((uintptr_t)raw + (uintptr_t)state->page_size - 1) &
        ~((uintptr_t)state->page_size - 1);
    volatile unsigned char *sentinels =
        (volatile unsigned char *)aligned_address;
    uint64_t sentinel_count =
        state->options.touch_bytes / state->page_size;

    for (uint64_t page = 0; page < sentinel_count; ++page) {
        sentinels[page * state->page_size] =
            sentinel_value(worker->task_id, page);
    }
    state->ready++;
    if (rco_yield() != 0) {
        record_worker_error(state, -ECANCELED);
        return -ECANCELED;
    }

    uint64_t checksum = validate_sentinels(sentinels, worker);
    if (checksum == UINT64_MAX) {
        record_worker_error(state, -EILSEQ);
        return -EILSEQ;
    }
    state->validated++;
    if (rco_yield() != 0) {
        record_worker_error(state, -ECANCELED);
        return -ECANCELED;
    }

    if (!state->measurement_started) {
        if (state->ready != state->options.tasks ||
            state->validated != state->options.tasks ||
            state->stop_claimed) {
            record_worker_error(state, -EPROTO);
            return -EPROTO;
        }
        state->stop_claimed = true;
        if (kill(getpid(), SIGSTOP) != 0) {
            int error = -errno;
            record_worker_error(state, error);
            return error;
        }
        if (getrusage(RUSAGE_SELF, &state->usage_start) != 0) {
            int error = -errno;
            record_worker_error(state, error);
            return error;
        }
        state->wall_start_ns = now_ns();
        if (state->wall_start_ns == 0) {
            record_worker_error(state, -EIO);
            return -EIO;
        }
        state->measurement_started = true;
    }

    for (uint64_t iteration = 0;
         iteration < state->options.yields_per_task; ++iteration) {
        if (rco_yield() != 0) {
            record_worker_error(state, -ECANCELED);
            return -ECANCELED;
        }
        uint64_t observed = validate_sentinels(sentinels, worker);
        if (observed == UINT64_MAX) {
            record_worker_error(state, -EILSEQ);
            return -EILSEQ;
        }
        checksum += observed;
    }
    worker->checksum = checksum;
    state->end_barrier_arrivals++;
    if (state->end_barrier_arrivals == state->options.tasks) {
        end_measurement(state);
    } else {
        while (!state->measurement_ended) {
            state->barrier_yields++;
            if (rco_yield() != 0) {
                record_worker_error(state, -ECANCELED);
                return -ECANCELED;
            }
            if (validate_sentinels(sentinels, worker) == UINT64_MAX) {
                record_worker_error(state, -EILSEQ);
                return -EILSEQ;
            }
        }
    }
    return 0;
}

static uint64_t timeval_us(const struct timeval *value)
{
    return (uint64_t)value->tv_sec * UINT64_C(1000000) +
           (uint64_t)value->tv_usec;
}

static int usage_delta(const struct rusage *start,
                       const struct rusage *end,
                       struct usage_delta *out)
{
    uint64_t start_user = timeval_us(&start->ru_utime);
    uint64_t end_user = timeval_us(&end->ru_utime);
    uint64_t start_system = timeval_us(&start->ru_stime);
    uint64_t end_system = timeval_us(&end->ru_stime);
    if (end_user < start_user || end_system < start_system ||
        end->ru_minflt < start->ru_minflt ||
        end->ru_majflt < start->ru_majflt ||
        end->ru_nvcsw < start->ru_nvcsw ||
        end->ru_nivcsw < start->ru_nivcsw) {
        return -ERANGE;
    }
    *out = (struct usage_delta){
        .user_us = end_user - start_user,
        .system_us = end_system - start_system,
        .minor_faults = (uint64_t)(end->ru_minflt - start->ru_minflt),
        .major_faults = (uint64_t)(end->ru_majflt - start->ru_majflt),
        .voluntary_switches =
            (uint64_t)(end->ru_nvcsw - start->ru_nvcsw),
        .involuntary_switches =
            (uint64_t)(end->ru_nivcsw - start->ru_nivcsw),
    };
    return 0;
}

static int run_benchmark(const struct options *options, uint64_t page_size)
{
    struct rco_config config = {
        .default_stack_size = (size_t)options->stack_bytes,
        .max_coroutines = (size_t)options->tasks,
        .max_fds = 16,
        .stack_cache_bytes = 0,
    };
    struct benchmark_state state = {
        .options = *options,
        .page_size = page_size,
    };
    struct worker *workers = calloc((size_t)options->tasks, sizeof(*workers));
    struct rco_runtime *runtime = NULL;
    if (workers == NULL) {
        fputs("worker allocation failed\n", stderr);
        return EXIT_FAILURE;
    }
    int result = rco_runtime_create(&config, &runtime);
    if (result != 0) {
        fprintf(stderr, "runtime creation failed: %d\n", result);
        free(workers);
        return EXIT_FAILURE;
    }

    uint64_t spawned = 0;
    for (; spawned < options->tasks; ++spawned) {
        workers[spawned].state = &state;
        uint64_t task_id = 0;
        result = rco_spawn(runtime, 0, high_concurrency_worker,
                           &workers[spawned], &task_id);
        if (result != 0 || task_id != spawned + 1) {
            fprintf(stderr, "task spawn failed at %" PRIu64 ": %d\n",
                    spawned + 1, result);
            (void)rco_runtime_stop(runtime);
            (void)rco_runtime_run(runtime);
            (void)rco_runtime_destroy(runtime);
            free(workers);
            return EXIT_FAILURE;
        }
        workers[spawned].task_id = task_id;
    }

    result = rco_runtime_run(runtime);
    struct rco_stats stats;
    int stats_result = rco_runtime_get_stats(runtime, &stats);

    uint64_t checksum = 0;
    for (uint64_t index = 0; index < options->tasks; ++index) {
        checksum += workers[index].checksum;
    }
    uint64_t expected = expected_checksum(options, page_size);
    uint64_t requested_yields =
        options->tasks * options->yields_per_task;
    uint64_t measured_yields =
        requested_yields + state.barrier_yields;
    uint64_t expected_switches =
        options->tasks * (2 * options->yields_per_task + 6) +
        2 * state.barrier_yields;
    struct usage_delta delta = {0};
    int delta_result =
        usage_delta(&state.usage_start, &state.usage_end, &delta);

    bool valid =
        result == 0 && state.worker_error == 0 &&
        state.measurement_started && state.measurement_ended &&
        state.end_barrier_arrivals == options->tasks &&
        state.wall_end_ns > state.wall_start_ns &&
        state.barrier_yields == options->tasks - 1 &&
        delta_result == 0 && stats_result == 0 &&
        checksum == expected && stats.spawned == options->tasks &&
        stats.completed == options->tasks && stats.peak_active == options->tasks &&
        stats.active == 0 && stats.context_switches == expected_switches;
    if (!valid) {
        fprintf(stderr,
                "benchmark validation failed: run=%d worker=%d delta=%d "
                "stats=%d checksum=%" PRIu64 "/%" PRIu64
                " barrier=%" PRIu64 " arrivals=%" PRIu64
                " switches=%" PRIu64 "/%" PRIu64 "\n",
                result, state.worker_error, delta_result, stats_result,
                checksum, expected, state.barrier_yields,
                state.end_barrier_arrivals,
                stats_result == 0 ? stats.context_switches : 0,
                expected_switches);
        (void)rco_runtime_destroy(runtime);
        free(workers);
        return EXIT_FAILURE;
    }

    benchmark_sink = checksum | UINT64_C(1);
    printf(
        "{\"schema\":\"%s\",\"backend_identity\":\"%s\""
        ",\"tasks\":%" PRIu64
        ",\"stack_bytes\":%" PRIu64 ",\"touch_bytes\":%" PRIu64
        ",\"page_size\":%" PRIu64 ",\"sentinels_per_task\":%" PRIu64
        ",\"yields_per_task\":%" PRIu64
        ",\"requested_yields\":%" PRIu64
        ",\"barrier_yields\":%" PRIu64
        ",\"measured_yields\":%" PRIu64
        ",\"checksum\":%" PRIu64 ",\"expected_checksum\":%" PRIu64
        ",\"runtime_switches\":%" PRIu64
        ",\"expected_runtime_switches\":%" PRIu64
        ",\"wall_ns\":%" PRIu64 ",\"user_us\":%" PRIu64
        ",\"system_us\":%" PRIu64 ",\"minor_faults_delta\":%" PRIu64
        ",\"major_faults_delta\":%" PRIu64
        ",\"voluntary_context_switches_delta\":%" PRIu64
        ",\"involuntary_context_switches_delta\":%" PRIu64
        ",\"spawned\":%" PRIu64 ",\"completed\":%" PRIu64
        ",\"peak_active\":%zu}\n",
        BENCH_SCHEMA, BENCH_BACKEND_IDENTITY, options->tasks,
        options->stack_bytes,
        options->touch_bytes, page_size, options->touch_bytes / page_size,
        options->yields_per_task, requested_yields, state.barrier_yields,
        measured_yields, checksum, expected,
        stats.context_switches, expected_switches,
        state.wall_end_ns - state.wall_start_ns, delta.user_us,
        delta.system_us,
        delta.minor_faults, delta.major_faults, delta.voluntary_switches,
        delta.involuntary_switches, stats.spawned, stats.completed,
        stats.peak_active);

    int destroy_result = rco_runtime_destroy(runtime);
    free(workers);
    if (destroy_result != 0 || benchmark_sink == 0) {
        fputs("benchmark teardown validation failed\n", stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    long raw_page_size = sysconf(_SC_PAGESIZE);
    if (raw_page_size <= 0) {
        fputs("failed to determine page size\n", stderr);
        return EXIT_FAILURE;
    }
    uint64_t page_size = (uint64_t)raw_page_size;
    if ((page_size & (page_size - 1)) != 0) {
        fputs("page size must be a power of two\n", stderr);
        return EXIT_FAILURE;
    }

    struct options options;
    if (parse_options(argc, argv, &options) != 0 ||
        validate_options(&options, page_size) != 0) {
        return 2;
    }
    return run_benchmark(&options, page_size);
}
