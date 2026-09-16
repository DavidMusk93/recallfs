#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "odt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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
#ifndef SSIZE_MAX
#define SSIZE_MAX ((ssize_t)(SIZE_MAX >> 1u))
#endif

enum {
    ODT_FILE_TEMP_ATTEMPTS = 128,
    ODT_FILE_TEMP_SUFFIX_CAPACITY = 48,
};

typedef struct odt_file_path {
    char *parent;
    char *base;
    char *temporary;
    size_t temporary_capacity;
} odt_file_path;

typedef struct odt_file_sink_context {
    const odt_internal_file_ops *ops;
    int fd;
} odt_file_sink_context;

typedef struct odt_file_source_context {
    const odt_internal_file_ops *ops;
    int fd;
} odt_file_source_context;

static int odt_posix_open_parent(void *context, const char *path) {
    (void)context;
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int odt_posix_open_read(void *context, const char *path) {
    (void)context;
    return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

static int odt_posix_create_exclusive_at(void *context, int parent_fd, const char *name) {
    (void)context;
    return openat(parent_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
}

static int odt_posix_get_size(void *context, int fd, uint64_t *out_size) {
    struct stat status;

    (void)context;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return -1;
    }
    *out_size = (uint64_t)status.st_size;
    return (off_t)*out_size == status.st_size ? 0 : -1;
}

static ssize_t odt_posix_read_at(void *context, int fd, void *data, size_t size, uint64_t offset) {
    (void)context;
    if (offset > (uint64_t)INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return pread(fd, data, size, (off_t)offset);
}

static ssize_t odt_posix_write(void *context, int fd, const void *data, size_t size) {
    (void)context;
    return write(fd, data, size);
}

static int odt_posix_sync_file(void *context, int fd) {
    (void)context;
#ifdef __APPLE__
    return fcntl(fd, F_FULLFSYNC);
#else
    return fsync(fd);
#endif
}

static int odt_posix_rename_at(void *context, int parent_fd, const char *from, const char *to) {
    (void)context;
    return renameat(parent_fd, from, parent_fd, to);
}

static int odt_posix_sync_directory(void *context, int fd) {
    (void)context;
    return fsync(fd);
}

static int odt_posix_unlink_at(void *context, int parent_fd, const char *name) {
    (void)context;
    return unlinkat(parent_fd, name, 0);
}

static int odt_posix_close(void *context, int fd) {
    (void)context;
    /* A failed close may already have released fd; retry could close an unrelated descriptor. */
    return close(fd);
}

static const odt_internal_file_ops odt_posix_ops = {
    NULL,
    odt_posix_open_parent,
    odt_posix_open_read,
    odt_posix_create_exclusive_at,
    odt_posix_get_size,
    odt_posix_read_at,
    odt_posix_write,
    odt_posix_sync_file,
    odt_posix_rename_at,
    odt_posix_sync_directory,
    odt_posix_unlink_at,
    odt_posix_close,
};

static bool odt_file_ops_valid(const odt_internal_file_ops *ops) {
    return ops != NULL && ops->open_parent != NULL && ops->open_read != NULL &&
           ops->create_exclusive_at != NULL && ops->get_size != NULL && ops->read_at != NULL &&
           ops->write != NULL && ops->sync_file != NULL && ops->rename_at != NULL &&
           ops->sync_directory != NULL && ops->unlink_at != NULL && ops->close != NULL;
}

static void odt_file_path_destroy(odt_file_path *path) {
    free(path->temporary);
    free(path->base);
    free(path->parent);
    memset(path, 0, sizeof(*path));
}

static odt_status odt_file_path_init(const char *path, odt_file_path *out_path) {
    const char *slash;
    const char *base;
    size_t path_size;
    size_t parent_size;
    size_t base_size;

    memset(out_path, 0, sizeof(*out_path));
    if (path == NULL || path[0] == '\0') {
        return ODT_INVALID_ARGUMENT;
    }
    path_size = strlen(path);
    if (path[path_size - 1u] == '/') {
        return ODT_INVALID_ARGUMENT;
    }
    slash = strrchr(path, '/');
    base = slash == NULL ? path : slash + 1;
    base_size = strlen(base);
    if (base_size == 0u || strcmp(base, ".") == 0 || strcmp(base, "..") == 0 ||
        base_size > SIZE_MAX - ODT_FILE_TEMP_SUFFIX_CAPACITY) {
        return ODT_INVALID_ARGUMENT;
    }
    if (slash == NULL) {
        parent_size = 1u;
    } else if (slash == path) {
        parent_size = 1u;
    } else {
        parent_size = (size_t)(slash - path);
    }
    out_path->parent = malloc(parent_size + 1u);
    out_path->base = malloc(base_size + 1u);
    out_path->temporary = malloc(base_size + ODT_FILE_TEMP_SUFFIX_CAPACITY);
    if (out_path->parent == NULL || out_path->base == NULL || out_path->temporary == NULL) {
        odt_file_path_destroy(out_path);
        return ODT_OUT_OF_MEMORY;
    }
    if (slash == NULL) {
        memcpy(out_path->parent, ".", 2u);
    } else if (slash == path) {
        memcpy(out_path->parent, "/", 2u);
    } else {
        memcpy(out_path->parent, path, parent_size);
        out_path->parent[parent_size] = '\0';
    }
    memcpy(out_path->base, base, base_size + 1u);
    out_path->temporary_capacity = base_size + ODT_FILE_TEMP_SUFFIX_CAPACITY;
    return ODT_OK;
}

static int odt_file_open_parent(const odt_internal_file_ops *ops, const char *path) {
    int fd;

    do {
        fd = ops->open_parent(ops->context, path);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static int odt_file_open_read(const odt_internal_file_ops *ops, const char *path) {
    int fd;

    do {
        fd = ops->open_read(ops->context, path);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static bool odt_file_sync(const odt_internal_file_ops *ops, int fd, bool directory) {
    int result;

    do {
        result =
            directory ? ops->sync_directory(ops->context, fd) : ops->sync_file(ops->context, fd);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool odt_file_rename(const odt_internal_file_ops *ops, int parent_fd, const char *from,
                            const char *to) {
    int result;

    do {
        result = ops->rename_at(ops->context, parent_fd, from, to);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool odt_file_get_size(const odt_internal_file_ops *ops, int fd, uint64_t *out_size) {
    int result;

    do {
        result = ops->get_size(ops->context, fd, out_size);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool odt_file_unlink(const odt_internal_file_ops *ops, int parent_fd, const char *name) {
    int result;

    do {
        result = ops->unlink_at(ops->context, parent_fd, name);
    } while (result != 0 && errno == EINTR);
    return result == 0 || errno == ENOENT;
}

static int odt_file_create_temporary(const odt_internal_file_ops *ops, int parent_fd,
                                     odt_file_path *path) {
    unsigned int attempt;

    for (attempt = 0u; attempt < ODT_FILE_TEMP_ATTEMPTS; ++attempt) {
        int length = snprintf(path->temporary, path->temporary_capacity, "%s.odt.tmp.%ld.%u",
                              path->base, (long)getpid(), attempt);
        int fd;

        if (length < 0 || (size_t)length >= path->temporary_capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
        do {
            fd = ops->create_exclusive_at(ops->context, parent_fd, path->temporary);
        } while (fd < 0 && errno == EINTR);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

static int odt_file_sink_write(void *context, const void *data, size_t size) {
    odt_file_sink_context *sink = context;
    const unsigned char *bytes = data;
    size_t completed = 0u;

    while (completed < size) {
        const size_t remaining = size - completed;
        const size_t request = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
        ssize_t result = sink->ops->write(sink->ops->context, sink->fd, bytes + completed, request);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (size_t)result > request) {
            return -1;
        }
        completed += (size_t)result;
    }
    return 0;
}

static int odt_file_source_read_at(void *context, uint64_t offset, void *data_out, size_t size) {
    odt_file_source_context *source = context;
    unsigned char *bytes = data_out;
    size_t completed = 0u;

    while (completed < size) {
        const size_t remaining = size - completed;
        const size_t request = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
        uint64_t current_offset;
        ssize_t result;

        if (offset > UINT64_MAX - (uint64_t)completed) {
            return -1;
        }
        current_offset = offset + (uint64_t)completed;
        result = source->ops->read_at(source->ops->context, source->fd, bytes + completed, request,
                                      current_offset);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (size_t)result > request) {
            return -1;
        }
        completed += (size_t)result;
    }
    return 0;
}

odt_status odt_internal_save_file_atomic_with_ops(const odt_generation *generation,
                                                  const char *path,
                                                  const odt_encode_options *options,
                                                  const odt_internal_file_ops *ops) {
    odt_file_path names;
    odt_file_sink_context sink_context;
    odt_sink sink;
    int parent_fd = -1;
    int temporary_fd = -1;
    bool owns_temporary = false;
    odt_status status;

    if (generation == NULL || !odt_file_ops_valid(ops)) {
        return ODT_INVALID_ARGUMENT;
    }
    if (options != NULL) {
        status = odt_internal_encode_options_validate(options);
        if (status != ODT_OK) {
            return status;
        }
    }
    status = odt_file_path_init(path, &names);
    if (status != ODT_OK) {
        return status;
    }
    parent_fd = odt_file_open_parent(ops, names.parent);
    if (parent_fd < 0) {
        status = ODT_IO_ERROR;
        goto out;
    }
    temporary_fd = odt_file_create_temporary(ops, parent_fd, &names);
    if (temporary_fd < 0) {
        status = ODT_IO_ERROR;
        goto out;
    }
    owns_temporary = true;
    sink_context.ops = ops;
    sink_context.fd = temporary_fd;
    sink.context = &sink_context;
    sink.write = odt_file_sink_write;
    status = odt_encode(generation, options, &sink, NULL);
    if (status != ODT_OK) {
        goto out;
    }
    if (!odt_file_sync(ops, temporary_fd, false)) {
        status = ODT_IO_ERROR;
        goto out;
    }
    if (ops->close(ops->context, temporary_fd) != 0) {
        temporary_fd = -1;
        status = ODT_IO_ERROR;
        goto out;
    }
    temporary_fd = -1;
    if (!odt_file_rename(ops, parent_fd, names.temporary, names.base)) {
        status = ODT_IO_ERROR;
        goto out;
    }
    owns_temporary = false;
    status = odt_file_sync(ops, parent_fd, true) ? ODT_OK : ODT_COMMIT_UNKNOWN;

out:
    if (temporary_fd >= 0) {
        (void)ops->close(ops->context, temporary_fd);
    }
    if (owns_temporary) {
        (void)odt_file_unlink(ops, parent_fd, names.temporary);
    }
    if (parent_fd >= 0) {
        (void)ops->close(ops->context, parent_fd);
    }
    odt_file_path_destroy(&names);
    return status;
}

odt_status odt_save_file_atomic(const odt_generation *generation, const char *path,
                                const odt_encode_options *options) {
    return odt_internal_save_file_atomic_with_ops(generation, path, options, &odt_posix_ops);
}

odt_status odt_internal_load_file_with_ops(const char *path, const odt_load_limits *limits,
                                           const odt_allocator *allocator,
                                           odt_generation **out_generation,
                                           const odt_internal_file_ops *ops) {
    odt_file_source_context source_context;
    odt_source source;
    int fd;
    uint64_t size;
    odt_status status;

    if (out_generation == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_generation = NULL;
    if (path == NULL || path[0] == '\0' || !odt_file_ops_valid(ops)) {
        return ODT_INVALID_ARGUMENT;
    }
    fd = odt_file_open_read(ops, path);
    if (fd < 0) {
        return ODT_IO_ERROR;
    }
    if (!odt_file_get_size(ops, fd, &size)) {
        (void)ops->close(ops->context, fd);
        return ODT_IO_ERROR;
    }
    source_context.ops = ops;
    source_context.fd = fd;
    source.context = &source_context;
    source.encoded_size = size;
    source.read_at = odt_file_source_read_at;
    status = odt_load(&source, limits, allocator, out_generation);
    (void)ops->close(ops->context, fd);
    return status;
}

odt_status odt_load_file(const char *path, const odt_load_limits *limits,
                         const odt_allocator *allocator, odt_generation **out_generation) {
    return odt_internal_load_file_with_ops(path, limits, allocator, out_generation, &odt_posix_ops);
}
