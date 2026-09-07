#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum file_method
{
    FILE_PREAD,
    FILE_MMAP
};

enum cache_state
{
    CACHE_COLD,
    CACHE_WARM
};

struct fault_counts
{
    long minor;
    long major;
};

static volatile uint64_t observation_sink;

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

static size_t mebibytes_to_bytes(size_t mebibytes)
{
    const size_t scale = 1024U * 1024U;

    check(mebibytes <= SIZE_MAX / scale,
          "MiB value overflows size_t: %zu",
          mebibytes);
    return mebibytes * scale;
}

static struct fault_counts read_fault_counts(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0)
        fail("getrusage");
    return (struct fault_counts){
        .minor = usage.ru_minflt,
        .major = usage.ru_majflt,
    };
}

static enum file_method parse_file_method(const char *text)
{
    if (strcmp(text, "pread") == 0)
        return FILE_PREAD;
    if (strcmp(text, "mmap") == 0)
        return FILE_MMAP;
    check(0, "invalid file method: %s", text);
    return FILE_PREAD;
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

static void fill_buffer(unsigned char *buffer, size_t length, uint64_t block)
{
    size_t index;

    for (index = 0; index < length; index++)
        buffer[index] = (unsigned char)((index + block) % 251U);
}

static void run_prepare(const char *path, size_t mebibytes)
{
    const size_t length = mebibytes_to_bytes(mebibytes);
    const size_t block_size = 1024U * 1024U;
    unsigned char *buffer;
    size_t offset;
    int fd;
    double started;
    double elapsed;

    check(posix_memalign((void **)&buffer, 4096U, block_size) == 0,
          "posix_memalign file buffer failed");
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        fail("open benchmark file");

    started = monotonic_seconds();
    for (offset = 0; offset < length; offset += block_size)
    {
        size_t chunk =
            length - offset < block_size ? length - offset : block_size;
        size_t written = 0;

        fill_buffer(buffer, chunk, offset / block_size);
        while (written < chunk)
        {
            ssize_t result =
                pwrite(fd, buffer + written, chunk - written, offset + written);

            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                fail("pwrite benchmark file");
            written += (size_t)result;
        }
    }
    if (fdatasync(fd) != 0)
        fail("fdatasync benchmark file");
    elapsed = monotonic_seconds() - started;

    printf("RESULT mode=file_prepare path=%s mib=%zu elapsed_ms=%.3f "
           "mib_per_s=%.3f\n",
           path,
           mebibytes,
           elapsed * 1.0e3,
           (double)mebibytes / elapsed);

    if (close(fd) != 0)
        fail("close benchmark file");
    free(buffer);
}

static uint64_t sample_bytes(const unsigned char *buffer, size_t length)
{
    uint64_t sum = 0;
    size_t offset;

    for (offset = 0; offset < length; offset += 64U)
        sum += buffer[offset];
    return sum;
}

static uint64_t read_with_pread(int fd, size_t length)
{
    const size_t block_size = 1024U * 1024U;
    unsigned char *buffer;
    uint64_t sum = 0;
    size_t offset;

    check(posix_memalign((void **)&buffer, 4096U, block_size) == 0,
          "posix_memalign pread buffer failed");
    memset(buffer, 0, block_size);

    for (offset = 0; offset < length; offset += block_size)
    {
        size_t chunk =
            length - offset < block_size ? length - offset : block_size;
        size_t consumed = 0;

        while (consumed < chunk)
        {
            ssize_t result = pread(
                fd, buffer + consumed, chunk - consumed, offset + consumed);

            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                fail("pread benchmark file");
            consumed += (size_t)result;
        }
        sum += sample_bytes(buffer, chunk);
    }

    free(buffer);
    return sum;
}

static uint64_t read_with_mmap(int fd, size_t length)
{
    unsigned char *mapping = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    uint64_t sum;

    if (mapping == MAP_FAILED)
        fail("mmap benchmark file");
    if (madvise(mapping, length, MADV_SEQUENTIAL) != 0)
        fail("madvise file MADV_SEQUENTIAL");
    sum = sample_bytes(mapping, length);
    if (munmap(mapping, length) != 0)
        fail("munmap benchmark file");
    return sum;
}

static uint64_t read_file_once(enum file_method method, int fd, size_t length)
{
    return method == FILE_PREAD ? read_with_pread(fd, length)
                                : read_with_mmap(fd, length);
}

static void run_read(enum file_method method,
                     enum cache_state cache,
                     const char *path,
                     size_t rounds)
{
    struct stat metadata;
    int fd = open(path, O_RDONLY);
    size_t round;

    if (fd < 0)
        fail("open file-read benchmark");
    if (fstat(fd, &metadata) != 0)
        fail("fstat file-read benchmark");
    check(metadata.st_size > 0, "benchmark file is empty: %s", path);
    check((uintmax_t)metadata.st_size <= SIZE_MAX,
          "benchmark file is too large for size_t: %jd",
          (intmax_t)metadata.st_size);

    if (cache == CACHE_WARM)
    {
        uint64_t warm_sum =
            read_file_once(method, fd, (size_t)metadata.st_size);
        observation_sink ^= warm_sum;
    }

    for (round = 0; round < rounds; round++)
    {
        struct fault_counts before;
        struct fault_counts after;
        uint64_t sum;
        double started;
        double elapsed;

        if (cache == CACHE_COLD)
        {
            int result =
                posix_fadvise(fd, 0, metadata.st_size, POSIX_FADV_DONTNEED);

            if (result != 0)
            {
                errno = result;
                fail("posix_fadvise DONTNEED");
            }
        }

        before = read_fault_counts();
        started = monotonic_seconds();
        sum = read_file_once(method, fd, (size_t)metadata.st_size);
        elapsed = monotonic_seconds() - started;
        after = read_fault_counts();
        observation_sink ^= sum;

        printf("RESULT mode=file_read round=%zu method=%s cache=%s "
               "bytes=%jd elapsed_ms=%.3f gib_per_s=%.3f minor=%ld "
               "major=%ld checksum=%" PRIu64 "\n",
               round,
               method == FILE_PREAD ? "pread" : "mmap",
               cache == CACHE_COLD ? "cold" : "warm",
               (intmax_t)metadata.st_size,
               elapsed * 1.0e3,
               ((double)metadata.st_size / (1024.0 * 1024.0 * 1024.0)) /
                   elapsed,
               after.minor - before.minor,
               after.major - before.major,
               sum);
    }

    if (close(fd) != 0)
        fail("close file-read benchmark");
}

static void run_selftest(void)
{
    const char *path = ".tmp/virtual-memory-file-selftest.bin";

    if (mkdir(".tmp", 0700) != 0 && errno != EEXIST)
        fail("mkdir .tmp");
    run_prepare(path, 8);
    run_read(FILE_PREAD, CACHE_WARM, path, 1);
    run_read(FILE_MMAP, CACHE_WARM, path, 1);
    if (unlink(path) != 0)
        fail("unlink selftest file");
    puts("All virtual-memory file benchmark selftests passed.");
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s selftest\n"
            "  %s prepare <path> <MiB>\n"
            "  %s read <pread|mmap> <cold|warm> <path> <rounds>\n",
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
    if (argc == 4 && strcmp(argv[1], "prepare") == 0)
    {
        run_prepare(argv[2], parse_positive_size(argv[3], "MiB"));
        return EXIT_SUCCESS;
    }
    if (argc == 6 && strcmp(argv[1], "read") == 0)
    {
        run_read(parse_file_method(argv[2]),
                 parse_cache_state(argv[3]),
                 argv[4],
                 parse_positive_size(argv[5], "rounds"));
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
