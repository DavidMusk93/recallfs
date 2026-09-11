#define _GNU_SOURCE

#include "rco_pool.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define RCO_POOL_NO_INDEX SIZE_MAX

enum rco_pool_state {
    RCO_POOL_CREATED,
    RCO_POOL_STARTING,
    RCO_POOL_RUNNING,
    RCO_POOL_DRAINING,
    RCO_POOL_CANCELLING,
    RCO_POOL_JOINED,
};

enum rco_pool_job_state {
    RCO_POOL_JOB_FREE,
    RCO_POOL_JOB_QUEUED,
    RCO_POOL_JOB_RESERVED,
    RCO_POOL_JOB_SPAWNED,
    RCO_POOL_JOB_RUNNING,
    RCO_POOL_JOB_FINALIZING,
};

struct rco_pool;

struct rco_pool_job {
    struct rco_pool *pool;
    struct rco_task_spec spec;
    uint64_t runtime_task_id;
    uint32_t generation;
    enum rco_pool_job_state state;
    enum rco_affinity affinity;
    size_t queued_worker;
    size_t owner_worker;
    size_t previous;
    size_t next;
    bool cancel_requested;
    bool cancel_delivered;
};

struct rco_pool_worker {
    struct rco_pool *pool;
    size_t index;
    pthread_t thread;
    pthread_mutex_t deque_mutex;
    pthread_mutex_t wake_mutex;
    size_t deque_head;
    size_t deque_tail;
    size_t *dispatch_slots;
    struct rco_runtime *runtime;
    int control_fd;
    bool created;
    bool joined;
};

struct rco_pool {
    struct rco_pool_config config;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    enum rco_pool_state state;
    int first_error;
    size_t startup_reports;
    size_t created_workers;
    size_t next_worker;
    size_t free_head;
    bool joining;
    struct rco_pool_stats stats;
    struct rco_pool_worker *workers;
    struct rco_pool_job *jobs;
};

static _Thread_local struct rco_pool_worker *rco_pool_tls_worker;

static int rco_pool_neg_errno(void)
{
    return errno == 0 ? -EIO : -errno;
}

static rco_job_id_t rco_pool_encode_id(size_t slot, uint32_t generation)
{
    return ((uint64_t)generation << 32) | (uint64_t)(slot + 1);
}

static bool rco_pool_decode_id(const struct rco_pool *pool,
                               rco_job_id_t id,
                               size_t *out_slot,
                               uint32_t *out_generation)
{
    uint32_t encoded_slot = (uint32_t)id;
    uint32_t generation = (uint32_t)(id >> 32);

    if (encoded_slot == 0 || generation == 0 ||
        (size_t)encoded_slot > pool->config.max_jobs) {
        return false;
    }
    *out_slot = (size_t)encoded_slot - 1;
    *out_generation = generation;
    return true;
}

static int rco_pool_deadline(int timeout_ms, struct timespec *out_deadline)
{
    if (clock_gettime(CLOCK_REALTIME, out_deadline) != 0) {
        return rco_pool_neg_errno();
    }
    out_deadline->tv_sec += timeout_ms / 1000;
    out_deadline->tv_nsec +=
        (long)(timeout_ms % 1000) * 1000000L;
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_sec++;
        out_deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int rco_pool_worker_wake(struct rco_pool_worker *worker)
{
    int result = pthread_mutex_lock(&worker->wake_mutex);
    if (result != 0) {
        return -result;
    }

    int error = 0;
    if (worker->control_fd >= 0) {
        eventfd_t value = 1;
        while (eventfd_write(worker->control_fd, value) != 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                error = rco_pool_neg_errno();
            }
            break;
        }
    }

    result = pthread_mutex_unlock(&worker->wake_mutex);
    return error != 0 ? error : (result == 0 ? 0 : -result);
}

static void rco_pool_wake_all(struct rco_pool *pool)
{
    for (size_t index = 0; index < pool->config.worker_count; ++index) {
        (void)rco_pool_worker_wake(&pool->workers[index]);
    }
}

static void rco_pool_deque_push_locked(struct rco_pool *pool,
                                       size_t worker_index,
                                       size_t slot)
{
    struct rco_pool_worker *worker = &pool->workers[worker_index];
    struct rco_pool_job *job = &pool->jobs[slot];

    (void)pthread_mutex_lock(&worker->deque_mutex);
    job->previous = worker->deque_tail;
    job->next = RCO_POOL_NO_INDEX;
    if (worker->deque_tail == RCO_POOL_NO_INDEX) {
        worker->deque_head = slot;
    } else {
        pool->jobs[worker->deque_tail].next = slot;
    }
    worker->deque_tail = slot;
    (void)pthread_mutex_unlock(&worker->deque_mutex);
}

static void rco_pool_deque_unlink(struct rco_pool *pool,
                                  struct rco_pool_worker *worker,
                                  size_t slot)
{
    struct rco_pool_job *job = &pool->jobs[slot];

    if (job->previous == RCO_POOL_NO_INDEX) {
        worker->deque_head = job->next;
    } else {
        pool->jobs[job->previous].next = job->next;
    }
    if (job->next == RCO_POOL_NO_INDEX) {
        worker->deque_tail = job->previous;
    } else {
        pool->jobs[job->next].previous = job->previous;
    }
    job->previous = RCO_POOL_NO_INDEX;
    job->next = RCO_POOL_NO_INDEX;
}

static size_t rco_pool_deque_take_locked(struct rco_pool *pool,
                                         size_t worker_index,
                                         bool steal)
{
    struct rco_pool_worker *worker = &pool->workers[worker_index];
    size_t slot = RCO_POOL_NO_INDEX;

    (void)pthread_mutex_lock(&worker->deque_mutex);
    if (!steal) {
        slot = worker->deque_head;
    } else {
        for (slot = worker->deque_tail;
             slot != RCO_POOL_NO_INDEX;
             slot = pool->jobs[slot].previous) {
            if (pool->jobs[slot].affinity != RCO_AFFINITY_REQUIRE) {
                break;
            }
        }
    }

    if (slot != RCO_POOL_NO_INDEX) {
        rco_pool_deque_unlink(pool, worker, slot);
    }
    (void)pthread_mutex_unlock(&worker->deque_mutex);
    return slot;
}

static void rco_pool_retire_job(struct rco_pool_job *job, bool cancelled)
{
    struct rco_pool *pool = job->pool;
    size_t slot = (size_t)(job - pool->jobs);
    bool wake_shutdown = false;

    (void)pthread_mutex_lock(&pool->mutex);
    if (cancelled) {
        pool->stats.cancelled++;
    } else {
        pool->stats.completed++;
    }
    pool->stats.outstanding--;
    job->spec = (struct rco_task_spec){0};
    job->runtime_task_id = 0;
    job->state = RCO_POOL_JOB_FREE;
    job->affinity = RCO_AFFINITY_ANY;
    job->queued_worker = RCO_POOL_NO_INDEX;
    job->owner_worker = RCO_POOL_NO_INDEX;
    job->previous = RCO_POOL_NO_INDEX;
    job->cancel_requested = false;
    job->cancel_delivered = false;
    if (job->generation == UINT32_MAX) {
        job->next = RCO_POOL_NO_INDEX;
    } else {
        job->generation++;
        job->next = pool->free_head;
        pool->free_head = slot;
    }
    wake_shutdown =
        pool->stats.outstanding == 0 &&
        (pool->state == RCO_POOL_DRAINING ||
         pool->state == RCO_POOL_CANCELLING);
    (void)pthread_cond_broadcast(&pool->condition);
    (void)pthread_mutex_unlock(&pool->mutex);

    if (wake_shutdown) {
        rco_pool_wake_all(pool);
    }
}

static void rco_pool_invoke_finalizer(struct rco_pool_job *job,
                                      bool cancelled)
{
    rco_finalizer_fn finalizer = job->spec.finalizer;
    void *argument = job->spec.argument;

    if (finalizer != NULL) {
        finalizer(argument);
    }
    rco_pool_retire_job(job, cancelled);
}

static void rco_pool_finalize_unstarted(struct rco_pool_job *job)
{
    struct rco_pool *pool = job->pool;

    (void)pthread_mutex_lock(&pool->mutex);
    if (job->state != RCO_POOL_JOB_RESERVED) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return;
    }
    job->cancel_requested = true;
    job->state = RCO_POOL_JOB_FINALIZING;
    (void)pthread_mutex_unlock(&pool->mutex);

    rco_pool_invoke_finalizer(job, true);
}

static void rco_pool_runtime_finalizer(void *argument)
{
    struct rco_pool_job *job = argument;
    struct rco_pool *pool = job->pool;
    bool cancelled;

    (void)pthread_mutex_lock(&pool->mutex);
    cancelled = job->cancel_requested;
    job->state = RCO_POOL_JOB_FINALIZING;
    job->runtime_task_id = 0;
    (void)pthread_mutex_unlock(&pool->mutex);

    rco_pool_invoke_finalizer(job, cancelled);
}

static int rco_pool_runtime_entry(void *argument)
{
    struct rco_pool_job *job = argument;
    struct rco_pool *pool = job->pool;
    rco_entry_fn entry;
    void *entry_argument;
    bool cancelled;

    (void)pthread_mutex_lock(&pool->mutex);
    cancelled = job->cancel_requested;
    job->state = RCO_POOL_JOB_RUNNING;
    entry = job->spec.entry;
    entry_argument = job->spec.argument;
    (void)pthread_mutex_unlock(&pool->mutex);

    return cancelled ? 0 : entry(entry_argument);
}

static void rco_pool_request_cancel_all(struct rco_pool *pool)
{
    (void)pthread_mutex_lock(&pool->mutex);
    for (size_t slot = 0; slot < pool->config.max_jobs; ++slot) {
        struct rco_pool_job *job = &pool->jobs[slot];
        if (job->state == RCO_POOL_JOB_QUEUED ||
            job->state == RCO_POOL_JOB_RESERVED ||
            job->state == RCO_POOL_JOB_SPAWNED ||
            job->state == RCO_POOL_JOB_RUNNING) {
            job->cancel_requested = true;
        }
    }
    (void)pthread_mutex_unlock(&pool->mutex);

    rco_pool_wake_all(pool);
}

static void rco_pool_fail(struct rco_pool *pool, int error)
{
    if (error >= 0) {
        error = -EIO;
    }

    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->first_error == 0) {
        pool->first_error = error;
    }
    if (pool->state != RCO_POOL_JOINED) {
        pool->state = RCO_POOL_CANCELLING;
    }
    (void)pthread_cond_broadcast(&pool->condition);
    (void)pthread_mutex_unlock(&pool->mutex);

    rco_pool_request_cancel_all(pool);
}

static size_t rco_pool_claim_one(struct rco_pool_worker *worker)
{
    struct rco_pool *pool = worker->pool;
    size_t slot = RCO_POOL_NO_INDEX;

    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->state == RCO_POOL_RUNNING ||
        pool->state == RCO_POOL_DRAINING ||
        pool->state == RCO_POOL_CANCELLING) {
        slot = rco_pool_deque_take_locked(pool, worker->index, false);
        if (slot == RCO_POOL_NO_INDEX) {
            for (size_t offset = 1;
                 offset < pool->config.worker_count;
                 ++offset) {
                size_t victim =
                    (worker->index + offset) % pool->config.worker_count;
                slot = rco_pool_deque_take_locked(pool, victim, true);
                if (slot != RCO_POOL_NO_INDEX) {
                    pool->stats.jobs_stolen_before_start++;
                    break;
                }
            }
        }
    }
    if (slot != RCO_POOL_NO_INDEX) {
        struct rco_pool_job *job = &pool->jobs[slot];
        job->state = RCO_POOL_JOB_RESERVED;
        job->owner_worker = worker->index;
    }
    (void)pthread_mutex_unlock(&pool->mutex);
    return slot;
}

static size_t rco_pool_claim_batch(struct rco_pool_worker *worker,
                                   size_t *slots)
{
    size_t count = 0;

    while (count < worker->pool->config.dispatch_batch) {
        size_t slot = rco_pool_claim_one(worker);
        if (slot == RCO_POOL_NO_INDEX) {
            break;
        }
        slots[count++] = slot;
    }
    return count;
}

static int rco_pool_deliver_cancellations(struct rco_pool_worker *worker)
{
    struct rco_pool *pool = worker->pool;

    for (size_t slot = 0; slot < pool->config.max_jobs; ++slot) {
        uint64_t task_id = 0;

        (void)pthread_mutex_lock(&pool->mutex);
        struct rco_pool_job *job = &pool->jobs[slot];
        if (job->owner_worker == worker->index &&
            (job->state == RCO_POOL_JOB_SPAWNED ||
             job->state == RCO_POOL_JOB_RUNNING) &&
            job->cancel_requested && !job->cancel_delivered &&
            job->runtime_task_id != 0) {
            job->cancel_delivered = true;
            task_id = job->runtime_task_id;
        }
        (void)pthread_mutex_unlock(&pool->mutex);

        if (task_id != 0) {
            int result = rco_cancel(worker->runtime, task_id);
            if (result != 0 && result != -ESRCH) {
                return result;
            }
        }
    }
    return 0;
}

static bool rco_pool_worker_should_exit(struct rco_pool_worker *worker)
{
    struct rco_pool *pool = worker->pool;
    bool should_exit;

    (void)pthread_mutex_lock(&pool->mutex);
    should_exit =
        (pool->state == RCO_POOL_DRAINING ||
         pool->state == RCO_POOL_CANCELLING) &&
        pool->stats.outstanding == 0;
    (void)pthread_mutex_unlock(&pool->mutex);
    return should_exit;
}

static bool rco_pool_worker_has_work(struct rco_pool_worker *worker)
{
    struct rco_pool *pool = worker->pool;
    bool has_work = false;

    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->state == RCO_POOL_RUNNING ||
        pool->state == RCO_POOL_DRAINING ||
        pool->state == RCO_POOL_CANCELLING) {
        for (size_t slot = 0; slot < pool->config.max_jobs; ++slot) {
            const struct rco_pool_job *job = &pool->jobs[slot];
            if (job->state == RCO_POOL_JOB_QUEUED &&
                (job->queued_worker == worker->index ||
                 job->affinity != RCO_AFFINITY_REQUIRE)) {
                has_work = true;
                break;
            }
        }
    }
    if (!has_work) {
        for (size_t slot = 0; slot < pool->config.max_jobs; ++slot) {
            const struct rco_pool_job *job = &pool->jobs[slot];
            if (job->owner_worker == worker->index &&
                (job->state == RCO_POOL_JOB_SPAWNED ||
                 job->state == RCO_POOL_JOB_RUNNING) &&
                job->cancel_requested && !job->cancel_delivered) {
                has_work = true;
                break;
            }
        }
    }
    (void)pthread_mutex_unlock(&pool->mutex);
    return has_work;
}

static int rco_pool_worker_drain_signal(struct rco_pool_worker *worker)
{
    for (;;) {
        eventfd_t value;
        if (eventfd_read(worker->control_fd, &value) == 0) {
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == EAGAIN ? 0 : rco_pool_neg_errno();
    }
}

static int rco_pool_start_reserved(struct rco_pool_worker *worker,
                                   size_t slot)
{
    struct rco_pool *pool = worker->pool;
    struct rco_pool_job *job = &pool->jobs[slot];
    const struct rco_task_spec runtime_spec = {
        .stack_size = job->spec.stack_size,
        .entry = rco_pool_runtime_entry,
        .argument = job,
        .finalizer = rco_pool_runtime_finalizer,
    };
    uint64_t task_id = 0;
    int result =
        rco_spawn_task(worker->runtime, &runtime_spec, &task_id);
    if (result != 0) {
        rco_pool_fail(pool, result);
        rco_pool_finalize_unstarted(job);
        return result;
    }

    (void)pthread_mutex_lock(&pool->mutex);
    job->runtime_task_id = task_id;
    job->state = RCO_POOL_JOB_SPAWNED;
    bool cancelled = job->cancel_requested ||
                     pool->state == RCO_POOL_CANCELLING;
    if (cancelled) {
        job->cancel_requested = true;
        job->cancel_delivered = true;
    }
    (void)pthread_mutex_unlock(&pool->mutex);

    if (cancelled) {
        result = rco_cancel(worker->runtime, task_id);
        if (result != 0 && result != -ESRCH) {
            rco_pool_fail(pool, result);
            return result;
        }
    }

    result = rco_yield();
    if (result != 0 && result != -ECANCELED) {
        rco_pool_fail(pool, result);
    }
    return result;
}

static int rco_pool_dispatcher(void *argument)
{
    struct rco_pool_worker *worker = argument;
    struct rco_pool *pool = worker->pool;

    for (;;) {
        int result = rco_pool_worker_drain_signal(worker);
        if (result != 0) {
            rco_pool_fail(pool, result);
            break;
        }

        result = rco_pool_deliver_cancellations(worker);
        if (result != 0) {
            rco_pool_fail(pool, result);
            break;
        }

        size_t count =
            rco_pool_claim_batch(worker, worker->dispatch_slots);
        for (size_t index = 0; index < count; ++index) {
            result = rco_pool_start_reserved(
                worker, worker->dispatch_slots[index]);
            if (result == -ECANCELED) {
                break;
            }
            result = rco_pool_deliver_cancellations(worker);
            if (result != 0) {
                rco_pool_fail(pool, result);
                break;
            }
        }

        if (rco_pool_worker_should_exit(worker)) {
            break;
        }
        if (count == pool->config.dispatch_batch ||
            rco_pool_worker_has_work(worker)) {
            continue;
        }

        unsigned ready = 0;
        result =
            rco_wait_fd(worker->control_fd, RCO_EVENT_READ, -1, &ready);
        if (result == -ECANCELED) {
            break;
        }
        if (result != 0) {
            rco_pool_fail(pool, result);
            break;
        }
    }

    return 0;
}

static int rco_pool_worker_pin(const struct rco_pool_worker *worker)
{
    const struct rco_pool *pool = worker->pool;

    if (!pool->config.pin_workers) {
        return 0;
    }

    int cpu = pool->config.first_cpu + (int)worker->index;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int result =
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    return result == 0 ? 0 : -result;
}

static void rco_pool_report_startup(struct rco_pool_worker *worker,
                                    int error)
{
    struct rco_pool *pool = worker->pool;

    (void)pthread_mutex_lock(&pool->mutex);
    pool->startup_reports++;
    if (error != 0) {
        if (pool->first_error == 0) {
            pool->first_error = error;
        }
        pool->state = RCO_POOL_CANCELLING;
    }
    (void)pthread_cond_broadcast(&pool->condition);
    (void)pthread_mutex_unlock(&pool->mutex);

    if (error != 0) {
        rco_pool_wake_all(pool);
    }
}

static void rco_pool_worker_close_control(struct rco_pool_worker *worker)
{
    (void)pthread_mutex_lock(&worker->wake_mutex);
    if (worker->control_fd >= 0) {
        (void)close(worker->control_fd);
        worker->control_fd = -1;
    }
    (void)pthread_mutex_unlock(&worker->wake_mutex);
}

static void rco_pool_finalize_worker_reservations(
    struct rco_pool_worker *worker)
{
    struct rco_pool *pool = worker->pool;

    for (size_t slot = 0; slot < pool->config.max_jobs; ++slot) {
        struct rco_pool_job *reserved = NULL;

        (void)pthread_mutex_lock(&pool->mutex);
        struct rco_pool_job *job = &pool->jobs[slot];
        if (job->state == RCO_POOL_JOB_RESERVED &&
            job->owner_worker == worker->index) {
            reserved = job;
        }
        (void)pthread_mutex_unlock(&pool->mutex);
        if (reserved != NULL) {
            rco_pool_finalize_unstarted(reserved);
        }
    }
}

static void *rco_pool_worker_main(void *argument)
{
    struct rco_pool_worker *worker = argument;
    struct rco_pool *pool = worker->pool;
    struct rco_config runtime_config = pool->config.runtime;
    sigset_t inherited_mask;
    bool mask_saved = false;
    int result;

    rco_pool_tls_worker = worker;
    result = rco_pool_worker_pin(worker);
    if (result == 0 && runtime_config.preempt_quantum_ns != 0) {
        result = pthread_sigmask(SIG_SETMASK, NULL, &inherited_mask);
        if (result == 0) {
            mask_saved = true;
            sigset_t preempt_set;
            if (sigemptyset(&preempt_set) != 0 ||
                sigaddset(&preempt_set, runtime_config.preempt_signal) != 0) {
                result = errno == 0 ? EINVAL : errno;
            } else {
                result = pthread_sigmask(SIG_BLOCK, &preempt_set, NULL);
            }
        }
        if (result != 0) {
            result = -result;
        }
    }
    if (result == 0) {
        worker->dispatch_slots =
            calloc(pool->config.dispatch_batch,
                   sizeof(*worker->dispatch_slots));
        if (worker->dispatch_slots == NULL) {
            result = -ENOMEM;
        }
    }

    if (result == 0) {
        int control_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (control_fd < 0) {
            result = rco_pool_neg_errno();
        } else {
            (void)pthread_mutex_lock(&worker->wake_mutex);
            worker->control_fd = control_fd;
            (void)pthread_mutex_unlock(&worker->wake_mutex);
        }
    }

    if (result == 0) {
        size_t required_coroutines = pool->config.max_jobs + 1;
        if (runtime_config.max_coroutines < required_coroutines) {
            runtime_config.max_coroutines = required_coroutines;
        }
        if (runtime_config.max_fds <= (size_t)worker->control_fd) {
            runtime_config.max_fds = (size_t)worker->control_fd + 1;
        }
        result = rco_runtime_create(&runtime_config, &worker->runtime);
    }
    if (result == 0) {
        const struct rco_task_spec dispatcher = {
            .entry = rco_pool_dispatcher,
            .argument = worker,
        };
        result = rco_spawn_task(worker->runtime, &dispatcher, NULL);
    }
    rco_pool_report_startup(worker, result);

    if (result == 0) {
        result = rco_runtime_run(worker->runtime);
        if (result != 0) {
            rco_pool_fail(pool, result);
        }
    }

    rco_pool_finalize_worker_reservations(worker);
    if (worker->runtime != NULL) {
        int destroy_result = rco_runtime_destroy(worker->runtime);
        worker->runtime = NULL;
        if (destroy_result != 0) {
            rco_pool_fail(pool, destroy_result);
        }
    }
    rco_pool_worker_close_control(worker);
    free(worker->dispatch_slots);
    worker->dispatch_slots = NULL;
    if (mask_saved) {
        int mask_result =
            pthread_sigmask(SIG_SETMASK, &inherited_mask, NULL);
        if (mask_result != 0) {
            rco_pool_fail(pool, -mask_result);
        }
    }
    rco_pool_tls_worker = NULL;
    return NULL;
}

static int rco_pool_validate_config(const struct rco_pool_config *config)
{
    if (config == NULL || config->worker_count == 0 ||
        config->max_jobs == 0 || config->dispatch_batch == 0 ||
        config->dispatch_batch > config->max_jobs ||
        config->max_jobs > (size_t)INT_MAX - 16 ||
        config->worker_count >
            SIZE_MAX / sizeof(struct rco_pool_worker) ||
        config->max_jobs > SIZE_MAX / sizeof(struct rco_pool_job) ||
        config->max_jobs == SIZE_MAX) {
        return -EINVAL;
    }
    if (config->pin_workers &&
        (config->first_cpu < 0 ||
         config->worker_count > (size_t)INT_MAX ||
         config->worker_count - 1 >
             (size_t)(INT_MAX - config->first_cpu) ||
         config->first_cpu >= CPU_SETSIZE ||
         config->worker_count - 1 >
             (size_t)(CPU_SETSIZE - 1 - config->first_cpu))) {
        return -EINVAL;
    }
    return 0;
}

int rco_pool_create(const struct rco_pool_config *config,
                    struct rco_pool **out_pool)
{
    if (out_pool == NULL) {
        return -EINVAL;
    }
    *out_pool = NULL;

    int result = rco_pool_validate_config(config);
    if (result != 0) {
        return result;
    }

    struct rco_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return -ENOMEM;
    }
    pool->workers =
        calloc(config->worker_count, sizeof(*pool->workers));
    pool->jobs = calloc(config->max_jobs, sizeof(*pool->jobs));
    if (pool->workers == NULL || pool->jobs == NULL) {
        free(pool->jobs);
        free(pool->workers);
        free(pool);
        return -ENOMEM;
    }
    pool->config = *config;
    pool->state = RCO_POOL_CREATED;

    result = pthread_mutex_init(&pool->mutex, NULL);
    if (result != 0) {
        free(pool->jobs);
        free(pool->workers);
        free(pool);
        return -result;
    }
    result = pthread_cond_init(&pool->condition, NULL);
    if (result != 0) {
        (void)pthread_mutex_destroy(&pool->mutex);
        free(pool->jobs);
        free(pool->workers);
        free(pool);
        return -result;
    }

    size_t initialized_workers = 0;
    for (; initialized_workers < config->worker_count;
         ++initialized_workers) {
        struct rco_pool_worker *worker =
            &pool->workers[initialized_workers];
        worker->pool = pool;
        worker->index = initialized_workers;
        worker->deque_head = RCO_POOL_NO_INDEX;
        worker->deque_tail = RCO_POOL_NO_INDEX;
        worker->control_fd = -1;
        result = pthread_mutex_init(&worker->deque_mutex, NULL);
        if (result != 0) {
            break;
        }
        result = pthread_mutex_init(&worker->wake_mutex, NULL);
        if (result != 0) {
            (void)pthread_mutex_destroy(&worker->deque_mutex);
            break;
        }
    }
    if (initialized_workers != config->worker_count) {
        while (initialized_workers != 0) {
            initialized_workers--;
            (void)pthread_mutex_destroy(
                &pool->workers[initialized_workers].wake_mutex);
            (void)pthread_mutex_destroy(
                &pool->workers[initialized_workers].deque_mutex);
        }
        (void)pthread_cond_destroy(&pool->condition);
        (void)pthread_mutex_destroy(&pool->mutex);
        free(pool->jobs);
        free(pool->workers);
        free(pool);
        return -result;
    }

    for (size_t slot = 0; slot < config->max_jobs; ++slot) {
        pool->jobs[slot].pool = pool;
        pool->jobs[slot].generation = 1;
        pool->jobs[slot].queued_worker = RCO_POOL_NO_INDEX;
        pool->jobs[slot].owner_worker = RCO_POOL_NO_INDEX;
        pool->jobs[slot].previous = RCO_POOL_NO_INDEX;
        pool->jobs[slot].next =
            slot + 1 < config->max_jobs ? slot + 1 : RCO_POOL_NO_INDEX;
    }

    *out_pool = pool;
    return 0;
}

int rco_pool_start(struct rco_pool *pool)
{
    if (pool == NULL) {
        return -EINVAL;
    }

    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->state != RCO_POOL_CREATED) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EALREADY;
    }
    pool->state = RCO_POOL_STARTING;
    (void)pthread_mutex_unlock(&pool->mutex);

    int create_error = 0;
    for (size_t index = 0; index < pool->config.worker_count; ++index) {
        struct rco_pool_worker *worker = &pool->workers[index];
        int result = pthread_create(&worker->thread, NULL,
                                    rco_pool_worker_main, worker);
        if (result != 0) {
            create_error = -result;
            break;
        }
        (void)pthread_mutex_lock(&pool->mutex);
        worker->created = true;
        pool->created_workers++;
        (void)pthread_mutex_unlock(&pool->mutex);
    }

    (void)pthread_mutex_lock(&pool->mutex);
    if (create_error != 0) {
        if (pool->first_error == 0) {
            pool->first_error = create_error;
        }
        pool->state = RCO_POOL_CANCELLING;
    }
    while (pool->startup_reports < pool->created_workers) {
        int result =
            pthread_cond_wait(&pool->condition, &pool->mutex);
        if (result != 0 && pool->first_error == 0) {
            pool->first_error = -result;
            pool->state = RCO_POOL_CANCELLING;
        }
    }
    if (pool->first_error == 0) {
        pool->state = RCO_POOL_RUNNING;
    }
    int result = pool->first_error;
    (void)pthread_cond_broadcast(&pool->condition);
    (void)pthread_mutex_unlock(&pool->mutex);

    if (result != 0) {
        rco_pool_request_cancel_all(pool);
        (void)rco_pool_join(pool, -1);
    }
    return result;
}

int rco_pool_submit(struct rco_pool *pool,
                    const struct rco_task_spec *spec,
                    const struct rco_submit_options *options,
                    rco_job_id_t *out_job_id)
{
    if (out_job_id != NULL) {
        *out_job_id = 0;
    }
    if (pool == NULL || spec == NULL || spec->entry == NULL) {
        return -EINVAL;
    }

    struct rco_submit_options selected = {
        .affinity = RCO_AFFINITY_ANY,
    };
    if (options != NULL) {
        selected = *options;
    }
    if (selected.affinity != RCO_AFFINITY_ANY &&
        selected.affinity != RCO_AFFINITY_PREFER &&
        selected.affinity != RCO_AFFINITY_REQUIRE) {
        return -EINVAL;
    }
    if (selected.affinity != RCO_AFFINITY_ANY &&
        selected.worker_index >= pool->config.worker_count) {
        return -EINVAL;
    }

    size_t slot = RCO_POOL_NO_INDEX;
    size_t target;
    rco_job_id_t id;

    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->state != RCO_POOL_RUNNING) {
        int result =
            pool->state == RCO_POOL_CREATED ||
                    pool->state == RCO_POOL_STARTING
                ? -EPERM
                : -ESHUTDOWN;
        (void)pthread_mutex_unlock(&pool->mutex);
        return result;
    }
    if (pool->stats.outstanding == pool->config.max_jobs) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EAGAIN;
    }
    if (pool->stats.submitted == UINT64_MAX) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EOVERFLOW;
    }
    slot = pool->free_head;
    if (slot == RCO_POOL_NO_INDEX) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EAGAIN;
    }
    pool->free_head = pool->jobs[slot].next;

    if (selected.affinity == RCO_AFFINITY_ANY) {
        target = pool->next_worker;
        pool->next_worker++;
        if (pool->next_worker == pool->config.worker_count) {
            pool->next_worker = 0;
        }
    } else {
        target = selected.worker_index;
    }

    struct rco_pool_job *job = &pool->jobs[slot];
    job->spec = *spec;
    job->runtime_task_id = 0;
    job->state = RCO_POOL_JOB_QUEUED;
    job->affinity = selected.affinity;
    job->queued_worker = target;
    job->owner_worker = RCO_POOL_NO_INDEX;
    job->cancel_requested = false;
    job->cancel_delivered = false;
    id = rco_pool_encode_id(slot, job->generation);
    rco_pool_deque_push_locked(pool, target, slot);
    pool->stats.submitted++;
    pool->stats.outstanding++;
    (void)pthread_mutex_unlock(&pool->mutex);

    if (out_job_id != NULL) {
        *out_job_id = id;
    }

    int wake_result = rco_pool_worker_wake(&pool->workers[target]);
    if (selected.affinity != RCO_AFFINITY_REQUIRE) {
        rco_pool_wake_all(pool);
    }
    if (wake_result != 0) {
        rco_pool_fail(pool, wake_result);
    }
    return 0;
}

int rco_pool_cancel(struct rco_pool *pool, rco_job_id_t job_id)
{
    if (pool == NULL || job_id == 0) {
        return -EINVAL;
    }

    size_t slot;
    uint32_t generation;
    if (!rco_pool_decode_id(pool, job_id, &slot, &generation)) {
        return -ESRCH;
    }

    size_t owner = RCO_POOL_NO_INDEX;

    (void)pthread_mutex_lock(&pool->mutex);
    struct rco_pool_job *job = &pool->jobs[slot];
    if (job->generation != generation ||
        job->state == RCO_POOL_JOB_FREE) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -ESRCH;
    }
    if (job->state == RCO_POOL_JOB_FINALIZING ||
        job->cancel_requested) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EALREADY;
    }

    job->cancel_requested = true;
    if (job->state == RCO_POOL_JOB_QUEUED) {
        owner = job->queued_worker;
    } else {
        owner = job->owner_worker;
    }
    (void)pthread_mutex_unlock(&pool->mutex);

    if (owner != RCO_POOL_NO_INDEX) {
        int result = rco_pool_worker_wake(&pool->workers[owner]);
        if (result != 0) {
            rco_pool_fail(pool, result);
        }
    }
    return 0;
}

int rco_pool_shutdown(struct rco_pool *pool,
                      enum rco_shutdown_mode mode)
{
    if (pool == NULL ||
        (mode != RCO_SHUTDOWN_DRAIN &&
         mode != RCO_SHUTDOWN_CANCEL)) {
        return -EINVAL;
    }

    bool cancel = false;
    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->state == RCO_POOL_CREATED ||
        pool->state == RCO_POOL_STARTING) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EPERM;
    }
    if (pool->state == RCO_POOL_JOINED) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EALREADY;
    }
    if (mode == RCO_SHUTDOWN_CANCEL) {
        pool->state = RCO_POOL_CANCELLING;
        cancel = true;
    } else if (pool->state == RCO_POOL_RUNNING) {
        pool->state = RCO_POOL_DRAINING;
    }
    (void)pthread_cond_broadcast(&pool->condition);
    (void)pthread_mutex_unlock(&pool->mutex);

    if (cancel) {
        rco_pool_request_cancel_all(pool);
    } else {
        rco_pool_wake_all(pool);
    }
    return 0;
}

int rco_pool_wait_idle(struct rco_pool *pool, int timeout_ms)
{
    if (pool == NULL) {
        return -EINVAL;
    }

    struct timespec deadline;
    if (timeout_ms >= 0) {
        int result = rco_pool_deadline(timeout_ms, &deadline);
        if (result != 0) {
            return result;
        }
    }

    (void)pthread_mutex_lock(&pool->mutex);
    while (pool->stats.outstanding != 0) {
        int result;
        if (timeout_ms < 0) {
            result = pthread_cond_wait(&pool->condition, &pool->mutex);
        } else {
            result = pthread_cond_timedwait(&pool->condition, &pool->mutex,
                                            &deadline);
        }
        if (result != 0) {
            (void)pthread_mutex_unlock(&pool->mutex);
            return result == ETIMEDOUT ? -ETIMEDOUT : -result;
        }
    }
    int result = pool->first_error;
    (void)pthread_mutex_unlock(&pool->mutex);
    return result;
}

static bool rco_pool_all_joined(const struct rco_pool *pool)
{
    for (size_t index = 0; index < pool->config.worker_count; ++index) {
        if (pool->workers[index].created &&
            !pool->workers[index].joined) {
            return false;
        }
    }
    return true;
}

int rco_pool_join(struct rco_pool *pool, int timeout_ms)
{
    if (pool == NULL) {
        return -EINVAL;
    }

    struct timespec deadline;
    if (timeout_ms >= 0) {
        int result = rco_pool_deadline(timeout_ms, &deadline);
        if (result != 0) {
            return result;
        }
    }

    (void)pthread_mutex_lock(&pool->mutex);
    if (pool->state == RCO_POOL_CREATED ||
        pool->state == RCO_POOL_STARTING ||
        pool->state == RCO_POOL_RUNNING) {
        (void)pthread_mutex_unlock(&pool->mutex);
        return -EBUSY;
    }
    while (pool->joining) {
        int result;
        if (timeout_ms < 0) {
            result = pthread_cond_wait(&pool->condition, &pool->mutex);
        } else {
            result = pthread_cond_timedwait(&pool->condition, &pool->mutex,
                                            &deadline);
        }
        if (result != 0) {
            (void)pthread_mutex_unlock(&pool->mutex);
            return result == ETIMEDOUT ? -ETIMEDOUT : -result;
        }
    }
    if (pool->state == RCO_POOL_JOINED) {
        int result = pool->first_error;
        (void)pthread_mutex_unlock(&pool->mutex);
        return result;
    }
    pool->joining = true;
    (void)pthread_mutex_unlock(&pool->mutex);

    int join_error = 0;
    for (size_t index = 0; index < pool->config.worker_count; ++index) {
        struct rco_pool_worker *worker = &pool->workers[index];
        if (!worker->created || worker->joined) {
            continue;
        }

        int result;
        if (timeout_ms < 0) {
            result = pthread_join(worker->thread, NULL);
        } else {
            result =
                pthread_timedjoin_np(worker->thread, NULL, &deadline);
        }
        if (result != 0) {
            join_error =
                result == ETIMEDOUT ? -ETIMEDOUT : -result;
            break;
        }
        worker->joined = true;
    }

    (void)pthread_mutex_lock(&pool->mutex);
    pool->joining = false;
    if (join_error == 0 && rco_pool_all_joined(pool)) {
        pool->state = RCO_POOL_JOINED;
    }
    int first_error = pool->first_error;
    (void)pthread_cond_broadcast(&pool->condition);
    (void)pthread_mutex_unlock(&pool->mutex);

    return join_error != 0 ? join_error : first_error;
}

int rco_pool_get_stats(const struct rco_pool *pool,
                       struct rco_pool_stats *out_stats)
{
    if (pool == NULL || out_stats == NULL) {
        return -EINVAL;
    }

    struct rco_pool *mutable_pool = (struct rco_pool *)pool;
    (void)pthread_mutex_lock(&mutable_pool->mutex);
    *out_stats = pool->stats;
    (void)pthread_mutex_unlock(&mutable_pool->mutex);
    return 0;
}

int rco_pool_destroy(struct rco_pool *pool)
{
    if (pool == NULL) {
        return -EINVAL;
    }

    (void)pthread_mutex_lock(&pool->mutex);
    bool destroyable =
        pool->state == RCO_POOL_JOINED ||
        (pool->state == RCO_POOL_CREATED &&
         pool->stats.submitted == 0);
    (void)pthread_mutex_unlock(&pool->mutex);
    if (!destroyable) {
        return -EBUSY;
    }

    for (size_t index = 0; index < pool->config.worker_count; ++index) {
        (void)pthread_mutex_destroy(&pool->workers[index].wake_mutex);
        (void)pthread_mutex_destroy(&pool->workers[index].deque_mutex);
    }
    (void)pthread_cond_destroy(&pool->condition);
    (void)pthread_mutex_destroy(&pool->mutex);
    free(pool->jobs);
    free(pool->workers);
    free(pool);
    return 0;
}

size_t rco_current_worker(void)
{
    return rco_pool_tls_worker == NULL ? SIZE_MAX
                                       : rco_pool_tls_worker->index;
}

int rco_submit_local(const struct rco_task_spec *spec,
                     rco_job_id_t *out_job_id)
{
    struct rco_pool_worker *worker = rco_pool_tls_worker;
    if (worker == NULL || rco_current_id() == 0) {
        if (out_job_id != NULL) {
            *out_job_id = 0;
        }
        return -EPERM;
    }

    const struct rco_submit_options options = {
        .affinity = RCO_AFFINITY_REQUIRE,
        .worker_index = worker->index,
    };
    return rco_pool_submit(worker->pool, spec, &options, out_job_id);
}
