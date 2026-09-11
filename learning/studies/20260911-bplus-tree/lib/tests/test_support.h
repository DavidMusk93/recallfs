#ifndef BTREE_TEST_SUPPORT_H
#define BTREE_TEST_SUPPORT_H

#include "btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_CHECK(condition)                                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define TEST_STATUS(expression, expected)                                                          \
    do {                                                                                           \
        btree_status test_status_value = (expression);                                             \
        if (test_status_value != (expected)) {                                                     \
            fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__,            \
                    #expression, btree_status_string(test_status_value),                           \
                    btree_status_string(expected));                                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef btree_status (*test_u64_scan_fn)(void *context, uint64_t key, uint64_t value);

typedef struct test_u64_scan_bridge {
    test_u64_scan_fn callback;
    void *context;
} test_u64_scan_bridge;

static inline void test_u64_encode(uint64_t value, unsigned char bytes[8]) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        bytes[7u - index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline uint64_t test_u64_decode(const void *data) {
    const unsigned char *bytes = data;
    uint64_t value = 0u;
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        value = (value << 8u) | bytes[index];
    }
    return value;
}

static inline btree_options test_u64_options(void) {
    btree_options options;

    btree_options_init(&options, 8u, 8u);
    return options;
}

static inline btree_status test_btree_create_u64(const btree_storage *storage, btree **tree_out) {
    btree_options options = test_u64_options();

    return btree_create(storage, &options, tree_out);
}

static inline btree_status test_btree_open_u64(const btree_storage *storage, btree **tree_out) {
    btree_options options = test_u64_options();

    return btree_open(storage, &options, tree_out);
}

static inline btree_status test_btree_put_u64(btree *tree, uint64_t key, uint64_t value,
                                              bool *inserted_out) {
    unsigned char encoded_key[8];
    unsigned char encoded_value[8];

    test_u64_encode(key, encoded_key);
    test_u64_encode(value, encoded_value);
    return btree_put(tree, encoded_key, encoded_value, inserted_out);
}

static inline btree_status test_btree_get_u64(btree *tree, uint64_t key, uint64_t *value_out) {
    unsigned char encoded_key[8];
    unsigned char encoded_value[8];
    btree_status status;

    test_u64_encode(key, encoded_key);
    status = btree_get(tree, encoded_key, value_out == NULL ? NULL : encoded_value);
    if (status == BTREE_OK && value_out != NULL) {
        *value_out = test_u64_decode(encoded_value);
    }
    return status;
}

static inline btree_status test_btree_delete_u64(btree *tree, uint64_t key, bool *removed_out) {
    unsigned char encoded_key[8];

    test_u64_encode(key, encoded_key);
    return btree_delete(tree, encoded_key, removed_out);
}

static inline btree_scan_action test_u64_scan_adapter(void *context, const void *key,
                                                      const void *value) {
    test_u64_scan_bridge *bridge = context;
    btree_status status =
        bridge->callback(bridge->context, test_u64_decode(key), test_u64_decode(value));

    TEST_CHECK(status == BTREE_OK || status == BTREE_STOPPED);
    return status == BTREE_OK ? BTREE_SCAN_CONTINUE : BTREE_SCAN_STOP;
}

static inline btree_status test_btree_scan_u64(btree *tree, uint64_t begin_key, uint64_t end_key,
                                               test_u64_scan_fn callback, void *context) {
    unsigned char encoded_begin[8];
    unsigned char encoded_end[8];
    test_u64_scan_bridge bridge;

    test_u64_encode(begin_key, encoded_begin);
    test_u64_encode(end_key, encoded_end);
    if (callback == NULL) {
        return btree_scan(tree, encoded_begin, encoded_end, NULL, context);
    }
    bridge.callback = callback;
    bridge.context = context;
    return btree_scan(tree, encoded_begin, encoded_end, test_u64_scan_adapter, &bridge);
}

static inline void test_temp_path(char *path, size_t capacity) {
    int fd;

    TEST_CHECK(capacity >= 32u);
    TEST_CHECK(snprintf(path, capacity, "/tmp/btree-test-XXXXXX") > 0);
    fd = mkstemp(path);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(close(fd) == 0);
    TEST_CHECK(unlink(path) == 0);
}

static inline void test_remove_database(const char *path) {
    char wal_path[512];
    char wal_temp_path[512];
    int length;

    (void)unlink(path);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    (void)unlink(wal_path);
    length = snprintf(wal_temp_path, sizeof(wal_temp_path), "%s.wal.tmp", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_temp_path));
    (void)unlink(wal_temp_path);
}

static inline void test_validate(btree *tree) {
    char error[256];
    btree_stats stats;
    btree_status status;

    memset(&stats, 0, sizeof(stats));
    memset(error, 0, sizeof(error));
    status = btree_validate(tree, &stats, error, sizeof(error));
    if (status != BTREE_OK) {
        fprintf(stderr, "validation failed: %s: %s\n", btree_status_string(status), error);
        exit(1);
    }
}

#endif
