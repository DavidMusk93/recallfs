#include "bptree_backends.h"
#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

enum {
    TEST_PAGE_SIZE = 512,
    META_ROOT_OFFSET = 16,
    META_NEXT_PAGE_OFFSET = 40,
    NODE_TYPE_OFFSET = 6,
    NODE_CHECKSUM_OFFSET = 48,
    NODE_NEXT_OFFSET = 32,
    NODE_PAYLOAD_OFFSET = 64
};

static uint32_t load_u32(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

static uint64_t load_u64(const unsigned char *data) {
    uint64_t value = 0u;
    unsigned shift;

    for (shift = 0u; shift < 64u; shift += 8u) {
        value |= (uint64_t)data[shift / 8u] << shift;
    }
    return value;
}

static void store_u32(unsigned char *data, uint32_t value) {
    unsigned index;

    for (index = 0u; index < 4u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static void store_u64(unsigned char *data, uint64_t value) {
    unsigned index;

    for (index = 0u; index < 8u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static uint32_t crc32c_page(unsigned char *page) {
    uint32_t crc = UINT32_MAX;
    size_t index;

    store_u32(page + NODE_CHECKSUM_OFFSET, 0u);
    for (index = 0u; index < TEST_PAGE_SIZE; ++index) {
        unsigned bit;

        crc ^= page[index];
        for (bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

static off_t logical_offset(uint64_t page_id) {
    return (off_t)((page_id + 1u) * TEST_PAGE_SIZE);
}

static void read_exact_at(int fd, void *data, size_t size, off_t offset) {
    ssize_t count = pread(fd, data, size, offset);
    TEST_CHECK(count == (ssize_t)size);
}

static void write_exact_at(int fd, const void *data, size_t size, off_t offset) {
    ssize_t count = pwrite(fd, data, size, offset);
    TEST_CHECK(count == (ssize_t)size);
    TEST_CHECK(fsync(fd) == 0);
}

static void create_populated(const char *path, uint64_t item_count) {
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    uint64_t key;

    TEST_STATUS(bpt_file_backend_create(path, TEST_PAGE_SIZE, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    for (key = 0u; key < item_count; ++key) {
        bool inserted = false;

        TEST_STATUS(bpt_tree_put(tree, key, key + 1u, &inserted), BPT_OK);
        TEST_CHECK(inserted);
    }
    test_validate(tree);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
}

static void expect_tree_corrupt(const char *path) {
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_CORRUPT);
    TEST_CHECK(tree == NULL);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
}

static void test_bad_file_header(void) {
    char path[256];
    unsigned char byte;
    int fd;

    test_temp_path(path, sizeof(path));
    create_populated(path, 4u);
    fd = open(path, O_RDWR);
    TEST_CHECK(fd >= 0);
    read_exact_at(fd, &byte, 1u, 0);
    byte ^= UINT8_C(0x80);
    write_exact_at(fd, &byte, 1u, 0);
    TEST_CHECK(close(fd) == 0);

    bpt_file_backend *backend = NULL;
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
    TEST_CHECK(backend == NULL);
    test_remove_database(path);
}

static void test_bad_page_checksum(void) {
    char path[256];
    unsigned char byte;
    int fd;

    test_temp_path(path, sizeof(path));
    create_populated(path, 4u);
    fd = open(path, O_RDWR);
    TEST_CHECK(fd >= 0);
    read_exact_at(fd, &byte, 1u, logical_offset(1u) + NODE_PAYLOAD_OFFSET);
    byte ^= UINT8_C(0x40);
    write_exact_at(fd, &byte, 1u, logical_offset(1u) + NODE_PAYLOAD_OFFSET);
    TEST_CHECK(close(fd) == 0);
    expect_tree_corrupt(path);
    test_remove_database(path);
}

static void test_out_of_range_child(void) {
    char path[256];
    unsigned char meta[TEST_PAGE_SIZE];
    unsigned char root[TEST_PAGE_SIZE];
    uint64_t root_id;
    uint64_t next_page_id;
    uint32_t checksum;
    int fd;

    test_temp_path(path, sizeof(path));
    create_populated(path, 2000u);
    fd = open(path, O_RDWR);
    TEST_CHECK(fd >= 0);
    read_exact_at(fd, meta, sizeof(meta), logical_offset(0u));
    root_id = load_u64(meta + META_ROOT_OFFSET);
    next_page_id = load_u64(meta + META_NEXT_PAGE_OFFSET);
    read_exact_at(fd, root, sizeof(root), logical_offset(root_id));
    TEST_CHECK(root[NODE_TYPE_OFFSET] == 2u);
    store_u64(root + NODE_PAYLOAD_OFFSET, next_page_id + 100u);
    checksum = crc32c_page(root);
    store_u32(root + NODE_CHECKSUM_OFFSET, checksum);
    write_exact_at(fd, root, sizeof(root), logical_offset(root_id));
    TEST_CHECK(close(fd) == 0);
    expect_tree_corrupt(path);
    test_remove_database(path);
}

static void test_leaf_cycle(void) {
    char path[256];
    unsigned char page[TEST_PAGE_SIZE];
    uint64_t page_id;
    uint32_t checksum;
    int fd;

    test_temp_path(path, sizeof(path));
    create_populated(path, 2000u);
    fd = open(path, O_RDWR);
    TEST_CHECK(fd >= 0);
    read_exact_at(fd, page, sizeof(page), logical_offset(0u));
    page_id = load_u64(page + META_ROOT_OFFSET);
    for (;;) {
        read_exact_at(fd, page, sizeof(page), logical_offset(page_id));
        if (page[NODE_TYPE_OFFSET] == 1u) {
            break;
        }
        TEST_CHECK(page[NODE_TYPE_OFFSET] == 2u);
        page_id = load_u64(page + NODE_PAYLOAD_OFFSET);
    }
    store_u64(page + NODE_NEXT_OFFSET, page_id);
    checksum = crc32c_page(page);
    store_u32(page + NODE_CHECKSUM_OFFSET, checksum);
    write_exact_at(fd, page, sizeof(page), logical_offset(page_id));
    TEST_CHECK(close(fd) == 0);
    expect_tree_corrupt(path);
    test_remove_database(path);
}

static void test_malformed_wal(void) {
    char path[256];
    char wal_path[512];
    int length;
    int fd;
    bpt_file_backend *backend = NULL;

    test_temp_path(path, sizeof(path));
    create_populated(path, 4u);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    fd = open(wal_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_CHECK(fd >= 0);
    write_exact_at(fd, "bad-wal", 7u, 0);
    TEST_CHECK(close(fd) == 0);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
    TEST_CHECK(backend == NULL);
    TEST_CHECK(access(wal_path, F_OK) == 0);
    test_remove_database(path);
}

int main(void) {
    TEST_CHECK(load_u32((const unsigned char *)"\x78\x56\x34\x12") == UINT32_C(0x12345678));
    test_bad_file_header();
    test_bad_page_checksum();
    test_out_of_range_child();
    test_leaf_cycle();
    test_malformed_wal();
    return 0;
}
