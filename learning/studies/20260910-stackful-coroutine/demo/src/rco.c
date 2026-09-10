#define _GNU_SOURCE

#include "rco.h"
#include "rco_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RCO_WITH_ASAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define RCO_WITH_ASAN 1
#endif

#if defined(RCO_WITH_ASAN)
#include <sanitizer/common_interface_defs.h>
#define RCO_NO_ASAN __attribute__((no_sanitize_address))
#else
#define RCO_NO_ASAN
#endif

#ifndef MAP_STACK
#define MAP_STACK 0
#endif

#ifndef ARCH_SHSTK_STATUS
#define ARCH_SHSTK_STATUS 0x5005
#endif

#ifndef ARCH_SHSTK_SHSTK
#define ARCH_SHSTK_SHSTK (1ULL << 0)
#endif

#define RCO_DEFAULT_MAX_COROUTINES ((size_t)65536)
#define RCO_DEFAULT_MAX_FDS ((size_t)65536)
#define RCO_DEFAULT_STACK_CACHE_BYTES ((size_t)8 * 1024 * 1024)
#define RCO_EPOLL_BATCH 128
#define RCO_READY_DISPATCH_BUDGET ((size_t)64)
#define RCO_FD_WATCH_CHUNK_SIZE ((size_t)256)
#define RCO_NO_TIMER SIZE_MAX

enum rco_task_state {
    RCO_TASK_READY,
    RCO_TASK_RUNNING,
    RCO_TASK_WAIT_IO,
    RCO_TASK_WAIT_TIMER,
    RCO_TASK_DONE,
    RCO_TASK_CANCELLED,
};

struct rco_stack {
    void *mapping;
    size_t mapping_size;
    unsigned char *base;
    size_t usable_size;
    struct rco_stack *next;
};

struct rco_task {
    struct rco_context context;
    struct rco_runtime *runtime;
    struct rco_stack stack;
    rco_entry_fn entry;
    rco_finalizer_fn finalizer;
    void *argument;
    uint64_t id;
    enum rco_task_state state;
    bool started;
    bool cancel_requested;
    bool queued;
    int wait_fd;
    unsigned ready_events;
    int wait_result;
    uint64_t deadline_ns;
    size_t timer_index;
#if defined(RCO_WITH_ASAN)
    void *asan_fake_stack;
#endif
    struct rco_task *ready_next;
    struct rco_task *all_next;
    struct rco_task **all_previous_next;
};

struct rco_fd_watch {
    struct rco_task *reader;
    struct rco_task *writer;
    uint32_t generation;
    bool registered;
};

struct rco_runtime {
    struct rco_context root_context;
    struct rco_config config;
    struct rco_stats stats;
    struct rco_task *current;
    struct rco_task *ready_head;
    struct rco_task *ready_tail;
    struct rco_task *all_tasks;
    struct rco_task **timer_heap;
    size_t timer_count;
    struct rco_fd_watch **fd_watch_chunks;
    size_t fd_watch_chunk_count;
    struct rco_stack *stack_cache;
    uint64_t next_id;
    long owner_tid;
    long page_size;
    int epoll_fd;
    int fatal_error;
    bool running;
    bool stop_requested;
#if defined(RCO_WITH_ASAN)
    void *asan_fake_stack;
    const void *asan_stack_bottom;
    size_t asan_stack_size;
#endif
};

static _Thread_local struct rco_runtime *rco_tls_runtime;

static int rco_neg_errno(void)
{
    return errno == 0 ? -EIO : -errno;
}

static bool rco_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool rco_round_up(size_t value, size_t alignment, size_t *out)
{
    size_t mask = alignment - 1;
    if (!rco_is_power_of_two(alignment) || value > SIZE_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static int rco_now_ns(uint64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return rco_neg_errno();
    }
    *out = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    return 0;
}

static int rco_check_shadow_stack(void)
{
#if defined(__x86_64__) && defined(SYS_arch_prctl) && !defined(RCO_FILC)
    uint64_t status = 0;
    errno = 0;
    long result = syscall(SYS_arch_prctl, ARCH_SHSTK_STATUS, &status);
    if (result == 0 && (status & ARCH_SHSTK_SHSTK) != 0) {
        return -ENOTSUP;
    }
    if (result < 0 && errno != EINVAL && errno != ENOSYS) {
        return rco_neg_errno();
    }
#endif
    return 0;
}

static int rco_validate_config(const struct rco_config *input,
                               struct rco_config *output)
{
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_DEFAULT,
        .max_coroutines = RCO_DEFAULT_MAX_COROUTINES,
        .max_fds = RCO_DEFAULT_MAX_FDS,
        .stack_cache_bytes = RCO_DEFAULT_STACK_CACHE_BYTES,
    };

    if (input != NULL) {
        if (input->default_stack_size != 0) {
            config.default_stack_size = input->default_stack_size;
        }
        if (input->max_coroutines != 0) {
            config.max_coroutines = input->max_coroutines;
        }
        if (input->max_fds != 0) {
            config.max_fds = input->max_fds;
        }
        config.stack_cache_bytes = input->stack_cache_bytes;
    }

    if (config.default_stack_size < RCO_STACK_SIZE_MIN ||
        config.max_coroutines == 0 || config.max_fds == 0 ||
        config.max_fds > (size_t)INT_MAX ||
        config.max_coroutines > SIZE_MAX / sizeof(struct rco_task *) ||
        config.max_fds > SIZE_MAX / sizeof(struct rco_fd_watch)) {
        return -EINVAL;
    }

    *output = config;
    return 0;
}

static int rco_stack_map(struct rco_runtime *runtime,
                         size_t requested_size,
                         struct rco_stack *out)
{
    size_t usable_size;
    if (requested_size < RCO_STACK_SIZE_MIN ||
        !rco_round_up(requested_size, (size_t)runtime->page_size,
                      &usable_size)) {
        return -EINVAL;
    }

    struct rco_stack **link = &runtime->stack_cache;
    while (*link != NULL) {
        if ((*link)->usable_size == usable_size) {
            struct rco_stack *cached = *link;
            *link = cached->next;
            *out = *cached;
            out->next = NULL;
            runtime->stats.cached_stack_bytes -= cached->mapping_size;
            runtime->stats.stacks_reused++;
            free(cached);
            return 0;
        }
        link = &(*link)->next;
    }

    size_t guards = (size_t)runtime->page_size * 2;
    if (usable_size > SIZE_MAX - guards) {
        return -EOVERFLOW;
    }
    size_t mapping_size = usable_size + guards;
    void *mapping = mmap(NULL, mapping_size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) {
        return rco_neg_errno();
    }

    unsigned char *base = (unsigned char *)mapping + runtime->page_size;
    if (mprotect(base, usable_size, PROT_READ | PROT_WRITE) != 0) {
        int error = rco_neg_errno();
        (void)munmap(mapping, mapping_size);
        return error;
    }

    *out = (struct rco_stack){
        .mapping = mapping,
        .mapping_size = mapping_size,
        .base = base,
        .usable_size = usable_size,
    };
    runtime->stats.stacks_mapped++;
    return 0;
}

static void rco_stack_unmap(struct rco_stack *stack)
{
    if (stack->mapping != NULL) {
        (void)munmap(stack->mapping, stack->mapping_size);
    }
    *stack = (struct rco_stack){0};
}

static void rco_stack_release(struct rco_runtime *runtime,
                              struct rco_stack *stack)
{
    bool cache_space =
        runtime->stats.cached_stack_bytes <=
            runtime->config.stack_cache_bytes &&
        stack->mapping_size <= runtime->config.stack_cache_bytes -
                                   runtime->stats.cached_stack_bytes;
    if (!cache_space ||
        madvise(stack->base, stack->usable_size, MADV_DONTNEED) != 0) {
        rco_stack_unmap(stack);
        return;
    }

    struct rco_stack *cached = malloc(sizeof(*cached));
    if (cached == NULL) {
        rco_stack_unmap(stack);
        return;
    }
    *cached = *stack;
    cached->next = runtime->stack_cache;
    runtime->stack_cache = cached;
    runtime->stats.cached_stack_bytes += cached->mapping_size;
    *stack = (struct rco_stack){0};
}

static void rco_ready_push(struct rco_runtime *runtime, struct rco_task *task)
{
    if (task->queued) {
        return;
    }
    task->queued = true;
    task->ready_next = NULL;
    if (runtime->ready_tail == NULL) {
        runtime->ready_head = task;
    } else {
        runtime->ready_tail->ready_next = task;
    }
    runtime->ready_tail = task;
}

static struct rco_task *rco_ready_pop(struct rco_runtime *runtime)
{
    struct rco_task *task = runtime->ready_head;
    if (task == NULL) {
        return NULL;
    }
    runtime->ready_head = task->ready_next;
    if (runtime->ready_head == NULL) {
        runtime->ready_tail = NULL;
    }
    task->ready_next = NULL;
    task->queued = false;
    return task;
}

static bool rco_timer_less(const struct rco_task *left,
                           const struct rco_task *right)
{
    if (left->deadline_ns != right->deadline_ns) {
        return left->deadline_ns < right->deadline_ns;
    }
    return left->id < right->id;
}

static void rco_timer_swap(struct rco_runtime *runtime,
                           size_t left,
                           size_t right)
{
    struct rco_task *temporary = runtime->timer_heap[left];
    runtime->timer_heap[left] = runtime->timer_heap[right];
    runtime->timer_heap[right] = temporary;
    runtime->timer_heap[left]->timer_index = left;
    runtime->timer_heap[right]->timer_index = right;
}

static void rco_timer_add(struct rco_runtime *runtime, struct rco_task *task)
{
    size_t index = runtime->timer_count;
    runtime->timer_heap[index] = task;
    task->timer_index = index;
    runtime->timer_count++;
    runtime->stats.timer_waits++;
    while (index != 0) {
        size_t parent = (index - 1) / 2;
        if (!rco_timer_less(runtime->timer_heap[index],
                            runtime->timer_heap[parent])) {
            break;
        }
        rco_timer_swap(runtime, index, parent);
        index = parent;
    }
}

static void rco_timer_remove(struct rco_runtime *runtime,
                             struct rco_task *task)
{
    if (task->timer_index == RCO_NO_TIMER) {
        return;
    }

    size_t index = task->timer_index;
    size_t count = runtime->timer_count;
    task->timer_index = RCO_NO_TIMER;
    count--;
    runtime->timer_count = count;
    if (index == count) {
        return;
    }

    runtime->timer_heap[index] = runtime->timer_heap[count];
    runtime->timer_heap[index]->timer_index = index;

    while (index != 0) {
        size_t parent = (index - 1) / 2;
        if (!rco_timer_less(runtime->timer_heap[index],
                            runtime->timer_heap[parent])) {
            break;
        }
        rco_timer_swap(runtime, index, parent);
        index = parent;
    }

    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;
        if (left < count &&
            rco_timer_less(runtime->timer_heap[left],
                           runtime->timer_heap[smallest])) {
            smallest = left;
        }
        if (right < count &&
            rco_timer_less(runtime->timer_heap[right],
                           runtime->timer_heap[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        rco_timer_swap(runtime, index, smallest);
        index = smallest;
    }
}

static uint64_t rco_watch_token(int fd, uint32_t generation)
{
    return ((uint64_t)generation << 32) | (uint32_t)fd;
}

static struct rco_fd_watch *rco_watch_get(struct rco_runtime *runtime,
                                          int fd,
                                          bool create)
{
    if (fd < 0 || (size_t)fd >= runtime->config.max_fds) {
        return NULL;
    }
    size_t chunk_index = (size_t)fd / RCO_FD_WATCH_CHUNK_SIZE;
    size_t item_index = (size_t)fd % RCO_FD_WATCH_CHUNK_SIZE;
    struct rco_fd_watch *chunk = runtime->fd_watch_chunks[chunk_index];
    if (chunk == NULL && create) {
        chunk = calloc(RCO_FD_WATCH_CHUNK_SIZE, sizeof(*chunk));
        if (chunk == NULL) {
            return NULL;
        }
        runtime->fd_watch_chunks[chunk_index] = chunk;
    }
    return chunk == NULL ? NULL : &chunk[item_index];
}

static void rco_watch_advance_generation(struct rco_fd_watch *watch)
{
    watch->generation++;
    if (watch->generation == 0) {
        watch->generation = 1;
    }
}

static int rco_watch_update(struct rco_runtime *runtime, int fd)
{
    struct rco_fd_watch *watch = rco_watch_get(runtime, fd, false);
    if (watch == NULL) {
        return -EINVAL;
    }
    uint32_t events = EPOLLRDHUP | EPOLLONESHOT;
    if (watch->reader != NULL) {
        events |= EPOLLIN;
    }
    if (watch->writer != NULL) {
        events |= EPOLLOUT;
    }

    if (watch->reader == NULL && watch->writer == NULL) {
        if (watch->registered &&
            epoll_ctl(runtime->epoll_fd, EPOLL_CTL_DEL, fd, NULL) != 0 &&
            errno != ENOENT && errno != EBADF) {
            return rco_neg_errno();
        }
        watch->registered = false;
        rco_watch_advance_generation(watch);
        return 0;
    }

    if (watch->generation == 0) {
        watch->generation = 1;
    }
    struct epoll_event event = {
        .events = events,
        .data.u64 = rco_watch_token(fd, watch->generation),
    };
    int operation = watch->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(runtime->epoll_fd, operation, fd, &event) != 0) {
        return rco_neg_errno();
    }
    watch->registered = true;
    return 0;
}

static void rco_record_fatal(struct rco_runtime *runtime, int error)
{
    if (runtime->fatal_error == 0) {
        runtime->fatal_error = error == 0 ? -EIO : error;
    }
}

static void rco_task_detach_wait(struct rco_runtime *runtime,
                                 struct rco_task *task)
{
    rco_timer_remove(runtime, task);
    if (task->wait_fd < 0) {
        return;
    }

    int fd = task->wait_fd;
    struct rco_fd_watch *watch = rco_watch_get(runtime, fd, false);
    if (watch == NULL) {
        task->wait_fd = -1;
        rco_record_fatal(runtime, -EPROTO);
        return;
    }
    if (watch->reader == task) {
        watch->reader = NULL;
    }
    if (watch->writer == task) {
        watch->writer = NULL;
    }
    task->wait_fd = -1;
    int result = rco_watch_update(runtime, fd);
    if (result != 0) {
        rco_record_fatal(runtime, result);
    }
}

static void rco_task_wake(struct rco_runtime *runtime,
                          struct rco_task *task,
                          int result,
                          unsigned ready_events)
{
    if (task == NULL ||
        (task->state != RCO_TASK_WAIT_IO &&
         task->state != RCO_TASK_WAIT_TIMER)) {
        return;
    }
    rco_task_detach_wait(runtime, task);
    task->wait_result = result;
    task->ready_events = ready_events;
    task->state = RCO_TASK_READY;
    rco_ready_push(runtime, task);
}

static void rco_task_wake_from_event(struct rco_runtime *runtime,
                                     struct rco_fd_watch *watch,
                                     struct rco_task *task,
                                     unsigned ready_events)
{
    if (task == NULL || task->state != RCO_TASK_WAIT_IO) {
        return;
    }
    rco_timer_remove(runtime, task);
    if (watch->reader == task) {
        watch->reader = NULL;
    }
    if (watch->writer == task) {
        watch->writer = NULL;
    }
    task->wait_fd = -1;
    task->wait_result = 0;
    task->ready_events = ready_events;
    task->state = RCO_TASK_READY;
    rco_ready_push(runtime, task);
}

static void rco_watch_invalidate(struct rco_runtime *runtime,
                                 int fd,
                                 int result)
{
    if (fd < 0 || (size_t)fd >= runtime->config.max_fds) {
        return;
    }
    struct rco_fd_watch *watch = rco_watch_get(runtime, fd, false);
    if (watch == NULL) {
        return;
    }
    struct rco_task *reader = watch->reader;
    struct rco_task *writer = watch->writer;
    watch->reader = NULL;
    watch->writer = NULL;

    if (watch->registered &&
        epoll_ctl(runtime->epoll_fd, EPOLL_CTL_DEL, fd, NULL) != 0 &&
        errno != ENOENT && errno != EBADF) {
        rco_record_fatal(runtime, rco_neg_errno());
    }
    watch->registered = false;
    rco_watch_advance_generation(watch);

    if (reader != NULL) {
        reader->wait_fd = -1;
        rco_task_wake(runtime, reader, result, 0);
    }
    if (writer != NULL && writer != reader) {
        writer->wait_fd = -1;
        rco_task_wake(runtime, writer, result, 0);
    }
}

static int rco_timeout_ms(const struct rco_runtime *runtime)
{
    if (runtime->timer_count == 0) {
        return -1;
    }

    uint64_t now = 0;
    if (rco_now_ns(&now) != 0) {
        return 0;
    }
    uint64_t deadline = runtime->timer_heap[0]->deadline_ns;
    if (deadline <= now) {
        return 0;
    }
    uint64_t remaining = deadline - now;
    uint64_t milliseconds = (remaining + 999999ULL) / 1000000ULL;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static void rco_expire_timers(struct rco_runtime *runtime)
{
    if (runtime->timer_count == 0) {
        return;
    }

    uint64_t now = 0;
    int result = rco_now_ns(&now);
    if (result != 0) {
        rco_record_fatal(runtime, result);
        return;
    }

    while (runtime->timer_count != 0 &&
           runtime->timer_heap[0]->deadline_ns <= now) {
        struct rco_task *task = runtime->timer_heap[0];
        rco_task_wake(runtime, task,
                      task->state == RCO_TASK_WAIT_IO ? -ETIMEDOUT : 0, 0);
    }
}

static void rco_dispatch_event(struct rco_runtime *runtime,
                               const struct epoll_event *event)
{
    int fd = (int)(uint32_t)event->data.u64;
    uint32_t generation = (uint32_t)(event->data.u64 >> 32);
    if (fd < 0 || (size_t)fd >= runtime->config.max_fds) {
        return;
    }

    struct rco_fd_watch *watch = rco_watch_get(runtime, fd, false);
    if (watch == NULL) {
        return;
    }
    if (!watch->registered || watch->generation != generation) {
        return;
    }

    struct rco_task *reader = watch->reader;
    struct rco_task *writer = watch->writer;
    uint32_t kernel_events = event->events;
    bool read_ready =
        (kernel_events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;
    bool write_ready =
        (kernel_events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0;

    if (reader != NULL && writer == reader) {
        unsigned ready = 0;
        if (read_ready) {
            ready |= RCO_EVENT_READ;
        }
        if (write_ready) {
            ready |= RCO_EVENT_WRITE;
        }
        if (ready != 0) {
            rco_task_wake_from_event(runtime, watch, reader, ready);
        }
    } else {
        if (reader != NULL && read_ready) {
            rco_task_wake_from_event(runtime, watch, reader, RCO_EVENT_READ);
        }
        if (writer != NULL && write_ready) {
            rco_task_wake_from_event(runtime, watch, writer, RCO_EVENT_WRITE);
        }
    }

    rco_watch_advance_generation(watch);
    if (watch->reader != NULL || watch->writer != NULL) {
        int result = rco_watch_update(runtime, fd);
        if (result != 0) {
            rco_record_fatal(runtime, result);
            (void)rco_runtime_stop(runtime);
        }
    }
}

static void rco_remove_task(struct rco_runtime *runtime,
                            struct rco_task *task)
{
    *task->all_previous_next = task->all_next;
    if (task->all_next != NULL) {
        task->all_next->all_previous_next = task->all_previous_next;
    }

    rco_task_detach_wait(runtime, task);
    rco_stack_release(runtime, &task->stack);
    runtime->stats.active--;
    runtime->stats.completed++;
    if (task->state == RCO_TASK_CANCELLED) {
        runtime->stats.cancelled++;
    }
    if (task->finalizer != NULL) {
        task->finalizer(task->argument);
    }
    free(task);
}

static void rco_switch_to_root(struct rco_task *task)
{
    task->runtime->stats.context_switches++;
#if defined(RCO_WITH_ASAN)
    __sanitizer_start_switch_fiber(&task->asan_fake_stack,
                                   task->runtime->asan_stack_bottom,
                                   task->runtime->asan_stack_size);
#endif
    rco_context_switch(&task->context, &task->runtime->root_context);
#if defined(RCO_WITH_ASAN)
    __sanitizer_finish_switch_fiber(task->asan_fake_stack, NULL, NULL);
#endif
}

__attribute__((noreturn)) static void rco_task_returned(void)
{
    abort();
}

RCO_NO_ASAN __attribute__((noreturn)) static void rco_task_trampoline(void)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    struct rco_task *task = runtime->current;

#if defined(RCO_WITH_ASAN)
    __sanitizer_finish_switch_fiber(task->asan_fake_stack,
                                    &runtime->asan_stack_bottom,
                                    &runtime->asan_stack_size);
#endif

    if (task->cancel_requested || runtime->stop_requested) {
        task->state = RCO_TASK_CANCELLED;
    } else {
        (void)task->entry(task->argument);
        task->state = task->cancel_requested ? RCO_TASK_CANCELLED : RCO_TASK_DONE;
    }
#if defined(RCO_WITH_ASAN)
    __sanitizer_start_switch_fiber(NULL, runtime->asan_stack_bottom,
                                   runtime->asan_stack_size);
#endif
    task->runtime->stats.context_switches++;
    rco_context_switch(&task->context, &task->runtime->root_context);
    abort();
}

int rco_runtime_create(const struct rco_config *config,
                       struct rco_runtime **out_runtime)
{
    if (out_runtime == NULL) {
        return -EINVAL;
    }
    *out_runtime = NULL;

#if !defined(__linux__) || !defined(__x86_64__)
    (void)config;
    return -ENOTSUP;
#else
    struct rco_config validated;
    int result = rco_validate_config(config, &validated);
    if (result != 0) {
        return result;
    }
    result = rco_check_shadow_stack();
    if (result != 0) {
        return result;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !rco_is_power_of_two((size_t)page_size)) {
        return -ENOTSUP;
    }

    struct rco_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        return -ENOMEM;
    }
    runtime->epoll_fd = -1;
    runtime->config = validated;
    runtime->page_size = page_size;
    runtime->owner_tid = syscall(SYS_gettid);
    runtime->next_id = 1;

    runtime->fd_watch_chunk_count =
        (validated.max_fds + RCO_FD_WATCH_CHUNK_SIZE - 1) /
        RCO_FD_WATCH_CHUNK_SIZE;
    runtime->fd_watch_chunks =
        calloc(runtime->fd_watch_chunk_count,
               sizeof(*runtime->fd_watch_chunks));
    runtime->timer_heap =
        calloc(validated.max_coroutines, sizeof(*runtime->timer_heap));
    if (runtime->fd_watch_chunks == NULL || runtime->timer_heap == NULL) {
        free(runtime->timer_heap);
        free(runtime->fd_watch_chunks);
        free(runtime);
        return -ENOMEM;
    }

    runtime->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (runtime->epoll_fd < 0) {
        result = rco_neg_errno();
        free(runtime->timer_heap);
        free(runtime->fd_watch_chunks);
        free(runtime);
        return result;
    }

    *out_runtime = runtime;
    return 0;
#endif
}

int rco_runtime_destroy(struct rco_runtime *runtime)
{
    if (runtime == NULL) {
        return -EINVAL;
    }
    if (runtime->running || runtime->owner_tid != syscall(SYS_gettid)) {
        return -EBUSY;
    }

    rco_tls_runtime = runtime;
    while (runtime->all_tasks != NULL) {
        struct rco_task *task = runtime->all_tasks;
        runtime->all_tasks = task->all_next;
        if (runtime->all_tasks != NULL) {
            runtime->all_tasks->all_previous_next = &runtime->all_tasks;
        }
        if (task->finalizer != NULL) {
            task->finalizer(task->argument);
        }
        rco_stack_unmap(&task->stack);
        free(task);
    }
    rco_tls_runtime = NULL;
    if (runtime->epoll_fd >= 0) {
        (void)close(runtime->epoll_fd);
    }
    while (runtime->stack_cache != NULL) {
        struct rco_stack *stack = runtime->stack_cache;
        runtime->stack_cache = stack->next;
        rco_stack_unmap(stack);
        free(stack);
    }
    free(runtime->timer_heap);
    for (size_t index = 0; index < runtime->fd_watch_chunk_count; ++index) {
        free(runtime->fd_watch_chunks[index]);
    }
    free(runtime->fd_watch_chunks);
    free(runtime);
    return 0;
}

int rco_runtime_get_stats(const struct rco_runtime *runtime,
                          struct rco_stats *out_stats)
{
    if (runtime == NULL || out_stats == NULL) {
        return -EINVAL;
    }
    *out_stats = runtime->stats;
    return 0;
}

int rco_spawn_task(struct rco_runtime *runtime,
                   const struct rco_task_spec *spec,
                   uint64_t *out_task_id)
{
    if (runtime == NULL || spec == NULL || spec->entry == NULL) {
        return -EINVAL;
    }
    if (runtime->owner_tid != syscall(SYS_gettid)) {
        return -EPERM;
    }
    if (runtime->stop_requested) {
        return -ECANCELED;
    }
    if (runtime->stats.active >= runtime->config.max_coroutines) {
        return -EAGAIN;
    }
    if (runtime->next_id == 0) {
        return -EOVERFLOW;
    }

    size_t requested =
        spec->stack_size == 0 ? runtime->config.default_stack_size
                              : spec->stack_size;
    struct rco_task *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return -ENOMEM;
    }
    task->wait_fd = -1;
    task->timer_index = RCO_NO_TIMER;

    int result = rco_stack_map(runtime, requested, &task->stack);
    if (result != 0) {
        free(task);
        return result;
    }

    task->runtime = runtime;
    task->entry = spec->entry;
    task->finalizer = spec->finalizer;
    task->argument = spec->argument;
    task->id = runtime->next_id++;
    task->state = RCO_TASK_READY;
    rco_context_capture_fp(&task->context);

    uintptr_t stack_top =
        (uintptr_t)task->stack.base + task->stack.usable_size;
    uintptr_t *initial_stack =
        (uintptr_t *)(stack_top - 2 * sizeof(uintptr_t));
    initial_stack[0] = (uintptr_t)rco_task_trampoline;
    initial_stack[1] = (uintptr_t)rco_task_returned;
    task->context.rsp = (uintptr_t)initial_stack;

    task->all_previous_next = &runtime->all_tasks;
    task->all_next = runtime->all_tasks;
    if (task->all_next != NULL) {
        task->all_next->all_previous_next = &task->all_next;
    }
    runtime->all_tasks = task;
    rco_ready_push(runtime, task);
    runtime->stats.spawned++;
    runtime->stats.active++;
    if (runtime->stats.active > runtime->stats.peak_active) {
        runtime->stats.peak_active = runtime->stats.active;
    }
    if (out_task_id != NULL) {
        *out_task_id = task->id;
    }
    return 0;
}

int rco_spawn(struct rco_runtime *runtime,
              size_t stack_size,
              rco_entry_fn entry,
              void *argument,
              uint64_t *out_task_id)
{
    const struct rco_task_spec spec = {
        .stack_size = stack_size,
        .entry = entry,
        .argument = argument,
    };
    return rco_spawn_task(runtime, &spec, out_task_id);
}

int rco_cancel(struct rco_runtime *runtime, uint64_t task_id)
{
    if (runtime == NULL || task_id == 0) {
        return -EINVAL;
    }
    if (runtime->owner_tid != syscall(SYS_gettid)) {
        return -EPERM;
    }

    for (struct rco_task *task = runtime->all_tasks; task != NULL;
         task = task->all_next) {
        if (task->id != task_id) {
            continue;
        }
        if (task->state == RCO_TASK_DONE ||
            task->state == RCO_TASK_CANCELLED) {
            return -ESRCH;
        }
        task->cancel_requested = true;
        rco_task_wake(runtime, task, -ECANCELED, 0);
        return 0;
    }
    return -ESRCH;
}

int rco_runtime_stop(struct rco_runtime *runtime)
{
    if (runtime == NULL) {
        return -EINVAL;
    }
    if (runtime->owner_tid != syscall(SYS_gettid)) {
        return -EPERM;
    }

    runtime->stop_requested = true;
    for (struct rco_task *task = runtime->all_tasks; task != NULL;
         task = task->all_next) {
        if (task->state == RCO_TASK_DONE ||
            task->state == RCO_TASK_CANCELLED) {
            continue;
        }
        task->cancel_requested = true;
        rco_task_wake(runtime, task, -ECANCELED, 0);
    }
    return 0;
}

int rco_runtime_run(struct rco_runtime *runtime)
{
    if (runtime == NULL) {
        return -EINVAL;
    }
    if (runtime->running || rco_tls_runtime != NULL) {
        return -EBUSY;
    }
    if (runtime->owner_tid != syscall(SYS_gettid)) {
        return -EPERM;
    }

    runtime->running = true;
    rco_tls_runtime = runtime;
    struct epoll_event events[RCO_EPOLL_BATCH];
    size_t ready_dispatches = 0;

    while (runtime->stats.active != 0) {
        rco_expire_timers(runtime);

        struct rco_task *task = NULL;
        if (ready_dispatches < RCO_READY_DISPATCH_BUDGET) {
            task = rco_ready_pop(runtime);
        }
        if (task != NULL) {
            ready_dispatches++;
            if (task->cancel_requested && !task->started) {
                task->state = RCO_TASK_CANCELLED;
                rco_remove_task(runtime, task);
                continue;
            }

            task->state = RCO_TASK_RUNNING;
            task->started = true;
            runtime->current = task;
            runtime->stats.context_switches++;
#if defined(RCO_WITH_ASAN)
            __sanitizer_start_switch_fiber(
                &runtime->asan_fake_stack, task->stack.base,
                task->stack.usable_size);
#endif
            rco_context_switch(&runtime->root_context, &task->context);
#if defined(RCO_WITH_ASAN)
            __sanitizer_finish_switch_fiber(runtime->asan_fake_stack, NULL,
                                            NULL);
#endif
            runtime->current = NULL;

            if (task->state == RCO_TASK_DONE ||
                task->state == RCO_TASK_CANCELLED) {
                rco_remove_task(runtime, task);
            } else if (task->state == RCO_TASK_RUNNING) {
                rco_record_fatal(runtime, -EPROTO);
                task->state = RCO_TASK_CANCELLED;
                rco_remove_task(runtime, task);
            }
            continue;
        }

        int timeout =
            runtime->ready_head == NULL ? rco_timeout_ms(runtime) : 0;
        int count =
            epoll_wait(runtime->epoll_fd, events, RCO_EPOLL_BATCH, timeout);
        ready_dispatches = 0;
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            rco_record_fatal(runtime, rco_neg_errno());
            (void)rco_runtime_stop(runtime);
            continue;
        }
        for (int index = 0; index < count; ++index) {
            rco_dispatch_event(runtime, &events[index]);
        }
    }

    int result = runtime->fatal_error;
    runtime->current = NULL;
    rco_tls_runtime = NULL;
    runtime->running = false;
    return result;
}

int rco_yield(void)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    if (runtime == NULL || runtime->current == NULL) {
        return -EPERM;
    }
    struct rco_task *task = runtime->current;
    if (task->cancel_requested || runtime->stop_requested) {
        return -ECANCELED;
    }

    task->state = RCO_TASK_READY;
    rco_ready_push(runtime, task);
    rco_switch_to_root(task);
    return task->cancel_requested || runtime->stop_requested ? -ECANCELED : 0;
}

int rco_wait_fd(int fd,
                unsigned events,
                int timeout_ms,
                unsigned *out_ready_events)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    if (runtime == NULL || runtime->current == NULL) {
        return -EPERM;
    }
    if (fd < 0 || (size_t)fd >= runtime->config.max_fds ||
        events == 0 ||
        (events & ~(RCO_EVENT_READ | RCO_EVENT_WRITE)) != 0 ||
        out_ready_events == NULL) {
        return -EINVAL;
    }
    *out_ready_events = 0;

    struct rco_task *task = runtime->current;
    if (task->cancel_requested || runtime->stop_requested) {
        return -ECANCELED;
    }
    struct rco_fd_watch *watch = rco_watch_get(runtime, fd, true);
    if (watch == NULL) {
        return -ENOMEM;
    }
    if (((events & RCO_EVENT_READ) != 0 && watch->reader != NULL) ||
        ((events & RCO_EVENT_WRITE) != 0 && watch->writer != NULL)) {
        return -EBUSY;
    }

    task->wait_fd = fd;
    task->ready_events = 0;
    task->wait_result = 0;
    if ((events & RCO_EVENT_READ) != 0) {
        watch->reader = task;
    }
    if ((events & RCO_EVENT_WRITE) != 0) {
        watch->writer = task;
    }
    int result = rco_watch_update(runtime, fd);
    if (result != 0) {
        if (watch->reader == task) {
            watch->reader = NULL;
        }
        if (watch->writer == task) {
            watch->writer = NULL;
        }
        task->wait_fd = -1;
        return result;
    }

    if (timeout_ms >= 0) {
        uint64_t now = 0;
        result = rco_now_ns(&now);
        if (result != 0) {
            rco_task_detach_wait(runtime, task);
            return result;
        }
        uint64_t delay = (uint64_t)timeout_ms * 1000000ULL;
        task->deadline_ns =
            now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
        rco_timer_add(runtime, task);
    }

    task->state = RCO_TASK_WAIT_IO;
    runtime->stats.io_waits++;
    rco_switch_to_root(task);
    *out_ready_events = task->ready_events;
    return task->wait_result;
}

int rco_sleep_ms(uint64_t delay_ms)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    if (runtime == NULL || runtime->current == NULL) {
        return -EPERM;
    }
    struct rco_task *task = runtime->current;
    if (task->cancel_requested || runtime->stop_requested) {
        return -ECANCELED;
    }
    if (delay_ms > UINT64_MAX / 1000000ULL) {
        return -EOVERFLOW;
    }

    uint64_t now = 0;
    int result = rco_now_ns(&now);
    if (result != 0) {
        return result;
    }
    uint64_t delay = delay_ms * 1000000ULL;
    task->deadline_ns =
        now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    task->wait_fd = -1;
    task->wait_result = 0;
    task->ready_events = 0;
    task->state = RCO_TASK_WAIT_TIMER;
    rco_timer_add(runtime, task);
    rco_switch_to_root(task);
    return task->wait_result;
}

int rco_close_fd(int fd)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    if (runtime == NULL) {
        return -EPERM;
    }
    if (fd < 0 || (size_t)fd >= runtime->config.max_fds) {
        return -EINVAL;
    }

    rco_watch_invalidate(runtime, fd, -EBADF);
    if (close(fd) != 0) {
        return rco_neg_errno();
    }
    return 0;
}

bool rco_cancelled(void)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    return runtime != NULL && runtime->current != NULL &&
           (runtime->current->cancel_requested || runtime->stop_requested);
}

uint64_t rco_current_id(void)
{
    struct rco_runtime *runtime = rco_tls_runtime;
    return runtime == NULL || runtime->current == NULL
               ? 0
               : runtime->current->id;
}
