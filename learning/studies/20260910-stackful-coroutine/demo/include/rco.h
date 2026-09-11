#ifndef RCO_H
#define RCO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RCO_VERSION_MAJOR 0
#define RCO_VERSION_MINOR 2
#define RCO_VERSION_PATCH 0

#define RCO_STACK_SIZE_DEFAULT ((size_t)128 * 1024)
#define RCO_STACK_SIZE_MIN ((size_t)16 * 1024)
#define RCO_TLS_KEYS_DEFAULT ((size_t)64)
#define RCO_TLS_KEYS_MAX ((size_t)65536)

#if defined(RCO_CACS_PRESERVE_NONE)
#if !defined(RCO_CACS)
#error "RCO_CACS_PRESERVE_NONE requires RCO_CACS"
#elif !defined(__clang__)
#error "RCO_CACS_PRESERVE_NONE requires Clang preserve_none support"
#elif !defined(__has_attribute)
#error "RCO_CACS_PRESERVE_NONE requires __has_attribute"
#elif !__has_attribute(preserve_none)
#error "RCO_CACS_PRESERVE_NONE requires the preserve_none attribute"
#endif
#define RCO_SUSPEND_ABI __attribute__((preserve_none))
#else
#define RCO_SUSPEND_ABI
#endif

#if defined(RCO_CACS_PRESERVE_NONE)
#define rco_yield rco_yield_cacs_preserve_none
#define rco_wait_fd rco_wait_fd_cacs_preserve_none
#define rco_sleep_ms rco_sleep_ms_cacs_preserve_none
#endif

enum rco_event {
    RCO_EVENT_READ = 1u << 0,
    RCO_EVENT_WRITE = 1u << 1,
};

/*
 * Zero flags select RCO_LOCAL_STATE_ERRNO. RCO_LOCAL_STATE_NONE explicitly
 * selects legacy switching and cannot be combined with another flag.
 */
enum rco_local_state {
    RCO_LOCAL_STATE_NONE = 1u << 0,
    RCO_LOCAL_STATE_ERRNO = 1u << 1,
    RCO_LOCAL_STATE_SIGNAL_MASK = 1u << 2,
    RCO_LOCAL_STATE_LOCALE = 1u << 3,
};

struct rco_runtime;

typedef int (*rco_entry_fn)(void *argument);
typedef void (*rco_finalizer_fn)(void *argument);
typedef uint64_t rco_tls_key_t;
typedef void (*rco_tls_destructor_fn)(void *value);

struct rco_task_spec {
    size_t stack_size;
    rco_entry_fn entry;
    void *argument;
    rco_finalizer_fn finalizer;
};

struct rco_config {
    size_t default_stack_size;
    size_t max_coroutines;
    size_t max_fds;
    size_t stack_cache_bytes;
    uint32_t local_state_flags;
    size_t max_tls_keys;
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
 * The runtime is single-threaded and cooperative. The thread that creates a
 * runtime owns it and must perform every subsequent operation.
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
 * A task finalizer runs exactly once on the scheduler stack after completion
 * or cancellation. It must not call a coroutine suspension function. When
 * rco_spawn_task() fails, ownership remains with the caller and the finalizer
 * is not called.
 */
int rco_spawn_task(struct rco_runtime *runtime,
                   const struct rco_task_spec *spec,
                   uint64_t *out_task_id);
int rco_spawn(struct rco_runtime *runtime,
              size_t stack_size,
              rco_entry_fn entry,
              void *argument,
              uint64_t *out_task_id);
int rco_cancel(struct rco_runtime *runtime, uint64_t task_id);

/*
 * TLS keys belong to one runtime. Values belong to the current task and are
 * allocated lazily. Deleting a key clears its values without running its
 * destructor. Task exit runs up to four destructor passes before the user
 * finalizer; TLS get/set remain valid in callbacks, but coroutine suspension
 * does not.
 */
int rco_tls_key_create(struct rco_runtime *runtime,
                       rco_tls_destructor_fn destructor,
                       rco_tls_key_t *out_key);
int rco_tls_key_delete(struct rco_runtime *runtime, rco_tls_key_t key);
int rco_tls_get(struct rco_runtime *runtime,
                rco_tls_key_t key,
                void **out_value);
int rco_tls_set(struct rco_runtime *runtime,
                rco_tls_key_t key,
                void *value);

/*
 * Yield, wait, sleep, cancellation status, and current ID are valid only
 * inside a running coroutine.
 * timeout_ms < 0 means no timeout. rco_wait_fd() writes the ready event mask
 * on success and returns -ETIMEDOUT, -ECANCELED, or another negative errno on
 * failure.
 */
RCO_SUSPEND_ABI int rco_yield(void);
RCO_SUSPEND_ABI int rco_wait_fd(int fd,
                                unsigned events,
                                int timeout_ms,
                                unsigned *out_ready_events);
RCO_SUSPEND_ABI int rco_sleep_ms(uint64_t delay_ms);
/* Also valid from a task finalizer while the runtime is running. */
int rco_close_fd(int fd);
bool rco_cancelled(void);
uint64_t rco_current_id(void);

#ifdef __cplusplus
}
#endif

#endif
