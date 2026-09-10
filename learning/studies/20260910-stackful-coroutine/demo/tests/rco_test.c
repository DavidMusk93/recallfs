#define _GNU_SOURCE

#include "rco.h"

#include <errno.h>
#include <fenv.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(bool condition, const char *expression, const char *file,
                  int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

struct order_case {
    int base;
    int *values;
    size_t *count;
};

static int record_with_nested_yield(struct order_case *test_case, int step)
{
    test_case->values[(*test_case->count)++] = test_case->base + step;
    return rco_yield();
}

static int order_worker(void *argument)
{
    struct order_case *test_case = argument;
    for (int step = 0; step < 3; ++step) {
        CHECK(record_with_nested_yield(test_case, step) == 0);
    }
    return test_case->base;
}

static void test_round_robin_and_nested_yield(void)
{
    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);

    int values[6] = {0};
    size_t count = 0;
    struct order_case first = {.base = 10, .values = values, .count = &count};
    struct order_case second = {.base = 20, .values = values, .count = &count};

    CHECK(rco_spawn(runtime, 0, order_worker, &first, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, order_worker, &second, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    const int expected[] = {10, 20, 11, 21, 12, 22};
    CHECK(count == sizeof(expected) / sizeof(expected[0]));
    CHECK(memcmp(values, expected, sizeof(expected)) == 0);

    struct rco_stats stats;
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.spawned == 2);
    CHECK(stats.completed == 2);
    CHECK(stats.context_switches >= 8);
    CHECK(stats.active == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct recursive_case {
    uint64_t before;
    uint64_t after;
};

__attribute__((noinline)) static uint64_t recursive_yield(unsigned depth)
{
    volatile uint64_t frame[8];
    for (size_t index = 0; index < 8; ++index) {
        frame[index] = ((uint64_t)depth << 32) ^ index;
    }

    if (depth == 0) {
        CHECK(rco_yield() == 0);
    } else {
        CHECK(recursive_yield(depth - 1) != UINT64_MAX);
    }

    uint64_t checksum = 0;
    for (size_t index = 0; index < 8; ++index) {
        checksum ^= frame[index];
    }
    return checksum;
}

static int recursive_worker(void *argument)
{
    struct recursive_case *test_case = argument;
    test_case->before = 1;
    test_case->after = recursive_yield(64);
    return 0;
}

static void test_deep_stack_survives_suspend(void)
{
    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);

    struct recursive_case test_case = {0};
    CHECK(rco_spawn(runtime, 0, recursive_worker, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.before == 1);
    CHECK(test_case.after == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct cancel_case {
    bool entered;
    bool finalized;
    int owned_fd;
    int close_result;
};

static int cancelled_worker(void *argument)
{
    struct cancel_case *test_case = argument;
    test_case->entered = true;
    return 0;
}

static void cancelled_worker_finalizer(void *argument)
{
    struct cancel_case *test_case = argument;
    test_case->close_result = rco_close_fd(test_case->owned_fd);
    test_case->finalized = true;
}

static void test_cancel_before_first_resume(void)
{
    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);

    int pipe_fds[2];
    CHECK(pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0);
    struct cancel_case test_case = {
        .owned_fd = pipe_fds[1],
        .close_result = -1,
    };
    uint64_t task_id = 0;
    const struct rco_task_spec spec = {
        .entry = cancelled_worker,
        .argument = &test_case,
        .finalizer = cancelled_worker_finalizer,
    };
    CHECK(rco_spawn_task(runtime, &spec, &task_id) == 0);
    CHECK(task_id != 0);
    CHECK(rco_cancel(runtime, task_id) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(!test_case.entered);
    CHECK(test_case.finalized);
    CHECK(test_case.close_result == 0);
    CHECK(fcntl(pipe_fds[1], F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(close(pipe_fds[0]) == 0);

    struct rco_stats stats;
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.cancelled == 1);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct pipe_case {
    int read_fd;
    int write_fd;
    char observed;
};

static int pipe_reader(void *argument)
{
    struct pipe_case *test_case = argument;
    unsigned ready = 0;
    CHECK(rco_wait_fd(test_case->read_fd, RCO_EVENT_READ, 1000, &ready) == 0);
    CHECK((ready & RCO_EVENT_READ) != 0);
    CHECK(read(test_case->read_fd, &test_case->observed, 1) == 1);
    return 0;
}

static int pipe_writer(void *argument)
{
    const struct pipe_case *test_case = argument;
    CHECK(rco_yield() == 0);
    CHECK(write(test_case->write_fd, "x", 1) == 1);
    return 0;
}

static void test_epoll_wakeup(void)
{
    int pipe_fds[2];
    CHECK(pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0);

    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);
    struct pipe_case test_case = {
        .read_fd = pipe_fds[0],
        .write_fd = pipe_fds[1],
    };

    CHECK(rco_spawn(runtime, 0, pipe_reader, &test_case, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, pipe_writer, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.observed == 'x');

    CHECK(close(pipe_fds[0]) == 0);
    CHECK(close(pipe_fds[1]) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct timeout_case {
    int read_fd;
    int result;
};

static int timeout_worker(void *argument)
{
    struct timeout_case *test_case = argument;
    unsigned ready = 0;
    test_case->result =
        rco_wait_fd(test_case->read_fd, RCO_EVENT_READ, 5, &ready);
    CHECK(ready == 0);
    return 0;
}

static void test_timeout(void)
{
    int pipe_fds[2];
    CHECK(pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0);

    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);
    struct timeout_case test_case = {.read_fd = pipe_fds[0]};

    CHECK(rco_spawn(runtime, 0, timeout_worker, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.result == -ETIMEDOUT);

    CHECK(close(pipe_fds[0]) == 0);
    CHECK(close(pipe_fds[1]) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct rounding_case {
    int mode;
    int observed;
};

static int rounding_worker(void *argument)
{
    struct rounding_case *test_case = argument;
    CHECK(fesetround(test_case->mode) == 0);
    CHECK(rco_yield() == 0);
    test_case->observed = fegetround();
    return 0;
}

static void test_floating_point_control_is_per_coroutine(void)
{
    int original = fegetround();
    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);

    struct rounding_case down = {.mode = FE_DOWNWARD};
    struct rounding_case up = {.mode = FE_UPWARD};
    CHECK(rco_spawn(runtime, 0, rounding_worker, &down, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, rounding_worker, &up, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(down.observed == FE_DOWNWARD);
    CHECK(up.observed == FE_UPWARD);
    CHECK(fegetround() == original);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

static int register_canary_worker(void *argument)
{
    (void)argument;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    register uint64_t value_bx __asm__("rbx") = 0x1122334455667788ULL;
    register uint64_t value_12 __asm__("r12") = 0x2233445566778899ULL;
    register uint64_t value_13 __asm__("r13") = 0x33445566778899aaULL;
    register uint64_t value_14 __asm__("r14") = 0x445566778899aabbULL;
    register uint64_t value_15 __asm__("r15") = 0x5566778899aabbccULL;
    __asm__ volatile("" : "+r"(value_bx), "+r"(value_12), "+r"(value_13),
                     "+r"(value_14), "+r"(value_15));
    CHECK(rco_yield() == 0);
    __asm__ volatile("" : "+r"(value_bx), "+r"(value_12), "+r"(value_13),
                     "+r"(value_14), "+r"(value_15));
    CHECK(value_bx == 0x1122334455667788ULL);
    CHECK(value_12 == 0x2233445566778899ULL);
    CHECK(value_13 == 0x33445566778899aaULL);
    CHECK(value_14 == 0x445566778899aabbULL);
    CHECK(value_15 == 0x5566778899aabbccULL);
#endif
    return 0;
}

static void test_callee_saved_registers_survive(void)
{
    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, register_canary_worker, NULL, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct stop_case {
    struct rco_runtime *runtime;
    int read_fd;
    int wait_result;
};

static int stopped_waiter(void *argument)
{
    struct stop_case *test_case = argument;
    unsigned ready = 0;
    test_case->wait_result =
        rco_wait_fd(test_case->read_fd, RCO_EVENT_READ, -1, &ready);
    return 0;
}

static int runtime_stopper(void *argument)
{
    struct stop_case *test_case = argument;
    CHECK(rco_yield() == 0);
    CHECK(rco_runtime_stop(test_case->runtime) == 0);
    return 0;
}

static void test_stop_cancels_waiters(void)
{
    int pipe_fds[2];
    CHECK(pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0);

    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(NULL, &runtime) == 0);
    struct stop_case test_case = {
        .runtime = runtime,
        .read_fd = pipe_fds[0],
    };
    CHECK(rco_spawn(runtime, 0, stopped_waiter, &test_case, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, runtime_stopper, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.wait_result == -ECANCELED);

    CHECK(close(pipe_fds[0]) == 0);
    CHECK(close(pipe_fds[1]) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

static int empty_worker(void *argument)
{
    (void)argument;
    return 0;
}

static void test_stack_cache_reuses_mapping(void)
{
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_MIN,
        .max_coroutines = 2,
        .max_fds = 16,
        .stack_cache_bytes = 2 * RCO_STACK_SIZE_MIN,
    };
    struct rco_runtime *runtime = NULL;
    CHECK(rco_runtime_create(&config, &runtime) == 0);

    CHECK(rco_spawn(runtime, 0, empty_worker, NULL, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(rco_spawn(runtime, 0, empty_worker, NULL, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    struct rco_stats stats;
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.stacks_mapped == 1);
    CHECK(stats.stacks_reused == 1);
    CHECK(stats.cached_stack_bytes != 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

static void test_invalid_arguments(void)
{
    struct rco_runtime *runtime = NULL;
    struct rco_config bad = {
        .default_stack_size = RCO_STACK_SIZE_MIN - 1,
        .max_coroutines = 1,
        .max_fds = 16,
    };
    CHECK(rco_runtime_create(&bad, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    CHECK(rco_runtime_create(NULL, NULL) == -EINVAL);
    CHECK(rco_runtime_destroy(NULL) == -EINVAL);
}

int main(void)
{
    test_invalid_arguments();
    test_round_robin_and_nested_yield();
    test_deep_stack_survives_suspend();
    test_cancel_before_first_resume();
    test_epoll_wakeup();
    test_timeout();
    test_floating_point_control_is_per_coroutine();
    test_callee_saved_registers_survive();
    test_stop_cancels_waiters();
    test_stack_cache_reuses_mapping();
    puts("rco native tests passed: 10 suites");
    return 0;
}
