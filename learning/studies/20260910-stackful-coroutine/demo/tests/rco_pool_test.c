#define _GNU_SOURCE

#include "rco_pool.h"

#include <errno.h>
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
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TEST_TIMEOUT_MS 3000
#define STEAL_WORKERS 4
#define PRODUCER_COUNT 4
#define JOBS_PER_PRODUCER 8
#define STEAL_JOBS (PRODUCER_COUNT * JOBS_PER_PRODUCER)
#define LOCAL_WORKERS 3
#define WAKE_ROUNDS 32
#define POOL_PREEMPT_QUANTUM_NS UINT64_C(1000000)
#define POOL_PREEMPT_SPIN_LIMIT UINT64_C(50000000)

_Static_assert(RCO_AFFINITY_ANY != RCO_AFFINITY_PREFER,
               "ANY and PREFER affinity must be distinct");
_Static_assert(RCO_AFFINITY_PREFER != RCO_AFFINITY_REQUIRE,
               "PREFER and REQUIRE affinity must be distinct");
_Static_assert(RCO_SHUTDOWN_DRAIN != RCO_SHUTDOWN_CANCEL,
               "DRAIN and CANCEL shutdown modes must be distinct");
_Static_assert(
    _Generic((rco_job_id_t)0, uint64_t: 1, default: 0),
    "rco_job_id_t must be uint64_t");
_Static_assert(
    _Generic(((struct rco_pool_config *)0)->worker_count,
             size_t: 1, default: 0),
    "worker_count must be size_t");
_Static_assert(
    _Generic(((struct rco_pool_config *)0)->max_jobs,
             size_t: 1, default: 0),
    "max_jobs must be size_t");
_Static_assert(
    _Generic(((struct rco_pool_config *)0)->dispatch_batch,
             size_t: 1, default: 0),
    "dispatch_batch must be size_t");
_Static_assert(
    _Generic(((struct rco_pool_config *)0)->first_cpu, int: 1, default: 0),
    "first_cpu must be int");
_Static_assert(
    _Generic(((struct rco_pool_config *)0)->pin_workers,
             bool: 1, default: 0),
    "pin_workers must be bool");
_Static_assert(
    sizeof(((struct rco_pool_config *)0)->runtime) ==
        sizeof(struct rco_config),
    "runtime must be an embedded struct rco_config");
_Static_assert(
    _Generic(((struct rco_submit_options *)0)->worker_index,
             size_t: 1, default: 0),
    "worker_index must be size_t");
_Static_assert(
    _Generic(((struct rco_pool_stats *)0)->submitted,
             uint64_t: 1, default: 0),
    "submitted must be uint64_t");
_Static_assert(
    _Generic(((struct rco_pool_stats *)0)->completed,
             uint64_t: 1, default: 0),
    "completed must be uint64_t");
_Static_assert(
    _Generic(((struct rco_pool_stats *)0)->cancelled,
             uint64_t: 1, default: 0),
    "cancelled must be uint64_t");
_Static_assert(
    _Generic(((struct rco_pool_stats *)0)->jobs_stolen_before_start,
             uint64_t: 1, default: 0),
    "jobs_stolen_before_start must be uint64_t");
_Static_assert(
    _Generic(((struct rco_pool_stats *)0)->coroutine_migrations,
             uint64_t: 1, default: 0),
    "coroutine_migrations must be uint64_t");
_Static_assert(
    _Generic(((struct rco_pool_stats *)0)->outstanding,
             size_t: 1, default: 0),
    "outstanding must be size_t");

static void check(bool condition, const char *expression, const char *file,
                  int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

struct gate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t arrived;
    bool open;
};

static struct timespec realtime_deadline(int timeout_ms)
{
    struct timespec deadline;

    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void gate_init(struct gate *gate)
{
    memset(gate, 0, sizeof(*gate));
    CHECK(pthread_mutex_init(&gate->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&gate->condition, NULL) == 0);
}

static void gate_destroy(struct gate *gate)
{
    CHECK(pthread_cond_destroy(&gate->condition) == 0);
    CHECK(pthread_mutex_destroy(&gate->mutex) == 0);
}

static void gate_arrive(struct gate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->arrived++;
    CHECK(pthread_cond_broadcast(&gate->condition) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void gate_wait_for(struct gate *gate, size_t expected)
{
    struct timespec deadline = realtime_deadline(TEST_TIMEOUT_MS);

    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    while (gate->arrived < expected) {
        int result =
            pthread_cond_timedwait(&gate->condition, &gate->mutex, &deadline);
        CHECK(result == 0);
    }
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void gate_arrive_and_wait(struct gate *gate)
{
    struct timespec deadline = realtime_deadline(TEST_TIMEOUT_MS);

    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->arrived++;
    CHECK(pthread_cond_broadcast(&gate->condition) == 0);
    while (!gate->open) {
        int result =
            pthread_cond_timedwait(&gate->condition, &gate->mutex, &deadline);
        CHECK(result == 0);
    }
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void gate_open(struct gate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->open = true;
    CHECK(pthread_cond_broadcast(&gate->condition) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static pid_t current_tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

static struct rco_pool_config pool_config(size_t workers, size_t max_jobs,
                                          size_t dispatch_batch)
{
    const struct rco_pool_config config = {
        .worker_count = workers,
        .max_jobs = max_jobs,
        .dispatch_batch = dispatch_batch,
        .first_cpu = -1,
        .pin_workers = false,
        .runtime = {
            .default_stack_size = RCO_STACK_SIZE_MIN,
            .max_coroutines = max_jobs,
            .max_fds = max_jobs + 16,
            .stack_cache_bytes = max_jobs * RCO_STACK_SIZE_MIN,
        },
    };
    return config;
}

static struct rco_submit_options submit_options(enum rco_affinity affinity,
                                                 size_t worker_index)
{
    const struct rco_submit_options options = {
        .affinity = affinity,
        .worker_index = worker_index,
    };
    return options;
}

static struct rco_pool *create_started_pool(
    const struct rco_pool_config *config)
{
    struct rco_pool *pool = NULL;

    CHECK(rco_pool_create(config, &pool) == 0);
    CHECK(pool != NULL);
    CHECK(rco_pool_start(pool) == 0);
    return pool;
}

static struct rco_pool_stats joined_stats(struct rco_pool *pool)
{
    struct rco_pool_stats stats;

    memset(&stats, 0, sizeof(stats));
    CHECK(rco_pool_get_stats(pool, &stats) == 0);
    CHECK(stats.submitted == stats.completed + stats.cancelled);
    CHECK(stats.outstanding == 0);
    CHECK(stats.coroutine_migrations == 0);
    return stats;
}

static void drain_join_destroy(struct rco_pool *pool)
{
    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN) == 0);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);
    (void)joined_stats(pool);
    CHECK(rco_pool_destroy(pool) == 0);
}

struct life_record {
    _Atomic unsigned executions;
    _Atomic unsigned finalizations;
};

static void life_init(struct life_record *life)
{
    atomic_init(&life->executions, 0);
    atomic_init(&life->finalizations, 0);
}

static void life_finalizer(void *argument)
{
    struct life_record *life = argument;

    atomic_fetch_add_explicit(&life->finalizations, 1, memory_order_relaxed);
}

struct blocking_job {
    struct life_record life;
    struct gate *gate;
    size_t expected_worker;
};

/* Occupy an OS worker so queue ownership and cancellation states are known. */
static int blocking_entry(void *argument)
{
    struct blocking_job *job = argument;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    CHECK(rco_current_worker() == job->expected_worker);
    gate_arrive_and_wait(job->gate);
    return 0;
}

static void blocking_job_init(struct blocking_job *job, struct gate *gate,
                              size_t expected_worker)
{
    memset(job, 0, sizeof(*job));
    life_init(&job->life);
    job->gate = gate;
    job->expected_worker = expected_worker;
}

static struct rco_task_spec blocking_spec(struct blocking_job *job)
{
    const struct rco_task_spec spec = {
        .entry = blocking_entry,
        .argument = job,
        .finalizer = life_finalizer,
    };
    return spec;
}

struct steal_shared {
    struct gate distribution_gate;
    struct gate fd_wait_gate;
    _Atomic size_t start_rank;
    _Atomic uint64_t worker_mask;
};

struct steal_job {
    struct life_record life;
    struct steal_shared *shared;
    int event_fd;
    size_t worker;
    pid_t tid;
    bool migration_seen;
    rco_job_id_t id;
};

static void check_job_identity(struct steal_job *job)
{
    if (rco_current_worker() != job->worker || current_tid() != job->tid) {
        job->migration_seen = true;
    }
}

static int steal_entry(void *argument)
{
    struct steal_job *job = argument;
    struct steal_shared *shared = job->shared;
    eventfd_t value = 0;
    unsigned ready = 0;

    CHECK(atomic_fetch_add_explicit(&job->life.executions, 1,
                                    memory_order_relaxed) == 0);
    job->worker = rco_current_worker();
    job->tid = current_tid();
    CHECK(job->worker < STEAL_WORKERS);
    atomic_fetch_or_explicit(&shared->worker_mask,
                             UINT64_C(1) << job->worker,
                             memory_order_relaxed);

    size_t rank =
        atomic_fetch_add_explicit(&shared->start_rank, 1, memory_order_relaxed);
    if (rank < STEAL_WORKERS - 1) {
        gate_arrive_and_wait(&shared->distribution_gate);
    }

    CHECK(rco_yield() == 0);
    check_job_identity(job);
    CHECK(rco_preempt_point() == 0);
    check_job_identity(job);

    gate_arrive(&shared->fd_wait_gate);
    CHECK(rco_wait_fd(job->event_fd, RCO_EVENT_READ, TEST_TIMEOUT_MS,
                      &ready) == 0);
    CHECK((ready & RCO_EVENT_READ) != 0);
    CHECK(eventfd_read(job->event_fd, &value) == 0);
    CHECK(value == 1);
    check_job_identity(job);
    return 0;
}

struct producer_case {
    struct rco_pool *pool;
    struct gate *start_gate;
    struct steal_job *jobs;
    size_t first;
    size_t count;
};

static void *producer_thread(void *argument)
{
    struct producer_case *producer = argument;
    const struct rco_submit_options options =
        submit_options(RCO_AFFINITY_ANY, 0);

    gate_arrive_and_wait(producer->start_gate);
    for (size_t offset = 0; offset < producer->count; ++offset) {
        struct steal_job *job = &producer->jobs[producer->first + offset];
        const struct rco_task_spec spec = {
            .entry = steal_entry,
            .argument = job,
            .finalizer = life_finalizer,
        };
        CHECK(rco_pool_submit(producer->pool, &spec, &options, &job->id) ==
              0);
        CHECK(job->id != 0);
    }
    return NULL;
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

static bool signal_sets_equal(const sigset_t *left, const sigset_t *right)
{
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (sigismember(left, signal_number) !=
            sigismember(right, signal_number)) {
            return false;
        }
    }
    return true;
}

static void test_concurrent_submit_steals_before_start_without_migration(void)
{
    struct rco_pool_config config =
        pool_config(STEAL_WORKERS, STEAL_JOBS + 8, 1);
    struct rco_pool *pool = create_started_pool(&config);
    struct gate blocker_gate;
    struct blocking_job blocker;
    struct steal_shared shared;
    struct steal_job jobs[STEAL_JOBS];
    struct gate producer_gate;
    struct producer_case producers[PRODUCER_COUNT];
    pthread_t producer_threads[PRODUCER_COUNT];
    const struct rco_submit_options require_zero =
        submit_options(RCO_AFFINITY_REQUIRE, 0);
    rco_job_id_t blocker_id = 0;

    gate_init(&blocker_gate);
    blocking_job_init(&blocker, &blocker_gate, 0);
    struct rco_task_spec blocker_task = blocking_spec(&blocker);
    CHECK(rco_pool_submit(pool, &blocker_task, &require_zero, &blocker_id) ==
          0);
    gate_wait_for(&blocker_gate, 1);

    gate_init(&shared.distribution_gate);
    gate_init(&shared.fd_wait_gate);
    atomic_init(&shared.start_rank, 0);
    atomic_init(&shared.worker_mask, 0);
    for (size_t index = 0; index < STEAL_JOBS; ++index) {
        memset(&jobs[index], 0, sizeof(jobs[index]));
        life_init(&jobs[index].life);
        jobs[index].shared = &shared;
        jobs[index].event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(jobs[index].event_fd >= 0);
    }

    gate_init(&producer_gate);
    for (size_t producer = 0; producer < PRODUCER_COUNT; ++producer) {
        producers[producer] = (struct producer_case){
            .pool = pool,
            .start_gate = &producer_gate,
            .jobs = jobs,
            .first = producer * JOBS_PER_PRODUCER,
            .count = JOBS_PER_PRODUCER,
        };
        CHECK(pthread_create(&producer_threads[producer], NULL,
                             producer_thread, &producers[producer]) == 0);
    }
    gate_wait_for(&producer_gate, PRODUCER_COUNT);
    gate_open(&producer_gate);
    for (size_t producer = 0; producer < PRODUCER_COUNT; ++producer) {
        CHECK(pthread_join(producer_threads[producer], NULL) == 0);
    }

    for (size_t left = 0; left < STEAL_JOBS; ++left) {
        for (size_t right = left + 1; right < STEAL_JOBS; ++right) {
            CHECK(jobs[left].id != jobs[right].id);
        }
    }

    gate_wait_for(&shared.distribution_gate, STEAL_WORKERS - 1);
    gate_open(&shared.distribution_gate);
    gate_wait_for(&shared.fd_wait_gate, STEAL_JOBS);
    gate_open(&blocker_gate);
    for (size_t index = 0; index < STEAL_JOBS; ++index) {
        CHECK(eventfd_write(jobs[index].event_fd, 1) == 0);
    }

    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN) == 0);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);

    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == STEAL_JOBS + 1);
    CHECK(stats.completed == STEAL_JOBS + 1);
    CHECK(stats.cancelled == 0);
    CHECK(stats.jobs_stolen_before_start > 0);
    CHECK(bit_count(atomic_load_explicit(&shared.worker_mask,
                                         memory_order_relaxed)) >= 2);
    CHECK(atomic_load_explicit(&blocker.life.executions,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&blocker.life.finalizations,
                               memory_order_relaxed) == 1);

    for (size_t index = 0; index < STEAL_JOBS; ++index) {
        CHECK(atomic_load_explicit(&jobs[index].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&jobs[index].life.finalizations,
                                   memory_order_relaxed) == 1);
        CHECK(!jobs[index].migration_seen);
        CHECK(jobs[index].worker != 0);
        CHECK(close(jobs[index].event_fd) == 0);
    }

    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&producer_gate);
    gate_destroy(&shared.fd_wait_gate);
    gate_destroy(&shared.distribution_gate);
    gate_destroy(&blocker_gate);
}

struct local_child {
    struct life_record life;
    size_t expected_worker;
    pid_t expected_tid;
    size_t worker;
    pid_t tid;
};

static int local_child_entry(void *argument)
{
    struct local_child *child = argument;

    atomic_fetch_add_explicit(&child->life.executions, 1,
                              memory_order_relaxed);
    child->worker = rco_current_worker();
    child->tid = current_tid();
    CHECK(child->worker == child->expected_worker);
    CHECK(child->tid == child->expected_tid);
    return 0;
}

struct local_parent {
    struct life_record life;
    struct local_child child;
    struct gate *fd_wait_gate;
    int event_fd;
    size_t expected_worker;
    size_t worker;
    pid_t tid;
    bool migration_seen;
    rco_job_id_t local_id;
};

static void check_parent_identity(struct local_parent *parent)
{
    if (rco_current_worker() != parent->worker ||
        current_tid() != parent->tid) {
        parent->migration_seen = true;
    }
}

static int local_parent_entry(void *argument)
{
    struct local_parent *parent = argument;
    eventfd_t value = 0;
    unsigned ready = 0;

    atomic_fetch_add_explicit(&parent->life.executions, 1,
                              memory_order_relaxed);
    parent->worker = rco_current_worker();
    parent->tid = current_tid();
    CHECK(parent->worker == parent->expected_worker);

    CHECK(rco_yield() == 0);
    check_parent_identity(parent);
    CHECK(rco_preempt_point() == 0);
    check_parent_identity(parent);
    gate_arrive(parent->fd_wait_gate);
    CHECK(rco_wait_fd(parent->event_fd, RCO_EVENT_READ, TEST_TIMEOUT_MS,
                      &ready) == 0);
    CHECK((ready & RCO_EVENT_READ) != 0);
    CHECK(eventfd_read(parent->event_fd, &value) == 0);
    CHECK(value == 1);
    check_parent_identity(parent);
    CHECK(rco_sleep_ms(1) == 0);
    check_parent_identity(parent);

    parent->child.expected_worker = parent->worker;
    parent->child.expected_tid = parent->tid;
    const struct rco_task_spec child_spec = {
        .entry = local_child_entry,
        .argument = &parent->child,
        .finalizer = life_finalizer,
    };
    CHECK(rco_submit_local(&child_spec, &parent->local_id) == 0);
    CHECK(parent->local_id != 0);
    return 0;
}

static void test_require_and_local_submit_stay_on_the_owner(void)
{
    struct rco_pool_config config =
        pool_config(LOCAL_WORKERS, LOCAL_WORKERS * 4, 2);
    struct rco_pool *pool = create_started_pool(&config);
    struct gate fd_wait_gate;
    struct local_parent parents[LOCAL_WORKERS];

    gate_init(&fd_wait_gate);
    for (size_t worker = 0; worker < LOCAL_WORKERS; ++worker) {
        memset(&parents[worker], 0, sizeof(parents[worker]));
        life_init(&parents[worker].life);
        life_init(&parents[worker].child.life);
        parents[worker].fd_wait_gate = &fd_wait_gate;
        parents[worker].event_fd =
            eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        parents[worker].expected_worker = worker;
        CHECK(parents[worker].event_fd >= 0);

        const struct rco_task_spec spec = {
            .entry = local_parent_entry,
            .argument = &parents[worker],
            .finalizer = life_finalizer,
        };
        const struct rco_submit_options options =
            submit_options(RCO_AFFINITY_REQUIRE, worker);
        CHECK(rco_pool_submit(pool, &spec, &options, NULL) == 0);
    }

    gate_wait_for(&fd_wait_gate, LOCAL_WORKERS);
    for (size_t worker = 0; worker < LOCAL_WORKERS; ++worker) {
        CHECK(eventfd_write(parents[worker].event_fd, 1) == 0);
    }

    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN) == 0);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);
    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == LOCAL_WORKERS * 2);
    CHECK(stats.completed == LOCAL_WORKERS * 2);

    for (size_t worker = 0; worker < LOCAL_WORKERS; ++worker) {
        CHECK(parents[worker].worker == worker);
        CHECK(!parents[worker].migration_seen);
        CHECK(atomic_load_explicit(&parents[worker].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&parents[worker].life.finalizations,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&parents[worker].child.life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(
                  &parents[worker].child.life.finalizations,
                  memory_order_relaxed) == 1);
        CHECK(close(parents[worker].event_fd) == 0);
    }
    for (size_t left = 0; left < LOCAL_WORKERS; ++left) {
        for (size_t right = left + 1; right < LOCAL_WORKERS; ++right) {
            CHECK(parents[left].tid != parents[right].tid);
        }
    }

    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&fd_wait_gate);
}

struct wake_job {
    struct life_record life;
    struct gate *finished;
    size_t expected_worker;
};

static int wake_entry(void *argument)
{
    struct wake_job *job = argument;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    CHECK(rco_current_worker() == job->expected_worker);
    gate_arrive(job->finished);
    return 0;
}

static void test_idle_workers_are_woken_without_lost_submissions(void)
{
    struct rco_pool_config config = pool_config(2, 4, 1);
    struct rco_pool *pool = create_started_pool(&config);
    struct gate finished;
    struct wake_job jobs[WAKE_ROUNDS];

    gate_init(&finished);
    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    for (size_t round = 0; round < WAKE_ROUNDS; ++round) {
        memset(&jobs[round], 0, sizeof(jobs[round]));
        life_init(&jobs[round].life);
        jobs[round].finished = &finished;
        jobs[round].expected_worker = round % 2;
        const struct rco_task_spec spec = {
            .entry = wake_entry,
            .argument = &jobs[round],
            .finalizer = life_finalizer,
        };
        const struct rco_submit_options options =
            submit_options(RCO_AFFINITY_REQUIRE,
                           jobs[round].expected_worker);

        CHECK(rco_pool_submit(pool, &spec, &options, NULL) == 0);
        gate_wait_for(&finished, round + 1);
        CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    }

    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN) == 0);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);
    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == WAKE_ROUNDS);
    CHECK(stats.completed == WAKE_ROUNDS);
    for (size_t round = 0; round < WAKE_ROUNDS; ++round) {
        CHECK(atomic_load_explicit(&jobs[round].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&jobs[round].life.finalizations,
                                   memory_order_relaxed) == 1);
    }

    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&finished);
}

struct count_job {
    struct life_record life;
};

static int count_entry(void *argument)
{
    struct count_job *job = argument;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    return 0;
}

static struct rco_task_spec count_spec(struct count_job *job)
{
    const struct rco_task_spec spec = {
        .entry = count_entry,
        .argument = job,
        .finalizer = life_finalizer,
    };
    return spec;
}

struct cancelling_wait_job {
    struct life_record life;
    struct gate *waiting;
    int event_fd;
    int wait_result;
};

static int cancelling_wait_entry(void *argument)
{
    struct cancelling_wait_job *job = argument;
    unsigned ready = 0;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    gate_arrive(job->waiting);
    job->wait_result =
        rco_wait_fd(job->event_fd, RCO_EVENT_READ, -1, &ready);
    return 0;
}

struct completing_job {
    struct life_record life;
    struct gate *race_gate;
};

static int completing_entry(void *argument)
{
    struct completing_job *job = argument;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    gate_arrive_and_wait(job->race_gate);
    return 0;
}

struct cancel_thread_case {
    struct rco_pool *pool;
    struct gate *race_gate;
    rco_job_id_t id;
    int result;
};

static void *cancel_thread(void *argument)
{
    struct cancel_thread_case *test_case = argument;

    gate_arrive_and_wait(test_case->race_gate);
    test_case->result = rco_pool_cancel(test_case->pool, test_case->id);
    return NULL;
}

static void test_cancel_queued_claimed_running_and_completing_jobs(void)
{
    enum {
        WORKERS = 2,
        QUEUED = 4,
        CLAIMED = 7,
    };
    struct rco_pool_config config = pool_config(WORKERS, 32, 8);
    struct rco_pool *pool = create_started_pool(&config);
    struct gate initial_gate;
    struct blocking_job initial[WORKERS];
    struct count_job queued[QUEUED];
    rco_job_id_t queued_ids[QUEUED];
    struct gate claimed_gate;
    struct blocking_job claimed_head;
    struct count_job claimed[CLAIMED];
    rco_job_id_t claimed_ids[CLAIMED];
    struct gate wait_gate;
    struct cancelling_wait_job running;
    rco_job_id_t running_id = 0;
    const struct rco_submit_options any_zero =
        submit_options(RCO_AFFINITY_ANY, 0);

    gate_init(&initial_gate);
    for (size_t worker = 0; worker < WORKERS; ++worker) {
        blocking_job_init(&initial[worker], &initial_gate, worker);
        struct rco_task_spec spec = blocking_spec(&initial[worker]);
        const struct rco_submit_options options =
            submit_options(RCO_AFFINITY_REQUIRE, worker);
        CHECK(rco_pool_submit(pool, &spec, &options, NULL) == 0);
    }
    gate_wait_for(&initial_gate, WORKERS);

    for (size_t index = 0; index < QUEUED; ++index) {
        life_init(&queued[index].life);
        struct rco_task_spec spec = count_spec(&queued[index]);
        CHECK(rco_pool_submit(pool, &spec, &any_zero,
                              &queued_ids[index]) == 0);
        CHECK(rco_pool_cancel(pool, queued_ids[index]) == 0);
        int repeated_cancel =
            rco_pool_cancel(pool, queued_ids[index]);
        CHECK(repeated_cancel == 0 || repeated_cancel == -EALREADY ||
              repeated_cancel == -ESRCH);
    }

    /*
     * Worker 0 claims one dispatch batch after its initial blocker leaves.
     * The first claimed job blocks before stack allocation for the rest.
     */
    gate_init(&claimed_gate);
    blocking_job_init(&claimed_head, &claimed_gate, 0);
    struct rco_task_spec claimed_head_spec = blocking_spec(&claimed_head);
    const struct rco_submit_options require_zero =
        submit_options(RCO_AFFINITY_REQUIRE, 0);
    CHECK(rco_pool_submit(pool, &claimed_head_spec, &require_zero, NULL) == 0);
    for (size_t index = 0; index < CLAIMED; ++index) {
        life_init(&claimed[index].life);
        struct rco_task_spec spec = count_spec(&claimed[index]);
        CHECK(rco_pool_submit(pool, &spec, &require_zero,
                              &claimed_ids[index]) == 0);
    }

    gate_init(&wait_gate);
    memset(&running, 0, sizeof(running));
    life_init(&running.life);
    running.waiting = &wait_gate;
    running.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(running.event_fd >= 0);
    const struct rco_task_spec running_spec = {
        .entry = cancelling_wait_entry,
        .argument = &running,
        .finalizer = life_finalizer,
    };
    const struct rco_submit_options require_one =
        submit_options(RCO_AFFINITY_REQUIRE, 1);
    CHECK(rco_pool_submit(pool, &running_spec, &require_one, &running_id) ==
          0);

    gate_open(&initial_gate);
    gate_wait_for(&claimed_gate, 1);
    gate_wait_for(&wait_gate, 1);
    for (size_t index = 0; index < CLAIMED; ++index) {
        CHECK(rco_pool_cancel(pool, claimed_ids[index]) == 0);
    }
    CHECK(rco_pool_cancel(pool, running_id) == 0);
    gate_open(&claimed_gate);
    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    CHECK(running.wait_result == -ECANCELED);

    struct gate completion_race;
    struct completing_job completing;
    struct cancel_thread_case canceller;
    pthread_t thread;
    rco_job_id_t completing_id = 0;

    gate_init(&completion_race);
    memset(&completing, 0, sizeof(completing));
    life_init(&completing.life);
    completing.race_gate = &completion_race;
    const struct rco_task_spec completing_spec = {
        .entry = completing_entry,
        .argument = &completing,
        .finalizer = life_finalizer,
    };
    CHECK(rco_pool_submit(pool, &completing_spec, &require_zero,
                          &completing_id) == 0);
    canceller = (struct cancel_thread_case){
        .pool = pool,
        .race_gate = &completion_race,
        .id = completing_id,
        .result = INT_MIN,
    };
    CHECK(pthread_create(&thread, NULL, cancel_thread, &canceller) == 0);
    gate_wait_for(&completion_race, 2);
    gate_open(&completion_race);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(canceller.result == 0 || canceller.result == -ESRCH ||
          canceller.result == -EALREADY);

    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN) == 0);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);
    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == WORKERS + QUEUED + 1 + CLAIMED + 1 + 1);
    CHECK(stats.completed == 3 || stats.completed == 4);
    CHECK(stats.cancelled == 12 || stats.cancelled == 13);

    for (size_t worker = 0; worker < WORKERS; ++worker) {
        CHECK(atomic_load_explicit(&initial[worker].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&initial[worker].life.finalizations,
                                   memory_order_relaxed) == 1);
    }
    for (size_t index = 0; index < QUEUED; ++index) {
        CHECK(atomic_load_explicit(&queued[index].life.executions,
                                   memory_order_relaxed) == 0);
        CHECK(atomic_load_explicit(&queued[index].life.finalizations,
                                   memory_order_relaxed) == 1);
    }
    CHECK(atomic_load_explicit(&claimed_head.life.executions,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&claimed_head.life.finalizations,
                               memory_order_relaxed) == 1);
    for (size_t index = 0; index < CLAIMED; ++index) {
        CHECK(atomic_load_explicit(&claimed[index].life.executions,
                                   memory_order_relaxed) == 0);
        CHECK(atomic_load_explicit(&claimed[index].life.finalizations,
                                   memory_order_relaxed) == 1);
    }
    CHECK(atomic_load_explicit(&running.life.executions,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&running.life.finalizations,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&completing.life.executions,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&completing.life.finalizations,
                               memory_order_relaxed) == 1);

    CHECK(close(running.event_fd) == 0);
    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&completion_race);
    gate_destroy(&wait_gate);
    gate_destroy(&claimed_gate);
    gate_destroy(&initial_gate);
}

static void test_capacity_stale_handles_and_shutdown_rejection(void)
{
    struct rco_pool_config config = pool_config(1, 2, 1);
    struct rco_pool *pool = create_started_pool(&config);
    const struct rco_submit_options require_zero =
        submit_options(RCO_AFFINITY_REQUIRE, 0);
    struct gate first_gate;
    struct blocking_job first;
    struct count_job queued;
    struct count_job rejected;
    rco_job_id_t queued_id = 0;

    gate_init(&first_gate);
    blocking_job_init(&first, &first_gate, 0);
    life_init(&queued.life);
    life_init(&rejected.life);
    struct rco_task_spec first_spec = blocking_spec(&first);
    struct rco_task_spec queued_spec = count_spec(&queued);
    struct rco_task_spec rejected_spec = count_spec(&rejected);
    CHECK(rco_pool_submit(pool, &first_spec, &require_zero, NULL) == 0);
    gate_wait_for(&first_gate, 1);
    CHECK(rco_pool_submit(pool, &queued_spec, &require_zero, &queued_id) ==
          0);
    CHECK(rco_pool_submit(pool, &rejected_spec, &require_zero, NULL) ==
          -EAGAIN);
    CHECK(rco_pool_cancel(pool, queued_id) == 0);
    gate_open(&first_gate);
    CHECK(rco_pool_wait_idle(pool, TEST_TIMEOUT_MS) == 0);

    struct gate replacement_gate;
    struct blocking_job replacement;
    rco_job_id_t replacement_id = 0;

    gate_init(&replacement_gate);
    blocking_job_init(&replacement, &replacement_gate, 0);
    struct rco_task_spec replacement_spec = blocking_spec(&replacement);
    CHECK(rco_pool_submit(pool, &replacement_spec, &require_zero,
                          &replacement_id) == 0);
    CHECK(replacement_id != queued_id);
    gate_wait_for(&replacement_gate, 1);
    CHECK(rco_pool_cancel(pool, queued_id) == -ESRCH);
    CHECK(rco_pool_wait_idle(pool, 0) == -ETIMEDOUT);
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_CANCEL) == 0);
    CHECK(rco_pool_submit(pool, &rejected_spec, &require_zero, NULL) < 0);
    gate_open(&replacement_gate);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);

    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == 3);
    CHECK(atomic_load_explicit(&first.life.finalizations,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&queued.life.executions,
                               memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&queued.life.finalizations,
                               memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&rejected.life.executions,
                               memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&rejected.life.finalizations,
                               memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&replacement.life.finalizations,
                               memory_order_relaxed) == 1);

    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&replacement_gate);
    gate_destroy(&first_gate);
}

struct drain_job {
    struct life_record life;
    struct gate *waiting;
    int event_fd;
};

static int drain_entry(void *argument)
{
    struct drain_job *job = argument;
    eventfd_t value = 0;
    unsigned ready = 0;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    gate_arrive(job->waiting);
    CHECK(rco_wait_fd(job->event_fd, RCO_EVENT_READ, TEST_TIMEOUT_MS,
                      &ready) == 0);
    CHECK((ready & RCO_EVENT_READ) != 0);
    CHECK(eventfd_read(job->event_fd, &value) == 0);
    return 0;
}

static void test_shutdown_drain_completes_accepted_jobs(void)
{
    enum { WORKERS = 3, JOBS = 9 };
    struct rco_pool_config config = pool_config(WORKERS, JOBS + 2, 2);
    struct rco_pool *pool = create_started_pool(&config);
    struct gate waiting;
    struct drain_job jobs[JOBS];
    struct count_job rejected;
    const struct rco_submit_options any_zero =
        submit_options(RCO_AFFINITY_ANY, 0);

    gate_init(&waiting);
    for (size_t index = 0; index < JOBS; ++index) {
        memset(&jobs[index], 0, sizeof(jobs[index]));
        life_init(&jobs[index].life);
        jobs[index].waiting = &waiting;
        jobs[index].event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(jobs[index].event_fd >= 0);
        const struct rco_task_spec spec = {
            .entry = drain_entry,
            .argument = &jobs[index],
            .finalizer = life_finalizer,
        };
        CHECK(rco_pool_submit(pool, &spec, &any_zero, NULL) == 0);
    }
    gate_wait_for(&waiting, JOBS);
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_DRAIN) == 0);

    life_init(&rejected.life);
    struct rco_task_spec rejected_spec = count_spec(&rejected);
    CHECK(rco_pool_submit(pool, &rejected_spec, &any_zero, NULL) < 0);
    for (size_t index = 0; index < JOBS; ++index) {
        CHECK(eventfd_write(jobs[index].event_fd, 1) == 0);
    }
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);

    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == JOBS);
    CHECK(stats.completed == JOBS);
    CHECK(stats.cancelled == 0);
    for (size_t index = 0; index < JOBS; ++index) {
        CHECK(atomic_load_explicit(&jobs[index].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&jobs[index].life.finalizations,
                                   memory_order_relaxed) == 1);
        CHECK(close(jobs[index].event_fd) == 0);
    }
    CHECK(atomic_load_explicit(&rejected.life.finalizations,
                               memory_order_relaxed) == 0);

    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&waiting);
}

static void test_shutdown_cancel_cancels_every_remaining_job(void)
{
    enum { WORKERS = 2, QUEUED = 8 };
    struct rco_pool_config config = pool_config(WORKERS, WORKERS + QUEUED, 4);
    struct rco_pool *pool = create_started_pool(&config);
    struct gate blocker_gate;
    struct blocking_job blockers[WORKERS];
    struct count_job queued[QUEUED];
    const struct rco_submit_options any_zero =
        submit_options(RCO_AFFINITY_ANY, 0);

    gate_init(&blocker_gate);
    for (size_t worker = 0; worker < WORKERS; ++worker) {
        blocking_job_init(&blockers[worker], &blocker_gate, worker);
        struct rco_task_spec spec = blocking_spec(&blockers[worker]);
        const struct rco_submit_options options =
            submit_options(RCO_AFFINITY_REQUIRE, worker);
        CHECK(rco_pool_submit(pool, &spec, &options, NULL) == 0);
    }
    gate_wait_for(&blocker_gate, WORKERS);

    for (size_t index = 0; index < QUEUED; ++index) {
        life_init(&queued[index].life);
        struct rco_task_spec spec = count_spec(&queued[index]);
        CHECK(rco_pool_submit(pool, &spec, &any_zero, NULL) == 0);
    }
    CHECK(rco_pool_shutdown(pool, RCO_SHUTDOWN_CANCEL) == 0);
    struct rco_task_spec rejected_spec = count_spec(&queued[0]);
    CHECK(rco_pool_submit(pool, &rejected_spec, &any_zero, NULL) < 0);
    gate_open(&blocker_gate);
    CHECK(rco_pool_join(pool, TEST_TIMEOUT_MS) == 0);

    struct rco_pool_stats stats = joined_stats(pool);
    CHECK(stats.submitted == WORKERS + QUEUED);
    CHECK(stats.completed == 0);
    CHECK(stats.cancelled == WORKERS + QUEUED);
    for (size_t worker = 0; worker < WORKERS; ++worker) {
        CHECK(atomic_load_explicit(&blockers[worker].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&blockers[worker].life.finalizations,
                                   memory_order_relaxed) == 1);
    }
    for (size_t index = 0; index < QUEUED; ++index) {
        CHECK(atomic_load_explicit(&queued[index].life.executions,
                                   memory_order_relaxed) == 0);
        CHECK(atomic_load_explicit(&queued[index].life.finalizations,
                                   memory_order_relaxed) == 1);
    }

    CHECK(rco_pool_destroy(pool) == 0);
    gate_destroy(&blocker_gate);
}

struct affinity_probe {
    int cpu;
};

static void *probe_affinity(void *argument)
{
    const struct affinity_probe *probe = argument;
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(probe->cpu, &set);
    return (void *)(intptr_t)pthread_setaffinity_np(
        pthread_self(), sizeof(set), &set);
}

struct cpu_job {
    struct life_record life;
    size_t expected_worker;
    int expected_cpu;
    int observed_cpu;
};

static int cpu_entry(void *argument)
{
    struct cpu_job *job = argument;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    CHECK(rco_current_worker() == job->expected_worker);
    job->observed_cpu = sched_getcpu();
    return 0;
}

static int first_contiguous_cpu_pair(const cpu_set_t *allowed)
{
    for (int cpu = 0; cpu + 1 < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, allowed) && CPU_ISSET(cpu + 1, allowed)) {
            return cpu;
        }
    }
    return -1;
}

static void test_requested_worker_cpu_pinning_when_permitted(void)
{
    enum { WORKERS = 2 };
    cpu_set_t allowed;

    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        fprintf(stderr, "SKIP affinity assertion: sched_getaffinity: %s\n",
                strerror(errno));
        return;
    }
    int first_cpu = first_contiguous_cpu_pair(&allowed);
    if (first_cpu < 0) {
        fputs("SKIP affinity assertion: no contiguous allowed CPU pair\n",
              stderr);
        return;
    }

    struct affinity_probe probe = {.cpu = first_cpu};
    pthread_t probe_thread;
    void *probe_result = NULL;
    CHECK(pthread_create(&probe_thread, NULL, probe_affinity, &probe) == 0);
    CHECK(pthread_join(probe_thread, &probe_result) == 0);
    if ((int)(intptr_t)probe_result != 0) {
        fprintf(stderr,
                "SKIP affinity assertion: pthread_setaffinity_np: %s\n",
                strerror((int)(intptr_t)probe_result));
        return;
    }

    struct rco_pool_config config = pool_config(WORKERS, 4, 1);
    config.first_cpu = first_cpu;
    config.pin_workers = true;
    struct rco_pool *pool = create_started_pool(&config);
    struct cpu_job jobs[WORKERS];

    for (size_t worker = 0; worker < WORKERS; ++worker) {
        memset(&jobs[worker], 0, sizeof(jobs[worker]));
        life_init(&jobs[worker].life);
        jobs[worker].expected_worker = worker;
        jobs[worker].expected_cpu = first_cpu + (int)worker;
        jobs[worker].observed_cpu = -1;
        const struct rco_task_spec spec = {
            .entry = cpu_entry,
            .argument = &jobs[worker],
            .finalizer = life_finalizer,
        };
        const struct rco_submit_options options =
            submit_options(RCO_AFFINITY_REQUIRE, worker);
        CHECK(rco_pool_submit(pool, &spec, &options, NULL) == 0);
    }

    drain_join_destroy(pool);
    for (size_t worker = 0; worker < WORKERS; ++worker) {
        CHECK(jobs[worker].observed_cpu == jobs[worker].expected_cpu);
        CHECK(atomic_load_explicit(&jobs[worker].life.finalizations,
                                   memory_order_relaxed) == 1);
    }
}

struct pool_preempt_case {
    _Atomic unsigned progress[2];
    _Atomic unsigned observations[2];
};

struct pool_preempt_job {
    struct life_record life;
    struct pool_preempt_case *shared;
    size_t index;
};

static int pool_preempt_entry(void *argument)
{
    struct pool_preempt_job *job = argument;
    size_t peer = 1 - job->index;
    unsigned peer_progress =
        atomic_load_explicit(&job->shared->progress[peer],
                             memory_order_relaxed);
    uint64_t deadline =
        (uint64_t)time(NULL) + (TEST_TIMEOUT_MS / 1000) + 1;

    atomic_fetch_add_explicit(&job->life.executions, 1,
                              memory_order_relaxed);
    for (uint64_t iteration = 0;
         iteration < POOL_PREEMPT_SPIN_LIMIT &&
         (uint64_t)time(NULL) <= deadline;
         ++iteration) {
        atomic_fetch_add_explicit(&job->shared->progress[job->index], 1,
                                  memory_order_relaxed);
        CHECK(rco_preempt_point() == 0);
        unsigned observed =
            atomic_load_explicit(&job->shared->progress[peer],
                                 memory_order_relaxed);
        if (observed != peer_progress) {
            peer_progress = observed;
            atomic_fetch_add_explicit(
                &job->shared->observations[job->index], 1,
                memory_order_relaxed);
        }
        if (atomic_load_explicit(
                &job->shared->observations[0], memory_order_relaxed) != 0 &&
            atomic_load_explicit(
                &job->shared->observations[1], memory_order_relaxed) != 0) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

static void test_pool_workers_support_deferred_preemption(void)
{
    struct rco_pool_config config = pool_config(2, 4, 1);
    config.runtime.preempt_quantum_ns = POOL_PREEMPT_QUANTUM_NS;
    config.runtime.preempt_signal = SIGRTMIN + 6;
    struct sigaction original_action;
    struct sigaction expected_action;

    CHECK(config.runtime.preempt_signal <= SIGRTMAX);
    CHECK(sigaction(config.runtime.preempt_signal, NULL, &original_action) ==
          0);
    CHECK(sigaction(config.runtime.preempt_signal, NULL, &expected_action) ==
          0);

    struct rco_pool *pool = create_started_pool(&config);
    struct pool_preempt_case shared;
    struct pool_preempt_job jobs[2];
    memset(&shared, 0, sizeof(shared));
    const struct rco_submit_options require_zero =
        submit_options(RCO_AFFINITY_REQUIRE, 0);
    for (size_t index = 0; index < 2; ++index) {
        memset(&jobs[index], 0, sizeof(jobs[index]));
        life_init(&jobs[index].life);
        jobs[index].shared = &shared;
        jobs[index].index = index;
        const struct rco_task_spec spec = {
            .entry = pool_preempt_entry,
            .argument = &jobs[index],
            .finalizer = life_finalizer,
        };
        CHECK(rco_pool_submit(pool, &spec, &require_zero, NULL) == 0);
    }

    drain_join_destroy(pool);
    CHECK(atomic_load_explicit(&shared.observations[0],
                               memory_order_relaxed) != 0);
    CHECK(atomic_load_explicit(&shared.observations[1],
                               memory_order_relaxed) != 0);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(atomic_load_explicit(&jobs[index].life.executions,
                                   memory_order_relaxed) == 1);
        CHECK(atomic_load_explicit(&jobs[index].life.finalizations,
                                   memory_order_relaxed) == 1);
    }

    struct sigaction restored_action;
    CHECK(sigaction(config.runtime.preempt_signal, NULL, &restored_action) ==
          0);
    CHECK(restored_action.sa_handler == expected_action.sa_handler);
    CHECK((restored_action.sa_flags &
           (SA_SIGINFO | SA_ONSTACK | SA_RESTART | SA_NODEFER)) ==
          (expected_action.sa_flags &
           (SA_SIGINFO | SA_ONSTACK | SA_RESTART | SA_NODEFER)));
    CHECK(signal_sets_equal(&restored_action.sa_mask,
                            &expected_action.sa_mask));
    CHECK(sigaction(config.runtime.preempt_signal, &original_action, NULL) ==
          0);
}

static void check_public_function_signatures(void)
{
    int (*create_fn)(const struct rco_pool_config *, struct rco_pool **) =
        rco_pool_create;
    int (*start_fn)(struct rco_pool *) = rco_pool_start;
    int (*submit_fn)(struct rco_pool *, const struct rco_task_spec *,
                     const struct rco_submit_options *, rco_job_id_t *) =
        rco_pool_submit;
    int (*cancel_fn)(struct rco_pool *, rco_job_id_t) = rco_pool_cancel;
    int (*shutdown_fn)(struct rco_pool *, enum rco_shutdown_mode) =
        rco_pool_shutdown;
    int (*wait_idle_fn)(struct rco_pool *, int) = rco_pool_wait_idle;
    int (*join_fn)(struct rco_pool *, int) = rco_pool_join;
    int (*get_stats_fn)(const struct rco_pool *, struct rco_pool_stats *) =
        rco_pool_get_stats;
    int (*destroy_fn)(struct rco_pool *) = rco_pool_destroy;
    size_t (*current_worker_fn)(void) = rco_current_worker;
    int (*submit_local_fn)(const struct rco_task_spec *, rco_job_id_t *) =
        rco_submit_local;

    CHECK(create_fn != NULL);
    CHECK(start_fn != NULL);
    CHECK(submit_fn != NULL);
    CHECK(cancel_fn != NULL);
    CHECK(shutdown_fn != NULL);
    CHECK(wait_idle_fn != NULL);
    CHECK(join_fn != NULL);
    CHECK(get_stats_fn != NULL);
    CHECK(destroy_fn != NULL);
    CHECK(current_worker_fn != NULL);
    CHECK(submit_local_fn != NULL);
}

int main(void)
{
    check_public_function_signatures();
    test_concurrent_submit_steals_before_start_without_migration();
    test_require_and_local_submit_stay_on_the_owner();
    test_idle_workers_are_woken_without_lost_submissions();
    test_cancel_queued_claimed_running_and_completing_jobs();
    test_capacity_stale_handles_and_shutdown_rejection();
    test_shutdown_drain_completes_accepted_jobs();
    test_shutdown_cancel_cancels_every_remaining_job();
    test_requested_worker_cpu_pinning_when_permitted();
    test_pool_workers_support_deferred_preemption();
    puts("rco pool contract tests passed: 9 suites");
    return EXIT_SUCCESS;
}
