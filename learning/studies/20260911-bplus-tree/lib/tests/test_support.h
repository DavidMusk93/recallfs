#ifndef RBT_TEST_SUPPORT_H
#define RBT_TEST_SUPPORT_H

#include "rbt_backends.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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

static inline struct rbt_schema rbt_test_u64_bytes_schema(void) {
    static const struct rbt_column key_columns[] = {
        {.id = 1u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u},
    };
    static const struct rbt_column value_columns[] = {
        {.id = 2u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u},
        {.id = 3u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 8192u},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000010),
        .key_columns = key_columns,
        .key_column_count = sizeof(key_columns) / sizeof(key_columns[0]),
        .value_columns = value_columns,
        .value_column_count = sizeof(value_columns) / sizeof(value_columns[0]),
    };

    return schema;
}

static inline int rbt_test_create_u64_bytes(const struct rbt_storage *storage,
                                            struct rbt **out_rbt) {
    struct rbt_schema schema = rbt_test_u64_bytes_schema();
    struct rbt_config config = {
        .storage = storage,
        .schema = &schema,
    };

    return rbt_create(&config, out_rbt);
}

static inline int rbt_test_put_u64_bytes(struct rbt *rbt, uint64_t key_number,
                                         uint64_t value_number, const void *bytes,
                                         size_t byte_count, bool *out_inserted) {
    struct rbt_value key = rbt_test_u64(key_number);
    struct rbt_value values[2] = {
        rbt_test_u64(value_number),
        rbt_test_bytes(bytes, byte_count),
    };
    struct rbt_record record = rbt_test_record(&key, 1u, values, 2u);

    return rbt_put(rbt, &record, out_inserted);
}

static inline uint32_t rbt_test_load_u32(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

static inline uint64_t rbt_test_load_u64(const unsigned char *data) {
    uint64_t value = 0u;
    unsigned int shift;

    for (shift = 0u; shift < 64u; shift += 8u) {
        value |= (uint64_t)data[shift / 8u] << shift;
    }
    return value;
}

static inline void rbt_test_store_u32(unsigned char *data, uint32_t value) {
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline void rbt_test_store_u64(unsigned char *data, uint64_t value) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline uint32_t rbt_test_crc32c_zeroed(const unsigned char *data, size_t size,
                                              size_t zero_offset, size_t zero_size) {
    uint32_t crc = UINT32_MAX;
    size_t index;

    RBT_TEST_CHECK(zero_offset <= size);
    RBT_TEST_CHECK(zero_size <= size - zero_offset);
    for (index = 0u; index < size; ++index) {
        unsigned char byte =
            index >= zero_offset && index - zero_offset < zero_size ? 0u : data[index];
        unsigned int bit;

        crc ^= byte;
        for (bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);

            crc = (crc >> 1u) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

static inline void rbt_test_read_exact_at(int descriptor, void *data, size_t size, off_t offset) {
    size_t consumed = 0u;

    while (consumed < size) {
        ssize_t count = pread(descriptor, (unsigned char *)data + consumed, size - consumed,
                              offset + (off_t)consumed);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        RBT_TEST_CHECK(count > 0);
        consumed += (size_t)count;
    }
}

static inline void rbt_test_write_exact_at(int descriptor, const void *data, size_t size,
                                           off_t offset) {
    size_t consumed = 0u;

    while (consumed < size) {
        ssize_t count = pwrite(descriptor, (const unsigned char *)data + consumed, size - consumed,
                               offset + (off_t)consumed);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        RBT_TEST_CHECK(count > 0);
        consumed += (size_t)count;
    }
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
