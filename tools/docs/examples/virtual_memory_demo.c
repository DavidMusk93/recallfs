#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

static void child_fail(const char *operation)
{
    perror(operation);
    _exit(EXIT_FAILURE);
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
    int found_vm_size = 0;
    int found_vm_rss = 0;
    int found_vm_pte = 0;
    char line[256];
    FILE *status = fopen("/proc/self/status", "r");

    if (status == NULL)
        fail("fopen /proc/self/status");
    while (fgets(line, sizeof(line), status) != NULL)
    {
        if (!found_vm_size &&
            sscanf(line, "VmSize: %ld kB", &memory.vm_size_kib) == 1)
        {
            found_vm_size = 1;
            continue;
        }
        if (!found_vm_rss &&
            sscanf(line, "VmRSS: %ld kB", &memory.vm_rss_kib) == 1)
        {
            found_vm_rss = 1;
            continue;
        }
        if (!found_vm_pte &&
            sscanf(line, "VmPTE: %ld kB", &memory.vm_pte_kib) == 1)
        {
            found_vm_pte = 1;
            continue;
        }
    }
    check(!ferror(status), "error while reading /proc/self/status");
    if (fclose(status) != 0)
        fail("fclose /proc/self/status");
    check(found_vm_size && found_vm_rss && found_vm_pte,
          "missing /proc/self/status fields: VmSize=%s VmRSS=%s VmPTE=%s",
          found_vm_size ? "present" : "missing",
          found_vm_rss ? "present" : "missing",
          found_vm_pte ? "present" : "missing");
    check(memory.vm_size_kib >= 0 && memory.vm_rss_kib >= 0 &&
              memory.vm_pte_kib >= 0,
          "negative /proc/self/status value: VmSize=%ld VmRSS=%ld VmPTE=%ld",
          memory.vm_size_kib,
          memory.vm_rss_kib,
          memory.vm_pte_kib);
    return memory;
}

static void child_write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    size_t written = 0;

    while (written < length)
    {
        ssize_t result = write(fd, bytes + written, length - written);

        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            child_fail("write");
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

    check(page * page_size + offset == address,
          "address split did not reconstruct 0x%" PRIx64,
          address);
    check(offset < page_size,
          "page offset %" PRIu64 " is outside page size %zu",
          offset,
          page_size);
    puts("[1] Virtual-address split");
    printf("  address=0x%" PRIx64 " -> virtual-page=%" PRIu64
           ", offset=%" PRIu64 " (page size=%zu)\n",
           address,
           page,
           offset,
           page_size);
}

static void demonstrate_memory_protection(size_t page_size)
{
    volatile unsigned char *mapping;
    pid_t child;
    int status;

    mapping = mmap(NULL,
                   page_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED)
        fail("mmap protection");
    mapping[0] = 0x5a;
    if (mprotect((void *)mapping, page_size, PROT_READ) != 0)
        fail("mprotect read-only");

    child = fork();
    if (child < 0)
        fail("fork protection");
    if (child == 0)
    {
        mapping[0] = 0xa5;
        _exit(42);
    }
    while (waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
            fail("waitpid protection");
    }
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "read-only write ended with unexpected status=%d signal=%d",
          status,
          WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    check(mapping[0] == 0x5a,
          "failed child write changed parent byte to 0x%02x",
          (unsigned int)mapping[0]);

    if (mprotect((void *)mapping, page_size, PROT_READ | PROT_WRITE) != 0)
        fail("mprotect restore read-write");
    mapping[0] = 0xa5;
    check(mapping[0] == 0xa5, "restored write permission did not take effect");

    puts("[2] Page permission enforcement");
    printf("  read-only child write terminated by signal=%d\n",
           WTERMSIG(status));

    if (munmap((void *)mapping, page_size) != 0)
        fail("munmap protection");
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
    long first_touch_minor_delta;
    long first_touch_major_delta;
    long second_touch_minor_delta;
    long second_touch_major_delta;
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
    for (index = 0; index < page_count; index++)
    {
        unsigned char expected = (unsigned char)(index & 0xffU);
        unsigned char actual = mapping[index * page_size];

        check(
            actual == expected,
            "page %zu changed after first touch: expected=0x%02x actual=0x%02x",
            index,
            (unsigned int)expected,
            (unsigned int)actual);
    }
    before_second_touch = read_fault_counts();
    for (index = 0; index < page_count; index++)
        mapping[index * page_size] ^= 1U;
    after_second_touch = read_fault_counts();

    first_touch_minor_delta =
        after_first_touch.minor - before_first_touch.minor;
    first_touch_major_delta =
        after_first_touch.major - before_first_touch.major;
    second_touch_minor_delta =
        after_second_touch.minor - before_second_touch.minor;
    second_touch_major_delta =
        after_second_touch.major - before_second_touch.major;
    check(first_touch_minor_delta > 0,
          "first touch produced no minor faults: first=%ld repeated=%ld",
          first_touch_minor_delta,
          second_touch_minor_delta);
    check(first_touch_major_delta >= 0 && second_touch_minor_delta >= 0 &&
              second_touch_major_delta >= 0,
          "fault counter moved backward: first major=%ld repeated minor=%ld "
          "repeated major=%ld",
          first_touch_major_delta,
          second_touch_minor_delta,
          second_touch_major_delta);
    check(first_touch_minor_delta > second_touch_minor_delta,
          "first touch was not stronger than repeated touch: first minor=%ld "
          "repeated minor=%ld",
          first_touch_minor_delta,
          second_touch_minor_delta);

    puts("[3] Reservation versus residency");
    printf("  anonymous mapping: address=%p, %zu pages (%zu KiB)\n",
           (void *)mapping,
           page_count,
           length / 1024);
    printf("  first pass faults:  minor=%ld major=%ld\n",
           first_touch_minor_delta,
           first_touch_major_delta);
    printf("  second pass faults: minor=%ld major=%ld\n",
           second_touch_minor_delta,
           second_touch_major_delta);
    print_memory("before mmap", before_map);
    print_memory("after mmap", after_map);
    print_memory("after first touch", after_touch);

    if (munmap((void *)mapping, length) != 0)
        fail("munmap anonymous");
}

static void demonstrate_copy_on_write(size_t page_size)
{
    const size_t page_count = 256;
    const size_t length = page_count * page_size;
    unsigned char *mapping;
    struct child_report report;
    int pipe_fds[2];
    pid_t child;
    size_t index;

    mapping = mmap(NULL,
                   length,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED)
        fail("mmap COW");
#ifdef MADV_NOHUGEPAGE
    if (madvise(mapping, length, MADV_NOHUGEPAGE) != 0)
        fail("madvise COW MADV_NOHUGEPAGE");
#endif
    for (index = 0; index < page_count; index++)
        mapping[index * page_size] = 0x11;
    if (pipe(pipe_fds) != 0)
        fail("pipe");

    child = fork();
    if (child < 0)
        fail("fork COW");
    if (child == 0)
    {
        struct fault_counts before;
        struct fault_counts after;
        size_t child_index;

        if (close(pipe_fds[0]) != 0)
            child_fail("close child read pipe");
        report.before = mapping[0];
        before = read_fault_counts();
        for (child_index = 0; child_index < page_count; child_index++)
            mapping[child_index * page_size] = 0x22;
        after = read_fault_counts();
        report.after = mapping[0];
        report.minor_fault_delta = after.minor - before.minor;
        report.major_fault_delta = after.major - before.major;
        child_write_all(pipe_fds[1], &report, sizeof(report));
        if (close(pipe_fds[1]) != 0)
            child_fail("close child write pipe");
        _exit(EXIT_SUCCESS);
    }

    if (close(pipe_fds[1]) != 0)
        fail("close parent write pipe");
    read_all(pipe_fds[0], &report, sizeof(report));
    if (close(pipe_fds[0]) != 0)
        fail("close parent read pipe");
    wait_for_success(child);

    check(report.before == 0x11,
          "child saw initial byte 0x%02x instead of 0x11",
          (unsigned int)report.before);
    check(report.after == 0x22,
          "child write produced byte 0x%02x instead of 0x22",
          (unsigned int)report.after);
    for (index = 0; index < page_count; index++)
        check(mapping[index * page_size] == 0x11,
              "child write changed parent page %zu to 0x%02x",
              index,
              (unsigned int)mapping[index * page_size]);
    check(report.minor_fault_delta >= (long)page_count,
          "child COW writes produced too few minor faults: "
          "pages=%zu minor=%ld major=%ld",
          page_count,
          report.minor_fault_delta,
          report.major_fault_delta);
    check(report.major_fault_delta >= 0,
          "child major-fault counter moved backward: delta=%ld",
          report.major_fault_delta);

    puts("[4] Copy-on-write after fork");
    printf("  child: 0x%02x -> 0x%02x across %zu pages, "
           "faults minor=%ld major=%ld\n",
           report.before,
           report.after,
           page_count,
           report.minor_fault_delta,
           report.major_fault_delta);
    printf("  parent still sees 0x%02x on every private page\n", mapping[0]);

    if (munmap(mapping, length) != 0)
        fail("munmap COW");
}

static void demonstrate_shared_file(size_t page_size)
{
    char path[] = ".tmp/virtual-memory-demo-XXXXXX";
    unsigned char *mapping;
    unsigned char persisted = 0;
    pid_t child;
    int fd;

    if (mkdir(".tmp", 0700) != 0 && errno != EEXIST)
        fail("mkdir .tmp");
    fd = mkstemp(path);
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

    check(mapping[0] == 0x42,
          "parent saw shared byte 0x%02x instead of 0x42",
          (unsigned int)mapping[0]);
    if (pread(fd, &persisted, sizeof(persisted), 0) !=
        (ssize_t)sizeof(persisted))
        fail("pread");
    check(persisted == 0x42,
          "file byte is 0x%02x instead of 0x42",
          (unsigned int)persisted);

    puts("[5] Shared file mapping");
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
    demonstrate_memory_protection((size_t)page_size);
    demonstrate_demand_paging((size_t)page_size);
    demonstrate_copy_on_write((size_t)page_size);
    demonstrate_shared_file((size_t)page_size);
    puts("All virtual-memory checks passed.");
    return EXIT_SUCCESS;
}
