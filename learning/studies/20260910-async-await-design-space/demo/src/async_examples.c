#include "async_examples.h"

#include "mini_async.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void observation_init(aa_observation *observation) {
    assert(observation != NULL);
    *observation = (aa_observation){0};
}

static void trace_push(aa_observation *observation, char event) {
    size_t length = strlen(observation->trace);

    assert(length + 1U < sizeof(observation->trace));
    observation->trace[length] = event;
    observation->trace[length + 1U] = '\0';
}

typedef struct lazy_state {
    aa_observation *observation;
    bool finished;
} lazy_state;

static ma_poll lazy_poll(void *opaque, const ma_context *context) {
    lazy_state *state = opaque;

    (void)context;
    assert(!state->finished);
    trace_push(state->observation, 'A');
    state->finished = true;
    return MA_POLL_READY;
}

static const ma_future_vtable lazy_vtable = {
    .poll = lazy_poll,
    .drop = NULL,
};

bool aa_observe_lazy(aa_observation *observation) {
    ma_executor executor;
    ma_task task;
    lazy_state state;
    ma_future future;

    observation_init(observation);
    state = (lazy_state){
        .observation = observation,
    };
    future = ma_future_make(&state, &lazy_vtable);
    observation->constructor_was_inert = observation->trace[0] == '\0';

    ma_executor_init(&executor);
    if (!ma_executor_spawn(&executor, &task, future)) {
        return false;
    }
    if (observation->trace[0] != '\0') {
        return false;
    }
    if (!ma_executor_run_until_stalled(&executor, 8U)) {
        return false;
    }

    observation->task_polls = task.poll_count;
    observation->completed = task.complete;
    return true;
}

typedef struct ready_child_state {
    aa_observation *observation;
    bool finished;
} ready_child_state;

static ma_poll ready_child_poll(void *opaque, const ma_context *context) {
    ready_child_state *state = opaque;

    (void)context;
    assert(!state->finished);
    trace_push(state->observation, 'B');
    state->finished = true;
    return MA_POLL_READY;
}

static const ma_future_vtable ready_child_vtable = {
    .poll = ready_child_poll,
    .drop = NULL,
};

typedef struct dynamic_parent_state {
    aa_observation *observation;
    ma_future child;
    bool started;
} dynamic_parent_state;

static ma_poll dynamic_parent_poll(void *opaque, const ma_context *context) {
    dynamic_parent_state *state = opaque;

    if (!state->started) {
        state->started = true;
        trace_push(state->observation, 'A');
    }
    /* A ready child lets the parent continue without yielding to the executor. */
    if (ma_future_poll(&state->child, context) == MA_POLL_PENDING) {
        return MA_POLL_PENDING;
    }
    return MA_POLL_READY;
}

static void dynamic_parent_drop(void *opaque) {
    dynamic_parent_state *state = opaque;

    ma_future_drop(&state->child);
}

static const ma_future_vtable dynamic_parent_vtable = {
    .poll = dynamic_parent_poll,
    .drop = dynamic_parent_drop,
};

bool aa_observe_dynamic_await(aa_observation *observation) {
    ma_executor executor;
    ma_task task;
    ready_child_state child_state;
    dynamic_parent_state parent_state;

    observation_init(observation);
    child_state = (ready_child_state){
        .observation = observation,
    };
    parent_state = (dynamic_parent_state){
        .observation = observation,
        .child = ma_future_make(&child_state, &ready_child_vtable),
    };
    observation->constructor_was_inert = observation->trace[0] == '\0';

    ma_executor_init(&executor);
    if (!ma_executor_spawn(&executor, &task, ma_future_make(&parent_state, &dynamic_parent_vtable))) {
        return false;
    }
    if (!ma_executor_run_until_stalled(&executor, 8U)) {
        return false;
    }

    observation->task_polls = task.poll_count;
    observation->completed = task.complete;
    return true;
}

typedef struct event_state {
    aa_observation *observation;
    ma_waker waker;
    bool has_waker;
    bool ready;
    bool started;
} event_state;

static ma_poll event_poll(void *opaque, const ma_context *context) {
    event_state *state = opaque;

    if (!state->started) {
        state->started = true;
        trace_push(state->observation, 'A');
    }
    if (!state->ready) {
        /* Pending futures retain the latest waker needed to make progress. */
        state->waker = context->waker;
        state->has_waker = true;
        return MA_POLL_PENDING;
    }

    trace_push(state->observation, 'B');
    return MA_POLL_READY;
}

static const ma_future_vtable event_vtable = {
    .poll = event_poll,
    .drop = NULL,
};

static void event_signal(event_state *state, bool signal_twice) {
    assert(state->has_waker);
    state->ready = true;
    state->waker.wake(state->waker.data);
    if (signal_twice) {
        state->waker.wake(state->waker.data);
    }
}

static bool observe_event(aa_observation *observation, bool drop_handle) {
    ma_executor executor;
    ma_join_handle handle;
    ma_task task;
    event_state state;
    size_t ready_after_signal;

    observation_init(observation);
    state = (event_state){
        .observation = observation,
    };
    ma_executor_init(&executor);
    if (!ma_executor_spawn(&executor, &task, ma_future_make(&state, &event_vtable))) {
        return false;
    }
    handle = ma_task_join_handle(&task);
    observation->constructor_was_inert = observation->trace[0] == '\0';
    if (drop_handle) {
        ma_join_handle_drop(&handle);
    }

    if (!ma_executor_run_until_stalled(&executor, 8U)) {
        return false;
    }
    if (task.complete || ma_executor_ready_count(&executor) != 0U) {
        return false;
    }

    event_signal(&state, true);
    ready_after_signal = ma_executor_ready_count(&executor);
    if (!ma_executor_run_until_stalled(&executor, 8U)) {
        return false;
    }

    observation->task_polls = task.poll_count;
    observation->wakeups_coalesced = ready_after_signal == 1U && task.poll_count == 2U;
    observation->completed = task.complete;
    observation->handle_dropped = handle.task == NULL;
    return true;
}

bool aa_observe_wake(aa_observation *observation) {
    return observe_event(observation, false);
}

bool aa_observe_handle_drop(aa_observation *observation) {
    return observe_event(observation, true);
}

typedef struct cleanup_child_state {
    aa_observation *observation;
    bool resource_open;
} cleanup_child_state;

static ma_poll cleanup_child_poll(void *opaque, const ma_context *context) {
    cleanup_child_state *state = opaque;

    (void)context;
    if (!state->resource_open) {
        state->resource_open = true;
        trace_push(state->observation, 'A');
    }
    return MA_POLL_PENDING;
}

static void cleanup_child_drop(void *opaque) {
    cleanup_child_state *state = opaque;

    if (state->resource_open) {
        state->resource_open = false;
        state->observation->cleanup_ran = true;
        trace_push(state->observation, 'D');
    }
}

static const ma_future_vtable cleanup_child_vtable = {
    .poll = cleanup_child_poll,
    .drop = cleanup_child_drop,
};

typedef struct cancel_parent_state {
    ma_future child;
} cancel_parent_state;

static ma_poll cancel_parent_poll(void *opaque, const ma_context *context) {
    cancel_parent_state *state = opaque;

    return ma_future_poll(&state->child, context);
}

static void cancel_parent_drop(void *opaque) {
    cancel_parent_state *state = opaque;

    /* Dropping the owner recursively cancels its in-frame child future. */
    ma_future_drop(&state->child);
}

static const ma_future_vtable cancel_parent_vtable = {
    .poll = cancel_parent_poll,
    .drop = cancel_parent_drop,
};

bool aa_observe_cancel(aa_observation *observation) {
    ma_executor executor;
    ma_task task;
    cleanup_child_state child_state;
    cancel_parent_state parent_state;

    observation_init(observation);
    child_state = (cleanup_child_state){
        .observation = observation,
    };
    parent_state = (cancel_parent_state){
        .child = ma_future_make(&child_state, &cleanup_child_vtable),
    };
    observation->constructor_was_inert = observation->trace[0] == '\0';
    ma_executor_init(&executor);
    if (!ma_executor_spawn(&executor, &task, ma_future_make(&parent_state, &cancel_parent_vtable))) {
        return false;
    }
    if (!ma_executor_run_until_stalled(&executor, 8U)) {
        return false;
    }
    if (!child_state.resource_open) {
        return false;
    }

    ma_task_abort(&task);
    if (!ma_executor_run_until_stalled(&executor, 8U)) {
        return false;
    }

    observation->task_polls = task.poll_count;
    observation->completed = task.complete;
    observation->cancelled = task.cancelled;
    return true;
}
