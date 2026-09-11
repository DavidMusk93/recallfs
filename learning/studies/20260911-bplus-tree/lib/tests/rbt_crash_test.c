#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

enum {
    TEST_PAGE_SIZE = 512,
    OLD_ITEM_COUNT = 6,
    NEW_KEY = 7,
    LARGE_VALUE_SIZE = 1536,
    TEST_FILE_UUID_OFFSET = 24,
    TEST_WAL_VERSION_OFFSET = 8,
    TEST_WAL_PAGE_SIZE_OFFSET = 12,
    TEST_WAL_HEADER_SIZE_OFFSET = 20,
    TEST_WAL_UUID_OFFSET = 24,
    TEST_WAL_RECORD_COUNT_OFFSET = 40,
    TEST_WAL_ORIGINAL_COUNT_OFFSET = 48,
    TEST_WAL_FINAL_COUNT_OFFSET = 56,
    TEST_WAL_TOTAL_SIZE_OFFSET = 64,
    TEST_WAL_CHECKSUM_OFFSET = 72,
    TEST_WAL_HEADER_SIZE = 80,
    TEST_WAL_PAGE_ID_SIZE = 8,
    TEST_LARGE_WAL_RECORD_COUNT = 4096,
};

static const unsigned char TEST_WAL_MAGIC[8] = {'R', 'B', 'T', 'W', 'A', 'L', '0', '1'};

static void fill_large_value(unsigned char value[LARGE_VALUE_SIZE]) {
    size_t index;

    for (index = 0u; index < LARGE_VALUE_SIZE; ++index) {
        value[index] = (unsigned char)((index * 37u + 11u) & 0xffu);
    }
    value[17] = 0u;
    value[901] = 0u;
}

static void small_value(uint64_t key, unsigned char value[4]) {
    value[0] = (unsigned char)key;
    value[1] = 0u;
    value[2] = (unsigned char)(key * 17u);
    value[3] = (unsigned char)(key ^ UINT64_C(0xa5));
}

static void expect_row(struct rbt *tree, uint64_t key_number, uint64_t expected_number,
                       const void *expected_bytes, size_t expected_size) {
    struct rbt_value key_value = rbt_test_u64(key_number);
    struct rbt_record key = rbt_test_key(&key_value, 1u);
    struct rbt_row *row = NULL;
    const struct rbt_value *number = NULL;
    const struct rbt_value *bytes = NULL;

    RBT_TEST_OK(rbt_get(tree, &key, &row));
    RBT_TEST_OK(rbt_row_get_value(row, 0u, &number));
    RBT_TEST_OK(rbt_row_get_value(row, 1u, &bytes));
    RBT_TEST_CHECK(number->type == RBT_TYPE_U64);
    RBT_TEST_CHECK(!number->is_null);
    RBT_TEST_CHECK(number->as.u64 == expected_number);
    RBT_TEST_CHECK(bytes->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!bytes->is_null);
    RBT_TEST_BYTES(&bytes->as.bytes, expected_bytes, expected_size);
    RBT_TEST_OK(rbt_row_destroy(row));
}

static struct rbt_stats create_old_state(const char *path) {
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = NULL;
    struct rbt_stats stats;
    uint64_t key;

    RBT_TEST_OK(rbt_file_create(path, TEST_PAGE_SIZE, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_test_create_u64_bytes(&storage, &tree));
    for (key = 1u; key <= OLD_ITEM_COUNT; ++key) {
        unsigned char payload[4];
        bool inserted = false;

        small_value(key, payload);
        RBT_TEST_OK(rbt_test_put_u64_bytes(tree, key, key * UINT64_C(10), payload, sizeof(payload),
                                           &inserted));
        RBT_TEST_CHECK(inserted);
    }
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.item_count == OLD_ITEM_COUNT);
    RBT_TEST_CHECK(stats.height == 1u);
    RBT_TEST_CHECK(stats.overflow_pages == 0u);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));
    return stats;
}

static int mutation_child(const char *path, const char *crash_point) {
    unsigned char large_value[LARGE_VALUE_SIZE];
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = NULL;
    bool inserted = false;

    fill_large_value(large_value);
    RBT_TEST_CHECK(setenv("RBT_TEST_CRASH_POINT", crash_point, 1) == 0);
    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    (void)rbt_test_put_u64_bytes(tree, NEW_KEY, UINT64_C(700), large_value, sizeof(large_value),
                                 &inserted);
    return 90;
}

static int recovery_child(const char *path, const char *crash_point) {
    struct rbt_file *file = NULL;

    RBT_TEST_CHECK(setenv("RBT_TEST_CRASH_POINT", crash_point, 1) == 0);
    (void)rbt_file_open(path, &file);
    return 90;
}

static void run_child_and_expect_crash(const char *path, const char *crash_point, bool recovery) {
    pid_t child = fork();
    int child_status;

    RBT_TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(recovery ? recovery_child(path, crash_point) : mutation_child(path, crash_point));
    }
    RBT_TEST_CHECK(waitpid(child, &child_status, 0) == child);
    RBT_TEST_CHECK(WIFEXITED(child_status));
    RBT_TEST_CHECK(WEXITSTATUS(child_status) == 86);
}

static void expect_wal_absent(const char *path) {
    char wal_path[512];
    int length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);

    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_path));
    RBT_TEST_CHECK(access(wal_path, F_OK) != 0);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal.tmp", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_path));
    RBT_TEST_CHECK(access(wal_path, F_OK) != 0);
}

static void check_reopened_state(const char *path, bool expect_new,
                                 const struct rbt_stats *before) {
    unsigned char large_value[LARGE_VALUE_SIZE];
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = NULL;
    struct rbt_stats after;
    uint64_t key;

    fill_large_value(large_value);
    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    for (key = 1u; key <= OLD_ITEM_COUNT; ++key) {
        unsigned char payload[4];

        small_value(key, payload);
        expect_row(tree, key, key * UINT64_C(10), payload, sizeof(payload));
    }
    if (expect_new) {
        expect_row(tree, NEW_KEY, UINT64_C(700), large_value, sizeof(large_value));
    } else {
        struct rbt_value key_value = rbt_test_u64(NEW_KEY);
        struct rbt_record key_record = rbt_test_key(&key_value, 1u);
        struct rbt_row *row = (struct rbt_row *)(uintptr_t)1u;

        RBT_TEST_ERRNO(rbt_get(tree, &key_record, &row), ENOENT);
        RBT_TEST_CHECK(row == NULL);
    }
    after = rbt_test_validate(tree);
    RBT_TEST_CHECK(after.item_count == OLD_ITEM_COUNT + (expect_new ? 1u : 0u));
    if (expect_new) {
        RBT_TEST_CHECK(after.height > before->height);
        RBT_TEST_CHECK(after.overflow_pages >= 4u);
        RBT_TEST_CHECK(after.allocated_pages > before->allocated_pages);
    } else {
        RBT_TEST_CHECK(after.height == before->height);
        RBT_TEST_CHECK(after.overflow_pages == before->overflow_pages);
        RBT_TEST_CHECK(after.allocated_pages == before->allocated_pages);
    }
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));
    expect_wal_absent(path);
}

static void run_mutation_crash_case(const char *crash_point, bool expect_new) {
    char path[256];
    struct rbt_stats before;

    rbt_test_temp_path(path, sizeof(path));
    before = create_old_state(path);
    run_child_and_expect_crash(path, crash_point, false);
    check_reopened_state(path, expect_new, &before);
    rbt_test_remove_database(path);
}

static void run_recovery_crash_case(const char *crash_point) {
    char path[256];
    struct rbt_stats before;

    rbt_test_temp_path(path, sizeof(path));
    before = create_old_state(path);
    run_child_and_expect_crash(path, "wal_synced", false);
    run_child_and_expect_crash(path, crash_point, true);
    check_reopened_state(path, true, &before);
    rbt_test_remove_database(path);
}

static unsigned char *read_active_wal(const char *path, char wal_path[512], size_t *out_size) {
    int length = snprintf(wal_path, 512u, "%s.wal", path);
    int descriptor;
    struct stat status;
    unsigned char *wal;

    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK(length < 512);
    descriptor = open(wal_path, O_RDONLY);
    RBT_TEST_CHECK(descriptor >= 0);
    RBT_TEST_CHECK(fstat(descriptor, &status) == 0);
    RBT_TEST_CHECK(status.st_size > TEST_WAL_HEADER_SIZE);
    RBT_TEST_CHECK((uint64_t)status.st_size <= (uint64_t)SIZE_MAX);
    *out_size = (size_t)status.st_size;
    wal = malloc(*out_size);
    RBT_TEST_CHECK(wal != NULL);
    rbt_test_read_exact_at(descriptor, wal, *out_size, 0);
    RBT_TEST_CHECK(close(descriptor) == 0);
    return wal;
}

static void write_active_wal(const char *wal_path, const unsigned char *wal, size_t size) {
    int descriptor = open(wal_path, O_WRONLY | O_TRUNC);

    RBT_TEST_CHECK(descriptor >= 0);
    rbt_test_write_exact_at(descriptor, wal, size, 0);
    RBT_TEST_CHECK(close(descriptor) == 0);
}

static void fill_recovery_page(uint64_t page_id, unsigned char page[TEST_PAGE_SIZE]) {
    size_t index;

    for (index = 0u; index < TEST_PAGE_SIZE; ++index) {
        page[index] = (unsigned char)((page_id * UINT64_C(37) + index) & UINT64_C(0xff));
    }
}

static void test_large_wal_recovery(void) {
    char path[256];
    char wal_path[512];
    unsigned char file_header[TEST_PAGE_SIZE];
    unsigned char expected[TEST_PAGE_SIZE];
    unsigned char actual[TEST_PAGE_SIZE];
    const size_t record_size = TEST_WAL_PAGE_ID_SIZE + TEST_PAGE_SIZE;
    const size_t wal_size = TEST_WAL_HEADER_SIZE + TEST_LARGE_WAL_RECORD_COUNT * record_size;
    unsigned char *wal;
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    uint64_t page_count = UINT64_MAX;
    size_t index;
    int descriptor;
    int length;

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_OK(rbt_file_create(path, TEST_PAGE_SIZE, &file));
    RBT_TEST_OK(rbt_file_close(file));

    descriptor = open(path, O_RDONLY);
    RBT_TEST_CHECK(descriptor >= 0);
    rbt_test_read_exact_at(descriptor, file_header, sizeof(file_header), 0);
    RBT_TEST_CHECK(close(descriptor) == 0);

    wal = calloc(1u, wal_size);
    RBT_TEST_CHECK(wal != NULL);
    memcpy(wal, TEST_WAL_MAGIC, sizeof(TEST_WAL_MAGIC));
    rbt_test_store_u32(wal + TEST_WAL_VERSION_OFFSET, RBT_FORMAT_VERSION);
    rbt_test_store_u32(wal + TEST_WAL_PAGE_SIZE_OFFSET, TEST_PAGE_SIZE);
    rbt_test_store_u32(wal + TEST_WAL_HEADER_SIZE_OFFSET, TEST_WAL_HEADER_SIZE);
    memcpy(wal + TEST_WAL_UUID_OFFSET, file_header + TEST_FILE_UUID_OFFSET, 16u);
    rbt_test_store_u64(wal + TEST_WAL_RECORD_COUNT_OFFSET, TEST_LARGE_WAL_RECORD_COUNT);
    rbt_test_store_u64(wal + TEST_WAL_ORIGINAL_COUNT_OFFSET, 0u);
    rbt_test_store_u64(wal + TEST_WAL_FINAL_COUNT_OFFSET, TEST_LARGE_WAL_RECORD_COUNT);
    rbt_test_store_u64(wal + TEST_WAL_TOTAL_SIZE_OFFSET, wal_size);
    for (index = 0u; index < TEST_LARGE_WAL_RECORD_COUNT; ++index) {
        uint64_t page_id = TEST_LARGE_WAL_RECORD_COUNT - (uint64_t)index - 1u;
        size_t offset = TEST_WAL_HEADER_SIZE + index * record_size;

        rbt_test_store_u64(wal + offset, page_id);
        fill_recovery_page(page_id, wal + offset + TEST_WAL_PAGE_ID_SIZE);
    }
    rbt_test_store_u32(
        wal + TEST_WAL_CHECKSUM_OFFSET,
        rbt_test_crc32c_zeroed(wal, wal_size, TEST_WAL_CHECKSUM_OFFSET, sizeof(uint32_t)));

    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_path));
    descriptor = open(wal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    RBT_TEST_CHECK(descriptor >= 0);
    rbt_test_write_exact_at(descriptor, wal, wal_size, 0);
    RBT_TEST_CHECK(close(descriptor) == 0);

    rbt_test_store_u64(wal + TEST_WAL_HEADER_SIZE + record_size, TEST_LARGE_WAL_RECORD_COUNT - 1u);
    rbt_test_store_u32(
        wal + TEST_WAL_CHECKSUM_OFFSET,
        rbt_test_crc32c_zeroed(wal, wal_size, TEST_WAL_CHECKSUM_OFFSET, sizeof(uint32_t)));
    write_active_wal(wal_path, wal, wal_size);
    file = (struct rbt_file *)(uintptr_t)1u;
    RBT_TEST_ERRNO(rbt_file_open(path, &file), EBADMSG);
    RBT_TEST_CHECK(file == NULL);

    rbt_test_store_u64(wal + TEST_WAL_HEADER_SIZE + record_size, TEST_LARGE_WAL_RECORD_COUNT - 2u);
    rbt_test_store_u32(
        wal + TEST_WAL_CHECKSUM_OFFSET,
        rbt_test_crc32c_zeroed(wal, wal_size, TEST_WAL_CHECKSUM_OFFSET, sizeof(uint32_t)));
    write_active_wal(wal_path, wal, wal_size);
    free(wal);

    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(storage.page_count(storage.context, &page_count));
    RBT_TEST_CHECK(page_count == TEST_LARGE_WAL_RECORD_COUNT);
    for (index = 0u; index < 3u; ++index) {
        static const uint64_t PAGE_IDS[] = {0u, TEST_LARGE_WAL_RECORD_COUNT / 2u,
                                            TEST_LARGE_WAL_RECORD_COUNT - 1u};
        uint64_t page_id = PAGE_IDS[index];

        fill_recovery_page(page_id, expected);
        RBT_TEST_OK(storage.read_page(storage.context, page_id, actual));
        RBT_TEST_CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    }
    RBT_TEST_OK(rbt_file_close(file));
    expect_wal_absent(path);
    rbt_test_remove_database(path);
}

static void expect_wal_open_error(const char *path, int expected_errno) {
    struct rbt_file *file = (struct rbt_file *)(uintptr_t)1u;

    RBT_TEST_ERRNO(rbt_file_open(path, &file), expected_errno);
    RBT_TEST_CHECK(file == NULL);
}

static void test_every_nonempty_truncated_wal_prefix(void) {
    char path[256];
    char wal_path[512];
    struct rbt_stats before;
    unsigned char *wal;
    size_t wal_size;
    size_t prefix;

    rbt_test_temp_path(path, sizeof(path));
    before = create_old_state(path);
    run_child_and_expect_crash(path, "wal_synced", false);
    wal = read_active_wal(path, wal_path, &wal_size);

    for (prefix = 1u; prefix < wal_size; ++prefix) {
        write_active_wal(wal_path, wal, prefix);
        expect_wal_open_error(path, EBADMSG);
    }

    write_active_wal(wal_path, wal, wal_size);
    free(wal);
    check_reopened_state(path, true, &before);
    rbt_test_remove_database(path);
}

static void test_wal_version_and_checksum(void) {
    char path[256];
    char wal_path[512];
    struct rbt_stats before;
    unsigned char *wal;
    unsigned char *mutated;
    size_t wal_size;
    uint32_t checksum;

    RBT_TEST_CHECK(RBT_FORMAT_VERSION == 1u);
    rbt_test_temp_path(path, sizeof(path));
    before = create_old_state(path);
    run_child_and_expect_crash(path, "wal_synced", false);
    wal = read_active_wal(path, wal_path, &wal_size);
    mutated = malloc(wal_size);
    RBT_TEST_CHECK(mutated != NULL);

    memcpy(mutated, wal, wal_size);
    mutated[wal_size - 1u] ^= UINT8_C(0x80);
    write_active_wal(wal_path, mutated, wal_size);
    expect_wal_open_error(path, EBADMSG);

    memcpy(mutated, wal, wal_size);
    rbt_test_store_u32(mutated + TEST_WAL_VERSION_OFFSET, RBT_FORMAT_VERSION + 1u);
    checksum =
        rbt_test_crc32c_zeroed(mutated, wal_size, TEST_WAL_CHECKSUM_OFFSET, sizeof(uint32_t));
    rbt_test_store_u32(mutated + TEST_WAL_CHECKSUM_OFFSET, checksum);
    write_active_wal(wal_path, mutated, wal_size);
    expect_wal_open_error(path, ENOTSUP);

    write_active_wal(wal_path, wal, wal_size);
    free(mutated);
    free(wal);
    check_reopened_state(path, true, &before);
    rbt_test_remove_database(path);
}

int main(void) {
    run_mutation_crash_case("wal_created", false);
    run_mutation_crash_case("wal_temp_synced", false);
    run_mutation_crash_case("wal_synced", true);
    run_mutation_crash_case("first_page_written", true);
    run_mutation_crash_case("data_synced", true);
    run_recovery_crash_case("recovery_first_page_written");
    run_recovery_crash_case("recovery_data_synced");
    test_every_nonempty_truncated_wal_prefix();
    test_wal_version_and_checksum();
    test_large_wal_recovery();
    return 0;
}
