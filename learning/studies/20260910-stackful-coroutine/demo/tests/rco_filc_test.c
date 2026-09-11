#define _GNU_SOURCE

#include "rco.h"
#include "rco_local.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void check(bool condition, const char *expression, const char *file,
                  int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

static int never_run(void *argument)
{
    (void)argument;
    abort();
}

static void count_finalizer(void *argument)
{
    size_t *count = argument;
    (*count)++;
}

static void test_public_guards_outside_runtime(void)
{
    unsigned ready = 0;
    void *value = NULL;
    locale_t locale = (locale_t)0;
    sigset_t mask;

    CHECK(sigemptyset(&mask) == 0);
    CHECK(rco_yield() == -EPERM);
    CHECK(rco_wait_fd(0, RCO_EVENT_READ, 0, &ready) == -EPERM);
    CHECK(rco_sleep_ms(0) == -EPERM);
    CHECK(rco_close_fd(0) == -EPERM);
    CHECK(rco_tls_get(NULL, 1, &value) == -EINVAL);
    CHECK(rco_tls_set(NULL, 1, value) == -EINVAL);
    CHECK(rco_sigmask(SIG_SETMASK, &mask, NULL) == -EPERM);
    CHECK(rco_locale_set(LC_GLOBAL_LOCALE) == -EPERM);
    CHECK(rco_locale_get(&locale) == -EPERM);
    CHECK(!rco_cancelled());
    CHECK(rco_current_id() == 0);
}

static void test_allocation_limits_and_teardown(void)
{
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_MIN,
        .max_coroutines = 8,
        .max_fds = 64,
        .stack_cache_bytes = 0,
    };
    struct rco_runtime *runtime = NULL;
    rco_tls_key_t key = 0;
    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_tls_key_create(runtime, NULL, &key) == 0);
    CHECK(key != 0);
    CHECK(rco_tls_key_delete(runtime, key) == 0);
    CHECK(rco_tls_key_delete(runtime, key) == -EINVAL);

    uint64_t ids[8] = {0};
    size_t finalized = 0;
    const struct rco_task_spec spec = {
        .entry = never_run,
        .argument = &finalized,
        .finalizer = count_finalizer,
    };
    for (size_t index = 0; index < 8; ++index) {
        CHECK(rco_spawn_task(runtime, &spec, &ids[index]) == 0);
        CHECK(ids[index] == index + 1);
    }
    CHECK(rco_spawn(runtime, 0, never_run, NULL, NULL) == -EAGAIN);
    CHECK(rco_cancel(runtime, ids[3]) == 0);
    CHECK(rco_cancel(runtime, UINT64_MAX) == -ESRCH);

    struct rco_stats stats;
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.spawned == 8);
    CHECK(stats.active == 8);
    CHECK(stats.peak_active == 8);
    CHECK(stats.stacks_mapped == 8);
    CHECK(stats.stacks_reused == 0);

    CHECK(rco_runtime_destroy(runtime) == 0);
    CHECK(finalized == 8);
}

static void test_repeated_create_destroy(void)
{
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_MIN,
        .max_coroutines = 2,
        .max_fds = 8,
        .stack_cache_bytes = 0,
    };
    for (size_t iteration = 0; iteration < 100; ++iteration) {
        struct rco_runtime *runtime = NULL;
        CHECK(rco_runtime_create(&config, &runtime) == 0);
        CHECK(rco_spawn(runtime, RCO_STACK_SIZE_MIN, never_run, NULL, NULL) ==
              0);
        CHECK(rco_runtime_destroy(runtime) == 0);
    }
}

static void test_local_state_config_guards(void)
{
    struct rco_runtime *runtime = NULL;
    struct rco_config mixed = {
        .local_state_flags =
            RCO_LOCAL_STATE_NONE | RCO_LOCAL_STATE_ERRNO,
    };
    struct rco_config too_many_keys = {
        .max_tls_keys = RCO_TLS_KEYS_MAX + 1,
    };

    CHECK(rco_runtime_create(&mixed, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    CHECK(rco_runtime_create(&too_many_keys, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
}

int main(void)
{
    test_public_guards_outside_runtime();
    test_local_state_config_guards();
    test_allocation_limits_and_teardown();
    test_repeated_create_destroy();
    puts("FIL-C rco tests passed: 4 suites");
    return 0;
}
