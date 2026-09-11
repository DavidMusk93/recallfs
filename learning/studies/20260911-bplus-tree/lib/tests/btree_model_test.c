#include "btree_backends.h"
#include "test_support.h"

#include <limits.h>

enum { KEY_SPACE = 2048, OPERATION_COUNT = 30000 };

typedef struct scan_expectation {
    const bool *present;
    const uint64_t *values;
    uint64_t next_key;
    uint64_t end_key;
    uint64_t seen;
} scan_expectation;

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;

    value ^= value << 13u;
    value ^= value >> 7u;
    value ^= value << 17u;
    *state = value;
    return value;
}

static btree_status check_scan_item(void *context, uint64_t key, uint64_t value) {
    scan_expectation *expectation = context;

    while (expectation->next_key < key) {
        TEST_CHECK(!expectation->present[expectation->next_key]);
        expectation->next_key++;
    }
    TEST_CHECK(key < expectation->end_key);
    TEST_CHECK(expectation->present[key]);
    TEST_CHECK(expectation->values[key] == value);
    expectation->next_key = key + 1u;
    expectation->seen++;
    return BTREE_OK;
}

static void check_full_model(btree *tree, const bool *present, const uint64_t *values) {
    scan_expectation expectation;
    uint64_t expected_count = 0u;
    uint64_t key;

    for (key = 0u; key < KEY_SPACE; ++key) {
        uint64_t actual = 0u;
        btree_status status = test_btree_get_u64(tree, key, &actual);

        if (present[key]) {
            TEST_CHECK(status == BTREE_OK);
            TEST_CHECK(actual == values[key]);
            expected_count++;
        } else {
            TEST_CHECK(status == BTREE_NOT_FOUND);
        }
    }

    expectation.present = present;
    expectation.values = values;
    expectation.next_key = 0u;
    expectation.end_key = KEY_SPACE;
    expectation.seen = 0u;
    TEST_STATUS(test_btree_scan_u64(tree, 0u, KEY_SPACE, check_scan_item, &expectation), BTREE_OK);
    while (expectation.next_key < KEY_SPACE) {
        TEST_CHECK(!present[expectation.next_key]);
        expectation.next_key++;
    }
    TEST_CHECK(expectation.seen == expected_count);
    test_validate(tree);
}

int main(void) {
    bool present[KEY_SPACE];
    uint64_t values[KEY_SPACE];
    btree_mem *backend = NULL;
    btree_storage storage;
    btree *tree = NULL;
    uint64_t random_state = UINT64_C(0x4d595df4d0f33173);
    uint64_t operation;
    bool inserted;
    bool removed;
    uint64_t value;

    memset(present, 0, sizeof(present));
    memset(values, 0, sizeof(values));

    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);

    TEST_STATUS(test_btree_get_u64(tree, 0u, &value), BTREE_NOT_FOUND);
    TEST_STATUS(test_btree_delete_u64(tree, 0u, &removed), BTREE_OK);
    TEST_CHECK(!removed);
    TEST_STATUS(test_btree_scan_u64(tree, 9u, 9u, check_scan_item, NULL), BTREE_OK);

    TEST_STATUS(test_btree_put_u64(tree, 0u, UINT64_MAX, &inserted), BTREE_OK);
    TEST_CHECK(inserted);
    present[0] = true;
    values[0] = UINT64_MAX;
    TEST_STATUS(test_btree_put_u64(tree, UINT64_MAX, 0u, &inserted), BTREE_OK);
    TEST_CHECK(inserted);
    TEST_STATUS(test_btree_get_u64(tree, UINT64_MAX, &value), BTREE_OK);
    TEST_CHECK(value == 0u);
    TEST_STATUS(test_btree_delete_u64(tree, UINT64_MAX, &removed), BTREE_OK);
    TEST_CHECK(removed);

    for (operation = 0u; operation < OPERATION_COUNT; ++operation) {
        uint64_t random = next_random(&random_state);
        uint64_t key = random % KEY_SPACE;
        uint64_t action = (random >> 32u) % 10u;

        if (action < 5u) {
            uint64_t new_value = next_random(&random_state);
            bool was_present = present[key];

            TEST_STATUS(test_btree_put_u64(tree, key, new_value, &inserted), BTREE_OK);
            TEST_CHECK(inserted == !was_present);
            present[key] = true;
            values[key] = new_value;
        } else if (action < 8u) {
            bool was_present = present[key];

            TEST_STATUS(test_btree_delete_u64(tree, key, &removed), BTREE_OK);
            TEST_CHECK(removed == was_present);
            present[key] = false;
        } else {
            btree_status status = test_btree_get_u64(tree, key, &value);

            TEST_CHECK(status == (present[key] ? BTREE_OK : BTREE_NOT_FOUND));
            if (present[key]) {
                TEST_CHECK(value == values[key]);
            }
        }

        if ((operation % 251u) == 0u) {
            check_full_model(tree, present, values);
            btree_close(tree);
            tree = NULL;
            TEST_STATUS(test_btree_open_u64(&storage, &tree), BTREE_OK);
        }
    }

    check_full_model(tree, present, values);
    btree_close(tree);
    btree_mem_destroy(backend);
    return 0;
}
