#include "test_support.h"

#include <limits.h>

enum {
    KEY_SPACE = 1024,
    OPERATION_COUNT = 30000,
    KEY_BYTES_SIZE = 5,
    VALUE_BYTES_SIZE = 9,
};

struct model_entry {
    bool present;
    bool flag_is_null;
    bool flag;
    int64_t signed_value;
    unsigned char bytes[VALUE_BYTES_SIZE];
};

struct scan_expectation {
    const struct model_entry *model;
    const size_t *ordered;
    size_t count;
    size_t seen;
};

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;

    value ^= value << 13u;
    value ^= value >> 7u;
    value ^= value << 17u;
    *state = value;
    return value;
}

static int64_t key_i64(size_t index) {
    return (int64_t)(index % 37u) - INT64_C(18);
}

static void key_bytes(size_t index, unsigned char bytes[KEY_BYTES_SIZE]) {
    bytes[0] = (unsigned char)(index >> 8u);
    bytes[1] = 0u;
    bytes[2] = (unsigned char)index;
    bytes[3] = (unsigned char)((index * 29u + 7u) & 0xffu);
    bytes[4] = 0u;
}

static int compare_key_indexes(const void *left_pointer, const void *right_pointer) {
    size_t left = *(const size_t *)left_pointer;
    size_t right = *(const size_t *)right_pointer;
    int64_t left_i64 = key_i64(left);
    int64_t right_i64 = key_i64(right);
    unsigned char left_bytes[KEY_BYTES_SIZE];
    unsigned char right_bytes[KEY_BYTES_SIZE];
    int compared;

    if (left_i64 != right_i64) {
        return left_i64 < right_i64 ? -1 : 1;
    }
    key_bytes(left, left_bytes);
    key_bytes(right, right_bytes);
    compared = memcmp(left_bytes, right_bytes, sizeof(left_bytes));
    return compared < 0 ? 1 : compared > 0 ? -1 : 0;
}

static void update_model_value(struct model_entry *entry, size_t key_index, uint64_t random) {
    size_t index;

    entry->flag_is_null = (random % 5u) == 0u;
    entry->flag = (random & 1u) != 0u;
    entry->signed_value =
        (int64_t)(random % UINT64_C(2000001)) - INT64_C(1000000) + (int64_t)key_index;
    for (index = 0u; index < sizeof(entry->bytes); ++index) {
        entry->bytes[index] =
            (unsigned char)((random >> ((index % 8u) * 8u)) ^ (uint64_t)(index * 31u));
    }
    entry->bytes[3] = 0u;
}

static struct rbt_record make_record(size_t key_index, const struct model_entry *entry,
                                     struct rbt_value values[5],
                                     unsigned char encoded_key[KEY_BYTES_SIZE]) {
    key_bytes(key_index, encoded_key);
    values[0] = rbt_test_i64(key_i64(key_index));
    values[1] = rbt_test_bytes(encoded_key, KEY_BYTES_SIZE);
    values[2] = entry->flag_is_null ? rbt_test_null(RBT_TYPE_BOOL) : rbt_test_bool(entry->flag);
    values[3] = rbt_test_i64(entry->signed_value);
    values[4] = rbt_test_bytes(entry->bytes, sizeof(entry->bytes));
    return rbt_test_record(values, 5u);
}

static struct rbt_record make_key(size_t key_index, struct rbt_value keys[2],
                                  unsigned char encoded_key[KEY_BYTES_SIZE]) {
    key_bytes(key_index, encoded_key);
    keys[0] = rbt_test_i64(key_i64(key_index));
    keys[1] = rbt_test_bytes(encoded_key, KEY_BYTES_SIZE);
    return rbt_test_key(keys, 2u);
}

static void expect_row(const struct rbt_row *row, size_t key_index,
                       const struct model_entry *entry) {
    unsigned char expected_key[KEY_BYTES_SIZE];
    const struct rbt_value *actual = NULL;

    key_bytes(key_index, expected_key);
    RBT_TEST_OK(rbt_row_get(row, 0u, &actual));
    RBT_TEST_CHECK(actual->type == RBT_TYPE_I64);
    RBT_TEST_CHECK(!actual->is_null);
    RBT_TEST_CHECK(actual->as.i64 == key_i64(key_index));
    RBT_TEST_OK(rbt_row_get(row, 1u, &actual));
    RBT_TEST_CHECK(actual->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!actual->is_null);
    RBT_TEST_BYTES(&actual->as.bytes, expected_key, sizeof(expected_key));

    RBT_TEST_OK(rbt_row_get(row, 2u, &actual));
    RBT_TEST_CHECK(actual->type == RBT_TYPE_BOOL);
    RBT_TEST_CHECK(actual->is_null == entry->flag_is_null);
    if (!actual->is_null) {
        RBT_TEST_CHECK(actual->as.boolean == entry->flag);
    }
    RBT_TEST_OK(rbt_row_get(row, 3u, &actual));
    RBT_TEST_CHECK(actual->type == RBT_TYPE_I64);
    RBT_TEST_CHECK(!actual->is_null);
    RBT_TEST_CHECK(actual->as.i64 == entry->signed_value);
    RBT_TEST_OK(rbt_row_get(row, 4u, &actual));
    RBT_TEST_CHECK(actual->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!actual->is_null);
    RBT_TEST_BYTES(&actual->as.bytes, entry->bytes, sizeof(entry->bytes));
}

static enum rbt_scan_action check_scan_row(void *context, const struct rbt_row *row) {
    struct scan_expectation *expectation = context;
    size_t model_index;

    RBT_TEST_CHECK(expectation->seen < expectation->count);
    model_index = expectation->ordered[expectation->seen];
    expect_row(row, model_index, &expectation->model[model_index]);
    ++expectation->seen;
    return RBT_SCAN_CONTINUE;
}

static void check_full_model(struct rbt *tree, const struct model_entry model[KEY_SPACE]) {
    size_t ordered[KEY_SPACE];
    size_t expected_count = 0u;
    size_t index;
    struct scan_expectation expectation;

    for (index = 0u; index < KEY_SPACE; ++index) {
        struct rbt_value keys[2];
        unsigned char encoded_key[KEY_BYTES_SIZE];
        struct rbt_record key = make_key(index, keys, encoded_key);
        struct rbt_row *row = (struct rbt_row *)(uintptr_t)1u;

        if (model[index].present) {
            RBT_TEST_OK(rbt_get(tree, &key, &row));
            expect_row(row, index, &model[index]);
            RBT_TEST_OK(rbt_row_destroy(row));
            ordered[expected_count++] = index;
        } else {
            RBT_TEST_ERRNO(rbt_get(tree, &key, &row), ENOENT);
            RBT_TEST_CHECK(row == NULL);
        }
    }
    qsort(ordered, expected_count, sizeof(ordered[0]), compare_key_indexes);
    expectation.model = model;
    expectation.ordered = ordered;
    expectation.count = expected_count;
    expectation.seen = 0u;
    RBT_TEST_OK(rbt_scan(tree, NULL, NULL, check_scan_row, &expectation));
    RBT_TEST_CHECK(expectation.seen == expected_count);
    RBT_TEST_CHECK(rbt_test_validate(tree).item_count == expected_count);
}

int main(void) {
    static const struct rbt_column COLUMNS[5] = {
        {.id = 10u, .type = RBT_TYPE_I64, .flags = RBT_COLUMN_KEY, .max_size = 0u},
        {.id = 11u,
         .type = RBT_TYPE_BYTES,
         .flags = RBT_COLUMN_KEY | RBT_COLUMN_DESCENDING,
         .max_size = KEY_BYTES_SIZE},
        {.id = 20u, .type = RBT_TYPE_BOOL, .flags = RBT_COLUMN_NULLABLE, .max_size = 0u},
        {.id = 21u, .type = RBT_TYPE_I64, .flags = 0u, .max_size = 0u},
        {.id = 22u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = VALUE_BYTES_SIZE},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000011),
        .columns = COLUMNS,
        .column_count = sizeof(COLUMNS) / sizeof(COLUMNS[0]),
    };
    struct model_entry model[KEY_SPACE];
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    uint64_t random_state = UINT64_C(0x4d595df4d0f33173);
    size_t operation;

    memset(model, 0, sizeof(model));
    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));
    RBT_TEST_OK(rbt_create(&config, &tree));
    check_full_model(tree, model);

    for (operation = 0u; operation < OPERATION_COUNT; ++operation) {
        uint64_t random = next_random(&random_state);
        size_t key_index = (size_t)(random % KEY_SPACE);
        uint64_t action = (random >> 32u) % 10u;
        struct rbt_value keys[2];
        unsigned char encoded_key[KEY_BYTES_SIZE];
        struct rbt_record key = make_key(key_index, keys, encoded_key);

        if (action < 5u) {
            struct rbt_value values[5];
            struct rbt_record record;
            bool was_present = model[key_index].present;
            bool inserted = !was_present;

            update_model_value(&model[key_index], key_index, next_random(&random_state));
            record = make_record(key_index, &model[key_index], values, encoded_key);
            RBT_TEST_OK(rbt_put(tree, &record, &inserted));
            RBT_TEST_CHECK(inserted == !was_present);
            model[key_index].present = true;
        } else if (action < 8u) {
            bool was_present = model[key_index].present;
            bool deleted = true;

            if (was_present) {
                RBT_TEST_OK(rbt_delete(tree, &key, &deleted));
                RBT_TEST_CHECK(deleted);
                model[key_index].present = false;
            } else {
                RBT_TEST_ERRNO(rbt_delete(tree, &key, &deleted), ENOENT);
                RBT_TEST_CHECK(!deleted);
            }
        } else {
            struct rbt_row *row = (struct rbt_row *)(uintptr_t)1u;

            if (model[key_index].present) {
                RBT_TEST_OK(rbt_get(tree, &key, &row));
                expect_row(row, key_index, &model[key_index]);
                RBT_TEST_OK(rbt_row_destroy(row));
            } else {
                RBT_TEST_ERRNO(rbt_get(tree, &key, &row), ENOENT);
                RBT_TEST_CHECK(row == NULL);
            }
        }

        if ((operation % 251u) == 0u) {
            check_full_model(tree, model);
            RBT_TEST_OK(rbt_destroy(tree));
            tree = NULL;
            RBT_TEST_OK(rbt_open(&storage, &tree));
        }
    }

    check_full_model(tree, model);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_mem_destroy(memory));
    return 0;
}
