#include "bptree_backends.h"
#include "test_support.h"

enum { ITEM_COUNT = 4096 };

static void put_range(bpt_tree *tree, bool ascending) {
    uint64_t index;

    for (index = 0u; index < ITEM_COUNT; ++index) {
        uint64_t key = ascending ? index : (ITEM_COUNT - 1u - index);
        bool inserted = false;

        TEST_STATUS(bpt_tree_put(tree, key, key * 17u + 3u, &inserted), BPT_OK);
        TEST_CHECK(inserted);
        if ((index % 97u) == 0u) {
            test_validate(tree);
        }
    }
}

static void delete_range(bpt_tree *tree, uint64_t keep_key) {
    uint64_t key;

    for (key = 0u; key < ITEM_COUNT; ++key) {
        bool removed = false;

        if (key == keep_key) {
            continue;
        }
        TEST_STATUS(bpt_tree_delete(tree, key, &removed), BPT_OK);
        TEST_CHECK(removed);
        if ((key % 89u) == 0u) {
            test_validate(tree);
        }
    }
}

static void run_order_case(bool ascending) {
    bpt_memory_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    bpt_stats full_stats;
    bpt_stats collapsed_stats;
    bpt_stats reused_stats;
    uint64_t key;
    uint64_t value;
    bool removed;

    TEST_STATUS(bpt_memory_backend_create(512u, &backend), BPT_OK);
    storage = bpt_memory_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    put_range(tree, ascending);
    TEST_STATUS(bpt_tree_validate(tree, &full_stats, NULL, 0u), BPT_OK);
    TEST_CHECK(full_stats.item_count == ITEM_COUNT);
    TEST_CHECK(full_stats.height >= 3u);

    delete_range(tree, 2047u);
    TEST_STATUS(bpt_tree_validate(tree, &collapsed_stats, NULL, 0u), BPT_OK);
    TEST_CHECK(collapsed_stats.item_count == 1u);
    TEST_CHECK(collapsed_stats.height == 1u);
    TEST_STATUS(bpt_tree_get(tree, 2047u, &value), BPT_OK);
    TEST_CHECK(value == 2047u * 17u + 3u);
    TEST_CHECK(collapsed_stats.free_pages > 0u);

    for (key = ITEM_COUNT; key < ITEM_COUNT * 2u; ++key) {
        bool inserted = false;

        TEST_STATUS(bpt_tree_put(tree, key, key ^ UINT64_C(0x55aa), &inserted), BPT_OK);
        TEST_CHECK(inserted);
    }
    TEST_STATUS(bpt_tree_validate(tree, &reused_stats, NULL, 0u), BPT_OK);
    TEST_CHECK(reused_stats.allocated_pages <= full_stats.allocated_pages + 2u);

    for (key = ITEM_COUNT; key < ITEM_COUNT * 2u; ++key) {
        TEST_STATUS(bpt_tree_delete(tree, key, &removed), BPT_OK);
        TEST_CHECK(removed);
    }
    TEST_STATUS(bpt_tree_delete(tree, 2047u, &removed), BPT_OK);
    TEST_CHECK(removed);
    TEST_STATUS(bpt_tree_validate(tree, &collapsed_stats, NULL, 0u), BPT_OK);
    TEST_CHECK(collapsed_stats.item_count == 0u);
    TEST_CHECK(collapsed_stats.height == 1u);

    bpt_tree_close(tree);
    bpt_memory_backend_destroy(backend);
}

int main(void) {
    run_order_case(true);
    run_order_case(false);
    return 0;
}
