#include "bptree_backends.h"
#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define TEST_PAGE_SIZE UINT32_C(512)
#define TEST_ROOT_LEAF_CAPACITY UINT32_C(28)
#define TEST_SPLIT_KEY UINT64_C(29)

enum {
    TEST_WAL_VERSION_OFFSET = 8,
    TEST_WAL_RECORD_COUNT_OFFSET = 48,
    TEST_WAL_TOTAL_SIZE_OFFSET = 72,
    TEST_WAL_CHECKSUM_OFFSET = 80,
    TEST_WAL_HEADER_SIZE = 96,
    TEST_WAL_RECORD_SIZE = 8 + TEST_PAGE_SIZE,
    TEST_WAL_MAX_TRANSACTION_PAGES = 256
};

static void store_u32(unsigned char *data, uint32_t value) {
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static void store_u64(unsigned char *data, uint64_t value) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static uint32_t wal_checksum(unsigned char *wal, size_t wal_size) {
    uint32_t crc = UINT32_MAX;
    size_t index;

    store_u32(wal + TEST_WAL_CHECKSUM_OFFSET, 0u);
    for (index = 0u; index < wal_size; ++index) {
        unsigned int bit;

        crc ^= wal[index];
        for (bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);

            crc = (crc >> 1u) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

static void write_wal_file(const char *wal_path, const void *data, size_t size) {
    int fd = open(wal_path, O_WRONLY | O_TRUNC);

    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, data, size) == (ssize_t)size);
    TEST_CHECK(close(fd) == 0);
}

static uint64_t test_value(uint64_t key) {
    return key * UINT64_C(10);
}

static bpt_stats check_split_state(bpt_tree *tree, uint64_t expected_item_count) {
    char error[256];
    bpt_stats stats;
    uint64_t key;
    uint64_t value = 0u;

    memset(&stats, 0, sizeof(stats));
    memset(error, 0, sizeof(error));
    TEST_STATUS(bpt_tree_validate(tree, &stats, error, sizeof(error)), BPT_OK);
    TEST_CHECK(stats.page_size == TEST_PAGE_SIZE);
    TEST_CHECK(stats.leaf_capacity == TEST_ROOT_LEAF_CAPACITY);
    TEST_CHECK(stats.item_count == expected_item_count);
    for (key = 1u; key <= expected_item_count; ++key) {
        TEST_STATUS(bpt_tree_get(tree, key, &value), BPT_OK);
        TEST_CHECK(value == test_value(key));
    }
    TEST_STATUS(bpt_tree_get(tree, expected_item_count + 1u, &value), BPT_NOT_FOUND);
    return stats;
}

static bpt_stats create_full_root_leaf(const char *path, bpt_file_backend **backend_out,
                                       bpt_tree **tree_out) {
    bpt_storage storage;
    bpt_stats stats;
    uint64_t key;

    TEST_STATUS(bpt_file_backend_create(path, TEST_PAGE_SIZE, backend_out), BPT_OK);
    storage = bpt_file_backend_storage(*backend_out);
    TEST_STATUS(bpt_tree_create(&storage, tree_out), BPT_OK);
    for (key = 1u; key <= (uint64_t)TEST_ROOT_LEAF_CAPACITY; ++key) {
        bool inserted = false;

        TEST_STATUS(bpt_tree_put(*tree_out, key, test_value(key), &inserted), BPT_OK);
        TEST_CHECK(inserted);
    }
    stats = check_split_state(*tree_out, TEST_ROOT_LEAF_CAPACITY);
    TEST_CHECK(stats.height == 1u);
    return stats;
}

static int child_main(const char *path, const char *crash_point, uint64_t key, uint64_t value) {
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    bool inserted = false;

    TEST_CHECK(setenv("BPTREE_TEST_CRASH_POINT", crash_point, 1) == 0);
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    (void)bpt_tree_put(tree, key, value, &inserted);
    return 90;
}

static int recovery_child_main(const char *path, const char *crash_point) {
    bpt_file_backend *backend = NULL;

    TEST_CHECK(setenv("BPTREE_TEST_CRASH_POINT", crash_point, 1) == 0);
    (void)bpt_file_backend_open(path, &backend);
    return 90;
}

static void run_crash_case(const char *crash_point, bool expect_committed) {
    char path[256];
    char wal_path[512];
    int length;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    bpt_stats before;
    bpt_stats after;

    test_temp_path(path, sizeof(path));
    before = create_full_root_leaf(path, &backend, &tree);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, crash_point, TEST_SPLIT_KEY, test_value(TEST_SPLIT_KEY)));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    if (expect_committed) {
        after = check_split_state(tree, TEST_SPLIT_KEY);
        TEST_CHECK(after.height > before.height);
        TEST_CHECK(after.allocated_pages > before.allocated_pages);
    } else {
        after = check_split_state(tree, TEST_ROOT_LEAF_CAPACITY);
        TEST_CHECK(after.height == before.height);
        TEST_CHECK(after.allocated_pages == before.allocated_pages);
    }

    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    TEST_CHECK(access(wal_path, F_OK) != 0);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal.tmp", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    TEST_CHECK(access(wal_path, F_OK) != 0);

    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

static void run_recovery_crash_case(const char *recovery_crash_point) {
    char path[256];
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    bpt_stats before;
    bpt_stats after;

    test_temp_path(path, sizeof(path));
    before = create_full_root_leaf(path, &backend, &tree);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, "wal_synced", TEST_SPLIT_KEY, test_value(TEST_SPLIT_KEY)));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(recovery_child_main(path, recovery_crash_point));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    after = check_split_state(tree, TEST_SPLIT_KEY);
    TEST_CHECK(after.height > before.height);
    TEST_CHECK(after.allocated_pages > before.allocated_pages);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

static void test_every_torn_wal_prefix(void) {
    char path[256];
    char wal_path[512];
    int length;
    int fd;
    struct stat status;
    unsigned char *wal;
    size_t prefix;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    uint64_t value = 0u;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, "wal_synced", 3u, 30u));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    fd = open(wal_path, O_RDONLY);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(fstat(fd, &status) == 0);
    TEST_CHECK(status.st_size > 1);
    TEST_CHECK((uint64_t)status.st_size <= (uint64_t)SIZE_MAX);
    wal = malloc((size_t)status.st_size);
    TEST_CHECK(wal != NULL);
    TEST_CHECK(read(fd, wal, (size_t)status.st_size) == status.st_size);
    TEST_CHECK(close(fd) == 0);

    for (prefix = 1u; prefix < (size_t)status.st_size; ++prefix) {
        fd = open(wal_path, O_WRONLY | O_TRUNC);
        TEST_CHECK(fd >= 0);
        TEST_CHECK(write(fd, wal, prefix) == (ssize_t)prefix);
        TEST_CHECK(close(fd) == 0);
        TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
        TEST_CHECK(backend == NULL);
    }

    wal[(size_t)status.st_size - 1u] ^= UINT8_C(0x80);
    fd = open(wal_path, O_WRONLY | O_TRUNC);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, wal, (size_t)status.st_size) == status.st_size);
    TEST_CHECK(close(fd) == 0);
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
    TEST_CHECK(backend == NULL);
    wal[(size_t)status.st_size - 1u] ^= UINT8_C(0x80);

    fd = open(wal_path, O_WRONLY | O_TRUNC);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, wal, (size_t)status.st_size) == status.st_size);
    TEST_CHECK(close(fd) == 0);
    free(wal);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(tree, 3u, &value), BPT_OK);
    TEST_CHECK(value == 30u);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

static void test_wal_preflight_contracts(void) {
    char path[256];
    char wal_path[512];
    int length;
    int fd;
    struct stat status;
    unsigned char *wal;
    unsigned char *valid_wal;
    size_t wal_size;
    uint64_t oversized_wal_size;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    uint64_t value = 0u;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, TEST_PAGE_SIZE, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, "wal_synced", 3u, 30u));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    fd = open(wal_path, O_RDONLY);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(fstat(fd, &status) == 0);
    TEST_CHECK(status.st_size > TEST_WAL_HEADER_SIZE);
    wal_size = (size_t)status.st_size;
    wal = malloc(wal_size);
    valid_wal = malloc(wal_size);
    TEST_CHECK(wal != NULL && valid_wal != NULL);
    TEST_CHECK(read(fd, wal, wal_size) == (ssize_t)wal_size);
    TEST_CHECK(close(fd) == 0);
    memcpy(valid_wal, wal, wal_size);

    store_u64(wal + TEST_WAL_RECORD_COUNT_OFFSET, (uint64_t)TEST_WAL_MAX_TRANSACTION_PAGES + 1u);
    oversized_wal_size = TEST_WAL_HEADER_SIZE +
                         ((uint64_t)TEST_WAL_MAX_TRANSACTION_PAGES + 1u) * TEST_WAL_RECORD_SIZE;
    store_u64(wal + TEST_WAL_TOTAL_SIZE_OFFSET, oversized_wal_size);
    fd = open(wal_path, O_WRONLY | O_TRUNC);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, wal, TEST_WAL_HEADER_SIZE) == TEST_WAL_HEADER_SIZE);
    TEST_CHECK(ftruncate(fd, (off_t)oversized_wal_size) == 0);
    TEST_CHECK(close(fd) == 0);
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
    TEST_CHECK(backend == NULL);

    memcpy(wal, valid_wal, wal_size);
    store_u32(wal + TEST_WAL_VERSION_OFFSET, 2u);
    store_u32(wal + TEST_WAL_CHECKSUM_OFFSET, wal_checksum(wal, wal_size));
    write_wal_file(wal_path, wal, wal_size);
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_UNSUPPORTED);
    TEST_CHECK(backend == NULL);

    memcpy(wal, valid_wal, wal_size);
    write_wal_file(wal_path, wal, wal_size);
    free(valid_wal);
    free(wal);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(tree, 3u, &value), BPT_OK);
    TEST_CHECK(value == 30u);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

int main(void) {
    run_crash_case("wal_created", false);
    run_crash_case("wal_temp_synced", false);
    run_crash_case("wal_synced", true);
    run_crash_case("first_page_written", true);
    run_crash_case("data_synced", true);
    run_recovery_crash_case("recovery_first_page_written");
    run_recovery_crash_case("recovery_data_synced");
    test_every_torn_wal_prefix();
    test_wal_preflight_contracts();
    return 0;
}
