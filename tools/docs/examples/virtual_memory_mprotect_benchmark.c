#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "virtual_memory_benchmark_support.h"

enum
{
    MAX_MEBIBYTES = 65536,
    MAX_ROUNDS = 10000,
    MAX_THREADS = CPU_SETSIZE
};

struct protect_shared
{
    volatile unsigned char *mapping;
    size_t length;
    size_t page_size;
    size_t worker_count;
    atomic_int ready;
    atomic_int start;
    atomic_int participating;
    atomic_int measurement_epoch;
    atomic_int stop;
    cpu_set_t allowed;
    int allowed_cpu_count;
};

struct protect_worker
{
    struct protect_shared *shared;
    int worker_index;
    atomic_int observed_epoch;
    uint64_t sum;
    uint64_t sweeps;
};

static volatile uint64_t observation_sink;

static size_t system_page_size(void)
{
    long page_size = sysconf(_SC_PAGESIZE);

    check(page_size > 0, "sysconf(_SC_PAGESIZE) failed");
    return (size_t)page_size;
}

static int expected_numa_node(void)
{
    const char *text = getenv("VM_BENCH_EXPECT_NODE");
    size_t node;

    if (text == NULL || *text == '\0')
        return -1;
    check(try_parse_bounded_size(text, 1023, &node) || strcmp(text, "0") == 0,
          "invalid VM_BENCH_EXPECT_NODE: %s",
          text);
    return strcmp(text, "0") == 0 ? 0 : (int)node;
}

static void verify_numa_line(const char *line, int expected_node)
{
    char copy[2048];
    char *save = NULL;
    char *token;
    size_t total_pages = 0;
    int observed_nodes = 0;

    check(strlen(line) < sizeof(copy), "numa_maps line is too long");
    strcpy(copy, line);
    for (token = strtok_r(copy, " \n", &save); token != NULL;
         token = strtok_r(NULL, " \n", &save))
    {
        int node;
        size_t pages;

        if (sscanf(token, "N%d=%zu", &node, &pages) != 2)
            continue;
        observed_nodes++;
        total_pages += pages;
        if (expected_node >= 0)
            check(node == expected_node,
                  "mapping reached NUMA node %d, expected node %d",
                  node,
                  expected_node);
    }
    check(observed_nodes > 0 && total_pages > 0,
          "numa_maps line has no resident node pages");
    printf("NUMA_VERIFY expected_node=%d observed_nodes=%d pages=%zu\n",
           expected_node,
           observed_nodes,
           total_pages);
}

static void print_numa_map(void *mapping)
{
    uintptr_t target = (uintptr_t)mapping;
    int expected_node = expected_numa_node();
    char line[2048];
    FILE *maps = fopen("/proc/self/numa_maps", "r");

    if (maps == NULL)
        fail("fopen /proc/self/numa_maps");
    while (fgets(line, sizeof(line), maps) != NULL)
    {
        unsigned long long start;

        if (sscanf(line, "%llx", &start) == 1 &&
            start == (unsigned long long)target)
        {
            printf("NUMA_MAP %s", line);
            verify_numa_line(line, expected_node);
            if (fclose(maps) != 0)
                fail("fclose /proc/self/numa_maps");
            return;
        }
    }
    check(!ferror(maps), "failed while reading /proc/self/numa_maps");
    if (fclose(maps) != 0)
        fail("fclose /proc/self/numa_maps");
    check(0, "mapping %p was not found in /proc/self/numa_maps", mapping);
}

static int nth_cpu(const cpu_set_t *set, int ordinal)
{
    int cpu;

    for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
    {
        if (!CPU_ISSET(cpu, set))
            continue;
        if (ordinal == 0)
            return cpu;
        ordinal--;
    }
    return -1;
}

static void pin_current_thread(int cpu)
{
    cpu_set_t target;
    int result;

    check(cpu >= 0, "could not resolve target CPU");
    CPU_ZERO(&target);
    CPU_SET(cpu, &target);
    result = pthread_setaffinity_np(pthread_self(), sizeof(target), &target);
    if (result != 0)
    {
        errno = result;
        fail("pthread_setaffinity_np");
    }
}

static uint64_t protect_sweep(const struct protect_worker *worker)
{
    const struct protect_shared *shared = worker->shared;
    uint64_t sum = 0;
    size_t offset;

    for (offset = (size_t)worker->worker_index * shared->page_size;
         offset < shared->length;
         offset += shared->worker_count * shared->page_size)
        sum += shared->mapping[offset];
    return sum;
}

static void *protect_worker_main(void *opaque)
{
    struct protect_worker *worker = opaque;
    struct protect_shared *shared = worker->shared;
    uint64_t sum = 0;
    int cpu = nth_cpu(&shared->allowed, worker->worker_index + 1);

    pin_current_thread(cpu);
    sum += protect_sweep(worker);
    atomic_fetch_add_explicit(&shared->ready, 1, memory_order_release);
    while (atomic_load_explicit(&shared->start, memory_order_acquire) == 0)
        sched_yield();

    sum += protect_sweep(worker);
    worker->sweeps = 1;
    atomic_fetch_add_explicit(&shared->participating, 1, memory_order_release);
    while (atomic_load_explicit(&shared->stop, memory_order_acquire) == 0)
    {
        int epoch;

        sum += protect_sweep(worker);
        worker->sweeps++;
        epoch = atomic_load_explicit(&shared->measurement_epoch,
                                     memory_order_acquire);
        if (epoch != 0)
            atomic_store_explicit(
                &worker->observed_epoch, epoch, memory_order_release);
    }
    worker->sum = sum;
    return NULL;
}

static void run_mprotect(size_t mebibytes, size_t thread_count, size_t rounds)
{
    const size_t page_size = system_page_size();
    const size_t length = mebibytes_to_bytes(mebibytes);
    struct protect_shared shared;
    struct protect_worker *workers;
    pthread_t *threads;
    uint64_t minimum_sweeps = UINT64_MAX;
    size_t index;
    double started;
    double elapsed;
    int coordinator_cpu;

    check(thread_count <= INT32_MAX && thread_count <= length / page_size,
          "too many mprotect workers");
    shared.mapping = mmap(NULL,
                          length,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1,
                          0);
    if (shared.mapping == MAP_FAILED)
        fail("mmap mprotect benchmark");
    if (madvise((void *)shared.mapping, length, MADV_NOHUGEPAGE) != 0)
        fail("madvise mprotect MADV_NOHUGEPAGE");
    shared.length = length;
    shared.page_size = page_size;
    shared.worker_count = thread_count;
    atomic_init(&shared.ready, 0);
    atomic_init(&shared.start, 0);
    atomic_init(&shared.participating, 0);
    atomic_init(&shared.measurement_epoch, 0);
    atomic_init(&shared.stop, 0);
    if (sched_getaffinity(0, sizeof(shared.allowed), &shared.allowed) != 0)
        fail("sched_getaffinity");
    shared.allowed_cpu_count = CPU_COUNT(&shared.allowed);
    check(shared.allowed_cpu_count > 1 &&
              thread_count <= (size_t)(shared.allowed_cpu_count - 1),
          "mprotect needs one coordinator CPU plus %zu reader CPUs; "
          "allowed CPUs=%d",
          thread_count,
          shared.allowed_cpu_count);
    coordinator_cpu = nth_cpu(&shared.allowed, 0);
    pin_current_thread(coordinator_cpu);

    for (index = 0; index < length; index += page_size)
        shared.mapping[index] = 1U;
    print_numa_map((void *)shared.mapping);

    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL)
        fail("allocate mprotect workers");
    for (index = 0; index < thread_count; index++)
    {
        int result;

        workers[index].shared = &shared;
        workers[index].worker_index = (int)index;
        atomic_init(&workers[index].observed_epoch, 0);
        result = pthread_create(
            &threads[index], NULL, protect_worker_main, &workers[index]);
        if (result != 0)
        {
            errno = result;
            fail("pthread_create");
        }
    }
    while (atomic_load_explicit(&shared.ready, memory_order_acquire) <
           (int)thread_count)
        sched_yield();
    atomic_store_explicit(&shared.start, 1, memory_order_release);
    while (atomic_load_explicit(&shared.participating, memory_order_acquire) <
           (int)thread_count)
        sched_yield();

    started = monotonic_seconds();
    for (index = 0; index < rounds; index++)
    {
        if (index == rounds / 2U)
            atomic_store_explicit(
                &shared.measurement_epoch, 1, memory_order_release);
        if (mprotect((void *)shared.mapping, length, PROT_READ) != 0)
            fail("mprotect read-only");
        if (mprotect((void *)shared.mapping, length, PROT_READ | PROT_WRITE) !=
            0)
            fail("mprotect read-write");
    }
    for (index = 0; index < thread_count; index++)
    {
        while (atomic_load_explicit(&workers[index].observed_epoch,
                                    memory_order_acquire) != 1)
            sched_yield();
    }
    elapsed = monotonic_seconds() - started;
    atomic_store_explicit(&shared.stop, 1, memory_order_release);

    for (index = 0; index < thread_count; index++)
    {
        int result = pthread_join(threads[index], NULL);

        if (result != 0)
        {
            errno = result;
            fail("pthread_join");
        }
        observation_sink ^= workers[index].sum;
        check(
            workers[index].sweeps > 0, "worker %zu did not participate", index);
        check(atomic_load_explicit(&workers[index].observed_epoch,
                                   memory_order_relaxed) == 1,
              "worker %zu missed the timed measurement epoch",
              index);
        if (workers[index].sweeps < minimum_sweeps)
            minimum_sweeps = workers[index].sweeps;
    }

    printf("RESULT mode=mprotect mib=%zu threads=%zu coordinator_cpu=%d "
           "reader_cpus=%d min_sweeps=%" PRIu64 " "
           "rounds=%zu transitions=%zu elapsed_ms=%.3f "
           "us_per_transition=%.3f\n",
           mebibytes,
           thread_count,
           coordinator_cpu,
           shared.allowed_cpu_count - 1,
           minimum_sweeps,
           rounds,
           rounds * 2U,
           elapsed * 1.0e3,
           elapsed * 1.0e6 / (double)(rounds * 2U));

    free(threads);
    free(workers);
    if (munmap((void *)shared.mapping, length) != 0)
        fail("munmap mprotect benchmark");
}

static void run_selftest(void)
{
    size_t parsed;

    check(!try_parse_bounded_size("-1", 100, &parsed),
          "parser accepted a negative value");
    check(!try_parse_bounded_size("0", 100, &parsed), "parser accepted zero");
    run_mprotect(4, 1, 2);
    puts("All mprotect benchmark selftests passed.");
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s selftest\n"
            "  %s mprotect <MiB> <threads> <rounds>\n",
            program,
            program);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "selftest") == 0)
    {
        run_selftest();
        finish_output();
        return EXIT_SUCCESS;
    }
    if (argc == 5 && strcmp(argv[1], "mprotect") == 0)
    {
        run_mprotect(parse_bounded_size(argv[2], "MiB", MAX_MEBIBYTES),
                     parse_bounded_size(argv[3], "threads", MAX_THREADS),
                     parse_bounded_size(argv[4], "rounds", MAX_ROUNDS));
        finish_output();
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
