#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "rbt_backends.h"
#include "rbt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

enum {
    RBT_FILE_MAGIC_SIZE = 8,
    RBT_FILE_UUID_SIZE = 16,
    RBT_FILE_VERSION_OFFSET = 8,
    RBT_FILE_PAGE_SIZE_OFFSET = 12,
    RBT_FILE_FLAGS_OFFSET = 16,
    RBT_FILE_UUID_OFFSET = 24,
    RBT_FILE_CHECKSUM_OFFSET = 40,
    RBT_FILE_HEADER_USED = 44,

    RBT_WAL_MAGIC_SIZE = 8,
    RBT_WAL_VERSION_OFFSET = 8,
    RBT_WAL_PAGE_SIZE_OFFSET = 12,
    RBT_WAL_FLAGS_OFFSET = 16,
    RBT_WAL_HEADER_SIZE_OFFSET = 20,
    RBT_WAL_UUID_OFFSET = 24,
    RBT_WAL_RECORD_COUNT_OFFSET = 40,
    RBT_WAL_ORIGINAL_COUNT_OFFSET = 48,
    RBT_WAL_FINAL_COUNT_OFFSET = 56,
    RBT_WAL_TOTAL_SIZE_OFFSET = 64,
    RBT_WAL_CHECKSUM_OFFSET = 72,
    RBT_WAL_HEADER_SIZE = 80,
    RBT_WAL_PAGE_ID_SIZE = 8,
    RBT_WAL_MAX_TRANSACTION_PAGES = 32768,
};

#define RBT_FILE_FORMAT_VERSION UINT32_C(1)
#define RBT_WAL_FORMAT_VERSION UINT32_C(1)

static const unsigned char RBT_FILE_MAGIC[RBT_FILE_MAGIC_SIZE] = {'R', 'B', 'T', 'D',
                                                                  'A', 'T', '0', '1'};
static const unsigned char RBT_WAL_MAGIC[RBT_WAL_MAGIC_SIZE] = {'R', 'B', 'T', 'W',
                                                                'A', 'L', '0', '1'};

struct rbt_file {
    int fd;
    int parent_fd;
    uint32_t page_size;
    uint64_t page_count;
    unsigned char uuid[RBT_FILE_UUID_SIZE];
    char *data_name;
    char *wal_name;
    char *wal_temp_name;
    bool poisoned;
};

static int rbt_file_close_descriptor(int descriptor) {
    int result;

    do {
        result = close(descriptor);
    } while (result != 0 && errno == EINTR);
    return result;
}

static int rbt_file_cloexec(int descriptor) {
    int flags;

    if (descriptor < 0) {
        return descriptor;
    }
    do {
        flags = fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        int saved = errno;

        (void)rbt_file_close_descriptor(descriptor);
        errno = saved;
        return -1;
    }
    if (flags >= 0 && (flags & FD_CLOEXEC) == 0) {
        int result;

        do {
            result = fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            int saved = errno;

            (void)rbt_file_close_descriptor(descriptor);
            errno = saved;
            return -1;
        }
    }
    return descriptor;
}

static int rbt_file_open_path(const char *path, int flags) {
    int descriptor;

    do {
        descriptor = open(path, flags | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    return rbt_file_cloexec(descriptor);
}

static int rbt_file_open_at(int parent, const char *name, int flags) {
    int descriptor;

    do {
        descriptor = openat(parent, name, flags | O_CLOEXEC | O_NOFOLLOW);
    } while (descriptor < 0 && errno == EINTR);
    return rbt_file_cloexec(descriptor);
}

static int rbt_file_create_at(int parent, const char *name, int flags) {
    int descriptor;

    do {
        descriptor = openat(parent, name, flags | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_EXCL,
                            (mode_t)(S_IRUSR | S_IWUSR));
    } while (descriptor < 0 && errno == EINTR);
    return rbt_file_cloexec(descriptor);
}

static bool rbt_file_stat(int descriptor, struct stat *out_status) {
    int result;

    do {
        result = fstat(descriptor, out_status);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool rbt_file_sync_regular(int descriptor) {
    int result;

    do {
#ifdef __APPLE__
        result = fcntl(descriptor, F_FULLFSYNC);
#else
        result = fsync(descriptor);
#endif
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool rbt_file_sync_directory(int descriptor) {
    int result;

    do {
        result = fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool rbt_file_truncate(int descriptor, off_t size) {
    int result;

    do {
        result = ftruncate(descriptor, size);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool rbt_file_make_private(int descriptor) {
    int result;

    do {
        result = fchmod(descriptor, (mode_t)(S_IRUSR | S_IWUSR));
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool rbt_file_read_exact(int descriptor, void *data, size_t size, off_t offset) {
    unsigned char *bytes = data;
    size_t completed = 0u;

    while (completed < size) {
        size_t remaining = size - completed;
        size_t request = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
        ssize_t result = pread(descriptor, bytes + completed, request, offset + (off_t)completed);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        completed += (size_t)result;
    }
    return true;
}

static bool rbt_file_write_exact(int descriptor, const void *data, size_t size, off_t offset) {
    const unsigned char *bytes = data;
    size_t completed = 0u;

    while (completed < size) {
        size_t remaining = size - completed;
        size_t request = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
        ssize_t result = pwrite(descriptor, bytes + completed, request, offset + (off_t)completed);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        completed += (size_t)result;
    }
    return true;
}

static bool rbt_file_descriptor_size(int descriptor, uint64_t *out_size) {
    struct stat status;

    if (!rbt_file_stat(descriptor, &status) || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return false;
    }
    *out_size = (uint64_t)status.st_size;
    return (off_t)*out_size == status.st_size;
}

static bool rbt_file_physical_size(uint64_t page_count, uint32_t page_size, uint64_t *out_size) {
    uint64_t physical_pages;
    uint64_t size;

    if (page_count == UINT64_MAX) {
        return false;
    }
    physical_pages = page_count + 1u;
    if (physical_pages > UINT64_MAX / page_size) {
        return false;
    }
    size = physical_pages * page_size;
    if ((off_t)size < 0 || (uint64_t)(off_t)size != size) {
        return false;
    }
    *out_size = size;
    return true;
}

static bool rbt_file_page_offset(uint64_t page_id, uint32_t page_size, off_t *out_offset) {
    uint64_t offset;

    if (!rbt_file_physical_size(page_id, page_size, &offset)) {
        return false;
    }
    *out_offset = (off_t)offset;
    return true;
}

static int rbt_file_lock(int descriptor) {
    int result;

    do {
        result = flock(descriptor, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return 0;
    }
    return errno == EACCES || errno == EAGAIN ? -EBUSY : -EIO;
}

static int rbt_file_absent(int parent, const char *name) {
    struct stat status;
    int flags = 0;
    int result;

#ifdef AT_SYMLINK_NOFOLLOW
    flags = AT_SYMLINK_NOFOLLOW;
#endif
    do {
        result = fstatat(parent, name, &status, flags);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return -EBUSY;
    }
    return errno == ENOENT ? 0 : -EIO;
}

static bool rbt_file_unlink(int parent, const char *name) {
    int result;

    do {
        result = unlinkat(parent, name, 0);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool rbt_file_rename(int parent, const char *from, const char *to) {
    int result;

    do {
        result = renameat(parent, from, parent, to);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static int rbt_file_build_names(const char *path, char **out_data, char **out_wal, char **out_temp,
                                char **out_parent) {
    const char *slash;
    const char *base;
    size_t base_size;
    size_t parent_size;

    if (path == NULL || path[0] == '\0' || path[strlen(path) - 1u] == '/') {
        return -EINVAL;
    }
    slash = strrchr(path, '/');
    base = slash == NULL ? path : slash + 1;
    base_size = strlen(base);
    if (base_size > SIZE_MAX - 9u) {
        return -EINVAL;
    }
    *out_data = malloc(base_size + 1u);
    *out_wal = malloc(base_size + 5u);
    *out_temp = malloc(base_size + 9u);
    if (*out_data == NULL || *out_wal == NULL || *out_temp == NULL) {
        return -ENOMEM;
    }
    memcpy(*out_data, base, base_size + 1u);
    memcpy(*out_wal, base, base_size);
    memcpy(*out_wal + base_size, ".wal", 5u);
    memcpy(*out_temp, base, base_size);
    memcpy(*out_temp + base_size, ".wal.tmp", 9u);

    if (slash == NULL) {
        *out_parent = malloc(2u);
        if (*out_parent != NULL) {
            memcpy(*out_parent, ".", 2u);
        }
    } else if (slash == path) {
        *out_parent = malloc(2u);
        if (*out_parent != NULL) {
            memcpy(*out_parent, "/", 2u);
        }
    } else {
        parent_size = (size_t)(slash - path);
        *out_parent = malloc(parent_size + 1u);
        if (*out_parent != NULL) {
            memcpy(*out_parent, path, parent_size);
            (*out_parent)[parent_size] = '\0';
        }
    }
    return *out_parent == NULL ? -ENOMEM : 0;
}

static void rbt_file_release(struct rbt_file *file) {
    if (file == NULL) {
        return;
    }
    if (file->fd >= 0) {
        (void)rbt_file_close_descriptor(file->fd);
    }
    if (file->parent_fd >= 0) {
        (void)rbt_file_close_descriptor(file->parent_fd);
    }
    free(file->data_name);
    free(file->wal_name);
    free(file->wal_temp_name);
    free(file);
}

static uint32_t rbt_file_checksum(unsigned char *data, size_t size, size_t offset) {
    uint32_t stored = rbt_load_u32(data + offset);
    uint32_t checksum;

    rbt_store_u32(data + offset, 0u);
    checksum = rbt_crc32c(data, size);
    rbt_store_u32(data + offset, stored);
    return checksum;
}

static void rbt_file_header_encode(unsigned char *header, uint32_t page_size,
                                   const unsigned char uuid[RBT_FILE_UUID_SIZE]) {
    memset(header, 0, (size_t)page_size);
    memcpy(header, RBT_FILE_MAGIC, RBT_FILE_MAGIC_SIZE);
    rbt_store_u32(header + RBT_FILE_VERSION_OFFSET, RBT_FILE_FORMAT_VERSION);
    rbt_store_u32(header + RBT_FILE_PAGE_SIZE_OFFSET, page_size);
    memcpy(header + RBT_FILE_UUID_OFFSET, uuid, RBT_FILE_UUID_SIZE);
    rbt_store_u32(header + RBT_FILE_CHECKSUM_OFFSET,
                  rbt_file_checksum(header, (size_t)page_size, RBT_FILE_CHECKSUM_OFFSET));
}

static int rbt_file_read_header(struct rbt_file *file) {
    unsigned char prefix[RBT_FILE_HEADER_USED];
    unsigned char *header;
    uint64_t size;
    uint32_t page_size;
    size_t index;
    int result = 0;

    if (!rbt_file_descriptor_size(file->fd, &size) || size < sizeof(prefix) ||
        !rbt_file_read_exact(file->fd, prefix, sizeof(prefix), 0) ||
        memcmp(prefix, RBT_FILE_MAGIC, RBT_FILE_MAGIC_SIZE) != 0) {
        return -EBADMSG;
    }
    page_size = rbt_load_u32(prefix + RBT_FILE_PAGE_SIZE_OFFSET);
    if (!rbt_page_size_valid(page_size) || size < page_size) {
        return -EBADMSG;
    }
    header = malloc((size_t)page_size);
    if (header == NULL) {
        return -ENOMEM;
    }
    if (!rbt_file_read_exact(file->fd, header, (size_t)page_size, 0)) {
        result = -EIO;
    } else if (rbt_load_u32(header + RBT_FILE_CHECKSUM_OFFSET) !=
               rbt_file_checksum(header, (size_t)page_size, RBT_FILE_CHECKSUM_OFFSET)) {
        result = -EBADMSG;
    } else if (rbt_load_u32(header + RBT_FILE_VERSION_OFFSET) != RBT_FILE_FORMAT_VERSION ||
               rbt_load_u32(header + RBT_FILE_FLAGS_OFFSET) != 0u) {
        result = -ENOTSUP;
    }
    for (index = RBT_FILE_HEADER_USED; result == 0 && index < (size_t)page_size; ++index) {
        if (header[index] != 0u) {
            result = -ENOTSUP;
        }
    }
    if (result == 0) {
        file->page_size = page_size;
        memcpy(file->uuid, header + RBT_FILE_UUID_OFFSET, RBT_FILE_UUID_SIZE);
    }
    free(header);
    return result;
}

static int rbt_file_measure(const struct rbt_file *file, uint64_t *out_count) {
    uint64_t size;

    if (!rbt_file_descriptor_size(file->fd, &size)) {
        return -EIO;
    }
    if (size < file->page_size || size % file->page_size != 0u) {
        return -EBADMSG;
    }
    *out_count = size / file->page_size - 1u;
    return 0;
}

static void rbt_file_crash(const char *point) {
#ifdef RBT_ENABLE_TEST_HOOKS
    const char *configured = getenv("RBT_TEST_CRASH_POINT");

    if (configured != NULL && strcmp(configured, point) == 0) {
        _exit(86);
    }
#else
    (void)point;
#endif
}

static int rbt_file_remove_temp(struct rbt_file *file) {
    int result = rbt_file_absent(file->parent_fd, file->wal_temp_name);

    if (result == 0) {
        return 0;
    }
    if (result != -EBUSY || !rbt_file_unlink(file->parent_fd, file->wal_temp_name) ||
        !rbt_file_sync_directory(file->parent_fd)) {
        return -EIO;
    }
    return 0;
}

static int rbt_file_validate_updates(const struct rbt_file *file,
                                     const struct rbt_page_update *updates, size_t update_count,
                                     uint64_t *out_final_count) {
    uint64_t final_count = file->page_count;
    uint64_t new_ids = 0u;
    size_t index;

    if ((update_count != 0u && updates == NULL) ||
        update_count > (size_t)RBT_WAL_MAX_TRANSACTION_PAGES) {
        return update_count > (size_t)RBT_WAL_MAX_TRANSACTION_PAGES ? -ENOMEM : -EINVAL;
    }
    for (index = 0u; index < update_count; ++index) {
        off_t ignored;
        size_t other;

        if (updates[index].data == NULL) {
            return -EINVAL;
        }
        if (!rbt_file_page_offset(updates[index].page_id, file->page_size, &ignored)) {
            return -ENOMEM;
        }
        for (other = 0u; other < index; ++other) {
            if (updates[other].page_id == updates[index].page_id) {
                return -EINVAL;
            }
        }
        if (updates[index].page_id >= file->page_count) {
            ++new_ids;
        }
        if (updates[index].page_id + 1u > final_count) {
            final_count = updates[index].page_id + 1u;
        }
    }
    if (final_count - file->page_count != new_ids) {
        return -EINVAL;
    }
    *out_final_count = final_count;
    return 0;
}

static int rbt_file_build_wal(const struct rbt_file *file, const struct rbt_page_update *updates,
                              size_t update_count, uint64_t final_count, unsigned char **out_wal,
                              size_t *out_size) {
    size_t record_size = RBT_WAL_PAGE_ID_SIZE + (size_t)file->page_size;
    size_t wal_size;
    unsigned char *wal;
    size_t index;

    if (update_count > (SIZE_MAX - RBT_WAL_HEADER_SIZE) / record_size) {
        return -ENOMEM;
    }
    wal_size = RBT_WAL_HEADER_SIZE + update_count * record_size;
    if ((off_t)wal_size < 0 || (size_t)(off_t)wal_size != wal_size) {
        return -ENOMEM;
    }
    wal = calloc(1u, wal_size);
    if (wal == NULL) {
        return -ENOMEM;
    }
    memcpy(wal, RBT_WAL_MAGIC, RBT_WAL_MAGIC_SIZE);
    rbt_store_u32(wal + RBT_WAL_VERSION_OFFSET, RBT_WAL_FORMAT_VERSION);
    rbt_store_u32(wal + RBT_WAL_PAGE_SIZE_OFFSET, file->page_size);
    rbt_store_u32(wal + RBT_WAL_HEADER_SIZE_OFFSET, RBT_WAL_HEADER_SIZE);
    memcpy(wal + RBT_WAL_UUID_OFFSET, file->uuid, RBT_FILE_UUID_SIZE);
    rbt_store_u64(wal + RBT_WAL_RECORD_COUNT_OFFSET, (uint64_t)update_count);
    rbt_store_u64(wal + RBT_WAL_ORIGINAL_COUNT_OFFSET, file->page_count);
    rbt_store_u64(wal + RBT_WAL_FINAL_COUNT_OFFSET, final_count);
    rbt_store_u64(wal + RBT_WAL_TOTAL_SIZE_OFFSET, (uint64_t)wal_size);
    for (index = 0u; index < update_count; ++index) {
        size_t offset = RBT_WAL_HEADER_SIZE + index * record_size;

        rbt_store_u64(wal + offset, updates[index].page_id);
        memcpy(wal + offset + RBT_WAL_PAGE_ID_SIZE, updates[index].data, (size_t)file->page_size);
    }
    rbt_store_u32(wal + RBT_WAL_CHECKSUM_OFFSET,
                  rbt_file_checksum(wal, wal_size, RBT_WAL_CHECKSUM_OFFSET));
    *out_wal = wal;
    *out_size = wal_size;
    return 0;
}

static int rbt_file_validate_wal(struct rbt_file *file, unsigned char *wal, size_t wal_size,
                                 uint64_t data_size, uint64_t *out_final_count) {
    size_t record_size = RBT_WAL_PAGE_ID_SIZE + (size_t)file->page_size;
    uint64_t record_count;
    uint64_t original_count;
    uint64_t final_count;
    uint64_t original_size;
    uint64_t final_size;
    uint64_t maximum;
    uint64_t new_ids = 0u;
    size_t expected;
    size_t index;

    if (wal_size < RBT_WAL_HEADER_SIZE || memcmp(wal, RBT_WAL_MAGIC, RBT_WAL_MAGIC_SIZE) != 0 ||
        rbt_load_u32(wal + RBT_WAL_PAGE_SIZE_OFFSET) != file->page_size ||
        rbt_load_u32(wal + RBT_WAL_HEADER_SIZE_OFFSET) != RBT_WAL_HEADER_SIZE) {
        return -EBADMSG;
    }
    record_count = rbt_load_u64(wal + RBT_WAL_RECORD_COUNT_OFFSET);
    if (record_count == 0u || record_count > RBT_WAL_MAX_TRANSACTION_PAGES ||
        record_count > (uint64_t)((SIZE_MAX - RBT_WAL_HEADER_SIZE) / record_size)) {
        return -EBADMSG;
    }
    expected = RBT_WAL_HEADER_SIZE + (size_t)record_count * record_size;
    if (expected != wal_size || rbt_load_u64(wal + RBT_WAL_TOTAL_SIZE_OFFSET) != wal_size ||
        rbt_load_u32(wal + RBT_WAL_CHECKSUM_OFFSET) !=
            rbt_file_checksum(wal, wal_size, RBT_WAL_CHECKSUM_OFFSET)) {
        return -EBADMSG;
    }
    if (rbt_load_u32(wal + RBT_WAL_VERSION_OFFSET) != RBT_WAL_FORMAT_VERSION ||
        rbt_load_u32(wal + RBT_WAL_FLAGS_OFFSET) != 0u) {
        return -ENOTSUP;
    }
    if (memcmp(wal + RBT_WAL_UUID_OFFSET, file->uuid, RBT_FILE_UUID_SIZE) != 0) {
        return -EBADMSG;
    }
    original_count = rbt_load_u64(wal + RBT_WAL_ORIGINAL_COUNT_OFFSET);
    final_count = rbt_load_u64(wal + RBT_WAL_FINAL_COUNT_OFFSET);
    if (final_count < original_count ||
        !rbt_file_physical_size(original_count, file->page_size, &original_size) ||
        !rbt_file_physical_size(final_count, file->page_size, &final_size) ||
        data_size < original_size || data_size > final_size) {
        return -EBADMSG;
    }
    maximum = original_count;
    for (index = 0u; index < (size_t)record_count; ++index) {
        size_t offset = RBT_WAL_HEADER_SIZE + index * record_size;
        uint64_t page_id = rbt_load_u64(wal + offset);
        size_t other;

        if (page_id >= final_count) {
            return -EBADMSG;
        }
        for (other = 0u; other < index; ++other) {
            if (rbt_load_u64(wal + RBT_WAL_HEADER_SIZE + other * record_size) == page_id) {
                return -EBADMSG;
            }
        }
        if (page_id >= original_count) {
            ++new_ids;
        }
        if (page_id + 1u > maximum) {
            maximum = page_id + 1u;
        }
    }
    if (maximum != final_count || final_count - original_count != new_ids) {
        return -EBADMSG;
    }
    *out_final_count = final_count;
    return 0;
}

static int rbt_file_recover(struct rbt_file *file) {
    int wal_fd;
    uint64_t wal_file_size;
    uint64_t data_size;
    uint64_t final_count;
    unsigned char header[RBT_WAL_HEADER_SIZE];
    unsigned char *wal;
    size_t record_size;
    uint64_t record_count;
    size_t index;
    int result;

    wal_fd = rbt_file_open_at(file->parent_fd, file->wal_name, O_RDONLY);
    if (wal_fd < 0) {
        if (errno == ENOENT) {
            return rbt_file_measure(file, &file->page_count);
        }
        return -EIO;
    }
    if (!rbt_file_descriptor_size(wal_fd, &wal_file_size) || wal_file_size < RBT_WAL_HEADER_SIZE ||
        !rbt_file_read_exact(wal_fd, header, sizeof(header), 0)) {
        (void)rbt_file_close_descriptor(wal_fd);
        return -EBADMSG;
    }
    record_size = RBT_WAL_PAGE_ID_SIZE + (size_t)file->page_size;
    record_count = rbt_load_u64(header + RBT_WAL_RECORD_COUNT_OFFSET);
    if (record_count == 0u || record_count > RBT_WAL_MAX_TRANSACTION_PAGES ||
        record_count > (uint64_t)((SIZE_MAX - RBT_WAL_HEADER_SIZE) / record_size) ||
        wal_file_size != RBT_WAL_HEADER_SIZE + record_count * record_size ||
        rbt_load_u64(header + RBT_WAL_TOTAL_SIZE_OFFSET) != wal_file_size ||
        wal_file_size > (uint64_t)SIZE_MAX) {
        (void)rbt_file_close_descriptor(wal_fd);
        return -EBADMSG;
    }
    wal = malloc((size_t)wal_file_size);
    if (wal == NULL) {
        (void)rbt_file_close_descriptor(wal_fd);
        return -ENOMEM;
    }
    if (!rbt_file_read_exact(wal_fd, wal, (size_t)wal_file_size, 0) ||
        rbt_file_close_descriptor(wal_fd) != 0 || !rbt_file_descriptor_size(file->fd, &data_size)) {
        free(wal);
        return -EIO;
    }
    result = rbt_file_validate_wal(file, wal, (size_t)wal_file_size, data_size, &final_count);
    if (result != 0) {
        free(wal);
        return result;
    }
    record_count = rbt_load_u64(wal + RBT_WAL_RECORD_COUNT_OFFSET);
    for (index = 0u; index < (size_t)record_count; ++index) {
        size_t offset = RBT_WAL_HEADER_SIZE + index * record_size;
        uint64_t page_id = rbt_load_u64(wal + offset);
        off_t page_offset;

        if (!rbt_file_page_offset(page_id, file->page_size, &page_offset) ||
            !rbt_file_write_exact(file->fd, wal + offset + RBT_WAL_PAGE_ID_SIZE,
                                  (size_t)file->page_size, page_offset)) {
            free(wal);
            return -EOWNERDEAD;
        }
        if (index == 0u) {
            rbt_file_crash("recovery_first_page_written");
        }
    }
    {
        uint64_t final_size;

        if (!rbt_file_physical_size(final_count, file->page_size, &final_size) ||
            !rbt_file_truncate(file->fd, (off_t)final_size) || !rbt_file_sync_regular(file->fd)) {
            free(wal);
            return -EOWNERDEAD;
        }
    }
    rbt_file_crash("recovery_data_synced");
    free(wal);
    if (!rbt_file_unlink(file->parent_fd, file->wal_name) ||
        !rbt_file_sync_directory(file->parent_fd)) {
        return -EOWNERDEAD;
    }
    file->page_count = final_count;
    return 0;
}

static int rbt_file_page_count(void *context, uint64_t *out_count) {
    struct rbt_file *file = context;
    uint64_t measured;
    int result;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (file == NULL || out_count == NULL) {
        return -EINVAL;
    }
    if (file->poisoned) {
        return -EOWNERDEAD;
    }
    result = rbt_file_measure(file, &measured);
    if (result != 0) {
        return result;
    }
    if (measured != file->page_count) {
        return -EBADMSG;
    }
    *out_count = measured;
    return 0;
}

static int rbt_file_read_page(void *context, uint64_t page_id, void *data_out) {
    struct rbt_file *file = context;
    off_t offset;

    if (file == NULL || data_out == NULL) {
        return -EINVAL;
    }
    if (file->poisoned) {
        return -EOWNERDEAD;
    }
    if (page_id >= file->page_count || !rbt_file_page_offset(page_id, file->page_size, &offset)) {
        return -EIO;
    }
    return rbt_file_read_exact(file->fd, data_out, (size_t)file->page_size, offset) ? 0 : -EIO;
}

static int rbt_file_commit_pages(void *context, const struct rbt_page_update *updates,
                                 size_t update_count) {
    struct rbt_file *file = context;
    uint64_t final_count;
    uint64_t measured;
    unsigned char *wal = NULL;
    size_t wal_size = 0u;
    size_t index;
    int wal_fd;
    int result;

    if (file == NULL) {
        return -EINVAL;
    }
    if (file->poisoned) {
        return -EOWNERDEAD;
    }
    result = rbt_file_validate_updates(file, updates, update_count, &final_count);
    if (result != 0 || update_count == 0u) {
        return result;
    }
    result = rbt_file_measure(file, &measured);
    if (result != 0 || measured != file->page_count) {
        return result != 0 ? result : -EBADMSG;
    }
    result = rbt_file_build_wal(file, updates, update_count, final_count, &wal, &wal_size);
    if (result != 0) {
        return result;
    }
    result = rbt_file_absent(file->parent_fd, file->wal_name);
    if (result != 0) {
        free(wal);
        return result == -EBUSY ? -EOWNERDEAD : result;
    }
    result = rbt_file_remove_temp(file);
    if (result != 0) {
        free(wal);
        return result;
    }
    wal_fd = rbt_file_create_at(file->parent_fd, file->wal_temp_name, O_WRONLY);
    if (wal_fd < 0) {
        free(wal);
        return -EIO;
    }
    rbt_file_crash("wal_created");
    if (!rbt_file_write_exact(wal_fd, wal, wal_size, 0) || !rbt_file_sync_regular(wal_fd) ||
        rbt_file_close_descriptor(wal_fd) != 0) {
        free(wal);
        (void)rbt_file_unlink(file->parent_fd, file->wal_temp_name);
        return -EIO;
    }
    rbt_file_crash("wal_temp_synced");
    if (!rbt_file_rename(file->parent_fd, file->wal_temp_name, file->wal_name)) {
        free(wal);
        (void)rbt_file_unlink(file->parent_fd, file->wal_temp_name);
        return -EIO;
    }
    file->poisoned = true;
    if (!rbt_file_sync_directory(file->parent_fd)) {
        free(wal);
        return -EOWNERDEAD;
    }
    rbt_file_crash("wal_synced");
    for (index = 0u; index < update_count; ++index) {
        off_t offset;

        if (!rbt_file_page_offset(updates[index].page_id, file->page_size, &offset) ||
            !rbt_file_write_exact(file->fd, updates[index].data, (size_t)file->page_size, offset)) {
            free(wal);
            return -EOWNERDEAD;
        }
        if (index == 0u) {
            rbt_file_crash("first_page_written");
        }
    }
    {
        uint64_t final_size;

        if (!rbt_file_physical_size(final_count, file->page_size, &final_size) ||
            !rbt_file_truncate(file->fd, (off_t)final_size) || !rbt_file_sync_regular(file->fd)) {
            free(wal);
            return -EOWNERDEAD;
        }
    }
    rbt_file_crash("data_synced");
    free(wal);
    if (!rbt_file_unlink(file->parent_fd, file->wal_name) ||
        !rbt_file_sync_directory(file->parent_fd)) {
        return -EOWNERDEAD;
    }
    file->page_count = final_count;
    file->poisoned = false;
    return 0;
}

static int rbt_file_allocate(const char *path, struct rbt_file **out_file, char **out_parent) {
    struct rbt_file *file;
    int result;

    file = calloc(1u, sizeof(*file));
    if (file == NULL) {
        return -ENOMEM;
    }
    file->fd = -1;
    file->parent_fd = -1;
    result = rbt_file_build_names(path, &file->data_name, &file->wal_name, &file->wal_temp_name,
                                  out_parent);
    if (result != 0) {
        rbt_file_release(file);
        return result;
    }
    file->parent_fd = rbt_file_open_path(*out_parent, O_RDONLY);
    if (file->parent_fd < 0) {
        rbt_file_release(file);
        return -EIO;
    }
    {
        struct stat status;

        if (!rbt_file_stat(file->parent_fd, &status) || !S_ISDIR(status.st_mode)) {
            rbt_file_release(file);
            return -EIO;
        }
    }
    *out_file = file;
    return 0;
}

int rbt_file_create(const char *path, uint32_t page_size, struct rbt_file **out_file) {
    struct rbt_file *file = NULL;
    char *parent = NULL;
    unsigned char *header = NULL;
    int random_fd = -1;
    int result;

    if (out_file != NULL) {
        *out_file = NULL;
    }
    if (out_file == NULL || path == NULL || !rbt_page_size_valid(page_size)) {
        return -EINVAL;
    }
    result = rbt_file_allocate(path, &file, &parent);
    free(parent);
    if (result != 0) {
        return result;
    }
    file->page_size = page_size;
    file->fd = rbt_file_create_at(file->parent_fd, file->data_name, O_RDWR);
    if (file->fd < 0) {
        result = errno == EEXIST ? -EBUSY : -EIO;
        rbt_file_release(file);
        return result;
    }
    result = rbt_file_lock(file->fd);
    if (result == 0 && rbt_file_absent(file->parent_fd, file->wal_name) != 0) {
        result = -EBUSY;
    }
    if (result == 0) {
        result = rbt_file_remove_temp(file);
    }
    random_fd = rbt_file_open_path("/dev/urandom", O_RDONLY);
    if (result == 0 &&
        (random_fd < 0 || !rbt_file_read_exact(random_fd, file->uuid, RBT_FILE_UUID_SIZE, 0))) {
        result = -EIO;
    }
    if (random_fd >= 0 && rbt_file_close_descriptor(random_fd) != 0 && result == 0) {
        result = -EIO;
    }
    header = malloc((size_t)page_size);
    if (header == NULL && result == 0) {
        result = -ENOMEM;
    }
    if (result == 0) {
        rbt_file_header_encode(header, page_size, file->uuid);
        if (!rbt_file_make_private(file->fd) ||
            !rbt_file_write_exact(file->fd, header, (size_t)page_size, 0) ||
            !rbt_file_sync_regular(file->fd) || !rbt_file_sync_directory(file->parent_fd)) {
            result = -EIO;
        }
    }
    free(header);
    if (result != 0) {
        (void)rbt_file_close_descriptor(file->fd);
        file->fd = -1;
        (void)rbt_file_unlink(file->parent_fd, file->data_name);
        (void)rbt_file_sync_directory(file->parent_fd);
        rbt_file_release(file);
        return result;
    }
    *out_file = file;
    return 0;
}

int rbt_file_open(const char *path, struct rbt_file **out_file) {
    struct rbt_file *file = NULL;
    char *parent = NULL;
    int result;

    if (out_file != NULL) {
        *out_file = NULL;
    }
    if (out_file == NULL || path == NULL) {
        return -EINVAL;
    }
    result = rbt_file_allocate(path, &file, &parent);
    free(parent);
    if (result != 0) {
        return result;
    }
    file->fd = rbt_file_open_at(file->parent_fd, file->data_name, O_RDWR);
    if (file->fd < 0) {
        rbt_file_release(file);
        return -EIO;
    }
    {
        struct stat status;

        if (!rbt_file_stat(file->fd, &status) || !S_ISREG(status.st_mode)) {
            rbt_file_release(file);
            return -EIO;
        }
    }
    result = rbt_file_lock(file->fd);
    if (result == 0) {
        result = rbt_file_read_header(file);
    }
    if (result == 0) {
        result = rbt_file_remove_temp(file);
    }
    if (result == 0) {
        result = rbt_file_recover(file);
    }
    if (result != 0) {
        rbt_file_release(file);
        return result;
    }
    *out_file = file;
    return 0;
}

int rbt_file_close(struct rbt_file *file) {
    bool failed = false;

    if (file == NULL) {
        return -EINVAL;
    }
    if (file->fd >= 0 && rbt_file_close_descriptor(file->fd) != 0) {
        failed = true;
    }
    file->fd = -1;
    if (file->parent_fd >= 0 && rbt_file_close_descriptor(file->parent_fd) != 0) {
        failed = true;
    }
    file->parent_fd = -1;
    rbt_file_release(file);
    return failed ? -EIO : 0;
}

int rbt_file_storage(struct rbt_file *file, struct rbt_storage *out_storage) {
    if (out_storage != NULL) {
        memset(out_storage, 0, sizeof(*out_storage));
    }
    if (file == NULL || out_storage == NULL) {
        return -EINVAL;
    }
    out_storage->context = file;
    out_storage->page_size = file->page_size;
    out_storage->read_page = rbt_file_read_page;
    out_storage->page_count = rbt_file_page_count;
    out_storage->commit_pages = rbt_file_commit_pages;
    return 0;
}
