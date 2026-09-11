#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "bptree_backends.h"
#include "bptree_internal.h"

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
    BPT_FILE_MAGIC_SIZE = 8,
    BPT_FILE_UUID_SIZE = 16,
    BPT_FILE_VERSION_OFFSET = 8,
    BPT_FILE_TREE_VERSION_OFFSET = 12,
    BPT_FILE_PAGE_SIZE_OFFSET = 16,
    BPT_FILE_FLAGS_OFFSET = 20,
    BPT_FILE_UUID_OFFSET = 24,
    BPT_FILE_CHECKSUM_OFFSET = 40,
    BPT_FILE_HEADER_USED_SIZE = 44,

    BPT_WAL_MAGIC_SIZE = 8,
    BPT_WAL_VERSION_OFFSET = 8,
    BPT_WAL_FILE_VERSION_OFFSET = 12,
    BPT_WAL_TREE_VERSION_OFFSET = 16,
    BPT_WAL_PAGE_SIZE_OFFSET = 20,
    BPT_WAL_FLAGS_OFFSET = 24,
    BPT_WAL_HEADER_SIZE_OFFSET = 28,
    BPT_WAL_UUID_OFFSET = 32,
    BPT_WAL_RECORD_COUNT_OFFSET = 48,
    BPT_WAL_ORIGINAL_PAGE_COUNT_OFFSET = 56,
    BPT_WAL_FINAL_PAGE_COUNT_OFFSET = 64,
    BPT_WAL_TOTAL_SIZE_OFFSET = 72,
    BPT_WAL_CHECKSUM_OFFSET = 80,
    BPT_WAL_HEADER_SIZE = 96,
    BPT_WAL_PAGE_ID_SIZE = 8,
    BPT_WAL_MAX_TRANSACTION_PAGES = 1024
};

#define BPT_FILE_FORMAT_VERSION UINT32_C(1)
#define BPT_WAL_FORMAT_VERSION UINT32_C(1)

static const unsigned char bpt_file_magic[BPT_FILE_MAGIC_SIZE] = {'B', 'P', 'T', 'D',
                                                                  'B', '0', '0', '1'};
static const unsigned char bpt_wal_magic[BPT_WAL_MAGIC_SIZE] = {'B', 'P', 'T', 'W',
                                                                'A', 'L', '0', '1'};

struct bpt_file_backend {
    int fd;
    int parent_fd;
    uint32_t page_size;
    uint64_t page_count;
    unsigned char uuid[BPT_FILE_UUID_SIZE];
    char *wal_name;
    char *wal_temp_name;
    bool recovery_required;
};

static int file_make_cloexec(int fd) {
    int descriptor_flags;
    int result;
    int saved_errno;

    if (fd < 0) {
        return fd;
    }
    do {
        descriptor_flags = fcntl(fd, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) == 0) {
        do {
            result = fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
        } while (result != 0 && errno == EINTR);
        if (result == 0) {
            return fd;
        }
    } else if (descriptor_flags >= 0) {
        return fd;
    }

    saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return -1;
}

static int file_open_existing(const char *path, int flags) {
    int fd;

    do {
        fd = open(path, flags | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return file_make_cloexec(fd);
}

static int file_open_exclusive(const char *path, int flags) {
    int fd;

    do {
        fd = open(path, flags | O_CLOEXEC | O_CREAT | O_EXCL, (mode_t)(S_IRUSR | S_IWUSR));
    } while (fd < 0 && errno == EINTR);
    return file_make_cloexec(fd);
}

static int file_open_existing_at(int parent_fd, const char *name, int flags) {
    int fd;

    do {
        fd = openat(parent_fd, name, flags | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    return file_make_cloexec(fd);
}

static int file_open_exclusive_at(int parent_fd, const char *name, int flags) {
    int fd;

    do {
        fd = openat(parent_fd, name, flags | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_EXCL,
                    (mode_t)(S_IRUSR | S_IWUSR));
    } while (fd < 0 && errno == EINTR);
    return file_make_cloexec(fd);
}

static bool file_fstat(int fd, struct stat *status_out) {
    int result;

    do {
        result = fstat(fd, status_out);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_sync_directory(int fd) {
    int result;

    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_sync_regular(int fd) {
    int result;

    do {
#ifdef __APPLE__
        result = fcntl(fd, F_FULLFSYNC);
#else
        result = fsync(fd);
#endif
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_ftruncate(int fd, off_t size) {
    int result;

    do {
        result = ftruncate(fd, size);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_fchmod_private(int fd) {
    int result;

    do {
        result = fchmod(fd, (mode_t)(S_IRUSR | S_IWUSR));
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_unlink(const char *path) {
    int result;

    do {
        result = unlink(path);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_unlink_at(int parent_fd, const char *name) {
    int result;

    do {
        result = unlinkat(parent_fd, name, 0);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_rename_at(int parent_fd, const char *old_name, const char *new_name) {
    int result;

    do {
        result = renameat(parent_fd, old_name, parent_fd, new_name);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool file_read_exact_at(int fd, void *data, size_t size, off_t offset) {
    unsigned char *bytes = data;
    size_t completed = 0u;

    while (completed < size) {
        size_t remaining = size - completed;
        size_t request = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
        ssize_t result = pread(fd, bytes + completed, request, offset + (off_t)completed);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            errno = EIO;
            return false;
        }
        completed += (size_t)result;
    }
    return true;
}

static bool file_write_exact_at(int fd, const void *data, size_t size, off_t offset) {
    const unsigned char *bytes = data;
    size_t completed = 0u;

    while (completed < size) {
        size_t remaining = size - completed;
        size_t request = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
        ssize_t result = pwrite(fd, bytes + completed, request, offset + (off_t)completed);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            errno = EIO;
            return false;
        }
        completed += (size_t)result;
    }
    return true;
}

static bool file_size_for_page_count(uint64_t page_count, uint32_t page_size, uint64_t *size_out) {
    uint64_t physical_pages;
    uint64_t size;
    off_t converted;

    if (page_count == UINT64_MAX) {
        return false;
    }
    physical_pages = page_count + 1u;
    if (physical_pages > UINT64_MAX / page_size) {
        return false;
    }
    size = physical_pages * page_size;
    converted = (off_t)size;
    if (converted < 0 || (uint64_t)converted != size) {
        return false;
    }
    *size_out = size;
    return true;
}

static bool file_page_offset(uint64_t page_id, uint32_t page_size, off_t *offset_out) {
    uint64_t offset;

    if (!file_size_for_page_count(page_id, page_size, &offset)) {
        return false;
    }
    *offset_out = (off_t)offset;
    return true;
}

static bool file_descriptor_size(int fd, uint64_t *size_out) {
    struct stat status;

    if (!file_fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return false;
    }
    *size_out = (uint64_t)status.st_size;
    return (off_t)*size_out == status.st_size;
}

static bpt_status file_lock_exclusive(int fd) {
    int result;

    do {
        result = flock(fd, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return BPT_OK;
    }
    return errno == EACCES || errno == EAGAIN ? BPT_BUSY : BPT_IO;
}

static bpt_status file_build_paths(const char *path, char **wal_name_out, char **wal_temp_name_out,
                                   char **parent_path_out) {
    const char *slash;
    const char *base_name;
    size_t path_length;
    size_t base_length;
    size_t parent_length;
    char *wal_name;
    char *wal_temp_name;
    char *parent_path;

    if (path == NULL || path[0] == '\0' || wal_name_out == NULL || wal_temp_name_out == NULL ||
        parent_path_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    path_length = strlen(path);
    if (path[path_length - 1u] == '/') {
        return BPT_INVALID_ARGUMENT;
    }
    slash = strrchr(path, '/');
    base_name = slash == NULL ? path : slash + 1;
    base_length = strlen(base_name);
    if (base_length > SIZE_MAX - 9u) {
        return BPT_INVALID_ARGUMENT;
    }
    wal_name = malloc(base_length + 5u);
    if (wal_name == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    memcpy(wal_name, base_name, base_length);
    memcpy(wal_name + base_length, ".wal", 5u);
    wal_temp_name = malloc(base_length + 9u);
    if (wal_temp_name == NULL) {
        free(wal_name);
        return BPT_OUT_OF_MEMORY;
    }
    memcpy(wal_temp_name, base_name, base_length);
    memcpy(wal_temp_name + base_length, ".wal.tmp", 9u);

    if (slash == NULL) {
        parent_path = malloc(2u);
        if (parent_path != NULL) {
            parent_path[0] = '.';
            parent_path[1] = '\0';
        }
    } else if (slash == path) {
        parent_path = malloc(2u);
        if (parent_path != NULL) {
            parent_path[0] = '/';
            parent_path[1] = '\0';
        }
    } else {
        parent_length = (size_t)(slash - path);
        parent_path = malloc(parent_length + 1u);
        if (parent_path != NULL) {
            memcpy(parent_path, path, parent_length);
            parent_path[parent_length] = '\0';
        }
    }
    if (parent_path == NULL) {
        free(wal_temp_name);
        free(wal_name);
        return BPT_OUT_OF_MEMORY;
    }

    *wal_name_out = wal_name;
    *wal_temp_name_out = wal_temp_name;
    *parent_path_out = parent_path;
    return BPT_OK;
}

static bpt_status file_open_parent(const char *parent_path, int *fd_out) {
    struct stat status;
    int fd = file_open_existing(parent_path, O_RDONLY);

    if (fd < 0) {
        return BPT_IO;
    }
    if (!file_fstat(fd, &status) || !S_ISDIR(status.st_mode)) {
        (void)close(fd);
        return BPT_IO;
    }
    *fd_out = fd;
    return BPT_OK;
}

static bpt_status file_require_absent_at(int parent_fd, const char *name) {
    struct stat status;
    int result;
    int flags = 0;

#ifdef AT_SYMLINK_NOFOLLOW
    flags = AT_SYMLINK_NOFOLLOW;
#endif
    do {
        result = fstatat(parent_fd, name, &status, flags);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return BPT_BUSY;
    }
    return errno == ENOENT ? BPT_OK : BPT_IO;
}

static bpt_status file_remove_stale_wal_temp(bpt_file_backend *backend) {
    bpt_status status = file_require_absent_at(backend->parent_fd, backend->wal_temp_name);

    if (status == BPT_OK) {
        return BPT_OK;
    }
    if (status != BPT_BUSY) {
        return status;
    }
    if (!file_unlink_at(backend->parent_fd, backend->wal_temp_name) ||
        !file_sync_directory(backend->parent_fd)) {
        return BPT_IO;
    }
    return BPT_OK;
}

static bpt_status file_random_uuid(unsigned char uuid[BPT_FILE_UUID_SIZE]) {
    int fd = file_open_existing("/dev/urandom", O_RDONLY);

    if (fd < 0) {
        return BPT_IO;
    }
    if (!file_read_exact_at(fd, uuid, BPT_FILE_UUID_SIZE, 0)) {
        (void)close(fd);
        return BPT_IO;
    }
    if (close(fd) != 0) {
        return BPT_IO;
    }
    return BPT_OK;
}

static uint32_t file_checksum(unsigned char *data, size_t size, size_t checksum_offset) {
    uint32_t checksum;

    bpt_store_u32(data + checksum_offset, 0u);
    checksum = bpt_crc32c(data, size);
    bpt_store_u32(data + checksum_offset, checksum);
    return checksum;
}

static void file_header_encode(unsigned char *header, uint32_t page_size,
                               const unsigned char uuid[BPT_FILE_UUID_SIZE]) {
    memset(header, 0, (size_t)page_size);
    memcpy(header, bpt_file_magic, BPT_FILE_MAGIC_SIZE);
    bpt_store_u32(header + BPT_FILE_VERSION_OFFSET, BPT_FILE_FORMAT_VERSION);
    bpt_store_u32(header + BPT_FILE_TREE_VERSION_OFFSET, BPTREE_FORMAT_VERSION);
    bpt_store_u32(header + BPT_FILE_PAGE_SIZE_OFFSET, page_size);
    bpt_store_u32(header + BPT_FILE_FLAGS_OFFSET, 0u);
    memcpy(header + BPT_FILE_UUID_OFFSET, uuid, BPT_FILE_UUID_SIZE);
    (void)file_checksum(header, (size_t)page_size, BPT_FILE_CHECKSUM_OFFSET);
}

static bpt_status file_header_validate(unsigned char *header, uint32_t expected_page_size,
                                       unsigned char uuid_out[BPT_FILE_UUID_SIZE]) {
    uint32_t stored_checksum;
    uint32_t actual_checksum;
    size_t index;

    if (memcmp(header, bpt_file_magic, BPT_FILE_MAGIC_SIZE) != 0) {
        return BPT_CORRUPT;
    }
    stored_checksum = bpt_load_u32(header + BPT_FILE_CHECKSUM_OFFSET);
    actual_checksum = file_checksum(header, (size_t)expected_page_size, BPT_FILE_CHECKSUM_OFFSET);
    if (stored_checksum != actual_checksum) {
        return BPT_CORRUPT;
    }
    if (bpt_load_u32(header + BPT_FILE_VERSION_OFFSET) != BPT_FILE_FORMAT_VERSION ||
        bpt_load_u32(header + BPT_FILE_TREE_VERSION_OFFSET) != BPTREE_FORMAT_VERSION ||
        bpt_load_u32(header + BPT_FILE_FLAGS_OFFSET) != 0u) {
        return BPT_UNSUPPORTED;
    }
    if (bpt_load_u32(header + BPT_FILE_PAGE_SIZE_OFFSET) != expected_page_size) {
        return BPT_CORRUPT;
    }
    for (index = BPT_FILE_HEADER_USED_SIZE; index < (size_t)expected_page_size; ++index) {
        if (header[index] != 0u) {
            return BPT_UNSUPPORTED;
        }
    }
    memcpy(uuid_out, header + BPT_FILE_UUID_OFFSET, BPT_FILE_UUID_SIZE);
    return BPT_OK;
}

static bpt_status file_read_header(int fd, uint32_t *page_size_out,
                                   unsigned char uuid_out[BPT_FILE_UUID_SIZE]) {
    unsigned char prefix[BPT_FILE_HEADER_USED_SIZE];
    unsigned char *header;
    uint64_t file_size;
    uint32_t page_size;
    bpt_status status;

    if (!file_descriptor_size(fd, &file_size)) {
        return BPT_IO;
    }
    if (file_size < BPT_FILE_HEADER_USED_SIZE) {
        return BPT_CORRUPT;
    }
    if (!file_read_exact_at(fd, prefix, sizeof(prefix), 0)) {
        return BPT_IO;
    }
    if (memcmp(prefix, bpt_file_magic, BPT_FILE_MAGIC_SIZE) != 0) {
        return BPT_CORRUPT;
    }
    page_size = bpt_load_u32(prefix + BPT_FILE_PAGE_SIZE_OFFSET);
    if (!bpt_page_size_valid(page_size) || file_size < page_size) {
        return BPT_CORRUPT;
    }
    header = malloc((size_t)page_size);
    if (header == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    if (!file_read_exact_at(fd, header, (size_t)page_size, 0)) {
        free(header);
        return BPT_IO;
    }
    status = file_header_validate(header, page_size, uuid_out);
    free(header);
    if (status == BPT_OK) {
        *page_size_out = page_size;
    }
    return status;
}

static bpt_status file_measure_page_count(const bpt_file_backend *backend,
                                          uint64_t *page_count_out) {
    uint64_t file_size;

    if (!file_descriptor_size(backend->fd, &file_size)) {
        return BPT_IO;
    }
    if (file_size < backend->page_size || (file_size % backend->page_size) != 0u) {
        return BPT_CORRUPT;
    }
    *page_count_out = file_size / backend->page_size - UINT64_C(1);
    return BPT_OK;
}

static bpt_status file_refresh_page_count(bpt_file_backend *backend) {
    uint64_t page_count;
    bpt_status status = file_measure_page_count(backend, &page_count);

    if (status != BPT_OK) {
        return status;
    }
    backend->page_count = page_count;
    return BPT_OK;
}

static void file_test_crash(const char *point) {
#ifdef BPTREE_ENABLE_TEST_HOOKS
    const char *configured = getenv("BPTREE_TEST_CRASH_POINT");

    if (configured != NULL && strcmp(configured, point) == 0) {
        _exit(86);
    }
#else
    (void)point;
#endif
}

static bpt_status file_validate_updates(const bpt_file_backend *backend,
                                        const bpt_page_update *updates, size_t update_count,
                                        uint64_t *final_page_count_out) {
    uint64_t final_page_count = backend->page_count;
    uint64_t new_id_count = 0u;
    size_t index;

    if (update_count != 0u && updates == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (update_count > (size_t)BPT_WAL_MAX_TRANSACTION_PAGES) {
        return BPT_OUT_OF_MEMORY;
    }
    for (index = 0u; index < update_count; ++index) {
        size_t other;
        off_t offset;

        if (updates[index].data == NULL) {
            return BPT_INVALID_ARGUMENT;
        }
        if (!file_page_offset(updates[index].page_id, backend->page_size, &offset)) {
            return BPT_OUT_OF_MEMORY;
        }
        for (other = 0u; other < index; ++other) {
            if (updates[other].page_id == updates[index].page_id) {
                return BPT_INVALID_ARGUMENT;
            }
        }
        if (updates[index].page_id >= backend->page_count) {
            new_id_count++;
        }
        if (updates[index].page_id + 1u > final_page_count) {
            final_page_count = updates[index].page_id + 1u;
        }
    }
    if (final_page_count - backend->page_count != new_id_count) {
        return BPT_INVALID_ARGUMENT;
    }
    {
        uint64_t final_size;

        if (!file_size_for_page_count(final_page_count, backend->page_size, &final_size)) {
            return BPT_OUT_OF_MEMORY;
        }
    }
    *final_page_count_out = final_page_count;
    return BPT_OK;
}

static bpt_status file_build_wal(const bpt_file_backend *backend, const bpt_page_update *updates,
                                 size_t update_count, uint64_t final_page_count,
                                 unsigned char **wal_out, size_t *wal_size_out) {
    size_t record_size = BPT_WAL_PAGE_ID_SIZE + (size_t)backend->page_size;
    size_t wal_size;
    unsigned char *wal;
    size_t index;

    if (update_count > (size_t)BPT_WAL_MAX_TRANSACTION_PAGES ||
        update_count > (SIZE_MAX - BPT_WAL_HEADER_SIZE) / record_size) {
        return BPT_OUT_OF_MEMORY;
    }
    wal_size = BPT_WAL_HEADER_SIZE + update_count * record_size;
    {
        off_t converted = (off_t)wal_size;

        if (converted < 0 || (size_t)converted != wal_size) {
            return BPT_OUT_OF_MEMORY;
        }
    }
    wal = calloc(1u, wal_size);
    if (wal == NULL) {
        return BPT_OUT_OF_MEMORY;
    }

    memcpy(wal, bpt_wal_magic, BPT_WAL_MAGIC_SIZE);
    bpt_store_u32(wal + BPT_WAL_VERSION_OFFSET, BPT_WAL_FORMAT_VERSION);
    bpt_store_u32(wal + BPT_WAL_FILE_VERSION_OFFSET, BPT_FILE_FORMAT_VERSION);
    bpt_store_u32(wal + BPT_WAL_TREE_VERSION_OFFSET, BPTREE_FORMAT_VERSION);
    bpt_store_u32(wal + BPT_WAL_PAGE_SIZE_OFFSET, backend->page_size);
    bpt_store_u32(wal + BPT_WAL_FLAGS_OFFSET, 0u);
    bpt_store_u32(wal + BPT_WAL_HEADER_SIZE_OFFSET, BPT_WAL_HEADER_SIZE);
    memcpy(wal + BPT_WAL_UUID_OFFSET, backend->uuid, BPT_FILE_UUID_SIZE);
    bpt_store_u64(wal + BPT_WAL_RECORD_COUNT_OFFSET, (uint64_t)update_count);
    bpt_store_u64(wal + BPT_WAL_ORIGINAL_PAGE_COUNT_OFFSET, backend->page_count);
    bpt_store_u64(wal + BPT_WAL_FINAL_PAGE_COUNT_OFFSET, final_page_count);
    bpt_store_u64(wal + BPT_WAL_TOTAL_SIZE_OFFSET, (uint64_t)wal_size);
    for (index = 0u; index < update_count; ++index) {
        size_t offset = BPT_WAL_HEADER_SIZE + index * record_size;

        bpt_store_u64(wal + offset, updates[index].page_id);
        memcpy(wal + offset + BPT_WAL_PAGE_ID_SIZE, updates[index].data,
               (size_t)backend->page_size);
    }
    (void)file_checksum(wal, wal_size, BPT_WAL_CHECKSUM_OFFSET);
    *wal_out = wal;
    *wal_size_out = wal_size;
    return BPT_OK;
}

static bpt_status file_wal_preflight(const bpt_file_backend *backend,
                                     const unsigned char header[BPT_WAL_HEADER_SIZE],
                                     uint64_t wal_file_size, size_t *wal_size_out) {
    uint64_t record_count;
    uint64_t total_size;
    size_t record_size = BPT_WAL_PAGE_ID_SIZE + (size_t)backend->page_size;
    size_t expected_size;

    if (wal_file_size < BPT_WAL_HEADER_SIZE || wal_file_size > (uint64_t)SIZE_MAX ||
        memcmp(header, bpt_wal_magic, BPT_WAL_MAGIC_SIZE) != 0 ||
        bpt_load_u32(header + BPT_WAL_PAGE_SIZE_OFFSET) != backend->page_size ||
        bpt_load_u32(header + BPT_WAL_HEADER_SIZE_OFFSET) != BPT_WAL_HEADER_SIZE) {
        return BPT_CORRUPT;
    }

    record_count = bpt_load_u64(header + BPT_WAL_RECORD_COUNT_OFFSET);
    total_size = bpt_load_u64(header + BPT_WAL_TOTAL_SIZE_OFFSET);
    if (record_count == 0u || record_count > BPT_WAL_MAX_TRANSACTION_PAGES ||
        record_count > (uint64_t)SIZE_MAX ||
        (size_t)record_count > (SIZE_MAX - BPT_WAL_HEADER_SIZE) / record_size) {
        return BPT_CORRUPT;
    }
    expected_size = BPT_WAL_HEADER_SIZE + (size_t)record_count * record_size;
    if (total_size != wal_file_size || expected_size != (size_t)wal_file_size) {
        return BPT_CORRUPT;
    }

    *wal_size_out = expected_size;
    return BPT_OK;
}

static bpt_status file_wal_validate(bpt_file_backend *backend, unsigned char *wal, size_t wal_size,
                                    uint64_t data_file_size, uint64_t *final_page_count_out) {
    uint64_t record_count;
    uint64_t original_page_count;
    uint64_t final_page_count;
    uint64_t original_file_size;
    uint64_t final_file_size;
    uint64_t maximum_page_count;
    uint64_t new_id_count = 0u;
    size_t record_size = BPT_WAL_PAGE_ID_SIZE + (size_t)backend->page_size;
    size_t expected_size;
    size_t index;
    uint32_t stored_checksum;
    uint32_t actual_checksum;

    if (wal_size < BPT_WAL_HEADER_SIZE || memcmp(wal, bpt_wal_magic, BPT_WAL_MAGIC_SIZE) != 0 ||
        bpt_load_u32(wal + BPT_WAL_PAGE_SIZE_OFFSET) != backend->page_size ||
        bpt_load_u32(wal + BPT_WAL_HEADER_SIZE_OFFSET) != BPT_WAL_HEADER_SIZE) {
        return BPT_CORRUPT;
    }

    record_count = bpt_load_u64(wal + BPT_WAL_RECORD_COUNT_OFFSET);
    original_page_count = bpt_load_u64(wal + BPT_WAL_ORIGINAL_PAGE_COUNT_OFFSET);
    final_page_count = bpt_load_u64(wal + BPT_WAL_FINAL_PAGE_COUNT_OFFSET);
    if (record_count == 0u || record_count > BPT_WAL_MAX_TRANSACTION_PAGES ||
        record_count > (uint64_t)SIZE_MAX ||
        (size_t)record_count > (SIZE_MAX - BPT_WAL_HEADER_SIZE) / record_size) {
        return BPT_CORRUPT;
    }
    expected_size = BPT_WAL_HEADER_SIZE + (size_t)record_count * record_size;
    if (expected_size != wal_size ||
        bpt_load_u64(wal + BPT_WAL_TOTAL_SIZE_OFFSET) != (uint64_t)wal_size) {
        return BPT_CORRUPT;
    }

    stored_checksum = bpt_load_u32(wal + BPT_WAL_CHECKSUM_OFFSET);
    actual_checksum = file_checksum(wal, wal_size, BPT_WAL_CHECKSUM_OFFSET);
    if (stored_checksum != actual_checksum) {
        return BPT_CORRUPT;
    }
    if (memcmp(wal + BPT_WAL_UUID_OFFSET, backend->uuid, BPT_FILE_UUID_SIZE) != 0) {
        return BPT_CORRUPT;
    }
    if (bpt_load_u32(wal + BPT_WAL_VERSION_OFFSET) != BPT_WAL_FORMAT_VERSION ||
        bpt_load_u32(wal + BPT_WAL_FILE_VERSION_OFFSET) != BPT_FILE_FORMAT_VERSION ||
        bpt_load_u32(wal + BPT_WAL_TREE_VERSION_OFFSET) != BPTREE_FORMAT_VERSION ||
        bpt_load_u32(wal + BPT_WAL_FLAGS_OFFSET) != 0u) {
        return BPT_UNSUPPORTED;
    }
    for (index = BPT_WAL_CHECKSUM_OFFSET + sizeof(uint32_t); index < BPT_WAL_HEADER_SIZE; ++index) {
        if (wal[index] != 0u) {
            return BPT_CORRUPT;
        }
    }
    if (final_page_count < original_page_count ||
        !file_size_for_page_count(original_page_count, backend->page_size, &original_file_size) ||
        !file_size_for_page_count(final_page_count, backend->page_size, &final_file_size) ||
        data_file_size < original_file_size || data_file_size > final_file_size) {
        return BPT_CORRUPT;
    }

    maximum_page_count = original_page_count;
    for (index = 0u; index < (size_t)record_count; ++index) {
        size_t offset = BPT_WAL_HEADER_SIZE + index * record_size;
        uint64_t page_id = bpt_load_u64(wal + offset);
        size_t other;
        off_t page_offset;

        if (!file_page_offset(page_id, backend->page_size, &page_offset) ||
            page_id >= final_page_count) {
            return BPT_CORRUPT;
        }
        for (other = 0u; other < index; ++other) {
            size_t other_offset = BPT_WAL_HEADER_SIZE + other * record_size;

            if (bpt_load_u64(wal + other_offset) == page_id) {
                return BPT_CORRUPT;
            }
        }
        if (page_id >= original_page_count) {
            new_id_count++;
        }
        if (page_id + 1u > maximum_page_count) {
            maximum_page_count = page_id + 1u;
        }
    }
    if (maximum_page_count != final_page_count ||
        final_page_count - original_page_count != new_id_count) {
        return BPT_CORRUPT;
    }

    *final_page_count_out = final_page_count;
    return BPT_OK;
}

static bpt_status file_remove_wal(bpt_file_backend *backend) {
    if (!file_unlink_at(backend->parent_fd, backend->wal_name)) {
        return BPT_RECOVERY_REQUIRED;
    }
    if (!file_sync_directory(backend->parent_fd)) {
        return BPT_RECOVERY_REQUIRED;
    }
    return BPT_OK;
}

static bpt_status file_recover_wal(bpt_file_backend *backend) {
    unsigned char wal_header[BPT_WAL_HEADER_SIZE];
    uint64_t wal_file_size;
    uint64_t data_file_size;
    uint64_t final_page_count;
    unsigned char *wal;
    size_t wal_size;
    size_t record_size;
    uint64_t record_count;
    size_t index;
    int wal_fd;
    bpt_status status;

    wal_fd = file_open_existing_at(backend->parent_fd, backend->wal_name, O_RDONLY);
    if (wal_fd < 0) {
        if (errno == ENOENT) {
            return file_refresh_page_count(backend);
        }
        return BPT_IO;
    }
    if (!file_descriptor_size(wal_fd, &wal_file_size)) {
        (void)close(wal_fd);
        return BPT_CORRUPT;
    }
    if (wal_file_size == 0u) {
        if (close(wal_fd) != 0) {
            return BPT_IO;
        }
        status = file_remove_wal(backend);
        if (status != BPT_OK) {
            return status;
        }
        return file_refresh_page_count(backend);
    }
    if (wal_file_size < BPT_WAL_HEADER_SIZE) {
        (void)close(wal_fd);
        return BPT_CORRUPT;
    }
    if (!file_read_exact_at(wal_fd, wal_header, sizeof(wal_header), 0)) {
        (void)close(wal_fd);
        return BPT_IO;
    }
    status = file_wal_preflight(backend, wal_header, wal_file_size, &wal_size);
    if (status != BPT_OK) {
        (void)close(wal_fd);
        return status;
    }
    wal = malloc(wal_size);
    if (wal == NULL) {
        (void)close(wal_fd);
        return BPT_OUT_OF_MEMORY;
    }
    if (!file_read_exact_at(wal_fd, wal, wal_size, 0)) {
        free(wal);
        (void)close(wal_fd);
        return BPT_IO;
    }
    if (close(wal_fd) != 0) {
        free(wal);
        return BPT_IO;
    }
    if (!file_descriptor_size(backend->fd, &data_file_size)) {
        free(wal);
        return BPT_IO;
    }
    status = file_wal_validate(backend, wal, wal_size, data_file_size, &final_page_count);
    if (status != BPT_OK) {
        free(wal);
        return status;
    }

    record_count = bpt_load_u64(wal + BPT_WAL_RECORD_COUNT_OFFSET);
    record_size = BPT_WAL_PAGE_ID_SIZE + (size_t)backend->page_size;
    for (index = 0u; index < (size_t)record_count; ++index) {
        size_t record_offset = BPT_WAL_HEADER_SIZE + index * record_size;
        uint64_t page_id = bpt_load_u64(wal + record_offset);
        off_t page_offset;

        if (!file_page_offset(page_id, backend->page_size, &page_offset) ||
            !file_write_exact_at(backend->fd, wal + record_offset + BPT_WAL_PAGE_ID_SIZE,
                                 (size_t)backend->page_size, page_offset)) {
            free(wal);
            return BPT_RECOVERY_REQUIRED;
        }
        if (index == 0u) {
            file_test_crash("recovery_first_page_written");
        }
    }
    {
        uint64_t final_size;

        if (!file_size_for_page_count(final_page_count, backend->page_size, &final_size) ||
            !file_ftruncate(backend->fd, (off_t)final_size) || !file_sync_regular(backend->fd)) {
            free(wal);
            return BPT_RECOVERY_REQUIRED;
        }
    }
    file_test_crash("recovery_data_synced");
    free(wal);
    status = file_remove_wal(backend);
    if (status != BPT_OK) {
        return status;
    }
    backend->page_count = final_page_count;
    return BPT_OK;
}

static bpt_status file_page_count(void *context, uint64_t *count_out) {
    bpt_file_backend *backend = context;
    uint64_t measured_page_count;
    bpt_status status;

    if (backend == NULL || count_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (backend->recovery_required) {
        return BPT_RECOVERY_REQUIRED;
    }
    status = file_measure_page_count(backend, &measured_page_count);
    if (status != BPT_OK) {
        return status;
    }
    if (measured_page_count != backend->page_count) {
        return BPT_CORRUPT;
    }
    *count_out = backend->page_count;
    return BPT_OK;
}

static bpt_status file_read_page(void *context, uint64_t page_id, void *data_out) {
    bpt_file_backend *backend = context;
    off_t offset;

    if (backend == NULL || data_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (backend->recovery_required) {
        return BPT_RECOVERY_REQUIRED;
    }
    if (page_id >= backend->page_count) {
        return BPT_IO;
    }
    if (!file_page_offset(page_id, backend->page_size, &offset)) {
        return BPT_IO;
    }
    return file_read_exact_at(backend->fd, data_out, (size_t)backend->page_size, offset) ? BPT_OK
                                                                                         : BPT_IO;
}

static bpt_status file_commit_pages(void *context, const bpt_page_update *updates,
                                    size_t update_count) {
    bpt_file_backend *backend = context;
    unsigned char *wal = NULL;
    size_t wal_size = 0u;
    uint64_t final_page_count;
    uint64_t measured_page_count;
    size_t index;
    int wal_fd = -1;
    bpt_status status;

    if (backend == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (backend->recovery_required) {
        return BPT_RECOVERY_REQUIRED;
    }
    status = file_validate_updates(backend, updates, update_count, &final_page_count);
    if (status != BPT_OK || update_count == 0u) {
        return status;
    }
    status = file_measure_page_count(backend, &measured_page_count);
    if (status != BPT_OK) {
        return status;
    }
    if (measured_page_count != backend->page_count) {
        return BPT_CORRUPT;
    }
    status = file_build_wal(backend, updates, update_count, final_page_count, &wal, &wal_size);
    if (status != BPT_OK) {
        return status;
    }

    status = file_require_absent_at(backend->parent_fd, backend->wal_name);
    if (status != BPT_OK) {
        free(wal);
        if (status == BPT_BUSY) {
            backend->recovery_required = true;
            return BPT_RECOVERY_REQUIRED;
        }
        return status;
    }
    status = file_remove_stale_wal_temp(backend);
    if (status != BPT_OK) {
        free(wal);
        return status;
    }
    wal_fd = file_open_exclusive_at(backend->parent_fd, backend->wal_temp_name, O_WRONLY);
    if (wal_fd < 0) {
        free(wal);
        return BPT_IO;
    }
    file_test_crash("wal_created");
    if (!file_write_exact_at(wal_fd, wal, wal_size, 0) || !file_sync_regular(wal_fd)) {
        free(wal);
        (void)close(wal_fd);
        (void)file_unlink_at(backend->parent_fd, backend->wal_temp_name);
        return BPT_IO;
    }
    if (close(wal_fd) != 0) {
        free(wal);
        (void)file_unlink_at(backend->parent_fd, backend->wal_temp_name);
        return BPT_IO;
    }
    file_test_crash("wal_temp_synced");
    status = file_require_absent_at(backend->parent_fd, backend->wal_name);
    if (status != BPT_OK) {
        free(wal);
        (void)file_unlink_at(backend->parent_fd, backend->wal_temp_name);
        if (status == BPT_BUSY) {
            backend->recovery_required = true;
            return BPT_RECOVERY_REQUIRED;
        }
        return status;
    }
    if (!file_rename_at(backend->parent_fd, backend->wal_temp_name, backend->wal_name)) {
        free(wal);
        (void)file_unlink_at(backend->parent_fd, backend->wal_temp_name);
        return BPT_IO;
    }
    backend->recovery_required = true;
    if (!file_sync_directory(backend->parent_fd)) {
        free(wal);
        return BPT_RECOVERY_REQUIRED;
    }
    file_test_crash("wal_synced");

    for (index = 0u; index < update_count; ++index) {
        off_t page_offset;

        if (!file_page_offset(updates[index].page_id, backend->page_size, &page_offset) ||
            !file_write_exact_at(backend->fd, updates[index].data, (size_t)backend->page_size,
                                 page_offset)) {
            free(wal);
            return BPT_RECOVERY_REQUIRED;
        }
        if (index == 0u) {
            file_test_crash("first_page_written");
        }
    }
    {
        uint64_t final_size;

        if (!file_size_for_page_count(final_page_count, backend->page_size, &final_size) ||
            !file_ftruncate(backend->fd, (off_t)final_size) || !file_sync_regular(backend->fd)) {
            free(wal);
            return BPT_RECOVERY_REQUIRED;
        }
    }
    file_test_crash("data_synced");
    free(wal);
    status = file_remove_wal(backend);
    if (status != BPT_OK) {
        return status;
    }

    backend->page_count = final_page_count;
    backend->recovery_required = false;
    return BPT_OK;
}

static void file_backend_release(bpt_file_backend *backend) {
    if (backend == NULL) {
        return;
    }
    if (backend->fd >= 0) {
        (void)close(backend->fd);
    }
    if (backend->parent_fd >= 0) {
        (void)close(backend->parent_fd);
    }
    free(backend->wal_name);
    free(backend->wal_temp_name);
    free(backend);
}

bpt_status bpt_file_backend_create(const char *path, uint32_t page_size,
                                   bpt_file_backend **backend_out) {
    bpt_file_backend *backend;
    char *parent_path = NULL;
    unsigned char *header = NULL;
    bool created = false;
    bpt_status status;

    if (backend_out != NULL) {
        *backend_out = NULL;
    }
    if (backend_out == NULL || !bpt_page_size_valid(page_size)) {
        return BPT_INVALID_ARGUMENT;
    }
    backend = calloc(1u, sizeof(*backend));
    if (backend == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    backend->fd = -1;
    backend->parent_fd = -1;
    backend->page_size = page_size;
    status = file_build_paths(path, &backend->wal_name, &backend->wal_temp_name, &parent_path);
    if (status != BPT_OK) {
        file_backend_release(backend);
        return status;
    }
    status = file_open_parent(parent_path, &backend->parent_fd);
    free(parent_path);
    if (status != BPT_OK) {
        file_backend_release(backend);
        return status;
    }
    backend->fd = file_open_exclusive(path, O_RDWR);
    if (backend->fd < 0) {
        file_backend_release(backend);
        return errno == EEXIST ? BPT_BUSY : BPT_IO;
    }
    created = true;
    status = file_lock_exclusive(backend->fd);
    if (status == BPT_OK) {
        status = file_require_absent_at(backend->parent_fd, backend->wal_name);
    }
    if (status == BPT_OK) {
        status = file_remove_stale_wal_temp(backend);
    }
    if (status == BPT_OK && !file_fchmod_private(backend->fd)) {
        status = BPT_IO;
    }
    if (status == BPT_OK) {
        status = file_random_uuid(backend->uuid);
    }
    if (status == BPT_OK) {
        header = malloc((size_t)page_size);
        if (header == NULL) {
            status = BPT_OUT_OF_MEMORY;
        }
    }
    if (status == BPT_OK) {
        file_header_encode(header, page_size, backend->uuid);
        if (!file_write_exact_at(backend->fd, header, (size_t)page_size, 0) ||
            !file_sync_regular(backend->fd) || !file_sync_directory(backend->parent_fd)) {
            status = BPT_IO;
        }
    }
    free(header);
    if (status != BPT_OK) {
        if (backend->fd >= 0) {
            (void)close(backend->fd);
            backend->fd = -1;
        }
        if (created) {
            (void)file_unlink(path);
            (void)file_sync_directory(backend->parent_fd);
        }
        file_backend_release(backend);
        return status;
    }

    *backend_out = backend;
    return BPT_OK;
}

bpt_status bpt_file_backend_open(const char *path, bpt_file_backend **backend_out) {
    bpt_file_backend *backend;
    char *parent_path = NULL;
    bpt_status status;

    if (backend_out != NULL) {
        *backend_out = NULL;
    }
    if (backend_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    backend = calloc(1u, sizeof(*backend));
    if (backend == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    backend->fd = -1;
    backend->parent_fd = -1;
    status = file_build_paths(path, &backend->wal_name, &backend->wal_temp_name, &parent_path);
    if (status != BPT_OK) {
        file_backend_release(backend);
        return status;
    }
    status = file_open_parent(parent_path, &backend->parent_fd);
    free(parent_path);
    if (status != BPT_OK) {
        file_backend_release(backend);
        return status;
    }
    backend->fd = file_open_existing(path, O_RDWR);
    if (backend->fd < 0) {
        file_backend_release(backend);
        return BPT_IO;
    }
    status = file_lock_exclusive(backend->fd);
    if (status == BPT_OK) {
        status = file_read_header(backend->fd, &backend->page_size, backend->uuid);
    }
    if (status == BPT_OK) {
        status = file_remove_stale_wal_temp(backend);
    }
    if (status == BPT_OK) {
        status = file_recover_wal(backend);
    }
    if (status != BPT_OK) {
        file_backend_release(backend);
        return status;
    }

    *backend_out = backend;
    return BPT_OK;
}

bpt_status bpt_file_backend_close(bpt_file_backend *backend) {
    bool close_failed = false;

    if (backend == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (backend->fd >= 0 && close(backend->fd) != 0) {
        close_failed = true;
    }
    if (backend->parent_fd >= 0 && close(backend->parent_fd) != 0) {
        close_failed = true;
    }
    free(backend->wal_name);
    free(backend->wal_temp_name);
    free(backend);
    return close_failed ? BPT_IO : BPT_OK;
}

bpt_storage bpt_file_backend_storage(bpt_file_backend *backend) {
    bpt_storage storage;

    memset(&storage, 0, sizeof(storage));
    if (backend == NULL) {
        return storage;
    }
    storage.context = backend;
    storage.page_size = backend->page_size;
    storage.read_page = file_read_page;
    storage.page_count = file_page_count;
    storage.commit_pages = file_commit_pages;
    return storage;
}
