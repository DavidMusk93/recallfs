#define _GNU_SOURCE

#include "rco_pool.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define BENCH_SCHEMA "rco-pool-bench-v1"
#define MAX_WORKERS UINT64_C(64)
#define MAX_JOBS UINT64_C(16384)
#define MAX_RUNTIME_SLOTS UINT64_C(262144)
#define MAX_TOTAL_STACK_BYTES (UINT64_C(512) * 1024 * 1024)
#define MAX_TOTAL_ITERATIONS UINT64_C(100000000)
#define MAX_PREEMPT_QUANTUM_NS UINT64_C(1000000000)
#define PREEMPT_CADENCE_ITERATIONS UINT64_C(64)
#define START_TIMEOUT_MS 10000
#define POOL_TIMEOUT_MS 60000

#if defined(RCO_CACS_PRESERVE_NONE)
#define BENCH_BACKEND_IDENTITY "cacs-preserve-none"
#elif defined(RCO_CACS)
#define BENCH_BACKEND_IDENTITY "cacs"
#else
#define BENCH_BACKEND_IDENTITY "sysv"
#endif

static volatile uint64_t benchmark_sink;

struct options {
    size_t workers;
    size_t jobs;
    uint64_t iterations;
    uint64_t preempt_quantum_ns;
    int first_cpu;
};

struct benchmark_state {
    struct options options;
    pthread_mutex_t start_mutex;
    pthread_cond_t start_condition;
    size_t workers_ready;
    pid_t worker_tids[MAX_WORKERS];
    bool start_open;
    bool abort;
    _Atomic uint64_t worker_mask;
    _Atomic int worker_error;
};

struct benchmark_job {
    struct benchmark_state *state;
    size_t index;
    size_t worker;
    pid_t tid;
    uint64_t expected_checksum;
    uint64_t checksum;
    uint64_t preempt_points;
    int entry_result;
    bool migration_seen;
    _Atomic unsigned executions;
    _Atomic unsigned finalizations;
};

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --workers N --jobs N --iterations N "
            "--preempt-quantum-ns N --pin-first-cpu N\n",
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

static bool parse_first_cpu(const char *text, int *out)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < -1 ||
        value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

static int parse_options(int argc, char **argv, struct options *out)
{
    enum {
        SEEN_WORKERS = 1u << 0,
        SEEN_JOBS = 1u << 1,
        SEEN_ITERATIONS = 1u << 2,
        SEEN_QUANTUM = 1u << 3,
        SEEN_FIRST_CPU = 1u << 4,
        SEEN_ALL = SEEN_WORKERS | SEEN_JOBS | SEEN_ITERATIONS |
                   SEEN_QUANTUM | SEEN_FIRST_CPU,
    };
    unsigned seen = 0;
    uint64_t workers = 0;
    uint64_t jobs = 0;
    struct options options = {0};

    if (argc != 11) {
        usage(argv[0]);
        return -EINVAL;
    }
    for (int index = 1; index < argc; index += 2) {
        const char *name = argv[index];
        uint64_t *destination = NULL;
        unsigned bit = 0;

        if (strcmp(name, "--workers") == 0) {
            destination = &workers;
            bit = SEEN_WORKERS;
        } else if (strcmp(name, "--jobs") == 0) {
            destination = &jobs;
            bit = SEEN_JOBS;
        } else if (strcmp(name, "--iterations") == 0) {
            destination = &options.iterations;
            bit = SEEN_ITERATIONS;
        } else if (strcmp(name, "--preempt-quantum-ns") == 0) {
            destination = &options.preempt_quantum_ns;
            bit = SEEN_QUANTUM;
        } else if (strcmp(name, "--pin-first-cpu") == 0) {
            bit = SEEN_FIRST_CPU;
            if ((seen & bit) != 0) {
                fprintf(stderr, "duplicate option: %s\n", name);
                return -EINVAL;
            }
            if (!parse_first_cpu(argv[index + 1], &options.first_cpu)) {
                fprintf(stderr, "invalid value for %s: %s\n", name,
                        argv[index + 1]);
                return -EINVAL;
            }
            seen |= bit;
            continue;
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
    if (seen != SEEN_ALL || workers > SIZE_MAX || jobs > SIZE_MAX) {
        usage(argv[0]);
        return -EINVAL;
    }
    options.workers = (size_t)workers;
    options.jobs = (size_t)jobs;
    *out = options;
    return 0;
}

static int validate_options(const struct options *options)
{
    uint64_t runtime_slots = 0;
    uint64_t slots_per_worker = 0;
    uint64_t total_stack_bytes = 0;
    uint64_t total_iterations = 0;

    if (options->workers == 0 || options->workers > MAX_WORKERS) {
        fputs("workers must be in [1, 64]\n", stderr);
        return -EINVAL;
    }
    if (options->jobs < options->workers || options->jobs > MAX_JOBS ||
        options->jobs > SIZE_MAX / sizeof(struct benchmark_job)) {
        fputs("jobs must be in [workers, 16384]\n", stderr);
        return -EINVAL;
    }
    if (!add_u64((uint64_t)options->jobs, 1, &slots_per_worker) ||
        !multiply_u64((uint64_t)options->workers, slots_per_worker,
                      &runtime_slots) ||
        runtime_slots > MAX_RUNTIME_SLOTS) {
        fputs("workers * (jobs + 1) must be <= 262144\n", stderr);
        return -EINVAL;
    }
    if (!multiply_u64((uint64_t)options->jobs,
                      (uint64_t)RCO_STACK_SIZE_MIN, &total_stack_bytes) ||
        total_stack_bytes > MAX_TOTAL_STACK_BYTES) {
        fputs("total coroutine stack bytes must be <= 536870912\n", stderr);
        return -EINVAL;
    }
    if (options->iterations == 0 ||
        !multiply_u64((uint64_t)options->jobs, options->iterations,
                      &total_iterations) ||
        total_iterations > MAX_TOTAL_ITERATIONS) {
        fputs("jobs * iterations must be in [1, 100000000]\n", stderr);
        return -EINVAL;
    }
    if (options->preempt_quantum_ns > MAX_PREEMPT_QUANTUM_NS) {
        fputs("preempt-quantum-ns must be in [0, 1000000000]\n", stderr);
        return -EINVAL;
    }
    if (options->first_cpu >= 0 &&
        ((size_t)options->first_cpu >= CPU_SETSIZE ||
         options->workers - 1 >
             (size_t)(CPU_SETSIZE - 1 - options->first_cpu))) {
        fputs("pinned worker CPU range exceeds CPU_SETSIZE\n", stderr);
        return -EINVAL;
    }
    return 0;
}

static int clock_ns(clockid_t clock_id, uint64_t *out)
{
    struct timespec now;

    if (clock_gettime(clock_id, &now) != 0) {
        return -errno;
    }
    if (now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return -ERANGE;
    }
    *out = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
    return 0;
}

static int realtime_deadline(int timeout_ms, struct timespec *out)
{
    if (clock_gettime(CLOCK_REALTIME, out) != 0) {
        return -errno;
    }
    out->tv_sec += timeout_ms / 1000;
    out->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec++;
        out->tv_nsec -= 1000000000L;
    }
    return 0;
}

static void record_worker_error(struct benchmark_state *state, int error)
{
    int expected = 0;

    if (error == 0) {
        error = -EIO;
    }
    (void)atomic_compare_exchange_strong_explicit(
        &state->worker_error, &expected, error, memory_order_relaxed,
        memory_order_relaxed);
}

static pid_t current_tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

static uint64_t rotate_left(uint64_t value, unsigned shift)
{
    return (value << shift) | (value >> (64 - shift));
}

static uint64_t initial_value(size_t job_index)
{
    return UINT64_C(0x9e3779b97f4a7c15) ^
           ((uint64_t)job_index + UINT64_C(1)) *
               UINT64_C(0xd1b54a32d192ed03);
}

static uint64_t integer_step(uint64_t value, size_t job_index,
                             uint64_t iteration)
{
    value ^= iteration + ((uint64_t)job_index << 32);
    value *= UINT64_C(0x94d049bb133111eb);
    value = rotate_left(value, 27);
    return value + UINT64_C(0x2545f4914f6cdd1d);
}

static uint64_t expected_job_checksum(size_t job_index,
                                      uint64_t iterations)
{
    uint64_t value = initial_value(job_index);

    for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
        value = integer_step(value, job_index, iteration);
    }
    return value;
}

static void check_job_worker(struct benchmark_job *job)
{
    if (rco_current_worker() != job->worker) {
        job->migration_seen = true;
        record_worker_error(job->state, -EXDEV);
    }
}

static void check_job_identity(struct benchmark_job *job)
{
    check_job_worker(job);
    if (current_tid() != job->tid) {
        job->migration_seen = true;
        record_worker_error(job->state, -EXDEV);
    }
}

static int wait_for_start(struct benchmark_job *job)
{
    struct benchmark_state *state = job->state;
    int result = pthread_mutex_lock(&state->start_mutex);
    if (result != 0) {
        record_worker_error(state, -result);
        return -result;
    }

    bool identity_valid =
        job->worker < state->options.workers && job->worker < MAX_WORKERS &&
        job->tid > 0;
    if (identity_valid) {
        uint64_t bit = UINT64_C(1) << job->worker;
        uint64_t previous = atomic_fetch_or_explicit(
            &state->worker_mask, bit, memory_order_relaxed);
        if ((previous & bit) == 0) {
            state->worker_tids[job->worker] = job->tid;
            state->workers_ready++;
        } else if (state->worker_tids[job->worker] != job->tid) {
            record_worker_error(state, -EXDEV);
        }
    } else {
        record_worker_error(state, -ERANGE);
    }
    result = pthread_cond_broadcast(&state->start_condition);
    if (result != 0) {
        record_worker_error(state, -result);
    }
    while (!state->start_open) {
        result =
            pthread_cond_wait(&state->start_condition, &state->start_mutex);
        if (result != 0) {
            record_worker_error(state, -result);
            break;
        }
    }
    bool abort = state->abort;
    int unlock_result = pthread_mutex_unlock(&state->start_mutex);
    if (unlock_result != 0) {
        record_worker_error(state, -unlock_result);
        return -unlock_result;
    }
    return abort ? -ECANCELED : (result == 0 ? 0 : -result);
}

/*
 * Every job checks deferred preemption after iterations 1, 65, 129, ...
 * This guarantees at least one preemption point for every accepted job.
 */
__attribute__((noinline)) static int pool_bench_entry(void *argument)
{
    struct benchmark_job *job = argument;
    struct benchmark_state *state = job->state;

    unsigned previous = atomic_fetch_add_explicit(
        &job->executions, 1, memory_order_relaxed);
    if (previous != 0) {
        record_worker_error(state, -EALREADY);
        job->entry_result = -EALREADY;
        return job->entry_result;
    }
    job->worker = rco_current_worker();
    job->tid = current_tid();
    job->entry_result = wait_for_start(job);
    if (job->entry_result != 0) {
        return job->entry_result;
    }
    check_job_identity(job);

    uint64_t value = initial_value(job->index);
    for (uint64_t iteration = 0; iteration < state->options.iterations;
         ++iteration) {
        value = integer_step(value, job->index, iteration);
        if (iteration % PREEMPT_CADENCE_ITERATIONS == 0) {
            job->preempt_points++;
            int result = rco_preempt_point();
            if (result != 0) {
                record_worker_error(state, result);
                job->entry_result = result;
                return result;
            }
            check_job_worker(job);
        }
    }
    check_job_identity(job);
    job->checksum = value;
    job->entry_result = 0;
    return 0;
}

static void pool_bench_finalizer(void *argument)
{
    struct benchmark_job *job = argument;
    unsigned previous = atomic_fetch_add_explicit(
        &job->finalizations, 1, memory_order_relaxed);

    if (previous != 0) {
        record_worker_error(job->state, -EALREADY);
    }
    if (atomic_load_explicit(&job->executions, memory_order_relaxed) == 1) {
        check_job_identity(job);
    }
}

static void abort_start(struct benchmark_state *state)
{
    int result = pthread_mutex_lock(&state->start_mutex);
    if (result != 0) {
        record_worker_error(state, -result);
        return;
    }
    state->abort = true;
    state->start_open = true;
    result = pthread_cond_broadcast(&state->start_condition);
    if (result != 0) {
        record_worker_error(state, -result);
    }
    result = pthread_mutex_unlock(&state->start_mutex);
    if (result != 0) {
        record_worker_error(state, -result);
    }
}

static int release_start(struct benchmark_state *state,
                         uint64_t *wall_start_ns, uint64_t *cpu_start_ns)
{
    struct timespec deadline;
    int result = realtime_deadline(START_TIMEOUT_MS, &deadline);
    if (result != 0) {
        return result;
    }
    result = pthread_mutex_lock(&state->start_mutex);
    if (result != 0) {
        return -result;
    }
    while (state->workers_ready < state->options.workers &&
           atomic_load_explicit(&state->worker_error,
                                memory_order_relaxed) == 0) {
        result = pthread_cond_timedwait(
            &state->start_condition, &state->start_mutex, &deadline);
        if (result != 0) {
            result = result == ETIMEDOUT ? -ETIMEDOUT : -result;
            break;
        }
    }
    int worker_error =
        atomic_load_explicit(&state->worker_error, memory_order_relaxed);
    if (result == 0 && worker_error != 0) {
        result = worker_error;
    }
    if (result == 0 && state->workers_ready != state->options.workers) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = clock_ns(CLOCK_MONOTONIC_RAW, wall_start_ns);
    }
    if (result == 0) {
        result = clock_ns(CLOCK_PROCESS_CPUTIME_ID, cpu_start_ns);
    }
    if (result != 0) {
        state->abort = true;
    }
    state->start_open = true;
    int broadcast_result =
        pthread_cond_broadcast(&state->start_condition);
    int unlock_result = pthread_mutex_unlock(&state->start_mutex);
    if (result == 0 && broadcast_result != 0) {
        result = -broadcast_result;
    }
    if (result == 0 && unlock_result != 0) {
        result = -unlock_result;
    }
    return result;
}

static unsigned bit_count(uint64_t value)
{
    unsigned count = 0;

    while (value != 0) {
        count += (unsigned)(value & 1);
        value >>= 1;
    }
    return count;
}

static bool validate_jobs(const struct benchmark_state *state,
                          const struct benchmark_job *jobs,
                          uint64_t *out_checksum)
{
    bool valid = true;
    uint64_t checksum = 0;
    uint64_t expected_preempt_points =
        (state->options.iterations - 1) /
            PREEMPT_CADENCE_ITERATIONS +
        1;

    for (size_t index = 0; index < state->options.jobs; ++index) {
        const struct benchmark_job *job = &jobs[index];
        unsigned executions =
            atomic_load_explicit(&job->executions, memory_order_relaxed);
        unsigned finalizations =
            atomic_load_explicit(&job->finalizations, memory_order_relaxed);

        checksum ^= job->checksum;
        if (executions != 1 || finalizations != 1 ||
            job->entry_result != 0 || job->worker >= state->options.workers ||
            job->tid <= 0 || job->migration_seen ||
            job->preempt_points != expected_preempt_points ||
            job->checksum != job->expected_checksum) {
            if (valid) {
                fprintf(stderr,
                        "job validation failed: index=%zu executions=%u "
                        "finalizations=%u result=%d worker=%zu tid=%ld "
                        "migration=%d preempt_points=%" PRIu64
                        "/%" PRIu64 " checksum=%" PRIu64 "/%" PRIu64 "\n",
                        index, executions, finalizations, job->entry_result,
                        job->worker, (long)job->tid, job->migration_seen,
                        job->preempt_points, expected_preempt_points,
                        job->checksum, job->expected_checksum);
            }
            valid = false;
        }
    }
    *out_checksum = checksum;
    return valid;
}

static bool validate_stats(const struct options *options,
                           const struct rco_pool_stats *stats,
                           uint64_t worker_mask, unsigned workers_used)
{
    uint64_t expected_mask =
        options->workers == 64
            ? UINT64_MAX
            : (UINT64_C(1) << options->workers) - 1;

    return stats->submitted == options->jobs &&
           stats->completed == options->jobs && stats->cancelled == 0 &&
           stats->submitted == stats->completed + stats->cancelled &&
           stats->outstanding == 0 && stats->queued_jobs == 0 &&
           stats->pending_cancellations == 0 &&
           stats->coroutine_migrations == 0 &&
           (options->workers == 1 ||
            stats->jobs_stolen_before_start > 0) &&
           stats->steal_attempts >= stats->jobs_stolen_before_start &&
           worker_mask == expected_mask && workers_used == options->workers;
}

static int run_benchmark(const struct options *options)
{
    struct benchmark_state state = {.options = *options};
    struct benchmark_job *jobs = NULL;
    struct rco_pool *pool = NULL;
    bool mutex_initialized = false;
    bool condition_initialized = false;
    bool pool_started = false;
    bool pool_joined = false;
    int result = EXIT_FAILURE;
    int operation_result = 0;
    uint64_t wall_start_ns = 0;
    uint64_t wall_end_ns = 0;
    uint64_t cpu_start_ns = 0;
    uint64_t cpu_end_ns = 0;
    uint64_t checksum = 0;
    uint64_t worker_mask = 0;
    uint64_t wall_ns = 0;
    uint64_t cpu_ns = 0;
    unsigned workers_used = 0;
    double jobs_per_second = 0.0;
    bool report_ready = false;
    struct rco_pool_stats stats = {0};

    atomic_init(&state.worker_mask, 0);
    atomic_init(&state.worker_error, 0);
    int pthread_result = pthread_mutex_init(&state.start_mutex, NULL);
    if (pthread_result != 0) {
        fprintf(stderr, "start mutex initialization failed: %d\n",
                pthread_result);
        goto cleanup;
    }
    mutex_initialized = true;
    pthread_result = pthread_cond_init(&state.start_condition, NULL);
    if (pthread_result != 0) {
        fprintf(stderr, "start condition initialization failed: %d\n",
                pthread_result);
        goto cleanup;
    }
    condition_initialized = true;

    jobs = calloc(options->jobs, sizeof(*jobs));
    if (jobs == NULL) {
        fputs("job allocation failed\n", stderr);
        goto cleanup;
    }
    uint64_t expected_checksum = 0;
    for (size_t index = 0; index < options->jobs; ++index) {
        jobs[index].state = &state;
        jobs[index].index = index;
        jobs[index].worker = SIZE_MAX;
        jobs[index].expected_checksum =
            expected_job_checksum(index, options->iterations);
        expected_checksum ^= jobs[index].expected_checksum;
        atomic_init(&jobs[index].executions, 0);
        atomic_init(&jobs[index].finalizations, 0);
    }

    const struct rco_pool_config config = {
        .worker_count = options->workers,
        .max_jobs = options->jobs,
        .dispatch_batch = 1,
        .first_cpu = options->first_cpu,
        .pin_workers = options->first_cpu >= 0,
        .runtime = {
            .default_stack_size = RCO_STACK_SIZE_MIN,
            .max_coroutines = options->jobs + 1,
            .max_fds = options->jobs + options->workers + 16,
            .stack_cache_bytes = 0,
            .preempt_quantum_ns = options->preempt_quantum_ns,
            .preempt_signal =
                options->preempt_quantum_ns == 0 ? 0 : SIGRTMIN + 6,
        },
    };
    operation_result = rco_pool_create(&config, &pool);
    if (operation_result != 0) {
        fprintf(stderr, "pool creation failed: %d\n", operation_result);
        goto cleanup;
    }
    operation_result = rco_pool_start(pool);
    if (operation_result != 0) {
        fprintf(stderr, "pool start failed: %d\n", operation_result);
        goto cleanup;
    }
    pool_started = true;

    const struct rco_submit_options submit = {
        .affinity = RCO_AFFINITY_PREFER,
        .worker_index = 0,
    };
    for (size_t index = 0; index < options->jobs; ++index) {
        const struct rco_task_spec spec = {
            .entry = pool_bench_entry,
            .argument = &jobs[index],
            .finalizer = pool_bench_finalizer,
        };
        operation_result = rco_pool_submit(pool, &spec, &submit, NULL);
        if (operation_result != 0) {
            fprintf(stderr, "job submission failed at %zu: %d\n", index,
                    operation_result);
            goto cleanup;
        }
    }

    operation_result =
        release_start(&state, &wall_start_ns, &cpu_start_ns);
    if (operation_result != 0) {
        fprintf(stderr, "worker startup barrier failed: %d\n",
                operation_result);
        goto cleanup;
    }
    operation_result = rco_pool_wait_idle(pool, POOL_TIMEOUT_MS);
    if (operation_result != 0) {
        fprintf(stderr, "pool wait failed: %d\n", operation_result);
        goto cleanup;
    }
    operation_result = clock_ns(CLOCK_PROCESS_CPUTIME_ID, &cpu_end_ns);
    if (operation_result == 0) {
        operation_result = clock_ns(CLOCK_MONOTONIC_RAW, &wall_end_ns);
    }
    if (operation_result != 0) {
        fprintf(stderr, "elapsed clock read failed: %d\n", operation_result);
        goto cleanup;
    }
    operation_result = rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN);
    if (operation_result != 0) {
        fprintf(stderr, "pool shutdown failed: %d\n", operation_result);
        goto cleanup;
    }
    operation_result = rco_pool_join(pool, POOL_TIMEOUT_MS);
    if (operation_result != 0) {
        fprintf(stderr, "pool join failed: %d\n", operation_result);
        goto cleanup;
    }
    pool_joined = true;
    operation_result = rco_pool_get_stats(pool, &stats);
    if (operation_result != 0) {
        fprintf(stderr, "pool stats failed: %d\n", operation_result);
        goto cleanup;
    }

    worker_mask =
        atomic_load_explicit(&state.worker_mask, memory_order_relaxed);
    workers_used = bit_count(worker_mask);
    int worker_error =
        atomic_load_explicit(&state.worker_error, memory_order_relaxed);
    bool jobs_valid = validate_jobs(&state, jobs, &checksum);
    bool stats_valid =
        validate_stats(options, &stats, worker_mask, workers_used);
    bool elapsed_valid =
        wall_end_ns > wall_start_ns && cpu_end_ns >= cpu_start_ns;
    if (worker_error != 0 || !jobs_valid || !stats_valid ||
        checksum != expected_checksum || !elapsed_valid) {
        fprintf(stderr,
                "benchmark validation failed: worker_error=%d "
                "jobs_valid=%d stats_valid=%d checksum=%" PRIu64
                "/%" PRIu64 " wall=%" PRIu64 "/%" PRIu64
                " cpu=%" PRIu64 "/%" PRIu64 " mask=0x%016" PRIx64
                " used=%u/%zu submitted=%" PRIu64
                " completed=%" PRIu64 " cancelled=%" PRIu64
                " stolen=%" PRIu64 " migrations=%" PRIu64
                " outstanding=%zu queued=%zu pending=%zu\n",
                worker_error, jobs_valid, stats_valid, checksum,
                expected_checksum, wall_start_ns, wall_end_ns, cpu_start_ns,
                cpu_end_ns, worker_mask, workers_used, options->workers,
                stats.submitted, stats.completed, stats.cancelled,
                stats.jobs_stolen_before_start, stats.coroutine_migrations,
                stats.outstanding, stats.queued_jobs,
                stats.pending_cancellations);
        goto cleanup;
    }

    wall_ns = wall_end_ns - wall_start_ns;
    cpu_ns = cpu_end_ns - cpu_start_ns;
    jobs_per_second =
        (double)options->jobs * 1000000000.0 / (double)wall_ns;
    benchmark_sink = checksum | UINT64_C(1);
    report_ready = true;
    result = benchmark_sink == 0 ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
    if (pool_started && !pool_joined) {
        abort_start(&state);
        (void)rco_pool_shutdown(pool, RCO_SHUTDOWN_CANCEL);
        if (rco_pool_join(pool, POOL_TIMEOUT_MS) == 0) {
            pool_joined = true;
        } else {
            fputs("pool cleanup join failed\n", stderr);
            result = EXIT_FAILURE;
        }
    }
    bool state_releasable = pool == NULL;
    if (pool != NULL) {
        int destroy_result = rco_pool_destroy(pool);
        if (destroy_result != 0) {
            fprintf(stderr, "pool destroy failed: %d\n", destroy_result);
            result = EXIT_FAILURE;
        } else {
            pool = NULL;
            state_releasable = true;
        }
    }
    if (state_releasable) {
        free(jobs);
    }
    if (condition_initialized && state_releasable) {
        pthread_result = pthread_cond_destroy(&state.start_condition);
        if (pthread_result != 0) {
            fprintf(stderr, "start condition destroy failed: %d\n",
                    pthread_result);
            result = EXIT_FAILURE;
        }
    }
    if (mutex_initialized && state_releasable) {
        pthread_result = pthread_mutex_destroy(&state.start_mutex);
        if (pthread_result != 0) {
            fprintf(stderr, "start mutex destroy failed: %d\n",
                    pthread_result);
            result = EXIT_FAILURE;
        }
    }
    if (result == EXIT_SUCCESS && report_ready) {
        int print_result = printf(
            "{\"schema\":\"%s\",\"backend\":\"%s\",\"config\":{"
            "\"workers\":%zu,\"jobs\":%zu,\"iterations\":%" PRIu64
            ",\"preempt_quantum_ns\":%" PRIu64
            ",\"preempt_cadence_iterations\":%" PRIu64
            ",\"pin_first_cpu\":%d},\"elapsed\":{\"wall_ns\":%" PRIu64
            ",\"cpu_ns\":%" PRIu64 "},\"jobs_per_second\":%.3f,"
            "\"workers\":{\"mask\":\"0x%016" PRIx64
            "\",\"count\":%u},\"pool_stats\":{\"submitted\":%" PRIu64
            ",\"completed\":%" PRIu64 ",\"cancelled\":%" PRIu64
            ",\"jobs_stolen_before_start\":%" PRIu64
            ",\"coroutine_migrations\":%" PRIu64
            ",\"outstanding\":%zu,\"wake_writes\":%" PRIu64
            ",\"wake_coalesced\":%" PRIu64
            ",\"steal_attempts\":%" PRIu64
            ",\"queued_jobs\":%zu,\"pending_cancellations\":%zu},"
            "\"checksum\":%" PRIu64 "}\n",
            BENCH_SCHEMA, BENCH_BACKEND_IDENTITY, options->workers,
            options->jobs, options->iterations, options->preempt_quantum_ns,
            PREEMPT_CADENCE_ITERATIONS, options->first_cpu, wall_ns, cpu_ns,
            jobs_per_second, worker_mask, workers_used, stats.submitted,
            stats.completed, stats.cancelled,
            stats.jobs_stolen_before_start, stats.coroutine_migrations,
            stats.outstanding, stats.wake_writes, stats.wake_coalesced,
            stats.steal_attempts, stats.queued_jobs,
            stats.pending_cancellations, checksum);
        if (print_result < 0 || fflush(stdout) != 0) {
            fputs("JSON output failed\n", stderr);
            result = EXIT_FAILURE;
        }
    }
    return result;
}

int main(int argc, char **argv)
{
    struct options options;

    if (parse_options(argc, argv, &options) != 0 ||
        validate_options(&options) != 0) {
        return 2;
    }
    return run_benchmark(&options);
}
