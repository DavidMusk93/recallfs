#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

struct child_report
{
    unsigned char before;
    unsigned char after;
    long minor_fault_delta;
    long major_fault_delta;
};

static void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static struct fault_counts read_fault_counts(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0)
        fail("getrusage");
    return (struct fault_counts){.minor = usage.ru_minflt,
                                 .major = usage.ru_majflt};
}

static struct process_memory read_process_memory(void)
{
    struct process_memory memory = {
        .vm_size_kib = -1, .vm_rss_kib = -1, .vm_pte_kib = -1};
    char line[256];
    FILE *status = fopen("/proc/self/status", "r");

    if (status == NULL)
        fail("fopen /proc/self/status");
    while (fgets(line, sizeof(line), status) != NULL)
    {
        (void)sscanf(line, "VmSize: %ld kB", &memory.vm_size_kib);
        (void)sscanf(line, "VmRSS: %ld kB", &memory.vm_rss_kib);
        (void)sscanf(line, "VmPTE: %ld kB", &memory.vm_pte_kib);
    }
    if (fclose(status) != 0)
        fail("fclose /proc/self/status");
    return memory;
}

static void write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    size_t written = 0;

    while (written < length)
    {
        ssize_t result = write(fd, bytes + written, length - written);

        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            fail("write");
        written += (size_t)result;
    }
}

static void read_all(int fd, void *buffer, size_t length)
{
    unsigned char *bytes = buffer;
    size_t consumed = 0;

    while (consumed < length)
    {
        ssize_t result = read(fd, bytes + consumed, length - consumed);

        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
        {
            if (result == 0)
                errno = EPIPE;
            fail("read");
        }
        consumed += (size_t)result;
    }
}

static void wait_for_success(pid_t child)
{
    int status;

    while (waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
            fail("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        fprintf(stderr, "child failed: status=%d\n", status);
        exit(EXIT_FAILURE);
    }
}

static void print_memory(const char *phase, struct process_memory memory)
{
    printf("  %-18s VmSize=%ld KiB  VmRSS=%ld KiB  VmPTE=%ld KiB\n",
           phase,
           memory.vm_size_kib,
           memory.vm_rss_kib,
           memory.vm_pte_kib);
}

static void demonstrate_address_split(size_t page_size)
{
    const uint64_t address = UINT64_C(0x12345);
    const uint64_t page = address / page_size;
    const uint64_t offset = address % page_size;

    assert(page * page_size + offset == address);
    assert(offset < page_size);
    puts("[1] Virtual-address split");
    printf("  address=0x%" PRIx64 " -> virtual-page=%" PRIu64
           ", offset=%" PRIu64 " (page size=%zu)\n",
           address,
           page,
           offset,
           page_size);
}

static void demonstrate_demand_paging(size_t page_size)
{
    const size_t page_count = 1024;
    const size_t length = page_count * page_size;
    struct process_memory before_map = read_process_memory();
    struct process_memory after_map;
    struct process_memory after_touch;
    struct fault_counts before_first_touch;
    struct fault_counts after_first_touch;
    struct fault_counts before_second_touch;
    struct fault_counts after_second_touch;
    volatile unsigned char *mapping;
    size_t index;

    mapping = mmap(NULL,
                   length,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED)
        fail("mmap anonymous");
#ifdef MADV_NOHUGEPAGE
    if (madvise((void *)mapping, length, MADV_NOHUGEPAGE) != 0)
        fail("madvise MADV_NOHUGEPAGE");
#endif

    after_map = read_process_memory();
    before_first_touch = read_fault_counts();
    for (index = 0; index < page_count; index++)
        mapping[index * page_size] = (unsigned char)(index & 0xffU);
    after_first_touch = read_fault_counts();
    after_touch = read_process_memory();
    before_second_touch = read_fault_counts();
    for (index = 0; index < page_count; index++)
        mapping[index * page_size] ^= 1U;
    after_second_touch = read_fault_counts();

    assert(after_first_touch.minor >= before_first_touch.minor);
    assert(after_first_touch.major >= before_first_touch.major);
    assert(after_second_touch.minor >= before_second_touch.minor);
    assert(after_second_touch.major >= before_second_touch.major);

    puts("[2] Reservation versus residency");
    printf("  anonymous mapping: address=%p, %zu pages (%zu KiB)\n",
           (void *)mapping,
           page_count,
           length / 1024);
    printf("  first pass faults:  minor=%ld major=%ld\n",
           after_first_touch.minor - before_first_touch.minor,
           after_first_touch.major - before_first_touch.major);
    printf("  second pass faults: minor=%ld major=%ld\n",
           after_second_touch.minor - before_second_touch.minor,
           after_second_touch.major - before_second_touch.major);
    print_memory("before mmap", before_map);
    print_memory("after mmap", after_map);
    print_memory("after first touch", after_touch);

    if (munmap((void *)mapping, length) != 0)
        fail("munmap anonymous");
}

static void demonstrate_copy_on_write(size_t page_size)
{
    unsigned char *mapping;
    struct child_report report;
    int pipe_fds[2];
    pid_t child;

    mapping = mmap(NULL,
                   page_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED)
        fail("mmap COW");
    mapping[0] = 0x11;
    if (pipe(pipe_fds) != 0)
        fail("pipe");

    child = fork();
    if (child < 0)
        fail("fork COW");
    if (child == 0)
    {
        struct fault_counts before = read_fault_counts();
        struct fault_counts after;

        if (close(pipe_fds[0]) != 0)
            fail("close child read pipe");
        report.before = mapping[0];
        mapping[0] = 0x22;
        report.after = mapping[0];
        after = read_fault_counts();
        report.minor_fault_delta = after.minor - before.minor;
        report.major_fault_delta = after.major - before.major;
        write_all(pipe_fds[1], &report, sizeof(report));
        if (close(pipe_fds[1]) != 0)
            fail("close child write pipe");
        _exit(EXIT_SUCCESS);
    }

    if (close(pipe_fds[1]) != 0)
        fail("close parent write pipe");
    read_all(pipe_fds[0], &report, sizeof(report));
    if (close(pipe_fds[0]) != 0)
        fail("close parent read pipe");
    wait_for_success(child);

    assert(report.before == 0x11);
    assert(report.after == 0x22);
    assert(mapping[0] == 0x11);
    assert(report.minor_fault_delta >= 0);
    assert(report.major_fault_delta >= 0);

    puts("[3] Copy-on-write after fork");
    printf("  child: 0x%02x -> 0x%02x, faults minor=%ld major=%ld\n",
           report.before,
           report.after,
           report.minor_fault_delta,
           report.major_fault_delta);
    printf("  parent still sees 0x%02x (private mapping)\n", mapping[0]);

    if (munmap(mapping, page_size) != 0)
        fail("munmap COW");
}

static void demonstrate_shared_file(size_t page_size)
{
    char path[] = "/tmp/virtual-memory-demo-XXXXXX";
    unsigned char *mapping;
    unsigned char persisted = 0;
    pid_t child;
    int fd = mkstemp(path);

    if (fd < 0)
        fail("mkstemp");
    if (unlink(path) != 0)
        fail("unlink temporary file");
    if (ftruncate(fd, (off_t)page_size) != 0)
        fail("ftruncate");
    mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        fail("mmap shared file");
    mapping[0] = 0x31;

    child = fork();
    if (child < 0)
        fail("fork shared");
    if (child == 0)
    {
        mapping[0] = 0x42;
        _exit(EXIT_SUCCESS);
    }
    wait_for_success(child);

    assert(mapping[0] == 0x42);
    if (pread(fd, &persisted, sizeof(persisted), 0) !=
        (ssize_t)sizeof(persisted))
        fail("pread");
    assert(persisted == 0x42);

    puts("[4] Shared file mapping");
    printf("  parent sees child byte=0x%02x; file byte=0x%02x\n",
           mapping[0],
           persisted);

    if (munmap(mapping, page_size) != 0)
        fail("munmap shared file");
    if (close(fd) != 0)
        fail("close temporary file");
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0)
        fail("sysconf _SC_PAGESIZE");
    puts("Virtual memory demo (Linux semantics)");
    demonstrate_address_split((size_t)page_size);
    demonstrate_demand_paging((size_t)page_size);
    demonstrate_copy_on_write((size_t)page_size);
    demonstrate_shared_file((size_t)page_size);
    puts("All virtual-memory checks passed.");
    return EXIT_SUCCESS;
}
