#ifndef RBT_TEST_SUPPORT_H
#define RBT_TEST_SUPPORT_H

#include "rbt_backends.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RBT_TEST_CHECK(condition)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define RBT_TEST_OK(expression)                                                                    \
    do {                                                                                           \
        int rbt_test_result_ = (expression);                                                       \
        if (rbt_test_result_ != 0) {                                                               \
            fprintf(stderr, "%s:%d: %s returned %d (%s), expected success\n", __FILE__, __LINE__,  \
                    #expression, rbt_test_result_, rbt_strerror(rbt_test_result_));                \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define RBT_TEST_ERRNO(expression, expected_errno)                                                 \
    do {                                                                                           \
        int rbt_test_result_ = (expression);                                                       \
        int rbt_test_expected_ = -(expected_errno);                                                \
        if (rbt_test_result_ != rbt_test_expected_) {                                              \
            fprintf(stderr, "%s:%d: %s returned %d (%s), expected -%s\n", __FILE__, __LINE__,      \
                    #expression, rbt_test_result_, rbt_strerror(rbt_test_result_),                 \
                    #expected_errno);                                                              \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define RBT_TEST_BYTES(actual_bytes, expected_data, expected_size)                                 \
    do {                                                                                           \
        const struct rbt_bytes *rbt_test_actual_ = (actual_bytes);                                 \
        size_t rbt_test_size_ = (expected_size);                                                   \
        RBT_TEST_CHECK(rbt_test_actual_->size == rbt_test_size_);                                  \
        RBT_TEST_CHECK(rbt_test_size_ == 0u ||                                                     \
                       memcmp(rbt_test_actual_->data, (expected_data), rbt_test_size_) == 0);      \
    } while (0)

static inline struct rbt_value rbt_test_bool(bool value) {
    struct rbt_value result = {
        .type = RBT_TYPE_BOOL,
        .is_null = false,
        .as.boolean = value,
    };

    return result;
}

static inline struct rbt_value rbt_test_i64(int64_t value) {
    struct rbt_value result = {
        .type = RBT_TYPE_I64,
        .is_null = false,
        .as.i64 = value,
    };

    return result;
}

static inline struct rbt_value rbt_test_u64(uint64_t value) {
    struct rbt_value result = {
        .type = RBT_TYPE_U64,
        .is_null = false,
        .as.u64 = value,
    };

    return result;
}

static inline struct rbt_value rbt_test_bytes(const void *data, size_t size) {
    struct rbt_value result = {
        .type = RBT_TYPE_BYTES,
        .is_null = false,
        .as.bytes = {.data = data, .size = size},
    };

    return result;
}

static inline struct rbt_value rbt_test_utf8(const char *data, size_t size) {
    struct rbt_value result = {
        .type = RBT_TYPE_UTF8,
        .is_null = false,
        .as.bytes = {.data = data, .size = size},
    };

    return result;
}

static inline struct rbt_value rbt_test_null(enum rbt_type type) {
    struct rbt_value result = {
        .type = type,
        .is_null = true,
        .as.u64 = 0u,
    };

    return result;
}

static inline struct rbt_record rbt_test_record(const struct rbt_value *key, size_t key_count,
                                                const struct rbt_value *value, size_t value_count) {
    struct rbt_record result = {
        .key = key,
        .key_count = key_count,
        .value = value,
        .value_count = value_count,
    };

    return result;
}

static inline struct rbt_record rbt_test_key(const struct rbt_value *key, size_t key_count) {
    return rbt_test_record(key, key_count, NULL, 0u);
}

static inline void rbt_test_temp_path(char *path, size_t capacity) {
    int descriptor;
    int length;

    RBT_TEST_CHECK(capacity >= 32u);
    length = snprintf(path, capacity, "/tmp/rbt-test-XXXXXX");
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < capacity);
    descriptor = mkstemp(path);
    RBT_TEST_CHECK(descriptor >= 0);
    RBT_TEST_CHECK(close(descriptor) == 0);
    RBT_TEST_CHECK(unlink(path) == 0);
}

static inline void rbt_test_remove_database(const char *path) {
    char wal_path[512];
    char wal_temp_path[512];
    int length;

    (void)unlink(path);
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_path));
    (void)unlink(wal_path);
    length = snprintf(wal_temp_path, sizeof(wal_temp_path), "%s.wal.tmp", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_temp_path));
    (void)unlink(wal_temp_path);
}

static inline struct rbt_stats rbt_test_validate(struct rbt *rbt) {
    char error[256] = {0};
    struct rbt_stats stats;
    int result;

    memset(&stats, 0xa5, sizeof(stats));
    result = rbt_validate(rbt, &stats, error, sizeof(error));
    if (result != 0) {
        fprintf(stderr, "validation failed: %d (%s): %s\n", result, rbt_strerror(result), error);
        exit(EXIT_FAILURE);
    }
    return stats;
}

#endif
