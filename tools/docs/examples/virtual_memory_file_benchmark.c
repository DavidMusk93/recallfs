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

static volatile uint64_t observation_sink;

enum
{
    MAX_FILE_MEBIBYTES = 1048576,
    MAX_FILE_ROUNDS = 10000
};

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

static uint64_t expected_checksum(size_t length)
{
    const size_t block_size = 1024U * 1024U;
    uint64_t sum = 0;
    size_t offset;

    for (offset = 0; offset < length; offset += block_size)
    {
        size_t chunk =
            length - offset < block_size ? length - offset : block_size;
        uint64_t block = offset / block_size;
        size_t sample;

        for (sample = 0; sample < chunk; sample += 64U)
            sum += (sample + block) % 251U;
    }
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

static void run_prepare(const char *path, size_t mebibytes)
{
    const size_t length = mebibytes_to_bytes(mebibytes);
    const size_t block_size = 1024U * 1024U;
    unsigned char *buffer;
    size_t offset;
    char *temporary_path;
    size_t temporary_length;
    int fd;
    double started;
    double elapsed;

    check(posix_memalign((void **)&buffer, 4096U, block_size) == 0,
          "posix_memalign file buffer failed");
    temporary_length = strlen(path) + sizeof(".tmp.XXXXXX");
    temporary_path = malloc(temporary_length);
    if (temporary_path == NULL)
        fail("malloc temporary file path");
    check(snprintf(temporary_path, temporary_length, "%s.tmp.XXXXXX", path) > 0,
          "failed to format temporary file path");
    fd = mkstemp(temporary_path);
    if (fd < 0)
        fail("mkstemp benchmark file");
    if (fchmod(fd, 0644) != 0)
        fail("fchmod benchmark file");

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

    if (close(fd) != 0)
        fail("close benchmark file");
    if (rename(temporary_path, path) != 0)
        fail("rename benchmark file");
    printf("RESULT mode=file_prepare path=%s mib=%zu elapsed_ms=%.3f "
           "mib_per_s=%.3f checksum=%" PRIu64 "\n",
           path,
           mebibytes,
           elapsed * 1.0e3,
           (double)mebibytes / elapsed,
           expected_checksum(length));

    free(temporary_path);
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
    uint64_t expected;
    const char *require_io_value = getenv("VM_BENCH_REQUIRE_IO");
    int require_io =
        require_io_value != NULL && strcmp(require_io_value, "1") == 0;
    size_t round;

    if (fd < 0)
        fail("open file-read benchmark");
    if (fstat(fd, &metadata) != 0)
        fail("fstat file-read benchmark");
    check(metadata.st_size > 0, "benchmark file is empty: %s", path);
    check((uintmax_t)metadata.st_size <= SIZE_MAX,
          "benchmark file is too large for size_t: %jd",
          (intmax_t)metadata.st_size);
    expected = expected_checksum((size_t)metadata.st_size);

    if (cache == CACHE_WARM)
    {
        uint64_t warm_sum =
            read_file_once(method, fd, (size_t)metadata.st_size);
        check(warm_sum == expected,
              "warm-up checksum mismatch: actual=%" PRIu64 " expected=%" PRIu64,
              warm_sum,
              expected);
        observation_sink ^= warm_sum;
    }

    for (round = 0; round < rounds; round++)
    {
        struct fault_counts before;
        struct fault_counts after;
        uint64_t sum;
        uint64_t read_bytes_before;
        uint64_t read_bytes_after;
        uint64_t read_bytes;
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
        read_bytes_before = read_process_io_bytes();
        started = monotonic_seconds();
        sum = read_file_once(method, fd, (size_t)metadata.st_size);
        elapsed = monotonic_seconds() - started;
        read_bytes_after = read_process_io_bytes();
        after = read_fault_counts();
        check(read_bytes_after >= read_bytes_before,
              "process read_bytes moved backward");
        read_bytes = read_bytes_after - read_bytes_before;
        check(sum == expected,
              "file checksum mismatch: actual=%" PRIu64 " expected=%" PRIu64,
              sum,
              expected);
        if (cache == CACHE_COLD && require_io)
            check(read_bytes > 0, "cold read produced no backing-device bytes");
        observation_sink ^= sum;

        printf("RESULT mode=file_read round=%zu method=%s cache=%s "
               "require_io=%d bytes=%jd elapsed_ms=%.3f "
               "gib_per_s=%.3f minor=%ld "
               "major=%ld read_bytes=%" PRIu64 " checksum=%" PRIu64 "\n",
               round,
               method == FILE_PREAD ? "pread" : "mmap",
               cache == CACHE_COLD ? "cold" : "warm",
               require_io,
               (intmax_t)metadata.st_size,
               elapsed * 1.0e3,
               ((double)metadata.st_size / (1024.0 * 1024.0 * 1024.0)) /
                   elapsed,
               after.minor - before.minor,
               after.major - before.major,
               read_bytes,
               sum);
    }

    if (close(fd) != 0)
        fail("close file-read benchmark");
}

static void run_selftest(void)
{
    const char *path = ".tmp/virtual-memory-file-selftest.bin";
    size_t parsed;

    if (mkdir(".tmp", 0700) != 0 && errno != EEXIST)
        fail("mkdir .tmp");
    check(!try_parse_bounded_size("-1", 100, &parsed),
          "parser accepted a negative value");
    check(!try_parse_bounded_size(" 1", 100, &parsed),
          "parser accepted leading whitespace");
    check(!try_parse_bounded_size("0", 100, &parsed), "parser accepted zero");
    run_prepare(path, 8);
    run_read(FILE_PREAD, CACHE_COLD, path, 1);
    run_read(FILE_MMAP, CACHE_COLD, path, 1);
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
        finish_output();
        return EXIT_SUCCESS;
    }
    if (argc == 4 && strcmp(argv[1], "prepare") == 0)
    {
        run_prepare(argv[2],
                    parse_bounded_size(argv[3], "MiB", MAX_FILE_MEBIBYTES));
        finish_output();
        return EXIT_SUCCESS;
    }
    if (argc == 6 && strcmp(argv[1], "read") == 0)
    {
        run_read(parse_file_method(argv[2]),
                 parse_cache_state(argv[3]),
                 argv[4],
                 parse_bounded_size(argv[5], "rounds", MAX_FILE_ROUNDS));
        finish_output();
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
