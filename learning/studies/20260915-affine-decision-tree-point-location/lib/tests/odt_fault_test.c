#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "odt_persistence_test_support.h"
#include "odt_test_support.h"

#include "odt_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

typedef enum file_fault {
    FILE_FAULT_NONE = 0,
    FILE_FAULT_OPEN_PARENT,
    FILE_FAULT_OPEN_READ,
    FILE_FAULT_CREATE,
    FILE_FAULT_SIZE,
    FILE_FAULT_WRITE,
    FILE_FAULT_SYNC_FILE,
    FILE_FAULT_RENAME,
    FILE_FAULT_SYNC_DIRECTORY,
    FILE_FAULT_READ,
    FILE_FAULT_CLOSE
} file_fault;

typedef struct file_fault_state {
    file_fault fault;
    bool inject_read_eintr;
    bool inject_write_eintr;
    bool inject_size_eintr;
    bool inject_sync_file_eintr;
    bool inject_rename_eintr;
    bool inject_sync_directory_eintr;
    bool force_short_read;
    bool force_short_write;
    size_t size_calls;
    size_t read_calls;
    size_t write_calls;
    size_t sync_file_calls;
    size_t rename_calls;
    size_t sync_directory_calls;
    size_t close_calls;
} file_fault_state;

static const odt_domain old_domain = {0.0, 0.0, 1.0, 1.0};
static const odt_site old_sites[] = {{0.5, 0.5, 7}};
static const odt_domain new_domain = {-2.0, -2.0, 2.0, 2.0};
static const odt_site new_sites[] = {
    {-1.0, 0.0, 11},
    {1.0, 0.0, 22},
    {0.0, 1.5, 33},
};

static int fault_open_parent(void *context, const char *path) {
    file_fault_state *state = context;

    if (state->fault == FILE_FAULT_OPEN_PARENT) {
        errno = EIO;
        return -1;
    }
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int fault_open_read(void *context, const char *path) {
    file_fault_state *state = context;

    if (state->fault == FILE_FAULT_OPEN_READ) {
        errno = EIO;
        return -1;
    }
    return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

static int fault_create_exclusive_at(void *context, int parent_fd, const char *name) {
    file_fault_state *state = context;

    if (state->fault == FILE_FAULT_CREATE) {
        errno = EIO;
        return -1;
    }
    return openat(parent_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
}

static int fault_get_size(void *context, int fd, uint64_t *out_size) {
    file_fault_state *state = context;
    struct stat status;

    state->size_calls += 1u;
    if (state->inject_size_eintr && state->size_calls == 1u) {
        errno = EINTR;
        return -1;
    }
    if (state->fault == FILE_FAULT_SIZE) {
        errno = EIO;
        return -1;
    }
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return -1;
    }
    *out_size = (uint64_t)status.st_size;
    return (off_t)*out_size == status.st_size ? 0 : -1;
}

static ssize_t fault_read_at(void *context, int fd, void *data, size_t size, uint64_t offset) {
    file_fault_state *state = context;
    size_t request = size;

    state->read_calls += 1u;
    if (state->inject_read_eintr && state->read_calls == 1u) {
        errno = EINTR;
        return -1;
    }
    if (state->fault == FILE_FAULT_READ) {
        errno = EIO;
        return -1;
    }
    if (state->force_short_read && request > 1u) {
        request /= 2u;
    }
    if (offset > (uint64_t)INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return pread(fd, data, request, (off_t)offset);
}

static ssize_t fault_write(void *context, int fd, const void *data, size_t size) {
    file_fault_state *state = context;
    size_t request = size;

    state->write_calls += 1u;
    if (state->inject_write_eintr && state->write_calls == 1u) {
        errno = EINTR;
        return -1;
    }
    if (state->fault == FILE_FAULT_WRITE) {
        errno = EIO;
        return -1;
    }
    if (state->force_short_write && request > 1u) {
        request /= 2u;
    }
    return write(fd, data, request);
}

static int fault_sync_file(void *context, int fd) {
    file_fault_state *state = context;

    state->sync_file_calls += 1u;
    if (state->inject_sync_file_eintr && state->sync_file_calls == 1u) {
        errno = EINTR;
        return -1;
    }
    if (state->fault == FILE_FAULT_SYNC_FILE) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int fault_rename_at(void *context, int parent_fd, const char *from, const char *to) {
    file_fault_state *state = context;

    state->rename_calls += 1u;
    if (state->inject_rename_eintr && state->rename_calls == 1u) {
        errno = EINTR;
        return -1;
    }
    if (state->fault == FILE_FAULT_RENAME) {
        errno = EIO;
        return -1;
    }
    return renameat(parent_fd, from, parent_fd, to);
}

static int fault_sync_directory(void *context, int fd) {
    file_fault_state *state = context;

    state->sync_directory_calls += 1u;
    if (state->inject_sync_directory_eintr && state->sync_directory_calls == 1u) {
        errno = EINTR;
        return -1;
    }
    if (state->fault == FILE_FAULT_SYNC_DIRECTORY) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int fault_unlink_at(void *context, int parent_fd, const char *name) {
    (void)context;
    return unlinkat(parent_fd, name, 0);
}

static int fault_close(void *context, int fd) {
    file_fault_state *state = context;
    int result = close(fd);

    state->close_calls += 1u;
    if (state->fault == FILE_FAULT_CLOSE && state->close_calls == 1u) {
        errno = EIO;
        return -1;
    }
    return result;
}

static odt_internal_file_ops fault_ops(file_fault_state *state) {
    odt_internal_file_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = state;
    ops.open_parent = fault_open_parent;
    ops.open_read = fault_open_read;
    ops.create_exclusive_at = fault_create_exclusive_at;
    ops.get_size = fault_get_size;
    ops.read_at = fault_read_at;
    ops.write = fault_write;
    ops.sync_file = fault_sync_file;
    ops.rename_at = fault_rename_at;
    ops.sync_directory = fault_sync_directory;
    ops.unlink_at = fault_unlink_at;
    ops.close = fault_close;
    return ops;
}

static odt_generation *build_generation(const odt_domain *domain, const odt_site *sites,
                                        size_t site_count) {
    odt_generation *generation = NULL;

    ODT_TEST_STATUS(odt_build(domain, sites, site_count, NULL, NULL, NULL, NULL, &generation),
                    ODT_OK);
    return generation;
}

static void expect_file_region(const char *path, odt_point point, int32_t region_id) {
    odt_generation *generation = NULL;
    odt_query_result result;

    ODT_TEST_STATUS(odt_load_file(path, NULL, NULL, &generation), ODT_OK);
    ODT_TEST_STATUS(odt_query(generation, &point, &result, NULL), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.region_id == region_id);
    odt_generation_destroy(generation);
}

static void expect_no_temporary_file(const char *directory) {
    DIR *stream = opendir(directory);
    struct dirent *entry;

    ODT_TEST_CHECK(stream != NULL);
    while ((entry = readdir(stream)) != NULL) {
        ODT_TEST_CHECK(strstr(entry->d_name, ".odt.tmp.") == NULL);
    }
    ODT_TEST_CHECK(closedir(stream) == 0);
}

static void test_encode_sink_fail_after_each_call(void) {
    odt_generation *generation =
        build_generation(&new_domain, new_sites, sizeof(new_sites) / sizeof(new_sites[0]));
    odt_test_byte_buffer encoded = {0};
    size_t successful_calls;
    size_t fail_at;

    ODT_TEST_STATUS(odt_test_encode_to_buffer(generation, &encoded), ODT_OK);
    successful_calls = encoded.write_calls;
    ODT_TEST_CHECK(successful_calls > 3u);
    for (fail_at = 1u; fail_at <= successful_calls; ++fail_at) {
        uint64_t output_size = UINT64_MAX;
        odt_sink sink = {&encoded, odt_test_buffer_sink_write};

        encoded.size = 0u;
        encoded.write_calls = 0u;
        encoded.fail_at_call = fail_at;
        ODT_TEST_STATUS(odt_encode(generation, NULL, &sink, &output_size), ODT_IO_ERROR);
        ODT_TEST_CHECK(output_size == UINT64_MAX);
    }
    odt_test_byte_buffer_destroy(&encoded);
    odt_generation_destroy(generation);
}

static void test_load_source_and_allocator_fail_after_each_call(void) {
    odt_test_byte_buffer encoded = {0};
    odt_test_memory_source successful_source;
    odt_test_allocator_state successful_state = {.live = true};
    odt_allocator successful_allocator;
    odt_generation *generation = NULL;
    size_t successful_reads;
    size_t successful_allocations;
    size_t fail_at;

    odt_test_independent_fixture(ODT_TEST_FIXTURE_REPRESENTATIVE, &encoded);
    memset(&successful_source, 0, sizeof(successful_source));
    successful_source.data = encoded.data;
    successful_source.size = encoded.size;
    odt_test_counting_allocator_init(&successful_allocator, &successful_state);
    ODT_TEST_STATUS(
        odt_test_load_from_memory(&successful_source, NULL, &successful_allocator, &generation),
        ODT_OK);
    successful_reads = successful_source.read_calls;
    successful_allocations = successful_state.allocate_calls;
    odt_generation_destroy(generation);
    ODT_TEST_CHECK(successful_state.live_allocations == 0u);
    ODT_TEST_CHECK(successful_reads > 3u);
    ODT_TEST_CHECK(successful_allocations >= 2u);

    for (fail_at = 1u; fail_at <= successful_reads; ++fail_at) {
        odt_test_memory_source source;
        odt_test_allocator_state state = {.live = true};
        odt_allocator allocator;

        memset(&source, 0, sizeof(source));
        source.data = encoded.data;
        source.size = encoded.size;
        source.fail_at_call = fail_at;
        odt_test_counting_allocator_init(&allocator, &state);
        generation = (odt_generation *)(uintptr_t)1u;
        ODT_TEST_STATUS(odt_test_load_from_memory(&source, NULL, &allocator, &generation),
                        ODT_IO_ERROR);
        ODT_TEST_CHECK(generation == NULL);
        ODT_TEST_CHECK(state.live_allocations == 0u);
    }
    for (fail_at = 1u; fail_at <= successful_allocations; ++fail_at) {
        odt_test_memory_source source;
        odt_test_allocator_state state = {.live = true, .fail_at_call = fail_at};
        odt_allocator allocator;

        memset(&source, 0, sizeof(source));
        source.data = encoded.data;
        source.size = encoded.size;
        odt_test_counting_allocator_init(&allocator, &state);
        generation = (odt_generation *)(uintptr_t)1u;
        ODT_TEST_STATUS(odt_test_load_from_memory(&source, NULL, &allocator, &generation),
                        ODT_OUT_OF_MEMORY);
        ODT_TEST_CHECK(generation == NULL);
        ODT_TEST_CHECK(state.live_allocations == 0u);
    }
    odt_test_byte_buffer_destroy(&encoded);
}

static void test_short_and_interrupted_file_io(void) {
    char directory[] = "/tmp/odt-fault-io-XXXXXX";
    char path[512];
    file_fault_state state = {
        .inject_read_eintr = true,
        .inject_write_eintr = true,
        .inject_size_eintr = true,
        .inject_sync_file_eintr = true,
        .inject_rename_eintr = true,
        .inject_sync_directory_eintr = true,
        .force_short_read = true,
        .force_short_write = true,
    };
    odt_internal_file_ops ops = fault_ops(&state);
    odt_generation *source =
        build_generation(&new_domain, new_sites, sizeof(new_sites) / sizeof(new_sites[0]));
    odt_generation *loaded = NULL;
    int length;

    ODT_TEST_CHECK(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/generation.odt", directory);
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(path));
    ODT_TEST_STATUS(odt_internal_save_file_atomic_with_ops(source, path, NULL, &ops), ODT_OK);
    ODT_TEST_CHECK(state.write_calls > 1u);
    ODT_TEST_CHECK(state.sync_file_calls == 2u);
    ODT_TEST_CHECK(state.rename_calls == 2u);
    ODT_TEST_CHECK(state.sync_directory_calls == 2u);
    ODT_TEST_STATUS(odt_internal_load_file_with_ops(path, NULL, NULL, &loaded, &ops), ODT_OK);
    ODT_TEST_CHECK(state.read_calls > 1u);
    ODT_TEST_CHECK(state.size_calls == 2u);
    ODT_TEST_CHECK(odt_test_query_digest(source) == odt_test_query_digest(loaded));
    expect_no_temporary_file(directory);

    odt_generation_destroy(loaded);
    odt_generation_destroy(source);
    ODT_TEST_CHECK(unlink(path) == 0);
    ODT_TEST_CHECK(rmdir(directory) == 0);
}

static void test_exclusive_temporary_ownership(void) {
    char directory[] = "/tmp/odt-fault-exclusive-XXXXXX";
    char path[512];
    char stale_path[640];
    odt_generation *generation =
        build_generation(&new_domain, new_sites, sizeof(new_sites) / sizeof(new_sites[0]));
    unsigned char stale_contents[] = {0x73u, 0x74u, 0x61u, 0x6cu, 0x65u};
    unsigned char observed[sizeof(stale_contents)];
    int stale_fd;
    int length;

    ODT_TEST_CHECK(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/generation.odt", directory);
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(path));
    length = snprintf(stale_path, sizeof(stale_path), "%s.odt.tmp.%ld.0", path, (long)getpid());
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(stale_path));
    stale_fd = open(stale_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    ODT_TEST_CHECK(stale_fd >= 0);
    ODT_TEST_CHECK(write(stale_fd, stale_contents, sizeof(stale_contents)) ==
                   (ssize_t)sizeof(stale_contents));
    ODT_TEST_CHECK(close(stale_fd) == 0);

    ODT_TEST_STATUS(odt_save_file_atomic(generation, path, NULL), ODT_OK);
    stale_fd = open(stale_path, O_RDONLY);
    ODT_TEST_CHECK(stale_fd >= 0);
    ODT_TEST_CHECK(read(stale_fd, observed, sizeof(observed)) == (ssize_t)sizeof(observed));
    ODT_TEST_CHECK(memcmp(observed, stale_contents, sizeof(observed)) == 0);
    ODT_TEST_CHECK(close(stale_fd) == 0);

    odt_generation_destroy(generation);
    ODT_TEST_CHECK(unlink(stale_path) == 0);
    ODT_TEST_CHECK(unlink(path) == 0);
    ODT_TEST_CHECK(rmdir(directory) == 0);
}

static void run_pre_rename_failure(file_fault fault) {
    char directory[] = "/tmp/odt-fault-prerename-XXXXXX";
    char path[512];
    file_fault_state state = {.fault = fault};
    odt_internal_file_ops ops = fault_ops(&state);
    odt_generation *old_generation =
        build_generation(&old_domain, old_sites, sizeof(old_sites) / sizeof(old_sites[0]));
    odt_generation *new_generation =
        build_generation(&new_domain, new_sites, sizeof(new_sites) / sizeof(new_sites[0]));
    int length;

    ODT_TEST_CHECK(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/generation.odt", directory);
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(path));
    if (fault != FILE_FAULT_OPEN_PARENT) {
        ODT_TEST_STATUS(odt_save_file_atomic(old_generation, path, NULL), ODT_OK);
    }
    ODT_TEST_STATUS(odt_internal_save_file_atomic_with_ops(new_generation, path, NULL, &ops),
                    ODT_IO_ERROR);
    if (fault == FILE_FAULT_OPEN_PARENT) {
        ODT_TEST_CHECK(access(path, F_OK) != 0);
    } else {
        expect_file_region(path, (odt_point){0.5, 0.5}, 7);
    }
    expect_no_temporary_file(directory);

    odt_generation_destroy(new_generation);
    odt_generation_destroy(old_generation);
    if (fault != FILE_FAULT_OPEN_PARENT) {
        ODT_TEST_CHECK(unlink(path) == 0);
    }
    ODT_TEST_CHECK(rmdir(directory) == 0);
}

static void test_pre_and_post_rename_failures(void) {
    char directory[] = "/tmp/odt-fault-postrename-XXXXXX";
    char path[512];
    file_fault_state state = {.fault = FILE_FAULT_SYNC_DIRECTORY};
    odt_internal_file_ops ops = fault_ops(&state);
    odt_generation *old_generation =
        build_generation(&old_domain, old_sites, sizeof(old_sites) / sizeof(old_sites[0]));
    odt_generation *new_generation =
        build_generation(&new_domain, new_sites, sizeof(new_sites) / sizeof(new_sites[0]));
    int length;

    run_pre_rename_failure(FILE_FAULT_WRITE);
    run_pre_rename_failure(FILE_FAULT_SYNC_FILE);
    run_pre_rename_failure(FILE_FAULT_RENAME);
    run_pre_rename_failure(FILE_FAULT_OPEN_PARENT);
    run_pre_rename_failure(FILE_FAULT_CREATE);
    run_pre_rename_failure(FILE_FAULT_CLOSE);

    ODT_TEST_CHECK(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/generation.odt", directory);
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(path));
    ODT_TEST_STATUS(odt_save_file_atomic(old_generation, path, NULL), ODT_OK);
    ODT_TEST_STATUS(odt_internal_save_file_atomic_with_ops(new_generation, path, NULL, &ops),
                    ODT_COMMIT_UNKNOWN);
    expect_file_region(path, (odt_point){1.0, 0.0}, 22);
    expect_no_temporary_file(directory);

    odt_generation_destroy(new_generation);
    odt_generation_destroy(old_generation);
    ODT_TEST_CHECK(unlink(path) == 0);
    ODT_TEST_CHECK(rmdir(directory) == 0);
}

static void run_file_load_failure(file_fault fault) {
    char directory[] = "/tmp/odt-fault-read-XXXXXX";
    char path[512];
    file_fault_state state = {.fault = fault};
    odt_internal_file_ops ops = fault_ops(&state);
    odt_generation *source =
        build_generation(&new_domain, new_sites, sizeof(new_sites) / sizeof(new_sites[0]));
    odt_generation *loaded = (odt_generation *)(uintptr_t)1u;
    int length;

    ODT_TEST_CHECK(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/generation.odt", directory);
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(path));
    ODT_TEST_STATUS(odt_save_file_atomic(source, path, NULL), ODT_OK);
    ODT_TEST_STATUS(odt_internal_load_file_with_ops(path, NULL, NULL, &loaded, &ops), ODT_IO_ERROR);
    ODT_TEST_CHECK(loaded == NULL);

    odt_generation_destroy(source);
    ODT_TEST_CHECK(unlink(path) == 0);
    ODT_TEST_CHECK(rmdir(directory) == 0);
}

int main(void) {
    test_encode_sink_fail_after_each_call();
    test_load_source_and_allocator_fail_after_each_call();
    test_short_and_interrupted_file_io();
    test_exclusive_temporary_ownership();
    test_pre_and_post_rename_failures();
    run_file_load_failure(FILE_FAULT_OPEN_READ);
    run_file_load_failure(FILE_FAULT_SIZE);
    run_file_load_failure(FILE_FAULT_READ);
    return EXIT_SUCCESS;
}
