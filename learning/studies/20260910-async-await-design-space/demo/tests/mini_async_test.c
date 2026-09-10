#include "async_examples.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(                                                           \
                stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
                #condition);                                                   \
            return false;                                                      \
        }                                                                      \
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

static bool test_detached_task_keeps_running(void) {
    aa_observation observation;

    CHECK(aa_observe_detach(&observation));
    CHECK(strcmp(observation.trace, "AB") == 0);
    CHECK(observation.task_polls == 2U);
    CHECK(observation.detached);
    CHECK(observation.completed);
    return true;
}

static bool test_unaware_top_down_cancellation(void) {
    aa_observation observation;

    CHECK(aa_observe_cancel(&observation));
    CHECK(strcmp(observation.trace, "AD") == 0);
    CHECK(observation.task_polls == 1U);
    CHECK(observation.cancelled);
    CHECK(observation.cleanup_ran);
    return true;
}

int main(void) {
    if (!test_lazy_construction() || !test_dynamic_await() ||
        !test_wake_driven_repoll() || !test_detached_task_keeps_running() ||
        !test_unaware_top_down_cancellation()) {
        return 1;
    }

    puts("mini_async_test: 5/5 anchors passed");
    return 0;
}
