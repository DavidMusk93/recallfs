#ifndef RCO_H
#define RCO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RCO_VERSION_MAJOR 0
#define RCO_VERSION_MINOR 1
#define RCO_VERSION_PATCH 0

#define RCO_STACK_SIZE_DEFAULT ((size_t)128 * 1024)
#define RCO_STACK_SIZE_MIN ((size_t)16 * 1024)

enum rco_event {
    RCO_EVENT_READ = 1u << 0,
    RCO_EVENT_WRITE = 1u << 1,
};

struct rco_runtime;

typedef int (*rco_entry_fn)(void *argument);

struct rco_config {
    size_t default_stack_size;
    size_t max_coroutines;
    size_t max_fds;
    size_t stack_cache_bytes;
};

struct rco_stats {
    uint64_t spawned;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t context_switches;
    uint64_t io_waits;
    uint64_t timer_waits;
    uint64_t stacks_mapped;
    uint64_t stacks_reused;
    size_t active;
    size_t peak_active;
    size_t cached_stack_bytes;
};

/*
 * The runtime is single-threaded and cooperative. A runtime and its tasks must
 * remain on the thread that calls rco_runtime_run().
 */
int rco_runtime_create(const struct rco_config *config,
                       struct rco_runtime **out_runtime);
int rco_runtime_destroy(struct rco_runtime *runtime);
int rco_runtime_run(struct rco_runtime *runtime);
int rco_runtime_stop(struct rco_runtime *runtime);
int rco_runtime_get_stats(const struct rco_runtime *runtime,
                          struct rco_stats *out_stats);

/*
 * A zero stack_size selects the configured default. Task identifiers are
 * monotonically increasing runtime-local handles and are never raw pointers.
 */
int rco_spawn(struct rco_runtime *runtime,
              size_t stack_size,
              rco_entry_fn entry,
              void *argument,
              uint64_t *out_task_id);
int rco_cancel(struct rco_runtime *runtime, uint64_t task_id);

/*
 * The following functions are valid only inside a running coroutine.
 * timeout_ms < 0 means no timeout. rco_wait_fd() writes the ready event mask
 * on success and returns -ETIMEDOUT, -ECANCELED, or another negative errno on
 * failure.
 */
int rco_yield(void);
int rco_wait_fd(int fd,
                unsigned events,
                int timeout_ms,
                unsigned *out_ready_events);
int rco_sleep_ms(uint64_t delay_ms);
int rco_close_fd(int fd);
bool rco_cancelled(void);
uint64_t rco_current_id(void);

#ifdef __cplusplus
}
#endif

#endif
