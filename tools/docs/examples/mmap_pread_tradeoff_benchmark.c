#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "virtual_memory_benchmark_support.h"

enum access_method
{
    METHOD_PREAD_REUSE,
    METHOD_MMAP_REMAP,
    METHOD_MMAP_PERSISTENT
};

enum cache_state
{
    CACHE_COLD,
    CACHE_WARM
};

enum
{
    IO_BLOCK_BYTES = 1024 * 1024,
    MAX_ROUNDS = 10000,
    MAX_PROBES = 100000000
};

struct scan_context
{
    enum access_method method;
    int fd;
    size_t length;
    unsigned char *buffer;
    unsigned char *mapping;
    double setup_us;
    double warmup_us;
};

static volatile uint64_t observation_sink;

static const char *method_name(enum access_method method)
{
    switch (method)
    {
    case METHOD_PREAD_REUSE:
        return "pread-reuse";
    case METHOD_MMAP_REMAP:
        return "mmap-remap";
    case METHOD_MMAP_PERSISTENT:
        return "mmap-persistent";
    }
    return "unknown";
}

static enum access_method parse_method(const char *text)
{
    if (strcmp(text, "pread-reuse") == 0)
        return METHOD_PREAD_REUSE;
    if (strcmp(text, "mmap-remap") == 0)
        return METHOD_MMAP_REMAP;
    if (strcmp(text, "mmap-persistent") == 0)
        return METHOD_MMAP_PERSISTENT;
    check(0, "invalid method: %s", text);
    return METHOD_PREAD_REUSE;
}

static enum cache_state parse_cache_state(const char *text)
{
    if (strcmp(text, "cold") == 0)
        return CACHE_COLD;
    if (strcmp(text, "warm") == 0)
        return CACHE_WARM;
    check(0, "invalid cache state: %s", text);
    return CACHE_COLD;
}

static size_t system_page_size(void)
{
    long page_size = sysconf(_SC_PAGESIZE);

    check(page_size > 0, "sysconf(_SC_PAGESIZE) failed");
    return (size_t)page_size;
}

static uint64_t generated_byte(size_t offset)
{
    size_t block = offset / IO_BLOCK_BYTES;
    size_t in_block = offset % IO_BLOCK_BYTES;

    return (in_block + block) % 251U;
}

static uint64_t expected_scan_checksum(size_t length)
{
    uint64_t sum = 0;
    size_t offset;

    for (offset = 0; offset < length; offset += 64U)
        sum += generated_byte(offset);
    return sum;
}

static uint64_t sample_bytes(const unsigned char *buffer, size_t length)
{
    uint64_t sum = 0;
    size_t offset;

    for (offset = 0; offset < length; offset += 64U)
        sum += buffer[offset];
    return sum;
}

static uint64_t read_process_io_bytes(void)
{
    char line[256];
    unsigned long long value = 0;
    int found = 0;
    FILE *io = fopen("/proc/self/io", "r");

    if (io == NULL)
        fail("fopen /proc/self/io");
    while (fgets(line, sizeof(line), io) != NULL)
    {
        if (sscanf(line, "read_bytes: %llu", &value) == 1)
        {
            found = 1;
            break;
        }
    }
    check(!ferror(io), "failed while reading /proc/self/io");
    if (fclose(io) != 0)
        fail("fclose /proc/self/io");
    check(found, "read_bytes is missing from /proc/self/io");
    return (uint64_t)value;
}

static void drop_file_cache(int fd, off_t length)
{
    int result = posix_fadvise(fd, 0, length, POSIX_FADV_DONTNEED);

    if (result != 0)
    {
        errno = result;
        fail("posix_fadvise DONTNEED");
    }
}

static void
read_fully(int fd, unsigned char *buffer, size_t length, off_t offset)
{
    size_t consumed = 0;

    while (consumed < length)
    {
        ssize_t result =
            pread(fd, buffer + consumed, length - consumed, offset + consumed);

        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            fail("pread");
        consumed += (size_t)result;
    }
}

static void initialize_scan_context(struct scan_context *context,
                                    enum access_method method,
                                    int fd,
                                    size_t length)
{
    double started = monotonic_seconds();

    memset(context, 0, sizeof(*context));
    context->method = method;
    context->fd = fd;
    context->length = length;

    if (method == METHOD_PREAD_REUSE)
    {
        check(posix_memalign(
                  (void **)&context->buffer, 4096U, IO_BLOCK_BYTES) == 0,
              "posix_memalign scan buffer failed");
        memset(context->buffer, 0, IO_BLOCK_BYTES);
    }
    else if (method == METHOD_MMAP_PERSISTENT)
    {
        context->mapping = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
        if (context->mapping == MAP_FAILED)
            fail("mmap persistent scan");
        if (madvise(context->mapping, length, MADV_SEQUENTIAL) != 0)
            fail("madvise persistent scan");
    }
    context->setup_us = (monotonic_seconds() - started) * 1.0e6;
}

static void destroy_scan_context(struct scan_context *context)
{
    if (context->mapping != NULL &&
        munmap(context->mapping, context->length) != 0)
        fail("munmap persistent scan");
    free(context->buffer);
}

static uint64_t scan_with_pread(struct scan_context *context,
                                uint64_t *logical_calls)
{
    uint64_t sum = 0;
    size_t offset;

    for (offset = 0; offset < context->length; offset += IO_BLOCK_BYTES)
    {
        size_t chunk = context->length - offset < IO_BLOCK_BYTES
                           ? context->length - offset
                           : IO_BLOCK_BYTES;

        read_fully(context->fd, context->buffer, chunk, (off_t)offset);
        *logical_calls += 1;
        sum += sample_bytes(context->buffer, chunk);
    }
    return sum;
}

static uint64_t scan_with_remap(struct scan_context *context,
                                uint64_t *logical_calls)
{
    unsigned char *mapping =
        mmap(NULL, context->length, PROT_READ, MAP_PRIVATE, context->fd, 0);
    uint64_t sum;

    if (mapping == MAP_FAILED)
        fail("mmap remap scan");
    if (madvise(mapping, context->length, MADV_SEQUENTIAL) != 0)
        fail("madvise remap scan");
    sum = sample_bytes(mapping, context->length);
    if (munmap(mapping, context->length) != 0)
        fail("munmap remap scan");
    *logical_calls += 2;
    return sum;
}

static uint64_t scan_once(struct scan_context *context, uint64_t *logical_calls)
{
    if (context->method == METHOD_PREAD_REUSE)
        return scan_with_pread(context, logical_calls);
    if (context->method == METHOD_MMAP_REMAP)
        return scan_with_remap(context, logical_calls);
    return sample_bytes(context->mapping, context->length);
}

static void warm_scan_context(struct scan_context *context, uint64_t expected)
{
    uint64_t logical_calls = 0;
    double started = monotonic_seconds();
    uint64_t sum = scan_once(context, &logical_calls);

    context->warmup_us = (monotonic_seconds() - started) * 1.0e6;
    check(sum == expected,
          "scan warm-up checksum mismatch: actual=%" PRIu64
          " expected=%" PRIu64,
          sum,
          expected);
    observation_sink ^= sum;
}

static void run_scan(enum access_method method,
                     enum cache_state cache,
                     const char *path,
                     size_t length,
                     size_t rounds)
{
    struct scan_context context;
    struct stat metadata;
    uint64_t expected;
    const char *require_io_value = getenv("VM_BENCH_REQUIRE_IO");
    int require_io =
        require_io_value != NULL && strcmp(require_io_value, "1") == 0;
    int fd = open(path, O_RDONLY);
    size_t round;

    if (fd < 0)
        fail("open scan file");
    if (fstat(fd, &metadata) != 0)
        fail("fstat scan file");
    check(length > 0 && length <= (size_t)metadata.st_size,
          "scan length %zu exceeds file size %jd",
          length,
          (intmax_t)metadata.st_size);
    check(!(cache == CACHE_COLD && method == METHOD_MMAP_PERSISTENT),
          "cold mmap-persistent is invalid: mapped pages defeat controlled "
          "per-round eviction");

    expected = expected_scan_checksum(length);
    if (cache == CACHE_WARM)
        drop_file_cache(fd, (off_t)length);
    initialize_scan_context(&context, method, fd, length);
    if (cache == CACHE_WARM)
        warm_scan_context(&context, expected);

    printf("SETUP mode=scan method=%s cache=%s bytes=%zu setup_us=%.3f "
           "warmup_us=%.3f\n",
           method_name(method),
           cache == CACHE_COLD ? "cold" : "warm",
           length,
           context.setup_us,
           context.warmup_us);

    for (round = 0; round < rounds; round++)
    {
        struct fault_counts before;
        struct fault_counts after;
        uint64_t io_before;
        uint64_t io_after;
        uint64_t logical_calls = 0;
        uint64_t sum;
        double started;
        double elapsed;

        if (cache == CACHE_COLD)
            drop_file_cache(fd, (off_t)length);
        before = read_fault_counts();
        io_before = read_process_io_bytes();
        started = monotonic_seconds();
        sum = scan_once(&context, &logical_calls);
        elapsed = monotonic_seconds() - started;
        io_after = read_process_io_bytes();
        after = read_fault_counts();

        check(sum == expected,
              "scan checksum mismatch: actual=%" PRIu64 " expected=%" PRIu64,
              sum,
              expected);
        check(io_after >= io_before, "scan read_bytes moved backward");
        if (cache == CACHE_COLD && require_io)
            check(io_after > io_before, "cold scan performed no device I/O");
        observation_sink ^= sum;

        printf("RESULT mode=tradeoff_scan round=%zu method=%s cache=%s "
               "require_io=%d bytes=%zu setup_us=%.3f warmup_us=%.3f "
               "elapsed_ms=%.3f gib_per_s=%.3f "
               "logical_calls=%" PRIu64 " minor=%ld major=%ld "
               "read_bytes=%" PRIu64 " checksum=%" PRIu64 "\n",
               round,
               method_name(method),
               cache == CACHE_COLD ? "cold" : "warm",
               require_io,
               length,
               context.setup_us,
               context.warmup_us,
               elapsed * 1.0e3,
               ((double)length / (1024.0 * 1024.0 * 1024.0)) / elapsed,
               logical_calls,
               after.minor - before.minor,
               after.major - before.major,
               io_after - io_before,
               sum);
    }

    destroy_scan_context(&context);
    if (close(fd) != 0)
        fail("close scan file");
}

static uint64_t next_random_u64(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static void build_random_pages(uint32_t *order, size_t page_count)
{
    uint64_t state = UINT64_C(0x243f6a8885a308d3);
    size_t index;

    check(page_count > 1 && page_count <= UINT32_MAX,
          "invalid page count: %zu",
          page_count);
    for (index = 0; index < page_count; index++)
        order[index] = (uint32_t)index;
    for (index = page_count - 1; index > 0; index--)
    {
        size_t other = (size_t)(next_random_u64(&state) % (index + 1));
        uint32_t temporary = order[index];

        order[index] = order[other];
        order[other] = temporary;
    }
}

static uint64_t
expected_lookup_checksum(const uint32_t *order, size_t probes, size_t page_size)
{
    uint64_t sum = 0;
    size_t index;

    for (index = 0; index < probes; index++)
        sum += generated_byte((size_t)order[index] * page_size);
    return sum;
}

static void warm_page_cache(int fd, size_t length)
{
    unsigned char *buffer;
    size_t offset;

    check(posix_memalign((void **)&buffer, 4096U, IO_BLOCK_BYTES) == 0,
          "posix_memalign warm buffer failed");
    for (offset = 0; offset < length; offset += IO_BLOCK_BYTES)
    {
        size_t chunk =
            length - offset < IO_BLOCK_BYTES ? length - offset : IO_BLOCK_BYTES;

        read_fully(fd, buffer, chunk, (off_t)offset);
    }
    free(buffer);
}

static uint64_t lookup_with_pread(int fd,
                                  const uint32_t *order,
                                  size_t probes,
                                  size_t page_size,
                                  uint64_t *logical_calls)
{
    uint64_t sum = 0;
    size_t index;

    for (index = 0; index < probes; index++)
    {
        unsigned char value;

        read_fully(fd,
                   &value,
                   sizeof(value),
                   (off_t)((size_t)order[index] * page_size));
        sum += value;
        *logical_calls += 1;
    }
    return sum;
}

static uint64_t lookup_mapped(const unsigned char *mapping,
                              const uint32_t *order,
                              size_t probes,
                              size_t page_size)
{
    uint64_t sum = 0;
    size_t index;

    for (index = 0; index < probes; index++)
        sum += mapping[(size_t)order[index] * page_size];
    return sum;
}

static void run_lookup(enum access_method method,
                       enum cache_state cache,
                       const char *path,
                       size_t length,
                       size_t probes,
                       size_t rounds)
{
    const size_t page_size = system_page_size();
    const size_t page_count = length / page_size;
    struct stat metadata;
    unsigned char *persistent_mapping = NULL;
    uint32_t *order;
    uint64_t expected;
    const char *require_io_value = getenv("VM_BENCH_REQUIRE_IO");
    int require_io =
        require_io_value != NULL && strcmp(require_io_value, "1") == 0;
    double setup_started;
    double setup_us;
    double warmup_us = 0.0;
    int fd = open(path, O_RDONLY);
    size_t round;

    if (fd < 0)
        fail("open lookup file");
    if (fstat(fd, &metadata) != 0)
        fail("fstat lookup file");
    check(length > 0 && length <= (size_t)metadata.st_size &&
              length % page_size == 0,
          "lookup length must be page-aligned and within the file");
    check(probes > 0 && probes <= page_count,
          "lookup probes %zu exceed page count %zu",
          probes,
          page_count);
    check(!(cache == CACHE_COLD && method == METHOD_MMAP_PERSISTENT),
          "cold mmap-persistent lookup is invalid");
    if (cache == CACHE_COLD)
    {
        int result = posix_fadvise(fd, 0, (off_t)length, POSIX_FADV_RANDOM);

        if (result != 0)
        {
            errno = result;
            fail("posix_fadvise RANDOM");
        }
    }

    order = malloc(page_count * sizeof(*order));
    if (order == NULL)
        fail("malloc lookup order");
    build_random_pages(order, page_count);
    expected = expected_lookup_checksum(order, probes, page_size);

    if (cache == CACHE_WARM)
        drop_file_cache(fd, (off_t)length);
    setup_started = monotonic_seconds();
    if (method == METHOD_MMAP_PERSISTENT)
    {
        persistent_mapping = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
        if (persistent_mapping == MAP_FAILED)
            fail("mmap persistent lookup");
        if (madvise(persistent_mapping, length, MADV_SEQUENTIAL) != 0)
            fail("madvise persistent lookup");
    }
    setup_us = (monotonic_seconds() - setup_started) * 1.0e6;

    if (cache == CACHE_WARM)
    {
        double warmup_started = monotonic_seconds();

        if (method == METHOD_MMAP_PERSISTENT)
        {
            uint64_t warm = 0;
            uint64_t warm_expected = 0;
            size_t page;

            for (page = 0; page < page_count; page++)
            {
                warm += persistent_mapping[page * page_size];
                warm_expected += generated_byte(page * page_size);
            }
            check(warm == warm_expected,
                  "persistent warm-up checksum mismatch");
            if (madvise(persistent_mapping, length, MADV_RANDOM) != 0)
                fail("madvise persistent random lookup");
            observation_sink ^= warm;
        }
        else
        {
            warm_page_cache(fd, length);
        }
        warmup_us = (monotonic_seconds() - warmup_started) * 1.0e6;
    }

    printf("SETUP mode=lookup method=%s cache=%s bytes=%zu probes=%zu "
           "setup_us=%.3f warmup_us=%.3f\n",
           method_name(method),
           cache == CACHE_COLD ? "cold" : "warm",
           length,
           probes,
           setup_us,
           warmup_us);

    for (round = 0; round < rounds; round++)
    {
        struct fault_counts before;
        struct fault_counts after;
        unsigned char *round_mapping = NULL;
        uint64_t io_before;
        uint64_t io_after;
        uint64_t logical_calls = 0;
        uint64_t sum;
        double started;
        double elapsed;

        if (cache == CACHE_COLD)
            drop_file_cache(fd, (off_t)length);
        before = read_fault_counts();
        io_before = read_process_io_bytes();
        started = monotonic_seconds();
        if (method == METHOD_MMAP_REMAP)
        {
            round_mapping = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
            if (round_mapping == MAP_FAILED)
                fail("mmap remap lookup");
            if (madvise(round_mapping, length, MADV_RANDOM) != 0)
                fail("madvise remap lookup");
            logical_calls += 2;
        }
        if (method == METHOD_PREAD_REUSE)
            sum =
                lookup_with_pread(fd, order, probes, page_size, &logical_calls);
        else
            sum = lookup_mapped(method == METHOD_MMAP_PERSISTENT
                                    ? persistent_mapping
                                    : round_mapping,
                                order,
                                probes,
                                page_size);
        if (round_mapping != NULL && munmap(round_mapping, length) != 0)
            fail("munmap remap lookup");
        elapsed = monotonic_seconds() - started;
        io_after = read_process_io_bytes();
        after = read_fault_counts();

        check(sum == expected,
              "lookup checksum mismatch: actual=%" PRIu64 " expected=%" PRIu64,
              sum,
              expected);
        check(io_after >= io_before, "lookup read_bytes moved backward");
        if (cache == CACHE_COLD && require_io)
            check(io_after > io_before, "cold lookup performed no device I/O");
        observation_sink ^= sum;

        printf("RESULT mode=tradeoff_lookup round=%zu method=%s cache=%s "
               "bytes=%zu probes=%zu setup_us=%.3f warmup_us=%.3f "
               "elapsed_ms=%.3f ns_per_probe=%.3f require_io=%d "
               "logical_calls=%" PRIu64
               " minor=%ld major=%ld read_bytes=%" PRIu64 " checksum=%" PRIu64
               "\n",
               round,
               method_name(method),
               cache == CACHE_COLD ? "cold" : "warm",
               length,
               probes,
               setup_us,
               warmup_us,
               elapsed * 1.0e3,
               elapsed * 1.0e9 / (double)probes,
               require_io,
               logical_calls,
               after.minor - before.minor,
               after.major - before.major,
               io_after - io_before,
               sum);
    }

    if (persistent_mapping != NULL && munmap(persistent_mapping, length) != 0)
        fail("munmap persistent lookup");
    free(order);
    if (close(fd) != 0)
        fail("close lookup file");
}

static void prepare_selftest_file(const char *path, size_t length)
{
    unsigned char *buffer;
    int fd;
    size_t offset;

    check(posix_memalign((void **)&buffer, 4096U, IO_BLOCK_BYTES) == 0,
          "posix_memalign prepare buffer failed");
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        fail("open selftest file");
    for (offset = 0; offset < length; offset += IO_BLOCK_BYTES)
    {
        size_t chunk =
            length - offset < IO_BLOCK_BYTES ? length - offset : IO_BLOCK_BYTES;
        size_t index;

        for (index = 0; index < chunk; index++)
            buffer[index] = (unsigned char)generated_byte(offset + index);
        {
            size_t written = 0;

            while (written < chunk)
            {
                ssize_t result = write(fd, buffer + written, chunk - written);

                if (result < 0 && errno == EINTR)
                    continue;
                if (result <= 0)
                    fail("write selftest file");
                written += (size_t)result;
            }
        }
    }
    if (close(fd) != 0)
        fail("close selftest file");
    free(buffer);
}

static void run_selftest(void)
{
    const char *path = ".tmp/mmap-pread-tradeoff-selftest.bin";
    const size_t length = 8U * 1024U * 1024U;
    const size_t probes = 1024;

    if (mkdir(".tmp", 0700) != 0 && errno != EEXIST)
        fail("mkdir .tmp");
    prepare_selftest_file(path, length);

    run_scan(METHOD_PREAD_REUSE, CACHE_COLD, path, length, 1);
    run_scan(METHOD_MMAP_REMAP, CACHE_COLD, path, length, 1);
    run_scan(METHOD_PREAD_REUSE, CACHE_WARM, path, length, 1);
    run_scan(METHOD_MMAP_REMAP, CACHE_WARM, path, length, 1);
    run_scan(METHOD_MMAP_PERSISTENT, CACHE_WARM, path, length, 1);

    run_lookup(METHOD_PREAD_REUSE, CACHE_COLD, path, length, probes, 1);
    run_lookup(METHOD_MMAP_REMAP, CACHE_COLD, path, length, probes, 1);
    run_lookup(METHOD_PREAD_REUSE, CACHE_WARM, path, length, probes, 1);
    run_lookup(METHOD_MMAP_REMAP, CACHE_WARM, path, length, probes, 1);
    run_lookup(METHOD_MMAP_PERSISTENT, CACHE_WARM, path, length, probes, 1);

    if (unlink(path) != 0)
        fail("unlink selftest file");
    puts("All mmap/pread tradeoff selftests passed.");
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s selftest\n"
            "  %s scan <pread-reuse|mmap-remap|mmap-persistent> "
            "<cold|warm> <path> <bytes> <rounds>\n"
            "  %s lookup <pread-reuse|mmap-remap|mmap-persistent> "
            "<cold|warm> <path> <bytes> <probes> <rounds>\n",
            program,
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
    if (argc == 7 && strcmp(argv[1], "scan") == 0)
    {
        run_scan(parse_method(argv[2]),
                 parse_cache_state(argv[3]),
                 argv[4],
                 parse_bounded_size(argv[5], "bytes", SIZE_MAX),
                 parse_bounded_size(argv[6], "rounds", MAX_ROUNDS));
        finish_output();
        return EXIT_SUCCESS;
    }
    if (argc == 8 && strcmp(argv[1], "lookup") == 0)
    {
        run_lookup(parse_method(argv[2]),
                   parse_cache_state(argv[3]),
                   argv[4],
                   parse_bounded_size(argv[5], "bytes", SIZE_MAX),
                   parse_bounded_size(argv[6], "probes", MAX_PROBES),
                   parse_bounded_size(argv[7], "rounds", MAX_ROUNDS));
        finish_output();
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
