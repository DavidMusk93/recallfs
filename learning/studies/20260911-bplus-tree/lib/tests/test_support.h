#ifndef BPTREE_TEST_SUPPORT_H
#define BPTREE_TEST_SUPPORT_H

#include "bptree.h"

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
        bpt_status test_status_value = (expression);                                               \
        if (test_status_value != (expected)) {                                                     \
            fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__,            \
                    #expression, bpt_status_string(test_status_value),                             \
                    bpt_status_string(expected));                                                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static inline void test_temp_path(char *path, size_t capacity) {
    int fd;

    TEST_CHECK(capacity >= 32u);
    TEST_CHECK(snprintf(path, capacity, "/tmp/bptree-test-XXXXXX") > 0);
    fd = mkstemp(path);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(close(fd) == 0);
    TEST_CHECK(unlink(path) == 0);
}

static inline void test_remove_database(const char *path) {
    char wal_path[512];
    int length;

    (void)unlink(path);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    (void)unlink(wal_path);
}

static inline void test_validate(bpt_tree *tree) {
    char error[256];
    bpt_stats stats;
    bpt_status status;

    memset(&stats, 0, sizeof(stats));
    memset(error, 0, sizeof(error));
    status = bpt_tree_validate(tree, &stats, error, sizeof(error));
    if (status != BPT_OK) {
        fprintf(stderr, "validation failed: %s: %s\n", bpt_status_string(status), error);
        exit(1);
    }
}

#endif
