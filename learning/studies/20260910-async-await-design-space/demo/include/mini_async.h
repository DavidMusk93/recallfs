#ifndef MINI_ASYNC_H
#define MINI_ASYNC_H

#include <stdbool.h>
#include <stddef.h>

#define MA_EXECUTOR_CAPACITY 16U

typedef enum ma_poll {
    MA_POLL_PENDING = 0,
    MA_POLL_READY = 1,
} ma_poll;

typedef struct ma_waker {
    void (*wake)(void *data);
    void *data;
} ma_waker;

typedef struct ma_context {
    ma_waker waker;
} ma_context;

typedef struct ma_future_vtable {
    ma_poll (*poll)(void *state, const ma_context *context);
    void (*drop)(void *state);
} ma_future_vtable;

typedef struct ma_future {
    void *state;
    const ma_future_vtable *vtable;
    bool dropped;
} ma_future;

struct ma_executor;

typedef struct ma_task {
    struct ma_executor *executor;
    ma_future future;
    unsigned poll_count;
    bool queued;
    bool complete;
    bool cancel_requested;
    bool cancelled;
    bool detached;
} ma_task;

typedef struct ma_executor {
    ma_task *queue[MA_EXECUTOR_CAPACITY];
    size_t head;
    size_t length;
    bool overflowed;
} ma_executor;

ma_future ma_future_make(void *state, const ma_future_vtable *vtable);
ma_poll ma_future_poll(ma_future *future, const ma_context *context);
void ma_future_drop(ma_future *future);

void ma_executor_init(ma_executor *executor);
bool ma_executor_spawn(ma_executor *executor, ma_task *task, ma_future future);
bool ma_executor_run_until_stalled(ma_executor *executor, unsigned max_polls);
size_t ma_executor_ready_count(const ma_executor *executor);

void ma_task_wake(ma_task *task);
void ma_task_abort(ma_task *task);
void ma_task_detach(ma_task *task);

#endif
