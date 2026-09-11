#include "btree_backends.h"
#include "test_support.h"

enum { ITEM_COUNT = 4096 };

static void put_range(btree *tree, bool ascending) {
    uint64_t index;

    for (index = 0u; index < ITEM_COUNT; ++index) {
        uint64_t key = ascending ? index : (ITEM_COUNT - 1u - index);
        bool inserted = false;

        TEST_STATUS(test_btree_put_u64(tree, key, key * 17u + 3u, &inserted), BTREE_OK);
        TEST_CHECK(inserted);
        if ((index % 97u) == 0u) {
            test_validate(tree);
        }
    }
}

static void delete_range(btree *tree, uint64_t keep_key) {
    uint64_t key;

    for (key = 0u; key < ITEM_COUNT; ++key) {
        bool removed = false;

        if (key == keep_key) {
            continue;
        }
        TEST_STATUS(test_btree_delete_u64(tree, key, &removed), BTREE_OK);
        TEST_CHECK(removed);
        if ((key % 89u) == 0u) {
            test_validate(tree);
        }
    }
}

static void run_order_case(bool ascending) {
    btree_mem *backend = NULL;
    btree_storage storage;
    btree *tree = NULL;
    btree_stats full_stats;
    btree_stats collapsed_stats;
    btree_stats reused_stats;
    uint64_t key;
    uint64_t value;
    bool removed;

    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    put_range(tree, ascending);
    TEST_STATUS(btree_validate(tree, &full_stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(full_stats.item_count == ITEM_COUNT);
    TEST_CHECK(full_stats.height >= 3u);

    delete_range(tree, 2047u);
    TEST_STATUS(btree_validate(tree, &collapsed_stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(collapsed_stats.item_count == 1u);
    TEST_CHECK(collapsed_stats.height == 1u);
    TEST_STATUS(test_btree_get_u64(tree, 2047u, &value), BTREE_OK);
    TEST_CHECK(value == 2047u * 17u + 3u);
    TEST_CHECK(collapsed_stats.free_pages > 0u);

    for (key = ITEM_COUNT; key < ITEM_COUNT * 2u; ++key) {
        bool inserted = false;

        TEST_STATUS(test_btree_put_u64(tree, key, key ^ UINT64_C(0x55aa), &inserted), BTREE_OK);
        TEST_CHECK(inserted);
    }
    TEST_STATUS(btree_validate(tree, &reused_stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(reused_stats.allocated_pages <= full_stats.allocated_pages + 2u);

    for (key = ITEM_COUNT; key < ITEM_COUNT * 2u; ++key) {
        TEST_STATUS(test_btree_delete_u64(tree, key, &removed), BTREE_OK);
        TEST_CHECK(removed);
    }
    TEST_STATUS(test_btree_delete_u64(tree, 2047u, &removed), BTREE_OK);
    TEST_CHECK(removed);
    TEST_STATUS(btree_validate(tree, &collapsed_stats, NULL, 0u), BTREE_OK);
    TEST_CHECK(collapsed_stats.item_count == 0u);
    TEST_CHECK(collapsed_stats.height == 1u);

    btree_close(tree);
    btree_mem_destroy(backend);
}

int main(void) {
    run_order_case(true);
    run_order_case(false);
    return 0;
}
