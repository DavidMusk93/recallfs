#define _GNU_SOURCE

#include "rco_local.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(RCO_VERSION_MINOR == 2,
               "coroutine-local state requires rco 0.2");
_Static_assert(sizeof(rco_tls_key_t) == sizeof(uint64_t),
               "rco_tls_key_t must be uint64_t");
_Static_assert(RCO_LOCAL_STATE_NONE != 0,
               "NONE must distinguish explicit legacy mode from defaults");
_Static_assert((RCO_LOCAL_STATE_NONE & RCO_LOCAL_STATE_ERRNO) == 0,
               "NONE and ERRNO must be distinct flags");
_Static_assert((RCO_LOCAL_STATE_SIGNAL_MASK & RCO_LOCAL_STATE_LOCALE) == 0,
               "signal-mask and locale state must be independently selectable");

static void check(bool condition, const char *expression, const char *file,
                  int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define TLS_HIGH_INDEX_KEY_COUNT 4096
#define TLS_HIGH_INDEX_TASK_COUNT 64

struct errno_case {
    int value;
    int observed[2];
};

static int errno_worker(void *argument)
{
    struct errno_case *test_case = argument;

    errno = test_case->value;
    for (size_t index = 0; index < 2; ++index) {
        CHECK(rco_yield() == 0);
        test_case->observed[index] = errno;
        errno = test_case->value;
    }
    return 0;
}

static void test_zero_flags_default_to_errno(void)
{
    const int root_errno = E2BIG;
    struct rco_config config = {0};
    struct rco_runtime *runtime = NULL;
    struct errno_case first = {.value = EDOM};
    struct errno_case second = {.value = ERANGE};

    CHECK(config.local_state_flags == 0);
    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, errno_worker, &first, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, errno_worker, &second, NULL) == 0);

    errno = root_errno;
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(errno == root_errno);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(first.observed[index] == first.value);
        CHECK(second.observed[index] == second.value);
    }

    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct tls_value_case {
    struct rco_runtime *runtime;
    rco_tls_key_t key;
    int value;
    void *observed[2];
};

static int tls_value_worker(void *argument)
{
    struct tls_value_case *test_case = argument;
    void *value = (void *)(uintptr_t)1;

    CHECK(rco_tls_get(test_case->runtime, test_case->key, &value) == 0);
    CHECK(value == NULL);
    CHECK(rco_tls_set(test_case->runtime, test_case->key, &test_case->value) ==
          0);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(rco_yield() == 0);
        CHECK(rco_tls_get(test_case->runtime, test_case->key, &value) == 0);
        test_case->observed[index] = value;
    }
    return 0;
}

struct tls_invalid_key_case {
    struct rco_runtime *runtime;
    rco_tls_key_t key;
    int get_result;
    int set_result;
};

static int tls_invalid_key_worker(void *argument)
{
    struct tls_invalid_key_case *test_case = argument;
    void *value = NULL;

    test_case->get_result =
        rco_tls_get(test_case->runtime, test_case->key, &value);
    test_case->set_result =
        rco_tls_set(test_case->runtime, test_case->key, test_case);
    return 0;
}

struct tls_destructor_case {
    struct rco_runtime *runtime;
    rco_tls_key_t key;
    int task_errno;
    size_t destructor_calls;
    size_t event_count;
    char events[PTHREAD_DESTRUCTOR_ITERATIONS + 1];
    int destructor_errno[PTHREAD_DESTRUCTOR_ITERATIONS];
    int finalizer_errno;
    int finalizer_get_result;
    int finalizer_set_result;
};

static void tls_rearming_destructor(void *value)
{
    struct tls_destructor_case *test_case = value;
    rco_tls_key_t forbidden_key = UINT64_MAX;
    void *current_value = (void *)(uintptr_t)1;

    CHECK(test_case->event_count < sizeof(test_case->events));
    test_case->events[test_case->event_count++] = 'D';
    CHECK(test_case->destructor_calls <
          sizeof(test_case->destructor_errno) /
              sizeof(test_case->destructor_errno[0]));
    test_case->destructor_errno[test_case->destructor_calls] = errno;
    test_case->destructor_calls++;
    CHECK(rco_tls_get(test_case->runtime, test_case->key, &current_value) ==
          0);
    CHECK(current_value == NULL);
    CHECK(rco_tls_key_create(test_case->runtime, NULL, &forbidden_key) ==
          -EPERM);
    CHECK(forbidden_key == 0);
    CHECK(rco_tls_key_delete(test_case->runtime, test_case->key) == -EPERM);
    CHECK(rco_tls_set(test_case->runtime, test_case->key, test_case) == 0);
}

static int tls_destructor_worker(void *argument)
{
    struct tls_destructor_case *test_case = argument;

    errno = test_case->task_errno;
    CHECK(rco_tls_set(test_case->runtime, test_case->key, test_case) == 0);
    return 0;
}

static void tls_user_finalizer(void *argument)
{
    struct tls_destructor_case *test_case = argument;
    void *value = (void *)(uintptr_t)1;

    CHECK(test_case->destructor_calls == PTHREAD_DESTRUCTOR_ITERATIONS);
    CHECK(test_case->event_count == PTHREAD_DESTRUCTOR_ITERATIONS);
    CHECK(test_case->event_count < sizeof(test_case->events));
    test_case->finalizer_get_result =
        rco_tls_get(test_case->runtime, test_case->key, &value);
    test_case->finalizer_set_result =
        rco_tls_set(test_case->runtime, test_case->key, test_case);
    CHECK(test_case->finalizer_get_result == -EPERM);
    CHECK(test_case->finalizer_set_result == -EPERM);
    CHECK(value == NULL);
    test_case->finalizer_errno = errno;
    test_case->events[test_case->event_count++] = 'F';
}

static void test_runtime_scoped_tls(void)
{
    struct rco_config config = {
        .local_state_flags = RCO_LOCAL_STATE_ERRNO,
        .max_tls_keys = 2,
    };
    struct rco_runtime *runtime = NULL;
    struct rco_runtime *other_runtime = NULL;
    rco_tls_key_t deleted_key = 0;
    rco_tls_key_t value_key = 0;
    rco_tls_key_t destructor_key = 0;
    void *root_value = NULL;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_runtime_create(&config, &other_runtime) == 0);

    CHECK(rco_tls_key_create(runtime, NULL, &deleted_key) == 0);
    CHECK(rco_tls_key_delete(runtime, deleted_key) == 0);

    struct tls_invalid_key_case deleted = {
        .runtime = runtime,
        .key = deleted_key,
    };
    CHECK(rco_spawn(runtime, 0, tls_invalid_key_worker, &deleted, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(deleted.get_result < 0);
    CHECK(deleted.set_result < 0);

    CHECK(rco_tls_key_create(runtime, NULL, &value_key) == 0);
    CHECK(value_key != deleted_key);
    CHECK(rco_tls_key_create(runtime, tls_rearming_destructor,
                             &destructor_key) == 0);
    rco_tls_key_t exhausted_key = 0;
    CHECK(rco_tls_key_create(runtime, NULL, &exhausted_key) == -EAGAIN);
    CHECK(exhausted_key == 0);

    CHECK(rco_tls_get(runtime, value_key, &root_value) == -EPERM);
    CHECK(rco_tls_set(runtime, value_key, &root_value) == -EPERM);

    struct tls_invalid_key_case stale = {
        .runtime = runtime,
        .key = deleted_key,
    };
    struct tls_value_case first = {
        .runtime = runtime,
        .key = value_key,
        .value = 101,
    };
    struct tls_value_case second = {
        .runtime = runtime,
        .key = value_key,
        .value = 202,
    };
    struct tls_destructor_case destructor = {
        .runtime = runtime,
        .key = destructor_key,
        .task_errno = EILSEQ,
    };
    const struct rco_task_spec destructor_spec = {
        .entry = tls_destructor_worker,
        .argument = &destructor,
        .finalizer = tls_user_finalizer,
    };

    const int root_errno = ENOTTY;
    CHECK(rco_spawn(runtime, 0, tls_invalid_key_worker, &stale, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, tls_value_worker, &first, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, tls_value_worker, &second, NULL) == 0);
    CHECK(rco_spawn_task(runtime, &destructor_spec, NULL) == 0);
    errno = root_errno;
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(errno == root_errno);

    CHECK(stale.get_result < 0);
    CHECK(stale.set_result < 0);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(first.observed[index] == &first.value);
        CHECK(second.observed[index] == &second.value);
    }
    CHECK(destructor.destructor_calls == PTHREAD_DESTRUCTOR_ITERATIONS);
    CHECK(destructor.event_count == PTHREAD_DESTRUCTOR_ITERATIONS + 1);
    for (size_t index = 0; index < PTHREAD_DESTRUCTOR_ITERATIONS; ++index) {
        CHECK(destructor.events[index] == 'D');
        CHECK(destructor.destructor_errno[index] == destructor.task_errno);
    }
    CHECK(destructor.events[PTHREAD_DESTRUCTOR_ITERATIONS] == 'F');
    CHECK(destructor.finalizer_errno == root_errno);
    CHECK(destructor.finalizer_get_result == -EPERM);
    CHECK(destructor.finalizer_set_result == -EPERM);

    struct tls_invalid_key_case cross_runtime = {
        .runtime = other_runtime,
        .key = value_key,
    };
    CHECK(rco_spawn(other_runtime, 0, tls_invalid_key_worker, &cross_runtime,
                    NULL) == 0);
    CHECK(rco_runtime_run(other_runtime) == 0);
    CHECK(cross_runtime.get_result < 0);
    CHECK(cross_runtime.set_result < 0);

    CHECK(rco_tls_key_delete(runtime, value_key) == 0);
    CHECK(rco_tls_key_delete(runtime, destructor_key) == 0);
    CHECK(rco_runtime_destroy(other_runtime) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct tls_high_index_case {
    struct rco_runtime *runtime;
    rco_tls_key_t original_key;
    rco_tls_key_t *replacement_key;
    int value;
    bool set_original;
    bool stale_rejected;
    bool cleared_on_reuse;
    bool replacement_round_tripped;
};

static int tls_high_index_worker(void *argument)
{
    struct tls_high_index_case *test_case = argument;
    void *value = NULL;

    CHECK(rco_tls_set(test_case->runtime, test_case->original_key,
                      &test_case->value) == 0);
    CHECK(rco_tls_get(test_case->runtime, test_case->original_key, &value) ==
          0);
    test_case->set_original = value == &test_case->value;
    CHECK(rco_yield() == 0);

    value = (void *)(uintptr_t)1;
    test_case->stale_rejected =
        rco_tls_get(test_case->runtime, test_case->original_key, &value) ==
            -EINVAL &&
        value == NULL;
    CHECK(*test_case->replacement_key != 0);
    CHECK(rco_tls_get(test_case->runtime, *test_case->replacement_key,
                      &value) == 0);
    test_case->cleared_on_reuse = value == NULL;
    CHECK(rco_tls_set(test_case->runtime, *test_case->replacement_key,
                      &test_case->value) == 0);
    CHECK(rco_tls_get(test_case->runtime, *test_case->replacement_key,
                      &value) == 0);
    test_case->replacement_round_tripped = value == &test_case->value;
    return 0;
}

struct tls_key_reuse_case {
    struct rco_runtime *runtime;
    rco_tls_key_t original_key;
    rco_tls_key_t *replacement_key;
};

static int tls_key_reuse_worker(void *argument)
{
    struct tls_key_reuse_case *test_case = argument;

    CHECK(rco_tls_key_delete(test_case->runtime, test_case->original_key) == 0);
    CHECK(rco_tls_key_create(test_case->runtime, NULL,
                             test_case->replacement_key) == 0);
    CHECK(*test_case->replacement_key != test_case->original_key);
    return 0;
}

static void test_high_index_tls_deletion_and_reuse(void)
{
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_MIN,
        .max_coroutines = TLS_HIGH_INDEX_TASK_COUNT + 1,
        .max_fds = 8,
        .stack_cache_bytes = 0,
        .local_state_flags = RCO_LOCAL_STATE_ERRNO,
        .max_tls_keys = RCO_TLS_KEYS_MAX,
    };
    struct rco_runtime *runtime = NULL;
    rco_tls_key_t keys[TLS_HIGH_INDEX_KEY_COUNT];
    rco_tls_key_t replacement_key = 0;
    struct tls_high_index_case cases[TLS_HIGH_INDEX_TASK_COUNT];

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    for (size_t index = 0; index < TLS_HIGH_INDEX_KEY_COUNT; ++index) {
        CHECK(rco_tls_key_create(runtime, NULL, &keys[index]) == 0);
    }

    for (size_t index = 0; index < TLS_HIGH_INDEX_TASK_COUNT; ++index) {
        cases[index] = (struct tls_high_index_case){
            .runtime = runtime,
            .original_key = keys[TLS_HIGH_INDEX_KEY_COUNT - 1],
            .replacement_key = &replacement_key,
            .value = (int)index + 1,
        };
        CHECK(rco_spawn(runtime, 0, tls_high_index_worker, &cases[index],
                        NULL) == 0);
    }
    struct tls_key_reuse_case reuse = {
        .runtime = runtime,
        .original_key = keys[TLS_HIGH_INDEX_KEY_COUNT - 1],
        .replacement_key = &replacement_key,
    };
    CHECK(rco_spawn(runtime, 0, tls_key_reuse_worker, &reuse, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    for (size_t index = 0; index < TLS_HIGH_INDEX_TASK_COUNT; ++index) {
        CHECK(cases[index].set_original);
        CHECK(cases[index].stale_rejected);
        CHECK(cases[index].cleared_on_reuse);
        CHECK(cases[index].replacement_round_tripped);
    }
    CHECK(rco_tls_key_delete(runtime, replacement_key) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct tls_delete_case {
    struct rco_runtime *runtime;
    rco_tls_key_t key;
    size_t destructor_calls;
    bool stale_rejected;
};

static void tls_deleted_value_destructor(void *value)
{
    struct tls_delete_case *test_case = value;

    test_case->destructor_calls++;
}

static int tls_delete_value_worker(void *argument)
{
    struct tls_delete_case *test_case = argument;
    void *value = (void *)(uintptr_t)1;

    CHECK(rco_tls_set(test_case->runtime, test_case->key, test_case) == 0);
    CHECK(rco_yield() == 0);
    test_case->stale_rejected =
        rco_tls_get(test_case->runtime, test_case->key, &value) == -EINVAL &&
        value == NULL;
    return 0;
}

static int tls_delete_key_worker(void *argument)
{
    struct tls_delete_case *test_case = argument;

    CHECK(rco_tls_key_delete(test_case->runtime, test_case->key) == 0);
    return 0;
}

static void test_tls_delete_skips_destructor(void)
{
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_MIN,
        .max_coroutines = 2,
        .max_fds = 8,
        .stack_cache_bytes = 0,
        .local_state_flags = RCO_LOCAL_STATE_ERRNO,
        .max_tls_keys = 1,
    };
    struct rco_runtime *runtime = NULL;
    struct tls_delete_case test_case = {
        .runtime = runtime,
    };

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    test_case.runtime = runtime;
    CHECK(rco_tls_key_create(runtime, tls_deleted_value_destructor,
                             &test_case.key) == 0);
    CHECK(rco_spawn(runtime, 0, tls_delete_value_worker, &test_case, NULL) ==
          0);
    CHECK(rco_spawn(runtime, 0, tls_delete_key_worker, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.stale_rejected);
    CHECK(test_case.destructor_calls == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
    CHECK(test_case.destructor_calls == 0);
}

static void test_invalid_local_state_configuration(void)
{
    struct rco_runtime *runtime = NULL;
    struct rco_config mixed_none = {
        .local_state_flags =
            RCO_LOCAL_STATE_NONE | RCO_LOCAL_STATE_ERRNO,
    };
    struct rco_config unknown = {
        .local_state_flags = UINT32_C(1) << 31,
    };
    struct rco_config too_many_keys = {
        .max_tls_keys = RCO_TLS_KEYS_MAX + 1,
    };

    CHECK(rco_runtime_create(&mixed_none, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    CHECK(rco_runtime_create(&unknown, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    CHECK(rco_runtime_create(&too_many_keys, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
}

static bool signal_is_blocked(int signal_number)
{
    sigset_t current;

    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    int member = sigismember(&current, signal_number);
    CHECK(member >= 0);
    return member != 0;
}

struct signal_case {
    bool block;
    bool observed[2];
};

static int signal_mask_worker(void *argument)
{
    struct signal_case *test_case = argument;
    sigset_t signal_set;
    sigset_t old_set;

    CHECK(sigemptyset(&signal_set) == 0);
    CHECK(sigaddset(&signal_set, SIGUSR1) == 0);
    CHECK(rco_sigmask(test_case->block ? SIG_BLOCK : SIG_UNBLOCK, &signal_set,
                      &old_set) == 0);
    CHECK(sigismember(&old_set, SIGUSR1) == 0);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(signal_is_blocked(SIGUSR1) == test_case->block);
        CHECK(rco_yield() == 0);
        test_case->observed[index] = signal_is_blocked(SIGUSR1);
    }
    return 0;
}

static void test_signal_masks_are_per_task(void)
{
    struct rco_config config = {
        .local_state_flags = RCO_LOCAL_STATE_SIGNAL_MASK,
    };
    sigset_t original_mask;
    sigset_t root_mask;
    struct rco_runtime *runtime = NULL;
    struct signal_case blocked = {.block = true};
    struct signal_case unblocked = {.block = false};

    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &original_mask) == 0);
    root_mask = original_mask;
    CHECK(sigdelset(&root_mask, SIGUSR1) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &root_mask, NULL) == 0);
    CHECK(!signal_is_blocked(SIGUSR1));

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, signal_mask_worker, &blocked, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, signal_mask_worker, &unblocked, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    for (size_t index = 0; index < 2; ++index) {
        CHECK(blocked.observed[index]);
        CHECK(!unblocked.observed[index]);
    }
    CHECK(!signal_is_blocked(SIGUSR1));
    CHECK(rco_runtime_destroy(runtime) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &original_mask, NULL) == 0);
}

struct reserved_signal_case {
    int signal_number;
    int block_result;
    int setmask_result;
};

static int reserved_signal_worker(void *argument)
{
    struct reserved_signal_case *test_case = argument;
    sigset_t signal_set;

    CHECK(sigemptyset(&signal_set) == 0);
    CHECK(sigaddset(&signal_set, test_case->signal_number) == 0);
    test_case->block_result = rco_sigmask(SIG_BLOCK, &signal_set, NULL);
    test_case->setmask_result = rco_sigmask(SIG_SETMASK, &signal_set, NULL);
    return 0;
}

static void test_preemption_signal_is_reserved(void)
{
    const int preempt_signal = SIGRTMIN + 5;
    struct rco_config config = {
        .local_state_flags = RCO_LOCAL_STATE_SIGNAL_MASK,
        .preempt_quantum_ns = UINT64_C(1000000),
        .preempt_signal = preempt_signal,
    };
    struct rco_runtime *runtime = NULL;
    struct reserved_signal_case test_case = {
        .signal_number = preempt_signal,
    };

    CHECK(preempt_signal <= SIGRTMAX);
    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, reserved_signal_worker, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.block_result == -EINVAL);
    CHECK(test_case.setmask_result == -EINVAL);
    CHECK(rco_runtime_destroy(runtime) == 0);
}

struct locale_case {
    locale_t source;
    locale_t root;
    locale_t borrowed_before;
    locale_t borrowed_after;
    bool survived_yield;
    bool finalizer_saw_root;
};

static int locale_worker(void *argument)
{
    struct locale_case *test_case = argument;
    locale_t source = test_case->source;

    CHECK(rco_locale_set(source) == 0);
    CHECK(rco_locale_get(&test_case->borrowed_before) == 0);
    CHECK(test_case->borrowed_before != (locale_t)0);
    freelocale(source);
    test_case->source = (locale_t)0;

    CHECK(uselocale((locale_t)0) == test_case->borrowed_before);
    CHECK(rco_yield() == 0);
    CHECK(rco_locale_get(&test_case->borrowed_after) == 0);
    CHECK(test_case->borrowed_after == test_case->borrowed_before);
    CHECK(uselocale((locale_t)0) == test_case->borrowed_after);
    test_case->survived_yield = true;
    return 0;
}

static void locale_finalizer(void *argument)
{
    struct locale_case *test_case = argument;

    test_case->finalizer_saw_root = uselocale((locale_t)0) == test_case->root;
}

static locale_t create_utf8_locale(void)
{
    static const char *const names[] = {"C.UTF-8", "C.utf8"};

    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        locale_t locale = newlocale(LC_ALL_MASK, names[index], (locale_t)0);
        if (locale != (locale_t)0) {
            return locale;
        }
    }
    return (locale_t)0;
}

static void test_locales_are_owned_and_per_task(void)
{
    struct rco_config config = {
        .local_state_flags = RCO_LOCAL_STATE_LOCALE,
    };
    locale_t original = uselocale((locale_t)0);
    locale_t root = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    struct rco_runtime *runtime = NULL;

    CHECK(root != (locale_t)0);
    CHECK(uselocale(root) == original);

    struct locale_case first = {
        .source = newlocale(LC_ALL_MASK, "C", (locale_t)0),
        .root = root,
    };
    struct locale_case second = {
        .source = create_utf8_locale(),
        .root = root,
    };
    CHECK(first.source != (locale_t)0);
    CHECK(second.source != (locale_t)0);
    const struct rco_task_spec first_spec = {
        .entry = locale_worker,
        .argument = &first,
        .finalizer = locale_finalizer,
    };
    const struct rco_task_spec second_spec = {
        .entry = locale_worker,
        .argument = &second,
        .finalizer = locale_finalizer,
    };

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn_task(runtime, &first_spec, NULL) == 0);
    CHECK(rco_spawn_task(runtime, &second_spec, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    CHECK(first.survived_yield);
    CHECK(second.survived_yield);
    CHECK(first.finalizer_saw_root);
    CHECK(second.finalizer_saw_root);
    CHECK(uselocale((locale_t)0) == root);
    CHECK(rco_runtime_destroy(runtime) == 0);

    CHECK(uselocale(original) == root);
    freelocale(root);
}

struct legacy_case {
    locale_t locale;
    int sigmask_result;
    int locale_set_result;
    int locale_get_result;
    size_t yields;
};

static int legacy_worker(void *argument)
{
    struct legacy_case *test_case = argument;
    sigset_t signal_set;
    locale_t borrowed = (locale_t)0;

    CHECK(sigemptyset(&signal_set) == 0);
    CHECK(sigaddset(&signal_set, SIGUSR1) == 0);
    test_case->sigmask_result = rco_sigmask(SIG_BLOCK, &signal_set, NULL);
    test_case->locale_set_result = rco_locale_set(test_case->locale);
    test_case->locale_get_result = rco_locale_get(&borrowed);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(rco_yield() == 0);
        test_case->yields++;
    }
    return 0;
}

static void test_explicit_none_uses_legacy_path(void)
{
    struct rco_config config = {
        .local_state_flags = RCO_LOCAL_STATE_NONE,
    };
    sigset_t original_mask;
    sigset_t root_mask;
    locale_t root_locale = uselocale((locale_t)0);
    struct rco_runtime *runtime = NULL;
    struct legacy_case first = {
        .locale = newlocale(LC_ALL_MASK, "C", (locale_t)0),
    };
    struct legacy_case second = {
        .locale = newlocale(LC_ALL_MASK, "C", (locale_t)0),
    };

    CHECK(first.locale != (locale_t)0);
    CHECK(second.locale != (locale_t)0);
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &original_mask) == 0);
    root_mask = original_mask;
    CHECK(sigdelset(&root_mask, SIGUSR1) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &root_mask, NULL) == 0);

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, legacy_worker, &first, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, legacy_worker, &second, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);

    CHECK(first.sigmask_result == -ENOTSUP);
    CHECK(first.locale_set_result == -ENOTSUP);
    CHECK(first.locale_get_result == -ENOTSUP);
    CHECK(first.yields == 2);
    CHECK(second.sigmask_result == -ENOTSUP);
    CHECK(second.locale_set_result == -ENOTSUP);
    CHECK(second.locale_get_result == -ENOTSUP);
    CHECK(second.yields == 2);
    CHECK(!signal_is_blocked(SIGUSR1));
    CHECK(uselocale((locale_t)0) == root_locale);

    CHECK(rco_runtime_destroy(runtime) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &original_mask, NULL) == 0);
    freelocale(first.locale);
    freelocale(second.locale);
}

int main(void)
{
    test_invalid_local_state_configuration();
    test_zero_flags_default_to_errno();
    test_runtime_scoped_tls();
    test_high_index_tls_deletion_and_reuse();
    test_tls_delete_skips_destructor();
    test_signal_masks_are_per_task();
    test_preemption_signal_is_reserved();
    test_locales_are_owned_and_per_task();
    test_explicit_none_uses_legacy_path();
    puts("rco coroutine-local state contract tests passed: 8 suites");
    return EXIT_SUCCESS;
}
