#define _GNU_SOURCE

#include "rco_local.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS_PER_MS UINT64_C(1000000)
#define PREEMPT_QUANTUM_NS NS_PER_MS
#define PENDING_CPU_LIMIT_NS (UINT64_C(250) * NS_PER_MS)
#define WALL_LIMIT_NS (UINT64_C(2000) * NS_PER_MS)
#define OWNER_TIMER_QUANTUM_NS (UINT64_C(50) * NS_PER_MS)
#define HELPER_CPU_WORK_NS (UINT64_C(100) * NS_PER_MS)
#define SPIN_ITERATION_LIMIT UINT64_C(50000000)
#define INTERLEAVE_OBSERVATIONS 2
#define RECREATE_ITERATIONS 32

_Static_assert(
    _Generic(((struct rco_config *)0)->preempt_quantum_ns,
             uint64_t: 1, default: 0),
    "preempt_quantum_ns must be uint64_t");
_Static_assert(
    _Generic(((struct rco_config *)0)->preempt_signal, int: 1, default: 0),
    "preempt_signal must be int");
_Static_assert(offsetof(struct rco_config, preempt_quantum_ns) >
                   offsetof(struct rco_config, max_tls_keys),
               "preempt_quantum_ns must be appended to rco_config");
_Static_assert(offsetof(struct rco_config, preempt_signal) >
                   offsetof(struct rco_config, preempt_quantum_ns),
               "preempt_signal must follow preempt_quantum_ns");
_Static_assert(
    _Generic(((struct rco_stats *)0)->preemption_requests,
             uint64_t: 1, default: 0),
    "preemption_requests must be uint64_t");
_Static_assert(
    _Generic(((struct rco_stats *)0)->preemption_switches,
             uint64_t: 1, default: 0),
    "preemption_switches must be uint64_t");
_Static_assert(offsetof(struct rco_stats, preemption_switches) >
                   offsetof(struct rco_stats, preemption_requests),
               "preemption_switches must follow preemption_requests");

static int preempt_signal;
static struct sigaction expected_action;
static sigset_t expected_owner_mask;
static size_t baseline_process_timers;
static volatile sig_atomic_t unexpected_signal_deliveries;

static size_t count_process_timers(void);

static void check(bool condition, const char *expression, const char *file,
                  int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

static uint64_t clock_now_ns(clockid_t clock_id)
{
    struct timespec now;

    CHECK(clock_gettime(clock_id, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

struct spin_budget {
    uint64_t cpu_deadline_ns;
    uint64_t wall_deadline_ns;
};

static struct spin_budget make_spin_budget(void)
{
    struct spin_budget budget = {
        .cpu_deadline_ns =
            clock_now_ns(CLOCK_THREAD_CPUTIME_ID) + PENDING_CPU_LIMIT_NS,
        .wall_deadline_ns =
            clock_now_ns(CLOCK_MONOTONIC) + WALL_LIMIT_NS,
    };
    return budget;
}

static bool spin_budget_expired(const struct spin_budget *budget,
                                uint64_t iteration)
{
    if ((iteration & UINT64_C(4095)) != 0) {
        return false;
    }
    return clock_now_ns(CLOCK_THREAD_CPUTIME_ID) >=
               budget->cpu_deadline_ns ||
           clock_now_ns(CLOCK_MONOTONIC) >= budget->wall_deadline_ns;
}

static bool wait_for_preempt_pending(void)
{
    struct spin_budget budget = make_spin_budget();
    volatile uint64_t work = UINT64_C(0x9e3779b97f4a7c15);

    for (uint64_t iteration = 0; iteration < SPIN_ITERATION_LIMIT;
         ++iteration) {
        if (rco_preempt_pending()) {
            return true;
        }
        work ^= work << 7;
        work ^= work >> 9;
        work += iteration;
        if (spin_budget_expired(&budget, iteration)) {
            break;
        }
    }
    return false;
}

static bool signal_is_blocked(int signal_number)
{
    sigset_t current;

    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    int member = sigismember(&current, signal_number);
    CHECK(member >= 0);
    return member != 0;
}

static bool signal_sets_equal(const sigset_t *left, const sigset_t *right)
{
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        int left_member = sigismember(left, signal_number);
        int right_member = sigismember(right, signal_number);
        if (left_member != right_member) {
            return false;
        }
    }
    return true;
}

static void assert_owner_state_restored(void)
{
    struct sigaction current_action;
    sigset_t current_mask;

    CHECK(sigaction(preempt_signal, NULL, &current_action) == 0);
    CHECK(current_action.sa_flags == expected_action.sa_flags);
    CHECK(current_action.sa_sigaction == expected_action.sa_sigaction);
    CHECK(signal_sets_equal(&current_action.sa_mask,
                            &expected_action.sa_mask));
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current_mask) == 0);
    CHECK(signal_sets_equal(&current_mask, &expected_owner_mask));
}

static void unexpected_signal_handler(int signal_number, siginfo_t *info,
                                      void *context)
{
    (void)signal_number;
    (void)info;
    (void)context;
    unexpected_signal_deliveries++;
}

static void reserve_preempt_signal(struct sigaction *original_action,
                                   sigset_t *original_mask)
{
    preempt_signal = SIGRTMIN + 4;
    CHECK(preempt_signal <= SIGRTMAX);
    CHECK(sigaction(preempt_signal, NULL, original_action) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, original_mask) == 0);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = unexpected_signal_handler;
    action.sa_flags = SA_SIGINFO;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(preempt_signal, &action, NULL) == 0);
    CHECK(sigaction(preempt_signal, NULL, &expected_action) == 0);

    expected_owner_mask = *original_mask;
    CHECK(sigaddset(&expected_owner_mask, preempt_signal) == 0);
    CHECK(sigdelset(&expected_owner_mask, SIGUSR1) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &expected_owner_mask, NULL) == 0);
    assert_owner_state_restored();
    baseline_process_timers = count_process_timers();
}

static void release_preempt_signal(const struct sigaction *original_action,
                                   const sigset_t *original_mask)
{
    sigset_t pending;

    assert_owner_state_restored();
    CHECK(unexpected_signal_deliveries == 0);
    CHECK(sigpending(&pending) == 0);
    CHECK(sigismember(&pending, preempt_signal) == 0);
    CHECK(sigaction(preempt_signal, original_action, NULL) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, original_mask, NULL) == 0);
}

static struct rco_config enabled_config(void)
{
    struct rco_config config = {
        .local_state_flags =
            RCO_LOCAL_STATE_ERRNO | RCO_LOCAL_STATE_SIGNAL_MASK,
        .preempt_quantum_ns = PREEMPT_QUANTUM_NS,
        .preempt_signal = preempt_signal,
    };
    return config;
}

static void check_preempt_stats(const struct rco_runtime *runtime,
                                uint64_t minimum_switches)
{
    struct rco_stats stats;

    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.preemption_requests >= minimum_switches);
    CHECK(stats.preemption_switches >= minimum_switches);
    CHECK(stats.preemption_requests >= stats.preemption_switches);
}

static int noop_worker(void *argument)
{
    (void)argument;
    return 0;
}

static void test_public_guards_and_configuration(void)
{
    struct rco_runtime *runtime = NULL;
    struct rco_config quantum_without_signal = {
        .preempt_quantum_ns = PREEMPT_QUANTUM_NS,
    };
    struct rco_config non_realtime_signal = {
        .preempt_quantum_ns = PREEMPT_QUANTUM_NS,
        .preempt_signal = SIGUSR1,
    };
    struct rco_config out_of_range_signal = {
        .preempt_quantum_ns = PREEMPT_QUANTUM_NS,
        .preempt_signal = SIGRTMAX + 1,
    };
    struct rco_config valid = enabled_config();

    CHECK(rco_preempt_point() == -EPERM);
    CHECK(rco_preempt_disable() == -EPERM);
    CHECK(rco_preempt_enable() == -EPERM);
    CHECK(!rco_preempt_pending());

    CHECK(rco_runtime_create(&quantum_without_signal, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    CHECK(rco_runtime_create(&non_realtime_signal, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    CHECK(rco_runtime_create(&out_of_range_signal, &runtime) == -EINVAL);
    CHECK(runtime == NULL);
    assert_owner_state_restored();

    CHECK(rco_runtime_create(&valid, &runtime) == 0);
    CHECK(rco_runtime_destroy(runtime) == 0);
    CHECK(count_process_timers() == baseline_process_timers);
    assert_owner_state_restored();
}

struct disabled_case {
    volatile sig_atomic_t peer_ran;
    size_t safe_points;
};

static int disabled_preempt_worker(void *argument)
{
    struct disabled_case *test_case = argument;

    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        CHECK(rco_preempt_point() == 0);
        CHECK(!rco_preempt_pending());
        CHECK(test_case->peer_ran == 0);
        test_case->safe_points++;
    }
    return 0;
}

static int mark_peer_ran(void *argument)
{
    volatile sig_atomic_t *peer_ran = argument;
    *peer_ran = 1;
    return 0;
}

static void test_zero_quantum_disables_preemption(void)
{
    struct rco_config config = {0};
    struct rco_runtime *runtime = NULL;
    struct disabled_case test_case = {0};
    struct rco_stats stats;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, disabled_preempt_worker, &test_case, NULL) ==
          0);
    CHECK(rco_spawn(runtime, 0, mark_peer_ran,
                    (void *)&test_case.peer_ran, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.safe_points == 10000);
    CHECK(test_case.peer_ran == 1);
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.preemption_requests == 0);
    CHECK(stats.preemption_switches == 0);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();
}

struct owner_timer_case {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool start_helper;
    bool helper_done;
    bool pending_after_helper_cpu;
};

static struct timespec realtime_deadline(void)
{
    struct timespec deadline;

    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    return deadline;
}

static void *non_owner_cpu_worker(void *argument)
{
    struct owner_timer_case *test_case = argument;
    struct timespec deadline = realtime_deadline();

    CHECK(pthread_mutex_lock(&test_case->mutex) == 0);
    while (!test_case->start_helper) {
        CHECK(pthread_cond_timedwait(&test_case->condition,
                                     &test_case->mutex, &deadline) == 0);
    }
    CHECK(pthread_mutex_unlock(&test_case->mutex) == 0);

    uint64_t cpu_deadline =
        clock_now_ns(CLOCK_THREAD_CPUTIME_ID) + HELPER_CPU_WORK_NS;
    uint64_t wall_deadline = clock_now_ns(CLOCK_MONOTONIC) + WALL_LIMIT_NS;
    volatile uint64_t work = UINT64_C(0x13198a2e03707344);
    uint64_t iteration = 0;
    while (iteration < SPIN_ITERATION_LIMIT &&
           clock_now_ns(CLOCK_THREAD_CPUTIME_ID) < cpu_deadline &&
           clock_now_ns(CLOCK_MONOTONIC) < wall_deadline) {
        work = work * UINT64_C(2862933555777941757) + iteration;
        iteration++;
    }
    CHECK(clock_now_ns(CLOCK_THREAD_CPUTIME_ID) >= cpu_deadline);

    CHECK(pthread_mutex_lock(&test_case->mutex) == 0);
    test_case->helper_done = true;
    CHECK(pthread_cond_signal(&test_case->condition) == 0);
    CHECK(pthread_mutex_unlock(&test_case->mutex) == 0);
    return NULL;
}

static int owner_waiting_worker(void *argument)
{
    struct owner_timer_case *test_case = argument;
    struct timespec deadline = realtime_deadline();

    CHECK(!signal_is_blocked(preempt_signal));
    CHECK(pthread_mutex_lock(&test_case->mutex) == 0);
    test_case->start_helper = true;
    CHECK(pthread_cond_signal(&test_case->condition) == 0);
    while (!test_case->helper_done) {
        CHECK(pthread_cond_timedwait(&test_case->condition,
                                     &test_case->mutex, &deadline) == 0);
    }
    CHECK(pthread_mutex_unlock(&test_case->mutex) == 0);

    test_case->pending_after_helper_cpu = rco_preempt_pending();
    CHECK(!test_case->pending_after_helper_cpu);
    return 0;
}

static void test_timer_counts_only_owner_thread_cpu(void)
{
    struct rco_config config = enabled_config();
    struct rco_runtime *runtime = NULL;
    struct owner_timer_case test_case;
    struct rco_stats stats;
    pthread_t helper;

    config.preempt_quantum_ns = OWNER_TIMER_QUANTUM_NS;
    memset(&test_case, 0, sizeof(test_case));
    CHECK(pthread_mutex_init(&test_case.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&test_case.condition, NULL) == 0);
    CHECK(pthread_create(&helper, NULL, non_owner_cpu_worker, &test_case) ==
          0);

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, owner_waiting_worker, &test_case, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(!test_case.pending_after_helper_cpu);
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.preemption_requests == 0);
    CHECK(stats.preemption_switches == 0);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();

    CHECK(pthread_join(helper, NULL) == 0);
    CHECK(pthread_cond_destroy(&test_case.condition) == 0);
    CHECK(pthread_mutex_destroy(&test_case.mutex) == 0);
}

struct deferred_case {
    volatile sig_atomic_t peer_ran;
    bool pending_seen;
    int errno_after_signal;
};

static int no_safe_point_worker(void *argument)
{
    struct deferred_case *test_case = argument;
    volatile uint64_t work = UINT64_C(0x243f6a8885a308d3);

    CHECK(!signal_is_blocked(preempt_signal));
    errno = ENOMSG;
    test_case->pending_seen = wait_for_preempt_pending();
    test_case->errno_after_signal = errno;
    CHECK(test_case->pending_seen);
    CHECK(test_case->peer_ran == 0);
    for (size_t iteration = 0; iteration < 100000; ++iteration) {
        work = work * UINT64_C(6364136223846793005) + iteration;
    }
    CHECK(test_case->peer_ran == 0);
    return work == 0;
}

static void test_no_safe_point_is_not_force_switched(void)
{
    struct rco_config config = enabled_config();
    struct rco_runtime *runtime = NULL;
    struct deferred_case test_case = {0};
    struct rco_stats stats;
    const int root_errno = E2BIG;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, no_safe_point_worker, &test_case, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, mark_peer_ran,
                    (void *)&test_case.peer_ran, NULL) == 0);
    errno = root_errno;
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(errno == root_errno);
    CHECK(test_case.pending_seen);
    CHECK(test_case.errno_after_signal == ENOMSG);
    CHECK(test_case.peer_ran == 1);
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.preemption_requests >= 1);
    CHECK(stats.preemption_switches == 0);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();
}

struct interleave_case {
    volatile sig_atomic_t progress[2];
    size_t observations[2];
};

struct interleave_worker_case {
    struct interleave_case *shared;
    size_t index;
    int task_errno;
};

static void configure_task_signal_mask(size_t index)
{
    sigset_t task_signal;
    sigset_t forbidden;

    CHECK(!signal_is_blocked(preempt_signal));
    CHECK(sigemptyset(&forbidden) == 0);
    CHECK(sigaddset(&forbidden, preempt_signal) == 0);
    CHECK(rco_sigmask(SIG_BLOCK, &forbidden, NULL) == -EINVAL);
    CHECK(!signal_is_blocked(preempt_signal));

    CHECK(sigemptyset(&task_signal) == 0);
    CHECK(sigaddset(&task_signal, SIGUSR1) == 0);
    CHECK(rco_sigmask(index == 0 ? SIG_BLOCK : SIG_UNBLOCK, &task_signal,
                      NULL) == 0);
    CHECK(signal_is_blocked(SIGUSR1) == (index == 0));

    CHECK(sigaddset(&task_signal, preempt_signal) == 0);
    CHECK(rco_sigmask(SIG_SETMASK, &task_signal, NULL) == -EINVAL);
    CHECK(!signal_is_blocked(preempt_signal));
    CHECK(signal_is_blocked(SIGUSR1) == (index == 0));
}

static int interleave_worker(void *argument)
{
    struct interleave_worker_case *test_case = argument;
    struct interleave_case *shared = test_case->shared;
    size_t index = test_case->index;
    size_t peer = 1 - index;
    sig_atomic_t peer_progress = shared->progress[peer];
    struct spin_budget budget = make_spin_budget();

    configure_task_signal_mask(index);
    errno = test_case->task_errno;
    for (uint64_t iteration = 0; iteration < SPIN_ITERATION_LIMIT;
         ++iteration) {
        shared->progress[index]++;
        CHECK(rco_preempt_point() == 0);
        CHECK(errno == test_case->task_errno);

        if (shared->progress[peer] != peer_progress) {
            peer_progress = shared->progress[peer];
            CHECK(!signal_is_blocked(preempt_signal));
            CHECK(signal_is_blocked(SIGUSR1) == (index == 0));
            shared->observations[index]++;
            if (shared->observations[index] >= INTERLEAVE_OBSERVATIONS &&
                shared->observations[peer] >= INTERLEAVE_OBSERVATIONS) {
                break;
            }
        }
        if (spin_budget_expired(&budget, iteration)) {
            break;
        }
    }
    if (shared->observations[index] < INTERLEAVE_OBSERVATIONS) {
        fprintf(stderr,
                "interleave timeout: worker=%zu progress=%d peer=%d "
                "observations=%zu pending=%d\n",
                index, shared->progress[index], shared->progress[peer],
                shared->observations[index], rco_preempt_pending());
    }
    CHECK(shared->observations[index] >= INTERLEAVE_OBSERVATIONS);
    return 0;
}

static void test_safe_points_interleave_and_preserve_state(void)
{
    struct rco_config config = enabled_config();
    struct rco_runtime *runtime = NULL;
    struct interleave_case shared = {0};
    struct interleave_worker_case first = {
        .shared = &shared,
        .index = 0,
        .task_errno = EDOM,
    };
    struct interleave_worker_case second = {
        .shared = &shared,
        .index = 1,
        .task_errno = ERANGE,
    };
    const int root_errno = ENOTTY;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, interleave_worker, &first, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, interleave_worker, &second, NULL) == 0);
    errno = root_errno;
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(errno == root_errno);
    CHECK(shared.progress[0] > 0);
    CHECK(shared.progress[1] > 0);
    CHECK(shared.observations[0] >= INTERLEAVE_OBSERVATIONS);
    CHECK(shared.observations[1] >= INTERLEAVE_OBSERVATIONS);
    check_preempt_stats(runtime, 4);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();
}

struct nested_disable_case {
    volatile sig_atomic_t peer_ran;
    bool pending_before_enable;
    bool pending_after_inner_enable;
    bool pending_after_outer_enable;
    int inner_enable_result;
    int outer_enable_result;
    int underflow_result;
};

static int nested_disable_worker(void *argument)
{
    struct nested_disable_case *test_case = argument;

    CHECK(rco_preempt_disable() == 0);
    CHECK(rco_preempt_disable() == 0);
    errno = EINTR;
    test_case->pending_before_enable = wait_for_preempt_pending();
    CHECK(errno == EINTR);
    CHECK(test_case->pending_before_enable);
    CHECK(test_case->peer_ran == 0);

    test_case->inner_enable_result = rco_preempt_enable();
    CHECK(test_case->inner_enable_result == 0);
    CHECK(errno == EINTR);
    test_case->pending_after_inner_enable = rco_preempt_pending();
    CHECK(test_case->pending_after_inner_enable);
    CHECK(test_case->peer_ran == 0);

    test_case->outer_enable_result = rco_preempt_enable();
    CHECK(test_case->outer_enable_result == 0);
    CHECK(errno == EINTR);
    CHECK(test_case->peer_ran == 1);
    test_case->pending_after_outer_enable = rco_preempt_pending();
    CHECK(!test_case->pending_after_outer_enable);
    test_case->underflow_result = rco_preempt_enable();
    CHECK(test_case->underflow_result == -EINVAL);
    return 0;
}

static void test_nested_disable_defers_one_switch(void)
{
    struct rco_config config = enabled_config();
    struct rco_runtime *runtime = NULL;
    struct nested_disable_case test_case = {0};
    struct rco_stats stats;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    CHECK(rco_spawn(runtime, 0, nested_disable_worker, &test_case, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, mark_peer_ran,
                    (void *)&test_case.peer_ran, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.pending_before_enable);
    CHECK(test_case.pending_after_inner_enable);
    CHECK(!test_case.pending_after_outer_enable);
    CHECK(test_case.inner_enable_result == 0);
    CHECK(test_case.outer_enable_result == 0);
    CHECK(test_case.underflow_result == -EINVAL);
    CHECK(test_case.peer_ran == 1);
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.preemption_requests >= 1);
    CHECK(stats.preemption_switches == 1);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();
}

struct cancellation_case {
    struct rco_runtime *runtime;
    uint64_t task_id;
    volatile sig_atomic_t peer_ran;
    bool pending_seen;
    int preempt_result;
};

static int self_cancelling_worker(void *argument)
{
    struct cancellation_case *test_case = argument;

    test_case->pending_seen = wait_for_preempt_pending();
    CHECK(test_case->pending_seen);
    CHECK(test_case->peer_ran == 0);
    CHECK(rco_cancel(test_case->runtime, test_case->task_id) == 0);
    CHECK(rco_cancelled());
    test_case->preempt_result = rco_preempt_point();
    CHECK(test_case->preempt_result == -ECANCELED);
    CHECK(test_case->peer_ran == 0);
    return 0;
}

static void test_cancellation_wins_over_pending(void)
{
    struct rco_config config = enabled_config();
    struct rco_runtime *runtime = NULL;
    struct cancellation_case test_case = {0};
    struct rco_stats stats;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    test_case.runtime = runtime;
    CHECK(rco_spawn(runtime, 0, self_cancelling_worker, &test_case,
                    &test_case.task_id) == 0);
    CHECK(rco_spawn(runtime, 0, mark_peer_ran,
                    (void *)&test_case.peer_ran, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.pending_seen);
    CHECK(test_case.preempt_result == -ECANCELED);
    CHECK(test_case.peer_ran == 1);
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.cancelled == 1);
    CHECK(stats.preemption_requests >= 1);
    CHECK(stats.preemption_switches == 0);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();
}

struct stop_case {
    struct rco_runtime *runtime;
    volatile sig_atomic_t peer_ran;
    bool pending_seen;
    int enable_result;
};

static int stopping_worker(void *argument)
{
    struct stop_case *test_case = argument;

    CHECK(rco_preempt_disable() == 0);
    test_case->pending_seen = wait_for_preempt_pending();
    CHECK(test_case->pending_seen);
    CHECK(test_case->peer_ran == 0);
    CHECK(rco_runtime_stop(test_case->runtime) == 0);
    CHECK(rco_cancelled());
    test_case->enable_result = rco_preempt_enable();
    CHECK(test_case->enable_result == -ECANCELED);
    CHECK(test_case->peer_ran == 0);
    return 0;
}

static void test_stop_wins_over_pending(void)
{
    struct rco_config config = enabled_config();
    struct rco_runtime *runtime = NULL;
    struct stop_case test_case = {0};
    struct rco_stats stats;

    CHECK(rco_runtime_create(&config, &runtime) == 0);
    test_case.runtime = runtime;
    CHECK(rco_spawn(runtime, 0, stopping_worker, &test_case, NULL) == 0);
    CHECK(rco_spawn(runtime, 0, mark_peer_ran,
                    (void *)&test_case.peer_ran, NULL) == 0);
    CHECK(rco_runtime_run(runtime) == 0);
    CHECK(test_case.pending_seen);
    CHECK(test_case.enable_result == -ECANCELED);
    CHECK(test_case.peer_ran == 0);
    CHECK(rco_runtime_get_stats(runtime, &stats) == 0);
    CHECK(stats.cancelled == 2);
    CHECK(stats.preemption_requests >= 1);
    CHECK(stats.preemption_switches == 0);
    assert_owner_state_restored();
    CHECK(rco_runtime_destroy(runtime) == 0);
    assert_owner_state_restored();
}

static size_t count_process_timers(void)
{
    FILE *timers = fopen("/proc/self/timers", "re");
    char line[256];
    size_t timer_count = 0;
    size_t line_count = 0;

    CHECK(timers != NULL);
    while (line_count < 4096 && fgets(line, sizeof(line), timers) != NULL) {
        if (strncmp(line, "ID:", 3) == 0) {
            timer_count++;
        }
        line_count++;
    }
    CHECK(!ferror(timers));
    CHECK(feof(timers));
    CHECK(fclose(timers) == 0);
    return timer_count;
}

static void test_repeated_lifecycle_restores_resources(void)
{
    struct rco_config config = enabled_config();

    for (size_t iteration = 0; iteration < RECREATE_ITERATIONS;
         ++iteration) {
        struct rco_runtime *runtime = NULL;
        int root_errno = (iteration & 1) == 0 ? EBUSY : ENOSPC;

        CHECK(rco_runtime_create(&config, &runtime) == 0);
        CHECK(rco_spawn(runtime, 0, noop_worker, NULL, NULL) == 0);
        errno = root_errno;
        CHECK(rco_runtime_run(runtime) == 0);
        CHECK(errno == root_errno);
        assert_owner_state_restored();
        CHECK(rco_runtime_destroy(runtime) == 0);
        assert_owner_state_restored();
        CHECK(count_process_timers() == baseline_process_timers);
    }
    CHECK(unexpected_signal_deliveries == 0);
}

int main(void)
{
    struct sigaction original_action;
    sigset_t original_mask;

    reserve_preempt_signal(&original_action, &original_mask);
    test_public_guards_and_configuration();
    test_zero_quantum_disables_preemption();
    test_timer_counts_only_owner_thread_cpu();
    test_no_safe_point_is_not_force_switched();
    test_safe_points_interleave_and_preserve_state();
    test_nested_disable_defers_one_switch();
    test_cancellation_wins_over_pending();
    test_stop_wins_over_pending();
    test_repeated_lifecycle_restores_resources();
    CHECK(count_process_timers() == baseline_process_timers);
    release_preempt_signal(&original_action, &original_mask);

    puts("rco preemption contract tests passed: 9 suites");
    return EXIT_SUCCESS;
}
