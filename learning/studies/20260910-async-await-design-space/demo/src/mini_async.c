#include "mini_async.h"

#include <assert.h>

static bool ma_executor_enqueue(ma_executor *executor, ma_task *task) {
    size_t tail;

    /* Like Rust's Waker contract, repeated wakeups may share one queued poll. */
    if (task->complete || task->queued) {
        return true;
    }
    if (executor->length == MA_EXECUTOR_CAPACITY) {
        executor->overflowed = true;
        return false;
    }

    tail = (executor->head + executor->length) % MA_EXECUTOR_CAPACITY;
    executor->queue[tail] = task;
    executor->length += 1U;
    task->queued = true;
    return true;
}

static void ma_wake_task(void *data) {
    ma_task_wake(data);
}

ma_future ma_future_make(void *state, const ma_future_vtable *vtable) {
    ma_future future = {
        .state = state,
        .vtable = vtable,
        .dropped = false,
    };

    assert(state != NULL);
    assert(vtable != NULL);
    assert(vtable->poll != NULL);
    return future;
}

ma_poll ma_future_poll(ma_future *future, const ma_context *context) {
    assert(future != NULL);
    assert(!future->dropped);
    assert(future->vtable != NULL);
    assert(future->vtable->poll != NULL);
    return future->vtable->poll(future->state, context);
}

void ma_future_drop(ma_future *future) {
    assert(future != NULL);
    if (future->dropped) {
        return;
    }

    future->dropped = true;
    if (future->vtable->drop != NULL) {
        future->vtable->drop(future->state);
    }
}

void ma_executor_init(ma_executor *executor) {
    assert(executor != NULL);
    *executor = (ma_executor){0};
}

bool ma_executor_spawn(ma_executor *executor, ma_task *task, ma_future future) {
    assert(executor != NULL);
    assert(task != NULL);

    *task = (ma_task){
        .executor = executor,
        .future = future,
    };
    if (executor->active_tasks >= MA_EXECUTOR_CAPACITY) {
        ma_future_drop(&task->future);
        task->complete = true;
        return false;
    }

    executor->active_tasks += 1U;
    if (!ma_executor_enqueue(executor, task)) {
        executor->active_tasks -= 1U;
        ma_future_drop(&task->future);
        task->complete = true;
        return false;
    }
    return true;
}

bool ma_executor_run_until_stalled(ma_executor *executor, unsigned max_polls) {
    unsigned polls = 0U;

    assert(executor != NULL);
    while (executor->length > 0U) {
        ma_context context;
        ma_poll result;
        ma_task *task;

        task = executor->queue[executor->head];
        if (task->complete) {
            executor->head = (executor->head + 1U) % MA_EXECUTOR_CAPACITY;
            executor->length -= 1U;
            task->queued = false;
            continue;
        }
        if (polls == max_polls) {
            return false;
        }

        executor->head = (executor->head + 1U) % MA_EXECUTOR_CAPACITY;
        executor->length -= 1U;
        task->queued = false;

        if (task->cancel_requested) {
            /* Unaware cancellation drops the frame instead of resuming it. */
            ma_future_drop(&task->future);
            task->cancelled = true;
            task->complete = true;
            assert(executor->active_tasks > 0U);
            executor->active_tasks -= 1U;
            polls += 1U;
            continue;
        }

        context.waker = (ma_waker){
            .wake = ma_wake_task,
            .data = task,
        };
        task->poll_count += 1U;
        polls += 1U;
        result = ma_future_poll(&task->future, &context);
        if (result == MA_POLL_READY) {
            task->complete = true;
            ma_future_drop(&task->future);
            assert(executor->active_tasks > 0U);
            executor->active_tasks -= 1U;
        }
    }

    return !executor->overflowed;
}

size_t ma_executor_ready_count(const ma_executor *executor) {
    assert(executor != NULL);
    return executor->length;
}

void ma_task_wake(ma_task *task) {
    assert(task != NULL);
    assert(task->executor != NULL);
    (void)ma_executor_enqueue(task->executor, task);
}

void ma_task_abort(ma_task *task) {
    assert(task != NULL);
    if (task->complete) {
        return;
    }

    task->cancel_requested = true;
    ma_task_wake(task);
}

ma_join_handle ma_task_join_handle(ma_task *task) {
    ma_join_handle handle;

    assert(task != NULL);
    assert(task->executor != NULL);
    handle.task = task;
    return handle;
}

void ma_join_handle_drop(ma_join_handle *handle) {
    assert(handle != NULL);
    handle->task = NULL;
}
