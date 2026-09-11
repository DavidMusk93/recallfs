#include "test_support.h"

#include <fcntl.h>

enum {
    TEST_PAGE_SIZE = 512,
    TEST_PAGE_LEAF = 1,
    TEST_PAGE_INTERNAL = 2,
    TEST_PAGE_SCHEMA = 3,
    TEST_PAGE_OVERFLOW = 4,
    TEST_PAGE_TYPE_OFFSET = 6,
    TEST_PAGE_COUNT_OFFSET = 16,
    TEST_PAGE_LINK0_OFFSET = 32,
    TEST_PAGE_LINK1_OFFSET = 40,
    TEST_PAGE_CHECKSUM_OFFSET = 48,
    TEST_PAGE_AUX_OFFSET = 56,
    TEST_PAGE_HEADER_SIZE = 64,
    TEST_META_ROOT_OFFSET = 64,
    TEST_META_NEXT_PAGE_OFFSET = 88,
    TEST_META_SCHEMA_HEAD_OFFSET = 104,
    TEST_META_SCHEMA_SIZE_OFFSET = 112,
    TEST_META_SCHEMA_PAGE_COUNT_OFFSET = 120,
    TEST_MAX_SCHEMA_SIZE = 24 + 64 * 16,
    TEST_SLOT_SIZE = 8,
    TEST_LEAF_CELL_HEADER_SIZE = 8,
    TEST_VALUE_DESCRIPTOR_SIZE = 16,
};

static off_t page_offset(uint64_t page_id) {
    return (off_t)((page_id + 1u) * TEST_PAGE_SIZE);
}

static void read_page(int descriptor, uint64_t page_id, unsigned char page[TEST_PAGE_SIZE]) {
    rbt_test_read_exact_at(descriptor, page, TEST_PAGE_SIZE, page_offset(page_id));
}

static void write_page(int descriptor, uint64_t page_id, unsigned char page[TEST_PAGE_SIZE]) {
    uint32_t checksum = rbt_test_crc32c_zeroed(page, TEST_PAGE_SIZE, TEST_PAGE_CHECKSUM_OFFSET, 4u);

    rbt_test_store_u32(page + TEST_PAGE_CHECKSUM_OFFSET, checksum);
    rbt_test_write_exact_at(descriptor, page, TEST_PAGE_SIZE, page_offset(page_id));
    RBT_TEST_CHECK(fsync(descriptor) == 0);
}

static void populate_database(const char *path, uint64_t item_count, size_t payload_size) {
    unsigned char *payload = malloc(payload_size == 0u ? 1u : payload_size);
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = NULL;
    uint64_t key;

    RBT_TEST_CHECK(payload != NULL);
    memset(payload, 0x5a, payload_size);
    RBT_TEST_OK(rbt_file_create(path, TEST_PAGE_SIZE, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_test_create_u64_bytes(&storage, &tree));
    for (key = 0u; key < item_count; ++key) {
        bool inserted = false;

        if (payload_size != 0u) {
            payload[0] = (unsigned char)key;
        }
        RBT_TEST_OK(rbt_test_put_u64_bytes(tree, key, key + 1u, payload, payload_size, &inserted));
        RBT_TEST_CHECK(inserted);
    }
    (void)rbt_test_validate(tree);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));
    free(payload);
}

static void expect_tree_corrupt(const char *path) {
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = (struct rbt *)(uintptr_t)1u;

    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_ERRNO(rbt_open(&storage, &tree), EBADMSG);
    RBT_TEST_CHECK(tree == NULL);
    RBT_TEST_OK(rbt_file_close(file));
}

static uint64_t find_page_with_type(int descriptor, uint8_t type, uint32_t minimum_count,
                                    unsigned char page[TEST_PAGE_SIZE]) {
    unsigned char metadata[TEST_PAGE_SIZE];
    uint64_t next_page;
    uint64_t page_id;

    read_page(descriptor, 0u, metadata);
    next_page = rbt_test_load_u64(metadata + TEST_META_NEXT_PAGE_OFFSET);
    for (page_id = 1u; page_id < next_page; ++page_id) {
        read_page(descriptor, page_id, page);
        if (page[TEST_PAGE_TYPE_OFFSET] == type &&
            rbt_test_load_u32(page + TEST_PAGE_COUNT_OFFSET) >= minimum_count) {
            return page_id;
        }
    }
    RBT_TEST_CHECK(false);
    return 0u;
}

static uint64_t root_page_id(int descriptor, unsigned char metadata[TEST_PAGE_SIZE]) {
    read_page(descriptor, 0u, metadata);
    return rbt_test_load_u64(metadata + TEST_META_ROOT_OFFSET);
}

static void test_bad_file_header(void) {
    char path[256];
    unsigned char byte;
    int descriptor;
    struct rbt_file *file = (struct rbt_file *)(uintptr_t)1u;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    rbt_test_read_exact_at(descriptor, &byte, 1u, 0);
    byte ^= UINT8_C(0x80);
    rbt_test_write_exact_at(descriptor, &byte, 1u, 0);
    RBT_TEST_CHECK(fsync(descriptor) == 0);
    RBT_TEST_CHECK(close(descriptor) == 0);

    RBT_TEST_ERRNO(rbt_file_open(path, &file), EBADMSG);
    RBT_TEST_CHECK(file == NULL);
    rbt_test_remove_database(path);
}

static void test_bad_page_checksum(void) {
    char path[256];
    unsigned char byte;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    rbt_test_read_exact_at(descriptor, &byte, 1u, page_offset(0u) + 200);
    byte ^= UINT8_C(0x40);
    rbt_test_write_exact_at(descriptor, &byte, 1u, page_offset(0u) + 200);
    RBT_TEST_CHECK(fsync(descriptor) == 0);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_slot_bounds(void) {
    char path[256];
    unsigned char page[TEST_PAGE_SIZE];
    uint64_t leaf;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 3u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    leaf = find_page_with_type(descriptor, TEST_PAGE_LEAF, 2u, page);
    rbt_test_store_u32(page + TEST_PAGE_HEADER_SIZE, TEST_PAGE_SIZE);
    rbt_test_store_u32(page + TEST_PAGE_HEADER_SIZE + 4u, 2u);
    write_page(descriptor, leaf, page);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_slot_overlap(void) {
    char path[256];
    unsigned char page[TEST_PAGE_SIZE];
    uint64_t leaf;
    uint32_t first_offset;
    uint32_t first_length;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 3u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    leaf = find_page_with_type(descriptor, TEST_PAGE_LEAF, 2u, page);
    first_offset = rbt_test_load_u32(page + TEST_PAGE_HEADER_SIZE);
    first_length = rbt_test_load_u32(page + TEST_PAGE_HEADER_SIZE + 4u);
    rbt_test_store_u32(page + TEST_PAGE_HEADER_SIZE + TEST_SLOT_SIZE, first_offset);
    rbt_test_store_u32(page + TEST_PAGE_HEADER_SIZE + TEST_SLOT_SIZE + 4u, first_length);
    write_page(descriptor, leaf, page);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_invalid_child_id(void) {
    char path[256];
    unsigned char metadata[TEST_PAGE_SIZE];
    unsigned char root[TEST_PAGE_SIZE];
    uint64_t root_id;
    uint64_t next_page;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 24u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    root_id = root_page_id(descriptor, metadata);
    next_page = rbt_test_load_u64(metadata + TEST_META_NEXT_PAGE_OFFSET);
    read_page(descriptor, root_id, root);
    RBT_TEST_CHECK(root[TEST_PAGE_TYPE_OFFSET] == TEST_PAGE_INTERNAL);
    rbt_test_store_u64(root + TEST_PAGE_LINK0_OFFSET, next_page + 17u);
    write_page(descriptor, root_id, root);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_leaf_link_cycle(void) {
    char path[256];
    unsigned char metadata[TEST_PAGE_SIZE];
    unsigned char page[TEST_PAGE_SIZE];
    uint64_t page_id;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 24u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    page_id = root_page_id(descriptor, metadata);
    for (;;) {
        read_page(descriptor, page_id, page);
        if (page[TEST_PAGE_TYPE_OFFSET] == TEST_PAGE_LEAF) {
            break;
        }
        RBT_TEST_CHECK(page[TEST_PAGE_TYPE_OFFSET] == TEST_PAGE_INTERNAL);
        page_id = rbt_test_load_u64(page + TEST_PAGE_LINK0_OFFSET);
    }
    rbt_test_store_u64(page + TEST_PAGE_LINK0_OFFSET, page_id);
    write_page(descriptor, page_id, page);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_malformed_schema_chain(void) {
    char path[256];
    unsigned char metadata[TEST_PAGE_SIZE];
    unsigned char schema_page[TEST_PAGE_SIZE];
    uint64_t schema_id;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    read_page(descriptor, 0u, metadata);
    schema_id = rbt_test_load_u64(metadata + TEST_META_SCHEMA_HEAD_OFFSET);
    read_page(descriptor, schema_id, schema_page);
    RBT_TEST_CHECK(schema_page[TEST_PAGE_TYPE_OFFSET] == TEST_PAGE_SCHEMA);
    rbt_test_store_u64(schema_page + TEST_PAGE_LINK1_OFFSET, schema_id);
    write_page(descriptor, schema_id, schema_page);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_truncated_schema_chain(void) {
    char path[256];
    unsigned char metadata[TEST_PAGE_SIZE];
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    read_page(descriptor, 0u, metadata);
    rbt_test_store_u64(metadata + TEST_META_SCHEMA_SIZE_OFFSET, TEST_PAGE_SIZE);
    write_page(descriptor, 0u, metadata);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_oversized_schema_size(void) {
    char path[256];
    unsigned char metadata[TEST_PAGE_SIZE];
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    read_page(descriptor, 0u, metadata);
    rbt_test_store_u64(metadata + TEST_META_SCHEMA_SIZE_OFFSET, TEST_MAX_SCHEMA_SIZE + 1u);
    write_page(descriptor, 0u, metadata);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_inconsistent_schema_page_count(void) {
    char path[256];
    unsigned char metadata[TEST_PAGE_SIZE];
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    read_page(descriptor, 0u, metadata);
    RBT_TEST_CHECK(rbt_test_load_u64(metadata + TEST_META_SCHEMA_PAGE_COUNT_OFFSET) == 1u);
    rbt_test_store_u64(metadata + TEST_META_SCHEMA_PAGE_COUNT_OFFSET, 2u);
    write_page(descriptor, 0u, metadata);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static uint64_t locate_overflow(int descriptor, unsigned char leaf[TEST_PAGE_SIZE],
                                uint64_t *out_leaf_id, unsigned char **out_descriptor) {
    uint64_t leaf_id = find_page_with_type(descriptor, TEST_PAGE_LEAF, 1u, leaf);
    uint32_t cell_offset = rbt_test_load_u32(leaf + TEST_PAGE_HEADER_SIZE);
    uint32_t cell_length = rbt_test_load_u32(leaf + TEST_PAGE_HEADER_SIZE + 4u);
    unsigned char *cell;
    uint32_t key_size;
    unsigned char *descriptor_entry;

    RBT_TEST_CHECK(cell_offset < TEST_PAGE_SIZE);
    RBT_TEST_CHECK(cell_length <= TEST_PAGE_SIZE - cell_offset);
    cell = leaf + cell_offset;
    key_size = rbt_test_load_u32(cell);
    RBT_TEST_CHECK(TEST_LEAF_CELL_HEADER_SIZE + key_size + 2u * TEST_VALUE_DESCRIPTOR_SIZE <=
                   cell_length);
    descriptor_entry = cell + TEST_LEAF_CELL_HEADER_SIZE + key_size + TEST_VALUE_DESCRIPTOR_SIZE;
    RBT_TEST_CHECK(descriptor_entry[0] == 2u);
    *out_leaf_id = leaf_id;
    *out_descriptor = descriptor_entry;
    return rbt_test_load_u64(descriptor_entry + 8u);
}

static void test_overflow_cycle(void) {
    char path[256];
    unsigned char leaf[TEST_PAGE_SIZE];
    unsigned char overflow[TEST_PAGE_SIZE];
    unsigned char *descriptor_entry;
    uint64_t leaf_id;
    uint64_t overflow_id;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1000u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    overflow_id = locate_overflow(descriptor, leaf, &leaf_id, &descriptor_entry);
    (void)leaf_id;
    (void)descriptor_entry;
    read_page(descriptor, overflow_id, overflow);
    RBT_TEST_CHECK(overflow[TEST_PAGE_TYPE_OFFSET] == TEST_PAGE_OVERFLOW);
    rbt_test_store_u64(overflow + TEST_PAGE_LINK0_OFFSET, overflow_id);
    write_page(descriptor, overflow_id, overflow);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_overflow_wrong_column(void) {
    char path[256];
    unsigned char leaf[TEST_PAGE_SIZE];
    unsigned char overflow[TEST_PAGE_SIZE];
    unsigned char *descriptor_entry;
    uint64_t leaf_id;
    uint64_t overflow_id;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1000u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    overflow_id = locate_overflow(descriptor, leaf, &leaf_id, &descriptor_entry);
    (void)leaf_id;
    (void)descriptor_entry;
    read_page(descriptor, overflow_id, overflow);
    RBT_TEST_CHECK(rbt_test_load_u64(overflow + TEST_PAGE_AUX_OFFSET) == 1u);
    rbt_test_store_u64(overflow + TEST_PAGE_AUX_OFFSET, 0u);
    write_page(descriptor, overflow_id, overflow);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_overflow_wrong_length(void) {
    char path[256];
    unsigned char leaf[TEST_PAGE_SIZE];
    unsigned char *descriptor_entry;
    uint64_t leaf_id;
    uint64_t overflow_id;
    uint32_t logical_size;
    int descriptor;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1000u);
    descriptor = open(path, O_RDWR);
    RBT_TEST_CHECK(descriptor >= 0);
    overflow_id = locate_overflow(descriptor, leaf, &leaf_id, &descriptor_entry);
    RBT_TEST_CHECK(overflow_id != 0u);
    logical_size = rbt_test_load_u32(descriptor_entry + 4u);
    RBT_TEST_CHECK(logical_size == 1000u);
    rbt_test_store_u32(descriptor_entry + 4u, logical_size - 1u);
    write_page(descriptor, leaf_id, leaf);
    RBT_TEST_CHECK(close(descriptor) == 0);
    expect_tree_corrupt(path);
    rbt_test_remove_database(path);
}

static void test_malformed_active_wal(void) {
    char path[256];
    char wal_path[512];
    int length;
    int descriptor;
    struct rbt_file *file = (struct rbt_file *)(uintptr_t)1u;

    rbt_test_temp_path(path, sizeof(path));
    populate_database(path, 1u, 1u);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_path));
    descriptor = open(wal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    RBT_TEST_CHECK(descriptor >= 0);
    rbt_test_write_exact_at(descriptor, "bad-wal", 7u, 0);
    RBT_TEST_CHECK(fsync(descriptor) == 0);
    RBT_TEST_CHECK(close(descriptor) == 0);

    RBT_TEST_ERRNO(rbt_file_open(path, &file), EBADMSG);
    RBT_TEST_CHECK(file == NULL);
    RBT_TEST_CHECK(access(wal_path, F_OK) == 0);
    rbt_test_remove_database(path);
}

int main(void) {
    RBT_TEST_CHECK(rbt_test_load_u32((const unsigned char *)"\x78\x56\x34\x12") ==
                   UINT32_C(0x12345678));
    test_bad_file_header();
    test_bad_page_checksum();
    test_slot_bounds();
    test_slot_overlap();
    test_invalid_child_id();
    test_leaf_link_cycle();
    test_malformed_schema_chain();
    test_truncated_schema_chain();
    test_oversized_schema_size();
    test_inconsistent_schema_page_count();
    test_overflow_cycle();
    test_overflow_wrong_column();
    test_overflow_wrong_length();
    test_malformed_active_wal();
    return 0;
}
