#include "async_examples.h"
#include "mini_async.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool test_lazy_construction(void) {
    aa_observation observation;

    CHECK(aa_observe_lazy(&observation));
    CHECK(strcmp(observation.trace, "A") == 0);
    CHECK(observation.constructor_was_inert);
    CHECK(observation.task_polls == 1U);
    CHECK(observation.completed);
    return true;
}

static bool test_dynamic_await(void) {
    aa_observation observation;

    CHECK(aa_observe_dynamic_await(&observation));
    CHECK(strcmp(observation.trace, "AB") == 0);
    CHECK(observation.constructor_was_inert);
    CHECK(observation.task_polls == 1U);
    CHECK(observation.completed);
    return true;
}

static bool test_wake_driven_repoll(void) {
    aa_observation observation;

    CHECK(aa_observe_wake(&observation));
    CHECK(strcmp(observation.trace, "AB") == 0);
    CHECK(observation.task_polls == 2U);
    CHECK(observation.wakeups_coalesced);
    CHECK(observation.completed);
    return true;
}

static bool test_handle_drop_does_not_abort(void) {
    aa_observation observation;

    CHECK(aa_observe_handle_drop(&observation));
    CHECK(strcmp(observation.trace, "AB") == 0);
    CHECK(observation.task_polls == 2U);
    CHECK(observation.handle_dropped);
    CHECK(observation.completed);
    return true;
}

static bool test_unaware_top_down_cancellation(void) {
    aa_observation observation;

    CHECK(aa_observe_cancel(&observation));
    CHECK(strcmp(observation.trace, "AD") == 0);
    CHECK(observation.constructor_was_inert);
    CHECK(observation.task_polls == 1U);
    CHECK(observation.cancelled);
    CHECK(observation.cleanup_ran);
    return true;
}

typedef struct drop_probe {
    unsigned drops;
} drop_probe;

static ma_poll drop_probe_poll(void *opaque, const ma_context *context) {
    (void)opaque;
    (void)context;
    return MA_POLL_PENDING;
}

static void drop_probe_drop(void *opaque) {
    drop_probe *probe = opaque;

    probe->drops += 1U;
}

static const ma_future_vtable drop_probe_vtable = {
    .poll = drop_probe_poll,
    .drop = drop_probe_drop,
};

static ma_poll self_wake_ready_poll(void *opaque, const ma_context *context) {
    (void)opaque;
    context->waker.wake(context->waker.data);
    return MA_POLL_READY;
}

static const ma_future_vtable self_wake_ready_vtable = {
    .poll = self_wake_ready_poll,
    .drop = drop_probe_drop,
};

static bool test_completed_self_wake_does_not_consume_budget(void) {
    ma_executor executor;
    ma_task task;
    drop_probe probe = {0};

    ma_executor_init(&executor);
    CHECK(ma_executor_spawn(&executor, &task, ma_future_make(&probe, &self_wake_ready_vtable)));
    CHECK(ma_executor_run_until_stalled(&executor, 1U));
    CHECK(ma_executor_ready_count(&executor) == 0U);
    CHECK(executor.active_tasks == 0U);
    CHECK(task.poll_count == 1U);
    CHECK(task.complete);
    CHECK(probe.drops == 1U);
    return true;
}

static bool test_failed_spawn_drops_future(void) {
    ma_executor executor;
    ma_task tasks[MA_EXECUTOR_CAPACITY + 1U];
    drop_probe probes[MA_EXECUTOR_CAPACITY + 1U] = {0};
    size_t index;

    ma_executor_init(&executor);
    for (index = 0U; index < MA_EXECUTOR_CAPACITY; index += 1U) {
        CHECK(ma_executor_spawn(&executor, &tasks[index], ma_future_make(&probes[index], &drop_probe_vtable)));
    }

    CHECK(!ma_executor_spawn(&executor, &tasks[MA_EXECUTOR_CAPACITY],
                             ma_future_make(&probes[MA_EXECUTOR_CAPACITY], &drop_probe_vtable)));
    CHECK(tasks[MA_EXECUTOR_CAPACITY].complete);
    CHECK(probes[MA_EXECUTOR_CAPACITY].drops == 1U);
    CHECK(executor.active_tasks == MA_EXECUTOR_CAPACITY);

    for (index = 0U; index < MA_EXECUTOR_CAPACITY; index += 1U) {
        ma_task_abort(&tasks[index]);
    }
    CHECK(ma_executor_run_until_stalled(&executor, MA_EXECUTOR_CAPACITY));
    CHECK(executor.active_tasks == 0U);
    for (index = 0U; index < MA_EXECUTOR_CAPACITY; index += 1U) {
        CHECK(probes[index].drops == 1U);
    }
    return true;
}

int main(void) {
    if (!test_lazy_construction() || !test_dynamic_await() || !test_wake_driven_repoll() ||
        !test_handle_drop_does_not_abort() || !test_unaware_top_down_cancellation() ||
        !test_completed_self_wake_does_not_consume_budget() || !test_failed_spawn_drops_future()) {
        return 1;
    }

    puts("mini_async_test: 7/7 checks passed");
    return 0;
}
