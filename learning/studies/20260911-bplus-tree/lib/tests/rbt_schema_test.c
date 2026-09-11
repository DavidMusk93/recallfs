#include "test_support.h"

#include <limits.h>

_Static_assert(RBT_VERSION_MAJOR == 0, "unexpected major version");
_Static_assert(RBT_VERSION_MINOR == 1, "unexpected minor version");
_Static_assert(RBT_VERSION_PATCH == 0, "unexpected patch version");
_Static_assert(RBT_FORMAT_VERSION == 1u, "unexpected format version");
_Static_assert(RBT_MIN_PAGE_SIZE == 512u, "unexpected minimum page size");
_Static_assert(RBT_MAX_PAGE_SIZE == 65536u, "unexpected maximum page size");
_Static_assert(RBT_MAX_COLUMNS == 64u, "unexpected column limit");
_Static_assert(RBT_MAX_FIELD_SIZE == 1024u * 1024u, "unexpected field limit");
_Static_assert(RBT_MAX_ROW_SIZE == 4u * 1024u * 1024u, "unexpected row limit");

static const unsigned char KEY_MIN_BYTES[] = {'z'};
static const unsigned char KEY_ZERO_HIGH_BYTES[] = {'b', '\0'};
static const unsigned char KEY_ZERO_LOW_BYTES[] = {'a', '\0'};
static const unsigned char KEY_MAX_BYTES[] = {'x'};

struct scan_expectation {
    const int64_t *i64_keys;
    const unsigned char *const *byte_keys;
    const size_t *byte_sizes;
    const uint64_t *values;
    size_t count;
    size_t seen;
};

static void expect_schema(const struct rbt_schema *schema) {
    RBT_TEST_CHECK(schema != NULL);
    RBT_TEST_CHECK(schema->id == UINT64_C(0x7262740000000001));
    RBT_TEST_CHECK(schema->key_column_count == 2u);
    RBT_TEST_CHECK(schema->value_column_count == 3u);

    RBT_TEST_CHECK(schema->key_columns[0].id == 10u);
    RBT_TEST_CHECK(schema->key_columns[0].type == RBT_TYPE_I64);
    RBT_TEST_CHECK(schema->key_columns[0].flags == 0u);
    RBT_TEST_CHECK(schema->key_columns[0].max_size == 0u);
    RBT_TEST_CHECK(schema->key_columns[1].id == 11u);
    RBT_TEST_CHECK(schema->key_columns[1].type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(schema->key_columns[1].flags == RBT_COLUMN_DESCENDING);
    RBT_TEST_CHECK(schema->key_columns[1].max_size == 8u);

    RBT_TEST_CHECK(schema->value_columns[0].id == 20u);
    RBT_TEST_CHECK(schema->value_columns[0].type == RBT_TYPE_BOOL);
    RBT_TEST_CHECK(schema->value_columns[0].flags == RBT_COLUMN_NULLABLE);
    RBT_TEST_CHECK(schema->value_columns[1].id == 21u);
    RBT_TEST_CHECK(schema->value_columns[1].type == RBT_TYPE_U64);
    RBT_TEST_CHECK(schema->value_columns[2].id == 22u);
    RBT_TEST_CHECK(schema->value_columns[2].type == RBT_TYPE_UTF8);
    RBT_TEST_CHECK(schema->value_columns[2].flags == RBT_COLUMN_NULLABLE);
    RBT_TEST_CHECK(schema->value_columns[2].max_size == 32u);
}

static void put_row(struct rbt *tree, int64_t signed_key, const void *byte_key,
                    size_t byte_key_size, struct rbt_value bool_value, uint64_t number,
                    struct rbt_value text_value) {
    struct rbt_value keys[2] = {
        rbt_test_i64(signed_key),
        rbt_test_bytes(byte_key, byte_key_size),
    };
    struct rbt_value values[3] = {
        bool_value,
        rbt_test_u64(number),
        text_value,
    };
    struct rbt_record record = rbt_test_record(keys, 2u, values, 3u);
    bool inserted = false;

    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);
}

static enum rbt_scan_action check_scan_row(void *context, const struct rbt_row *row) {
    struct scan_expectation *expected = context;
    const struct rbt_value *signed_key = NULL;
    const struct rbt_value *byte_key = NULL;
    const struct rbt_value *number = NULL;
    size_t index = expected->seen;

    RBT_TEST_CHECK(index < expected->count);
    RBT_TEST_ERRNO(rbt_row_destroy((struct rbt_row *)row), EINVAL);
    RBT_TEST_OK(rbt_row_get_key(row, 0u, &signed_key));
    RBT_TEST_OK(rbt_row_get_key(row, 1u, &byte_key));
    RBT_TEST_OK(rbt_row_get_value(row, 1u, &number));
    RBT_TEST_CHECK(signed_key->type == RBT_TYPE_I64);
    RBT_TEST_CHECK(!signed_key->is_null);
    RBT_TEST_CHECK(signed_key->as.i64 == expected->i64_keys[index]);
    RBT_TEST_CHECK(byte_key->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!byte_key->is_null);
    RBT_TEST_BYTES(&byte_key->as.bytes, expected->byte_keys[index], expected->byte_sizes[index]);
    RBT_TEST_CHECK(number->type == RBT_TYPE_U64);
    RBT_TEST_CHECK(number->as.u64 == expected->values[index]);
    expected->seen++;
    return RBT_SCAN_CONTINUE;
}

static enum rbt_scan_action stop_after_one(void *context, const struct rbt_row *row) {
    size_t *seen = context;
    const struct rbt_value *key = NULL;

    RBT_TEST_OK(rbt_row_get_key(row, 0u, &key));
    RBT_TEST_CHECK(key != NULL);
    (*seen)++;
    return RBT_SCAN_STOP;
}

static void test_persisted_schema_and_rows(void) {
    static const int64_t EXPECTED_I64[] = {INT64_MIN, 0, 0, INT64_MAX};
    static const unsigned char *const EXPECTED_BYTES[] = {
        KEY_MIN_BYTES,
        KEY_ZERO_HIGH_BYTES,
        KEY_ZERO_LOW_BYTES,
        KEY_MAX_BYTES,
    };
    static const size_t EXPECTED_BYTE_SIZES[] = {1u, 2u, 2u, 1u};
    static const uint64_t EXPECTED_VALUES[] = {1u, 2u, 3u, 4u};
    struct rbt_column key_columns[2] = {
        {.id = 10u, .type = RBT_TYPE_I64, .flags = 0u, .max_size = 0u},
        {.id = 11u, .type = RBT_TYPE_BYTES, .flags = RBT_COLUMN_DESCENDING, .max_size = 8u},
    };
    struct rbt_column value_columns[3] = {
        {.id = 20u, .type = RBT_TYPE_BOOL, .flags = RBT_COLUMN_NULLABLE, .max_size = 0u},
        {.id = 21u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u},
        {.id = 22u, .type = RBT_TYPE_UTF8, .flags = RBT_COLUMN_NULLABLE, .max_size = 32u},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000001),
        .key_columns = key_columns,
        .key_column_count = 2u,
        .value_columns = value_columns,
        .value_column_count = 3u,
    };
    char path[256];
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt_config create_config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    const struct rbt_schema *actual_schema = NULL;
    struct rbt_row *owned_row = NULL;
    unsigned char mutable_key[] = {'b', '\0'};
    char mutable_text[] = "hello";
    struct rbt_value lookup_values[2];
    struct rbt_record lookup;
    struct scan_expectation expected = {
        .i64_keys = EXPECTED_I64,
        .byte_keys = EXPECTED_BYTES,
        .byte_sizes = EXPECTED_BYTE_SIZES,
        .values = EXPECTED_VALUES,
        .count = 4u,
        .seen = 0u,
    };
    const struct rbt_value *actual_bool = NULL;
    const struct rbt_value *actual_text = NULL;
    size_t stopped = 0u;

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_OK(rbt_file_create(path, 512u, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_create(&create_config, &tree));

    key_columns[0].id = 999u;
    key_columns[1].max_size = 1u;
    value_columns[2].flags = 0u;
    RBT_TEST_OK(rbt_get_schema(tree, &actual_schema));
    expect_schema(actual_schema);

    put_row(tree, INT64_MIN, KEY_MIN_BYTES, sizeof(KEY_MIN_BYTES), rbt_test_bool(false), 1u,
            rbt_test_utf8("minimum", 7u));
    put_row(tree, 0, mutable_key, sizeof(mutable_key), rbt_test_null(RBT_TYPE_BOOL), 2u,
            rbt_test_utf8(mutable_text, strlen(mutable_text)));
    mutable_key[0] = 'q';
    mutable_text[0] = 'X';
    put_row(tree, 0, KEY_ZERO_LOW_BYTES, sizeof(KEY_ZERO_LOW_BYTES), rbt_test_bool(true), 3u,
            rbt_test_null(RBT_TYPE_UTF8));
    put_row(tree, INT64_MAX, KEY_MAX_BYTES, sizeof(KEY_MAX_BYTES), rbt_test_bool(false), 4u,
            rbt_test_utf8("maximum", 7u));

    lookup_values[0] = rbt_test_i64(0);
    lookup_values[1] = rbt_test_bytes(KEY_ZERO_HIGH_BYTES, sizeof(KEY_ZERO_HIGH_BYTES));
    lookup = rbt_test_key(lookup_values, 2u);
    RBT_TEST_OK(rbt_get(tree, &lookup, &owned_row));
    RBT_TEST_OK(rbt_scan(tree, NULL, NULL, check_scan_row, &expected));
    RBT_TEST_CHECK(expected.seen == expected.count);
    RBT_TEST_OK(rbt_scan(tree, NULL, NULL, stop_after_one, &stopped));
    RBT_TEST_CHECK(stopped == 1u);

    RBT_TEST_OK(rbt_row_get_value(owned_row, 0u, &actual_bool));
    RBT_TEST_CHECK(actual_bool->type == RBT_TYPE_BOOL);
    RBT_TEST_CHECK(actual_bool->is_null);
    RBT_TEST_OK(rbt_row_get_value(owned_row, 2u, &actual_text));
    RBT_TEST_CHECK(!actual_text->is_null);
    RBT_TEST_BYTES(&actual_text->as.bytes, "hello", 5u);
    RBT_TEST_OK(rbt_row_destroy(owned_row));
    owned_row = NULL;

    RBT_TEST_CHECK(rbt_test_validate(tree).item_count == 4u);
    RBT_TEST_OK(rbt_destroy(tree));
    tree = NULL;
    RBT_TEST_OK(rbt_file_close(file));
    file = NULL;

    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    RBT_TEST_OK(rbt_get_schema(tree, &actual_schema));
    expect_schema(actual_schema);
    RBT_TEST_OK(rbt_get(tree, &lookup, &owned_row));
    RBT_TEST_OK(rbt_row_get_value(owned_row, 2u, &actual_text));
    RBT_TEST_BYTES(&actual_text->as.bytes, "hello", 5u);
    RBT_TEST_OK(rbt_row_destroy(owned_row));

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));
    rbt_test_remove_database(path);
}

static void expect_invalid_create(const struct rbt_storage *storage,
                                  const struct rbt_schema *schema) {
    struct rbt_config config = {
        .storage = storage,
        .schema = schema,
    };
    struct rbt *tree = (struct rbt *)(uintptr_t)1u;

    RBT_TEST_ERRNO(rbt_create(&config, &tree), EINVAL);
    RBT_TEST_CHECK(tree == NULL);
}

static void test_schema_and_record_validation(void) {
    struct rbt_column key_column = {
        .id = 1u,
        .type = RBT_TYPE_U64,
        .flags = 0u,
        .max_size = 0u,
    };
    struct rbt_column value_columns[3] = {
        {.id = 2u, .type = RBT_TYPE_BOOL, .flags = 0u, .max_size = 0u},
        {.id = 3u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 3u},
        {.id = 4u, .type = RBT_TYPE_UTF8, .flags = RBT_COLUMN_NULLABLE, .max_size = 4u},
    };
    struct rbt_schema schema = {
        .id = 7u,
        .key_columns = &key_column,
        .key_column_count = 1u,
        .value_columns = value_columns,
        .value_column_count = 3u,
    };
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    struct rbt_value key = rbt_test_u64(9u);
    struct rbt_value values[3] = {
        rbt_test_bool(true),
        rbt_test_bytes("abc", 3u),
        rbt_test_null(RBT_TYPE_UTF8),
    };
    struct rbt_record record = rbt_test_record(&key, 1u, values, 3u);
    struct rbt_record key_record;
    bool inserted = true;
    bool deleted = true;
    struct rbt_column invalid_column;
    struct rbt_row *row = (struct rbt_row *)(uintptr_t)1u;
    const struct rbt_value *value_view = (const struct rbt_value *)(uintptr_t)1u;

    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));

    invalid_column = key_column;
    invalid_column.max_size = 1u;
    schema.key_columns = &invalid_column;
    expect_invalid_create(&storage, &schema);
    invalid_column = key_column;
    invalid_column.type = RBT_TYPE_BYTES;
    invalid_column.max_size = 0u;
    schema.key_columns = &invalid_column;
    expect_invalid_create(&storage, &schema);
    invalid_column = key_column;
    invalid_column.type = (enum rbt_type)99;
    schema.key_columns = &invalid_column;
    expect_invalid_create(&storage, &schema);
    invalid_column = key_column;
    invalid_column.flags = UINT32_C(0x80000000);
    schema.key_columns = &invalid_column;
    expect_invalid_create(&storage, &schema);
    invalid_column = key_column;
    invalid_column.id = value_columns[0].id;
    schema.key_columns = &invalid_column;
    expect_invalid_create(&storage, &schema);
    schema.key_columns = &key_column;
    value_columns[0].flags = RBT_COLUMN_DESCENDING;
    expect_invalid_create(&storage, &schema);
    value_columns[0].flags = 0u;

    RBT_TEST_OK(rbt_create(&config, &tree));
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);
    key_record = rbt_test_key(&key, 1u);
    RBT_TEST_OK(rbt_get(tree, &key_record, &row));
    RBT_TEST_ERRNO(rbt_row_get_value(row, 3u, &value_view), EINVAL);
    RBT_TEST_CHECK(value_view == NULL);
    RBT_TEST_OK(rbt_row_destroy(row));

    inserted = true;
    key.type = RBT_TYPE_I64;
    RBT_TEST_ERRNO(rbt_put(tree, &record, &inserted), EINVAL);
    RBT_TEST_CHECK(!inserted);
    key = rbt_test_u64(10u);
    values[0] = rbt_test_null(RBT_TYPE_BOOL);
    RBT_TEST_ERRNO(rbt_put(tree, &record, &inserted), EINVAL);
    RBT_TEST_CHECK(!inserted);
    values[0] = rbt_test_bool(false);
    values[1] = rbt_test_bytes("four", 4u);
    RBT_TEST_ERRNO(rbt_put(tree, &record, &inserted), EINVAL);
    RBT_TEST_CHECK(!inserted);
    values[1] = rbt_test_bytes("ok", 2u);
    {
        static const unsigned char INVALID_UTF8[] = {0xc3u, 0x28u};

        values[2] = rbt_test_utf8((const char *)INVALID_UTF8, sizeof(INVALID_UTF8));
        RBT_TEST_ERRNO(rbt_put(tree, &record, &inserted), EINVAL);
        RBT_TEST_CHECK(!inserted);
    }

    key = rbt_test_u64(99u);
    key_record = rbt_test_key(&key, 1u);
    row = (struct rbt_row *)(uintptr_t)1u;
    RBT_TEST_ERRNO(rbt_get(tree, &key_record, &row), ENOENT);
    RBT_TEST_CHECK(row == NULL);
    RBT_TEST_ERRNO(rbt_delete(tree, &key_record, &deleted), ENOENT);
    RBT_TEST_CHECK(!deleted);

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_mem_destroy(memory));
}

static void test_defensive_out_parameters(void) {
    struct rbt_mem *memory = (struct rbt_mem *)(uintptr_t)1u;
    struct rbt_file *file = (struct rbt_file *)(uintptr_t)1u;
    struct rbt *tree = (struct rbt *)(uintptr_t)1u;
    struct rbt_row *row = (struct rbt_row *)(uintptr_t)1u;
    const struct rbt_schema *schema = (const struct rbt_schema *)(uintptr_t)1u;
    struct rbt_storage storage;
    struct rbt_stats stats;

    RBT_TEST_ERRNO(rbt_mem_create(511u, &memory), EINVAL);
    RBT_TEST_CHECK(memory == NULL);
    RBT_TEST_ERRNO(rbt_file_open(NULL, &file), EINVAL);
    RBT_TEST_CHECK(file == NULL);

    memset(&storage, 0xa5, sizeof(storage));
    RBT_TEST_ERRNO(rbt_mem_storage(NULL, &storage), EINVAL);
    RBT_TEST_CHECK(storage.context == NULL);
    RBT_TEST_CHECK(storage.page_size == 0u);
    RBT_TEST_CHECK(storage.read_page == NULL);
    RBT_TEST_CHECK(storage.page_count == NULL);
    RBT_TEST_CHECK(storage.commit_pages == NULL);

    RBT_TEST_ERRNO(rbt_create(NULL, &tree), EINVAL);
    RBT_TEST_CHECK(tree == NULL);
    tree = (struct rbt *)(uintptr_t)1u;
    RBT_TEST_ERRNO(rbt_open(NULL, &tree), EINVAL);
    RBT_TEST_CHECK(tree == NULL);
    RBT_TEST_ERRNO(rbt_get_schema(NULL, &schema), EINVAL);
    RBT_TEST_CHECK(schema == NULL);
    RBT_TEST_ERRNO(rbt_get(NULL, NULL, &row), EINVAL);
    RBT_TEST_CHECK(row == NULL);

    memset(&stats, 0xa5, sizeof(stats));
    RBT_TEST_ERRNO(rbt_validate(NULL, &stats, NULL, 0u), EINVAL);
    {
        const unsigned char zero[sizeof(stats)] = {0};

        RBT_TEST_CHECK(memcmp(&stats, zero, sizeof(stats)) == 0);
    }
}

int main(void) {
    test_persisted_schema_and_rows();
    test_schema_and_record_validation();
    test_defensive_out_parameters();
    return 0;
}
