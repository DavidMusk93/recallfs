#ifndef VIRTUAL_MEMORY_BENCHMARK_SUPPORT_H
#define VIRTUAL_MEMORY_BENCHMARK_SUPPORT_H

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

struct fault_counts
{
    long minor;
    long major;
};

static inline void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static inline void check(int condition, const char *format, ...)
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

static inline double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime");
    return (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
}

static inline int
try_parse_bounded_size(const char *text, size_t maximum, size_t *parsed)
{
    const unsigned char *cursor = (const unsigned char *)text;
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || parsed == NULL || maximum == 0 || *cursor == '\0')
        return 0;
    for (; *cursor != '\0'; cursor++)
    {
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
            return 0;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > maximum)
        return 0;

    *parsed = (size_t)value;
    return 1;
}

static inline size_t
parse_bounded_size(const char *text, const char *name, size_t maximum)
{
    size_t parsed = 0;

    check(try_parse_bounded_size(text, maximum, &parsed),
          "invalid %s: %s (expected 1..%zu)",
          name,
          text == NULL ? "(null)" : text,
          maximum);
    return parsed;
}

static inline size_t mebibytes_to_bytes(size_t mebibytes)
{
    const size_t scale = 1024U * 1024U;

    check(mebibytes <= SIZE_MAX / scale,
          "MiB value overflows size_t: %zu",
          mebibytes);
    return mebibytes * scale;
}

static inline struct fault_counts read_fault_counts(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0)
        fail("getrusage");
    return (struct fault_counts){
        .minor = usage.ru_minflt,
        .major = usage.ru_majflt,
    };
}

static inline void finish_output(void)
{
    check(fflush(stdout) == 0 && !ferror(stdout),
          "failed to flush benchmark output");
}

#endif
