#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

enum mapping_kind
{
    MAPPING_BASE,
    MAPPING_THP,
    MAPPING_HUGETLB
};

enum walk_kind
{
    WALK_SEQUENTIAL,
    WALK_RANDOM
};

struct fault_counts
{
    long minor;
    long major;
};

struct process_memory
{
    long vm_size_kib;
    long vm_rss_kib;
    long vm_pte_kib;
};

struct mapping_info
{
    long rss_kib;
    long anon_huge_kib;
    long private_hugetlb_kib;
    long kernel_page_kib;
    long mmu_page_kib;
};

struct protect_shared
{
    volatile unsigned char *mapping;
    size_t length;
    size_t page_size;
    size_t worker_count;
    atomic_int ready;
    atomic_int stop;
    cpu_set_t allowed;
    int allowed_cpu_count;
};

struct protect_worker
{
    struct protect_shared *shared;
    int worker_index;
    uint64_t sum;
};

static volatile uint64_t observation_sink;
static uint64_t random_state = UINT64_C(0x9e3779b97f4a7c15);

static void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static void check(int condition, const char *format, ...)
{
    va_list arguments;

    if (condition)
        return;

    fputs("check failed: ", stderr);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime");
    return (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
}

static uint64_t next_random_u64(void)
{
    uint64_t value = random_state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    random_state = value;
    return value;
}

static size_t parse_positive_size(const char *text, const char *name)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    check(errno == 0 && end != text && *end == '\0' && value > 0 &&
              value <= SIZE_MAX,
          "invalid %s: %s",
          name,
          text);
    return (size_t)value;
}

static size_t system_page_size(void)
{
    long page_size = sysconf(_SC_PAGESIZE);

    check(page_size > 0, "sysconf(_SC_PAGESIZE) failed");
    return (size_t)page_size;
}

static size_t mebibytes_to_bytes(size_t mebibytes)
{
    const size_t scale = 1024U * 1024U;

    check(mebibytes <= SIZE_MAX / scale,
          "MiB value overflows size_t: %zu",
          mebibytes);
    return mebibytes * scale;
}

static struct fault_counts read_fault_counts_self(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0)
        fail("getrusage self");
    return (struct fault_counts){
        .minor = usage.ru_minflt,
        .major = usage.ru_majflt,
    };
}

static struct process_memory read_process_memory(void)
{
    struct process_memory memory = {
        .vm_size_kib = -1,
        .vm_rss_kib = -1,
        .vm_pte_kib = -1,
    };
    unsigned int found = 0;
    char line[256];
    FILE *status = fopen("/proc/self/status", "r");

    if (status == NULL)
        fail("fopen /proc/self/status");

    while (fgets(line, sizeof(line), status) != NULL)
    {
        long value;

        if ((found & 1U) == 0 && sscanf(line, "VmSize: %ld kB", &value) == 1)
        {
            memory.vm_size_kib = value;
            found |= 1U;
        }
        else if ((found & 2U) == 0 &&
                 sscanf(line, "VmRSS: %ld kB", &value) == 1)
        {
            memory.vm_rss_kib = value;
            found |= 2U;
        }
        else if ((found & 4U) == 0 &&
                 sscanf(line, "VmPTE: %ld kB", &value) == 1)
        {
            memory.vm_pte_kib = value;
            found |= 4U;
        }
    }

    check(!ferror(status), "failed while reading /proc/self/status");
    if (fclose(status) != 0)
        fail("fclose /proc/self/status");
    check(found == 7U, "missing fields in /proc/self/status: mask=%u", found);
    check(memory.vm_size_kib >= 0 && memory.vm_rss_kib >= 0 &&
              memory.vm_pte_kib >= 0,
          "negative process memory value");
    return memory;
}

static const char *mapping_kind_name(enum mapping_kind kind)
{
    switch (kind)
    {
    case MAPPING_BASE:
        return "base";
    case MAPPING_THP:
        return "thp";
    case MAPPING_HUGETLB:
        return "hugetlb";
    }
    return "unknown";
}

static enum mapping_kind parse_mapping_kind(const char *text)
{
    if (strcmp(text, "base") == 0)
        return MAPPING_BASE;
    if (strcmp(text, "thp") == 0)
        return MAPPING_THP;
    if (strcmp(text, "hugetlb") == 0)
        return MAPPING_HUGETLB;
    check(0, "invalid mapping kind: %s", text);
    return MAPPING_BASE;
}

static enum walk_kind parse_walk_kind(const char *text)
{
    if (strcmp(text, "seq") == 0)
        return WALK_SEQUENTIAL;
    if (strcmp(text, "random") == 0)
        return WALK_RANDOM;
    check(0, "invalid walk kind: %s", text);
    return WALK_SEQUENTIAL;
}

static void *map_memory(size_t length, enum mapping_kind kind)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void *mapping;

    if (kind == MAPPING_HUGETLB)
    {
        check(length % (2U * 1024U * 1024U) == 0,
              "HugeTLB length must be 2 MiB aligned");
        flags |= MAP_HUGETLB | MAP_HUGE_2MB;
    }

    mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapping == MAP_FAILED)
        fail("mmap benchmark memory");

    if (kind == MAPPING_BASE)
    {
        if (madvise(mapping, length, MADV_NOHUGEPAGE) != 0)
            fail("madvise MADV_NOHUGEPAGE");
    }
    else if (kind == MAPPING_THP)
    {
        if (madvise(mapping, length, MADV_HUGEPAGE) != 0)
            fail("madvise MADV_HUGEPAGE");
    }

    return mapping;
}

static void touch_pages(volatile unsigned char *mapping,
                        size_t length,
                        size_t page_size,
                        unsigned char value)
{
    size_t offset;

    for (offset = 0; offset < length; offset += page_size)
        mapping[offset] = value;
}

static struct mapping_info read_mapping_info(void *mapping)
{
    struct mapping_info info = {
        .rss_kib = -1,
        .anon_huge_kib = -1,
        .private_hugetlb_kib = -1,
        .kernel_page_kib = -1,
        .mmu_page_kib = -1,
    };
    uintptr_t target = (uintptr_t)mapping;
    int active = 0;
    char line[512];
    FILE *smaps = fopen("/proc/self/smaps", "r");

    if (smaps == NULL)
        fail("fopen /proc/self/smaps");

    while (fgets(line, sizeof(line), smaps) != NULL)
    {
        unsigned long long start;
        unsigned long long end;
        char permissions[8];
        long value;

        if (sscanf(line, "%llx-%llx %7s", &start, &end, permissions) == 3)
        {
            if (active)
                break;
            active = start == (unsigned long long)target;
            continue;
        }
        if (!active)
            continue;

        if (sscanf(line, "Rss: %ld kB", &value) == 1)
            info.rss_kib = value;
        else if (sscanf(line, "AnonHugePages: %ld kB", &value) == 1)
            info.anon_huge_kib = value;
        else if (sscanf(line, "Private_Hugetlb: %ld kB", &value) == 1)
            info.private_hugetlb_kib = value;
        else if (sscanf(line, "KernelPageSize: %ld kB", &value) == 1)
            info.kernel_page_kib = value;
        else if (sscanf(line, "MMUPageSize: %ld kB", &value) == 1)
            info.mmu_page_kib = value;
    }

    check(!ferror(smaps), "failed while reading /proc/self/smaps");
    if (fclose(smaps) != 0)
        fail("fclose /proc/self/smaps");
    check(active, "mapping %p was not found in /proc/self/smaps", mapping);
    return info;
}

static void print_numa_map(void *mapping)
{
    uintptr_t target = (uintptr_t)mapping;
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

static void shuffle_indices(uint32_t *indices, size_t count)
{
    size_t index;

    check(
        count > 1 && count <= UINT32_MAX, "invalid shuffle count: %zu", count);
    for (index = 0; index < count; index++)
        indices[index] = (uint32_t)index;
    for (index = count - 1; index > 0; index--)
    {
        size_t other = (size_t)(next_random_u64() % (index + 1));
        uint32_t temporary = indices[index];

        indices[index] = indices[other];
        indices[other] = temporary;
    }
}

static void run_fault(size_t mebibytes, size_t rounds)
{
    const size_t page_size = system_page_size();
    const size_t length = mebibytes_to_bytes(mebibytes);
    size_t round;

    check(length % page_size == 0, "fault size is not page aligned");

    for (round = 0; round < rounds; round++)
    {
        struct process_memory before_map = read_process_memory();
        struct process_memory after_map;
        struct process_memory after_touch;
        struct fault_counts before_first;
        struct fault_counts after_first;
        struct fault_counts before_second;
        struct fault_counts after_second;
        volatile unsigned char *mapping;
        double started;
        double reserve_seconds;
        double first_seconds;
        double second_seconds;

        started = monotonic_seconds();
        mapping = map_memory(length, MAPPING_BASE);
        reserve_seconds = monotonic_seconds() - started;
        after_map = read_process_memory();

        before_first = read_fault_counts_self();
        started = monotonic_seconds();
        touch_pages(mapping, length, page_size, (unsigned char)(round + 1));
        first_seconds = monotonic_seconds() - started;
        after_first = read_fault_counts_self();
        after_touch = read_process_memory();

        before_second = read_fault_counts_self();
        started = monotonic_seconds();
        touch_pages(mapping, length, page_size, (unsigned char)(round + 2));
        second_seconds = monotonic_seconds() - started;
        after_second = read_fault_counts_self();

        check(after_first.minor > before_first.minor,
              "first touch produced no minor faults");
        check(after_second.minor >= before_second.minor,
              "second-touch minor-fault counter moved backward");

        printf("RESULT mode=fault round=%zu mib=%zu reserve_us=%.3f "
               "first_ms=%.3f second_ms=%.3f first_minor=%ld "
               "first_major=%ld second_minor=%ld second_major=%ld "
               "vmsize_before_kib=%ld vmsize_map_kib=%ld "
               "rss_map_kib=%ld rss_touch_kib=%ld pte_map_kib=%ld "
               "pte_touch_kib=%ld\n",
               round,
               mebibytes,
               reserve_seconds * 1.0e6,
               first_seconds * 1.0e3,
               second_seconds * 1.0e3,
               after_first.minor - before_first.minor,
               after_first.major - before_first.major,
               after_second.minor - before_second.minor,
               after_second.major - before_second.major,
               before_map.vm_size_kib,
               after_map.vm_size_kib,
               after_map.vm_rss_kib,
               after_touch.vm_rss_kib,
               after_map.vm_pte_kib,
               after_touch.vm_pte_kib);

        if (munmap((void *)mapping, length) != 0)
            fail("munmap fault benchmark");
    }
}

static void run_walk(enum walk_kind walk,
                     enum mapping_kind mapping_kind,
                     size_t mebibytes,
                     size_t passes,
                     size_t rounds)
{
    const size_t page_size = system_page_size();
    const size_t length = mebibytes_to_bytes(mebibytes);
    const size_t page_count = length / page_size;
    volatile unsigned char *mapping;
    uint32_t *order;
    struct mapping_info info;
    size_t round;

    check(page_size > 0 && length % page_size == 0 && page_count <= UINT32_MAX,
          "invalid walk benchmark size");
    mapping = map_memory(length, mapping_kind);
    touch_pages(mapping, length, page_size, 1U);

    order = malloc(page_count * sizeof(*order));
    if (order == NULL)
        fail("malloc walk order");
    shuffle_indices(order, page_count);

    info = read_mapping_info((void *)mapping);
    printf("MAPPING mode=walk kind=%s access=%s mib=%zu "
           "rss_kib=%ld anon_huge_kib=%ld private_hugetlb_kib=%ld "
           "kernel_page_kib=%ld mmu_page_kib=%ld\n",
           mapping_kind_name(mapping_kind),
           walk == WALK_SEQUENTIAL ? "seq" : "random",
           mebibytes,
           info.rss_kib,
           info.anon_huge_kib,
           info.private_hugetlb_kib,
           info.kernel_page_kib,
           info.mmu_page_kib);
    print_numa_map((void *)mapping);

    for (round = 0; round < rounds; round++)
    {
        struct fault_counts before;
        struct fault_counts after;
        uint64_t sum = 0;
        size_t pass;
        double started;
        double elapsed;
        size_t accesses = page_count * passes;

        before = read_fault_counts_self();
        started = monotonic_seconds();
        for (pass = 0; pass < passes; pass++)
        {
            size_t index;

            if (walk == WALK_SEQUENTIAL)
            {
                for (index = 0; index < page_count; index++)
                    sum += mapping[index * page_size];
            }
            else
            {
                for (index = 0; index < page_count; index++)
                    sum += mapping[(size_t)order[index] * page_size];
            }
        }
        elapsed = monotonic_seconds() - started;
        after = read_fault_counts_self();
        observation_sink ^= sum;

        printf("RESULT mode=walk round=%zu map=%s access=%s mib=%zu "
               "passes=%zu accesses=%zu elapsed_ms=%.3f ns_per_access=%.3f "
               "minor=%ld major=%ld checksum=%" PRIu64 "\n",
               round,
               mapping_kind_name(mapping_kind),
               walk == WALK_SEQUENTIAL ? "seq" : "random",
               mebibytes,
               passes,
               accesses,
               elapsed * 1.0e3,
               elapsed * 1.0e9 / (double)accesses,
               after.minor - before.minor,
               after.major - before.major,
               sum);
    }

    free(order);
    if (munmap((void *)mapping, length) != 0)
        fail("munmap walk benchmark");
}

static uint64_t sum_words(const uint64_t *words, size_t word_count)
{
    uint64_t sum0 = 0;
    uint64_t sum1 = 0;
    uint64_t sum2 = 0;
    uint64_t sum3 = 0;
    size_t index = 0;

    for (; index + 4 <= word_count; index += 4)
    {
        sum0 += words[index];
        sum1 += words[index + 1];
        sum2 += words[index + 2];
        sum3 += words[index + 3];
    }
    for (; index < word_count; index++)
        sum0 += words[index];
    return sum0 + sum1 + sum2 + sum3;
}

static void
run_bandwidth(enum mapping_kind mapping_kind, size_t mebibytes, size_t rounds)
{
    const size_t length = mebibytes_to_bytes(mebibytes);
    const size_t word_count = length / sizeof(uint64_t);
    uint64_t *mapping = map_memory(length, mapping_kind);
    struct mapping_info info;
    size_t index;
    size_t round;

    for (index = 0; index < word_count; index++)
        mapping[index] = (uint64_t)(index % 251U);

    info = read_mapping_info(mapping);
    printf("MAPPING mode=bandwidth map=%s mib=%zu rss_kib=%ld "
           "anon_huge_kib=%ld private_hugetlb_kib=%ld "
           "kernel_page_kib=%ld mmu_page_kib=%ld\n",
           mapping_kind_name(mapping_kind),
           mebibytes,
           info.rss_kib,
           info.anon_huge_kib,
           info.private_hugetlb_kib,
           info.kernel_page_kib,
           info.mmu_page_kib);
    print_numa_map(mapping);

    for (round = 0; round < rounds; round++)
    {
        uint64_t sum;
        double started = monotonic_seconds();
        double elapsed;

        sum = sum_words(mapping, word_count);
        elapsed = monotonic_seconds() - started;
        observation_sink ^= sum;

        printf("RESULT mode=bandwidth round=%zu map=%s mib=%zu "
               "elapsed_ms=%.3f gib_per_s=%.3f checksum=%" PRIu64 "\n",
               round,
               mapping_kind_name(mapping_kind),
               mebibytes,
               elapsed * 1.0e3,
               ((double)length / (1024.0 * 1024.0 * 1024.0)) / elapsed,
               sum);
    }

    if (munmap(mapping, length) != 0)
        fail("munmap bandwidth benchmark");
}

static void wait_child(pid_t child, struct rusage *usage)
{
    int status;

    while (wait4(child, &status, 0, usage) < 0)
    {
        if (errno != EINTR)
            fail("wait4");
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child failed: status=%d",
          status);
}

static void run_cow(size_t mebibytes, size_t rounds)
{
    const size_t page_size = system_page_size();
    const size_t length = mebibytes_to_bytes(mebibytes);
    volatile unsigned char *mapping = map_memory(length, MAPPING_BASE);
    size_t round;

    touch_pages(mapping, length, page_size, 0x11U);

    for (round = 0; round < rounds; round++)
    {
        struct rusage baseline_usage;
        struct rusage cow_usage;
        pid_t child;
        double started;
        double baseline_seconds;
        double cow_seconds;
        size_t offset;

        started = monotonic_seconds();
        child = fork();
        if (child < 0)
            fail("fork baseline");
        if (child == 0)
            _exit(EXIT_SUCCESS);
        wait_child(child, &baseline_usage);
        baseline_seconds = monotonic_seconds() - started;

        started = monotonic_seconds();
        child = fork();
        if (child < 0)
            fail("fork COW");
        if (child == 0)
        {
            for (offset = 0; offset < length; offset += page_size)
                mapping[offset] = 0x22U;
            _exit(EXIT_SUCCESS);
        }
        wait_child(child, &cow_usage);
        cow_seconds = monotonic_seconds() - started;

        for (offset = 0; offset < length; offset += page_size)
            check(mapping[offset] == 0x11U,
                  "child changed parent page at offset %zu",
                  offset);

        printf("RESULT mode=cow round=%zu mib=%zu baseline_ms=%.3f "
               "baseline_minor=%ld baseline_major=%ld cow_ms=%.3f "
               "cow_minor=%ld cow_major=%ld delta_minor=%ld\n",
               round,
               mebibytes,
               baseline_seconds * 1.0e3,
               baseline_usage.ru_minflt,
               baseline_usage.ru_majflt,
               cow_seconds * 1.0e3,
               cow_usage.ru_minflt,
               cow_usage.ru_majflt,
               cow_usage.ru_minflt - baseline_usage.ru_minflt);
    }

    if (munmap((void *)mapping, length) != 0)
        fail("munmap COW benchmark");
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

static void *protect_worker_main(void *opaque)
{
    struct protect_worker *worker = opaque;
    struct protect_shared *shared = worker->shared;
    cpu_set_t target;
    uint64_t sum = 0;
    int cpu = nth_cpu(&shared->allowed,
                      worker->worker_index % shared->allowed_cpu_count);

    check(cpu >= 0, "worker could not resolve CPU");
    CPU_ZERO(&target);
    CPU_SET(cpu, &target);
    {
        int result =
            pthread_setaffinity_np(pthread_self(), sizeof(target), &target);

        if (result != 0)
        {
            errno = result;
            fail("pthread_setaffinity_np");
        }
    }

    atomic_fetch_add_explicit(&shared->ready, 1, memory_order_release);
    while (atomic_load_explicit(&shared->stop, memory_order_acquire) == 0)
    {
        size_t offset;

        for (offset = (size_t)worker->worker_index * shared->page_size;
             offset < shared->length;
             offset += shared->worker_count * shared->page_size)
            sum += shared->mapping[offset];
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
    size_t index;
    double started;
    double elapsed;

    check(thread_count <= INT32_MAX, "too many mprotect workers");
    shared.mapping = map_memory(length, MAPPING_BASE);
    shared.length = length;
    shared.page_size = page_size;
    shared.worker_count = thread_count;
    atomic_init(&shared.ready, 0);
    atomic_init(&shared.stop, 0);
    if (sched_getaffinity(0, sizeof(shared.allowed), &shared.allowed) != 0)
        fail("sched_getaffinity");
    shared.allowed_cpu_count = CPU_COUNT(&shared.allowed);
    check(shared.allowed_cpu_count > 0, "empty CPU affinity mask");

    touch_pages(shared.mapping, length, page_size, 1U);
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL)
        fail("allocate mprotect workers");

    for (index = 0; index < thread_count; index++)
    {
        workers[index].shared = &shared;
        workers[index].worker_index = (int)index;
        {
            int result = pthread_create(
                &threads[index], NULL, protect_worker_main, &workers[index]);

            if (result != 0)
            {
                errno = result;
                fail("pthread_create");
            }
        }
    }
    while (atomic_load_explicit(&shared.ready, memory_order_acquire) <
           (int)thread_count)
        sched_yield();

    started = monotonic_seconds();
    for (index = 0; index < rounds; index++)
    {
        if (mprotect((void *)shared.mapping, length, PROT_READ) != 0)
            fail("mprotect read-only");
        if (mprotect((void *)shared.mapping, length, PROT_READ | PROT_WRITE) !=
            0)
            fail("mprotect read-write");
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
    }

    printf("RESULT mode=mprotect mib=%zu threads=%zu allowed_cpus=%d "
           "rounds=%zu transitions=%zu elapsed_ms=%.3f "
           "us_per_transition=%.3f\n",
           mebibytes,
           thread_count,
           shared.allowed_cpu_count,
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
    puts("SELFTEST fault");
    run_fault(4, 1);
    puts("SELFTEST walk");
    run_walk(WALK_RANDOM, MAPPING_BASE, 8, 2, 1);
    puts("SELFTEST bandwidth");
    run_bandwidth(MAPPING_BASE, 8, 1);
    puts("SELFTEST cow");
    run_cow(4, 1);
    puts("SELFTEST mprotect");
    run_mprotect(4, 2, 2);
    puts("All virtual-memory benchmark selftests passed.");
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s selftest\n"
            "  %s fault <MiB> <rounds>\n"
            "  %s walk <seq|random> <base|thp|hugetlb> "
            "<MiB> <passes> <rounds>\n"
            "  %s bandwidth <base|thp|hugetlb> <MiB> <rounds>\n"
            "  %s cow <MiB> <rounds>\n"
            "  %s mprotect <MiB> <threads> <rounds>\n",
            program,
            program,
            program,
            program,
            program,
            program);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "selftest") == 0)
    {
        run_selftest();
        return EXIT_SUCCESS;
    }
    if (argc == 4 && strcmp(argv[1], "fault") == 0)
    {
        run_fault(parse_positive_size(argv[2], "MiB"),
                  parse_positive_size(argv[3], "rounds"));
        return EXIT_SUCCESS;
    }
    if (argc == 7 && strcmp(argv[1], "walk") == 0)
    {
        run_walk(parse_walk_kind(argv[2]),
                 parse_mapping_kind(argv[3]),
                 parse_positive_size(argv[4], "MiB"),
                 parse_positive_size(argv[5], "passes"),
                 parse_positive_size(argv[6], "rounds"));
        return EXIT_SUCCESS;
    }
    if (argc == 5 && strcmp(argv[1], "bandwidth") == 0)
    {
        run_bandwidth(parse_mapping_kind(argv[2]),
                      parse_positive_size(argv[3], "MiB"),
                      parse_positive_size(argv[4], "rounds"));
        return EXIT_SUCCESS;
    }
    if (argc == 4 && strcmp(argv[1], "cow") == 0)
    {
        run_cow(parse_positive_size(argv[2], "MiB"),
                parse_positive_size(argv[3], "rounds"));
        return EXIT_SUCCESS;
    }
    if (argc == 5 && strcmp(argv[1], "mprotect") == 0)
    {
        run_mprotect(parse_positive_size(argv[2], "MiB"),
                     parse_positive_size(argv[3], "threads"),
                     parse_positive_size(argv[4], "rounds"));
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
