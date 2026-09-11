#include "btree_backends.h"
#include "test_support.h"

enum {
    BYTE_KEY_SIZE = 13,
    BYTE_VALUE_SIZE = 21,
    BYTE_ITEM_COUNT = 96,
    ODD_SPLIT_ITEM_COUNT = 14,
    NUMERIC_KEY_SIZE = 8,
    NUMERIC_VALUE_SIZE = 5,
    NUMERIC_ITEM_COUNT = 2048
};

#define NUMERIC_COMPARATOR_ID (BTREE_COMPARATOR_USER_MIN + UINT64_C(17))
#define REENTRANT_COMPARATOR_ID (BTREE_COMPARATOR_USER_MIN + UINT64_C(18))

typedef struct byte_scan_context {
    uint64_t expected;
    uint64_t seen;
} byte_scan_context;

typedef struct numeric_compare_context {
    uint64_t calls;
} numeric_compare_context;

typedef struct reentrant_compare_context {
    btree *tree;
    bool attempt_nested_put;
    bool nested_put_called;
    btree_status nested_status;
    bool nested_inserted;
    unsigned char nested_key[NUMERIC_KEY_SIZE];
    unsigned char nested_value[NUMERIC_VALUE_SIZE];
} reentrant_compare_context;

typedef struct numeric_scan_context {
    uint64_t expected;
    uint64_t seen;
    bool even_only;
} numeric_scan_context;

static void byte_key(uint64_t index, unsigned char key[BYTE_KEY_SIZE]) {
    size_t offset;

    for (offset = 0u; offset < BYTE_KEY_SIZE; ++offset) {
        key[offset] = (unsigned char)((index * UINT64_C(37) + (uint64_t)offset * UINT64_C(53)) &
                                      UINT64_C(0xff));
    }
    key[0] = (unsigned char)(index >> 8u);
    key[1] = (unsigned char)index;
    key[4] = 0u;
    key[9] = 0u;
}

static void byte_value(uint64_t index, unsigned char value[BYTE_VALUE_SIZE]) {
    size_t offset;

    for (offset = 0u; offset < BYTE_VALUE_SIZE; ++offset) {
        value[offset] = (unsigned char)((index * UINT64_C(71) + (uint64_t)offset * UINT64_C(29) +
                                         UINT64_C(0xa5)) &
                                        UINT64_C(0xff));
    }
    value[3] = 0u;
    value[BYTE_VALUE_SIZE - 2u] = 0u;
}

static btree_scan_action check_byte_item(void *context, const void *key, const void *value) {
    byte_scan_context *scan = context;
    unsigned char expected_key[BYTE_KEY_SIZE];
    unsigned char expected_value[BYTE_VALUE_SIZE];

    byte_key(scan->expected, expected_key);
    byte_value(scan->expected, expected_value);
    TEST_CHECK(memcmp(key, expected_key, sizeof(expected_key)) == 0);
    TEST_CHECK(memcmp(value, expected_value, sizeof(expected_value)) == 0);
    scan->expected++;
    scan->seen++;
    return BTREE_SCAN_CONTINUE;
}

static void encode_u64_little_endian(uint64_t value, unsigned char encoded[NUMERIC_KEY_SIZE]) {
    size_t offset;

    for (offset = 0u; offset < NUMERIC_KEY_SIZE; ++offset) {
        encoded[offset] = (unsigned char)(value >> (offset * 8u));
    }
}

static uint64_t decode_u64_little_endian(const void *encoded) {
    const unsigned char *bytes = encoded;
    uint64_t value = 0u;
    size_t offset;

    for (offset = 0u; offset < NUMERIC_KEY_SIZE; ++offset) {
        value |= (uint64_t)bytes[offset] << (offset * 8u);
    }
    return value;
}

static void numeric_value(uint64_t key, unsigned char value[NUMERIC_VALUE_SIZE]) {
    value[0] = (unsigned char)(key >> 8u);
    value[1] = 0u;
    value[2] = (unsigned char)(key * UINT64_C(17) + UINT64_C(3));
    value[3] = (unsigned char)(key >> 3u);
    value[4] = (unsigned char)(key ^ UINT64_C(0x5a));
}

static int compare_u64_little_endian(void *context, const void *left_key, const void *right_key,
                                     size_t key_size) {
    numeric_compare_context *comparison = context;
    uint64_t left_value;
    uint64_t right_value;

    TEST_CHECK(comparison != NULL);
    TEST_CHECK(key_size == NUMERIC_KEY_SIZE);
    comparison->calls++;
    left_value = decode_u64_little_endian(left_key);
    right_value = decode_u64_little_endian(right_key);
    return (left_value > right_value) - (left_value < right_value);
}

static int compare_bytes(void *context, const void *left_key, const void *right_key,
                         size_t key_size) {
    TEST_CHECK(context == NULL);
    return memcmp(left_key, right_key, key_size);
}

static int compare_with_nested_put(void *context, const void *left_key, const void *right_key,
                                   size_t key_size) {
    reentrant_compare_context *comparison = context;
    uint64_t left_value;
    uint64_t right_value;

    TEST_CHECK(comparison != NULL);
    TEST_CHECK(key_size == NUMERIC_KEY_SIZE);
    if (comparison->attempt_nested_put) {
        comparison->attempt_nested_put = false;
        comparison->nested_put_called = true;
        comparison->nested_status =
            btree_put(comparison->tree, comparison->nested_key, comparison->nested_value,
                      &comparison->nested_inserted);
    }
    left_value = decode_u64_little_endian(left_key);
    right_value = decode_u64_little_endian(right_key);
    return (left_value > right_value) - (left_value < right_value);
}

static btree_scan_action check_numeric_item(void *context, const void *key, const void *value) {
    numeric_scan_context *scan = context;
    unsigned char expected_value[NUMERIC_VALUE_SIZE];
    uint64_t decoded_key = decode_u64_little_endian(key);

    if (scan->even_only && (scan->expected & UINT64_C(1)) != 0u) {
        scan->expected++;
    }
    TEST_CHECK(decoded_key == scan->expected);
    numeric_value(decoded_key, expected_value);
    TEST_CHECK(memcmp(value, expected_value, sizeof(expected_value)) == 0);
    scan->expected += scan->even_only ? 2u : 1u;
    scan->seen++;
    return BTREE_SCAN_CONTINUE;
}

static uint64_t shuffled_numeric_key(uint64_t index) {
    return (index * UINT64_C(1301) + UINT64_C(977)) & ((uint64_t)NUMERIC_ITEM_COUNT - UINT64_C(1));
}

static void assert_schema(const btree_schema *schema, uint32_t key_size, uint32_t value_size,
                          uint64_t comparator_id) {
    TEST_CHECK(schema->key_size == key_size);
    TEST_CHECK(schema->value_size == value_size);
    TEST_CHECK(schema->comparator_id == comparator_id);
}

static void test_file_byte_schema_and_copy_semantics(void) {
    char path[256];
    btree_file *backend = NULL;
    btree_storage storage;
    btree_options options;
    btree_options wrong;
    btree_schema schema;
    btree *tree = NULL;
    byte_scan_context scan;
    uint64_t index;

    test_temp_path(path, sizeof(path));
    btree_options_init(&options, BYTE_KEY_SIZE, BYTE_VALUE_SIZE);
    TEST_STATUS(btree_file_create(path, 512u, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(btree_create(&storage, &options, &tree), BTREE_OK);

    for (index = 0u; index < BYTE_ITEM_COUNT; ++index) {
        uint64_t item = BYTE_ITEM_COUNT - 1u - index;
        unsigned char key[BYTE_KEY_SIZE];
        unsigned char value[BYTE_VALUE_SIZE];
        bool inserted = false;

        byte_key(item, key);
        byte_value(item, value);
        TEST_CHECK(key[4] == 0u && key[9] == 0u);
        TEST_CHECK(value[3] == 0u && value[BYTE_VALUE_SIZE - 2u] == 0u);
        TEST_STATUS(btree_put(tree, key, value, &inserted), BTREE_OK);
        TEST_CHECK(inserted);

        memset(key, 0x3c, sizeof(key));
        memset(value, 0xc3, sizeof(value));
    }

    test_validate(tree);
    memset(&scan, 0, sizeof(scan));
    TEST_STATUS(btree_scan(tree, NULL, NULL, check_byte_item, &scan), BTREE_OK);
    TEST_CHECK(scan.seen == BYTE_ITEM_COUNT);
    btree_close(tree);
    tree = NULL;
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    backend = NULL;

    TEST_STATUS(btree_file_open(path, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    memset(&schema, 0xa5, sizeof(schema));
    TEST_STATUS(btree_read_schema(&storage, &schema), BTREE_OK);
    assert_schema(&schema, BYTE_KEY_SIZE, BYTE_VALUE_SIZE, BTREE_COMPARATOR_LEXICOGRAPHIC);

    wrong = options;
    wrong.key_size++;
    TEST_STATUS(btree_open(&storage, &wrong, &tree), BTREE_SCHEMA_MISMATCH);
    TEST_CHECK(tree == NULL);
    wrong = options;
    wrong.value_size++;
    TEST_STATUS(btree_open(&storage, &wrong, &tree), BTREE_SCHEMA_MISMATCH);
    TEST_CHECK(tree == NULL);
    wrong = options;
    wrong.comparator_id = BTREE_COMPARATOR_USER_MIN;
    wrong.compare = compare_bytes;
    TEST_STATUS(btree_open(&storage, &wrong, &tree), BTREE_SCHEMA_MISMATCH);
    TEST_CHECK(tree == NULL);

    TEST_STATUS(btree_open(&storage, &options, &tree), BTREE_OK);
    test_validate(tree);
    memset(&scan, 0, sizeof(scan));
    TEST_STATUS(btree_scan(tree, NULL, NULL, check_byte_item, &scan), BTREE_OK);
    TEST_CHECK(scan.seen == BYTE_ITEM_COUNT);

    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    test_remove_database(path);
}

static void test_file_odd_capacity_leaf_split(void) {
    char path[256];
    btree_file *backend = NULL;
    btree_storage storage;
    btree_options options;
    btree_stats stats;
    btree *tree = NULL;
    uint64_t index;

    test_temp_path(path, sizeof(path));
    btree_options_init(&options, BYTE_KEY_SIZE, BYTE_VALUE_SIZE);
    TEST_STATUS(btree_file_create(path, 512u, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(btree_create(&storage, &options, &tree), BTREE_OK);

    for (index = 0u; index < ODD_SPLIT_ITEM_COUNT; ++index) {
        unsigned char key[BYTE_KEY_SIZE];
        unsigned char value[BYTE_VALUE_SIZE];
        bool inserted = false;

        byte_key(index, key);
        byte_value(index, value);
        TEST_STATUS(btree_put(tree, key, value, &inserted), BTREE_OK);
        TEST_CHECK(inserted);
    }
    TEST_STATUS(btree_validate(tree, &stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(stats.item_count == ODD_SPLIT_ITEM_COUNT);
    TEST_CHECK(stats.leaf_capacity == 13u);
    TEST_CHECK(stats.height == 2u);

    btree_close(tree);
    tree = NULL;
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    backend = NULL;
    TEST_STATUS(btree_file_open(path, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(btree_open(&storage, &options, &tree), BTREE_OK);
    TEST_STATUS(btree_validate(tree, &stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(stats.item_count == ODD_SPLIT_ITEM_COUNT);

    for (index = 0u; index < ODD_SPLIT_ITEM_COUNT; ++index) {
        unsigned char key[BYTE_KEY_SIZE];
        unsigned char expected[BYTE_VALUE_SIZE];
        unsigned char actual[BYTE_VALUE_SIZE];

        byte_key(index, key);
        byte_value(index, expected);
        TEST_STATUS(btree_get(tree, key, actual), BTREE_OK);
        TEST_CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    }

    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    test_remove_database(path);
}

static void test_invalid_comparator_configuration(void) {
    btree_mem *backend = NULL;
    btree_storage storage;
    btree_options valid;
    btree_options invalid;
    btree *tree = NULL;

    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    btree_options_init(&valid, NUMERIC_KEY_SIZE, NUMERIC_VALUE_SIZE);

    invalid = valid;
    invalid.comparator_id = BTREE_COMPARATOR_USER_MIN;
    TEST_STATUS(btree_create(&storage, &invalid, &tree), BTREE_INVALID_ARGUMENT);
    TEST_CHECK(tree == NULL);
    invalid = valid;
    invalid.compare = compare_bytes;
    TEST_STATUS(btree_create(&storage, &invalid, &tree), BTREE_INVALID_ARGUMENT);
    TEST_CHECK(tree == NULL);
    invalid.comparator_id = BTREE_COMPARATOR_USER_MIN - UINT64_C(1);
    TEST_STATUS(btree_create(&storage, &invalid, &tree), BTREE_INVALID_ARGUMENT);
    TEST_CHECK(tree == NULL);

    TEST_STATUS(btree_create(&storage, &valid, &tree), BTREE_OK);
    btree_close(tree);
    tree = NULL;

    invalid = valid;
    invalid.comparator_id = BTREE_COMPARATOR_USER_MIN;
    TEST_STATUS(btree_open(&storage, &invalid, &tree), BTREE_INVALID_ARGUMENT);
    TEST_CHECK(tree == NULL);
    invalid = valid;
    invalid.compare = compare_bytes;
    TEST_STATUS(btree_open(&storage, &invalid, &tree), BTREE_INVALID_ARGUMENT);
    TEST_CHECK(tree == NULL);
    invalid.comparator_id = BTREE_COMPARATOR_USER_MIN - UINT64_C(1);
    TEST_STATUS(btree_open(&storage, &invalid, &tree), BTREE_INVALID_ARGUMENT);
    TEST_CHECK(tree == NULL);

    btree_mem_destroy(backend);
}

static void test_custom_comparator_reentry_is_busy(void) {
    btree_mem *backend = NULL;
    btree_storage storage;
    btree_options options;
    btree_stats stats;
    btree *tree = NULL;
    reentrant_compare_context comparison;
    unsigned char first_key[NUMERIC_KEY_SIZE];
    unsigned char first_value[NUMERIC_VALUE_SIZE];
    unsigned char second_key[NUMERIC_KEY_SIZE];
    unsigned char second_value[NUMERIC_VALUE_SIZE];
    unsigned char actual[NUMERIC_VALUE_SIZE];
    bool inserted = false;

    memset(&comparison, 0, sizeof(comparison));
    btree_options_init(&options, NUMERIC_KEY_SIZE, NUMERIC_VALUE_SIZE);
    options.comparator_id = REENTRANT_COMPARATOR_ID;
    options.compare = compare_with_nested_put;
    options.compare_context = &comparison;
    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    TEST_STATUS(btree_create(&storage, &options, &tree), BTREE_OK);
    comparison.tree = tree;

    encode_u64_little_endian(UINT64_C(1), first_key);
    numeric_value(UINT64_C(1), first_value);
    TEST_STATUS(btree_put(tree, first_key, first_value, &inserted), BTREE_OK);
    TEST_CHECK(inserted);

    encode_u64_little_endian(UINT64_C(3), comparison.nested_key);
    numeric_value(UINT64_C(3), comparison.nested_value);
    encode_u64_little_endian(UINT64_C(2), second_key);
    numeric_value(UINT64_C(2), second_value);
    inserted = false;
    comparison.attempt_nested_put = true;
    TEST_STATUS(btree_put(tree, second_key, second_value, &inserted), BTREE_OK);
    TEST_CHECK(inserted);
    TEST_CHECK(comparison.nested_put_called);
    TEST_STATUS(comparison.nested_status, BTREE_BUSY);
    TEST_CHECK(!comparison.nested_inserted);

    TEST_STATUS(btree_validate(tree, &stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(stats.item_count == 2u);
    TEST_STATUS(btree_get(tree, comparison.nested_key, actual), BTREE_NOT_FOUND);
    TEST_STATUS(btree_get(tree, first_key, actual), BTREE_OK);
    TEST_CHECK(memcmp(actual, first_value, sizeof(actual)) == 0);
    TEST_STATUS(btree_get(tree, second_key, actual), BTREE_OK);
    TEST_CHECK(memcmp(actual, second_value, sizeof(actual)) == 0);

    btree_close(tree);
    btree_mem_destroy(backend);
}

static void test_custom_little_endian_comparator(void) {
    btree_mem *backend = NULL;
    btree_storage storage;
    btree_options options;
    btree_schema schema;
    btree_stats stats;
    btree *tree = NULL;
    numeric_compare_context comparison;
    numeric_scan_context scan;
    uint64_t index;

    memset(&comparison, 0, sizeof(comparison));
    btree_options_init(&options, NUMERIC_KEY_SIZE, NUMERIC_VALUE_SIZE);
    options.comparator_id = NUMERIC_COMPARATOR_ID;
    options.compare = compare_u64_little_endian;
    options.compare_context = &comparison;
    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    TEST_STATUS(btree_create(&storage, &options, &tree), BTREE_OK);

    for (index = 0u; index < NUMERIC_ITEM_COUNT; ++index) {
        uint64_t item = shuffled_numeric_key(index);
        unsigned char key[NUMERIC_KEY_SIZE];
        unsigned char value[NUMERIC_VALUE_SIZE];
        bool inserted = false;

        encode_u64_little_endian(item, key);
        numeric_value(item, value);
        TEST_STATUS(btree_put(tree, key, value, &inserted), BTREE_OK);
        TEST_CHECK(inserted);
    }

    TEST_STATUS(btree_validate(tree, &stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(stats.item_count == NUMERIC_ITEM_COUNT);
    TEST_CHECK(stats.height >= 3u);
    TEST_CHECK(stats.leaf_capacity < NUMERIC_ITEM_COUNT);
    TEST_CHECK(stats.key_size == NUMERIC_KEY_SIZE);
    TEST_CHECK(stats.value_size == NUMERIC_VALUE_SIZE);
    TEST_CHECK(stats.comparator_id == NUMERIC_COMPARATOR_ID);

    memset(&scan, 0, sizeof(scan));
    TEST_STATUS(btree_scan(tree, NULL, NULL, check_numeric_item, &scan), BTREE_OK);
    TEST_CHECK(scan.seen == NUMERIC_ITEM_COUNT);
    TEST_CHECK(scan.expected == NUMERIC_ITEM_COUNT);

    for (index = 0u; index < NUMERIC_ITEM_COUNT; ++index) {
        uint64_t item = shuffled_numeric_key(index);

        if ((item & UINT64_C(1)) != 0u) {
            unsigned char key[NUMERIC_KEY_SIZE];
            bool removed = false;

            encode_u64_little_endian(item, key);
            TEST_STATUS(btree_delete(tree, key, &removed), BTREE_OK);
            TEST_CHECK(removed);
        }
    }
    TEST_STATUS(btree_validate(tree, &stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(stats.item_count == NUMERIC_ITEM_COUNT / 2u);

    btree_close(tree);
    tree = NULL;
    TEST_STATUS(btree_read_schema(&storage, &schema), BTREE_OK);
    assert_schema(&schema, NUMERIC_KEY_SIZE, NUMERIC_VALUE_SIZE, NUMERIC_COMPARATOR_ID);
    TEST_STATUS(btree_open(&storage, &options, &tree), BTREE_OK);
    test_validate(tree);

    memset(&scan, 0, sizeof(scan));
    scan.even_only = true;
    TEST_STATUS(btree_scan(tree, NULL, NULL, check_numeric_item, &scan), BTREE_OK);
    TEST_CHECK(scan.seen == NUMERIC_ITEM_COUNT / 2u);
    TEST_CHECK(scan.expected == NUMERIC_ITEM_COUNT);
    TEST_CHECK(comparison.calls > NUMERIC_ITEM_COUNT);

    btree_close(tree);
    btree_mem_destroy(backend);
}

int main(void) {
    test_file_byte_schema_and_copy_semantics();
    test_file_odd_capacity_leaf_split();
    test_invalid_comparator_configuration();
    test_custom_comparator_reentry_is_busy();
    test_custom_little_endian_comparator();
    return 0;
}
