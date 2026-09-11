#ifndef RCO_POOL_H
#define RCO_POOL_H

#include "rco.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rco_pool;

typedef uint64_t rco_job_id_t;

enum rco_affinity {
    RCO_AFFINITY_ANY,
    RCO_AFFINITY_PREFER,
    RCO_AFFINITY_REQUIRE,
};

enum rco_shutdown_mode {
    RCO_SHUTDOWN_DRAIN,
    RCO_SHUTDOWN_CANCEL,
};

struct rco_pool_config {
    size_t worker_count;
    size_t max_jobs;
    size_t dispatch_batch;
    int first_cpu;
    bool pin_workers;
    struct rco_config runtime;
};

struct rco_submit_options {
    enum rco_affinity affinity;
    size_t worker_index;
};

struct rco_pool_stats {
    uint64_t submitted;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t jobs_stolen_before_start;
    uint64_t coroutine_migrations;
    size_t outstanding;
    uint64_t wake_writes;
    uint64_t wake_coalesced;
    uint64_t steal_attempts;
    size_t queued_jobs;
    size_t pending_cancellations;
};

/*
 * rco_pool_start() returns the negated pthread affinity error when required
 * worker pinning cannot be applied. A successful submit transfers finalizer
 * ownership to the pool; a failed submit never invokes the finalizer.
 */
int rco_pool_create(const struct rco_pool_config *config,
                    struct rco_pool **out_pool);
int rco_pool_start(struct rco_pool *pool);
int rco_pool_submit(struct rco_pool *pool,
                    const struct rco_task_spec *spec,
                    const struct rco_submit_options *options,
                    rco_job_id_t *out_job_id);
int rco_pool_cancel(struct rco_pool *pool, rco_job_id_t job_id);
int rco_pool_shutdown(struct rco_pool *pool,
                      enum rco_shutdown_mode mode);
/* Blocking pool waits return -EDEADLK from one of the pool's worker threads. */
int rco_pool_wait_idle(struct rco_pool *pool, int timeout_ms);
int rco_pool_join(struct rco_pool *pool, int timeout_ms);
int rco_pool_get_stats(const struct rco_pool *pool,
                       struct rco_pool_stats *out_stats);

/*
 * Destroy is valid only after a successful join, or before start when no job
 * has ever been accepted.
 */
int rco_pool_destroy(struct rco_pool *pool);

/* SIZE_MAX is returned outside a pool worker thread. */
size_t rco_current_worker(void);

/*
 * Local submission is valid only from a running pool task. The accepted job
 * is pinned to the caller's worker and cannot be stolen.
 */
int rco_submit_local(const struct rco_task_spec *spec,
                     rco_job_id_t *out_job_id);

#ifdef __cplusplus
}
#endif

#endif
