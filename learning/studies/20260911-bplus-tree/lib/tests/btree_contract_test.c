#include "btree_backends.h"
#include "test_support.h"

enum { SPY_MAX_UPDATE_PAGE_IDS = 128, MULTI_LEVEL_ITEM_COUNT = 512 };

typedef struct spy_storage {
    btree_storage inner;
    size_t read_count;
    size_t commit_count;
    size_t last_update_count;
    uint64_t last_update_page_ids[SPY_MAX_UPDATE_PAGE_IDS];
    btree_status next_commit_status;
} spy_storage;

static btree_status spy_read_page(void *context, uint64_t page_id, void *data_out) {
    spy_storage *spy = context;

    spy->read_count++;
    return spy->inner.read_page(spy->inner.context, page_id, data_out);
}

static btree_status spy_page_count(void *context, uint64_t *count_out) {
    spy_storage *spy = context;

    return spy->inner.page_count(spy->inner.context, count_out);
}

static btree_status spy_commit_pages(void *context, const btree_page_update *updates,
                                     size_t update_count) {
    spy_storage *spy = context;
    size_t index;

    spy->commit_count++;
    spy->last_update_count = update_count;
    TEST_CHECK(update_count <= SPY_MAX_UPDATE_PAGE_IDS);
    memset(spy->last_update_page_ids, 0, sizeof(spy->last_update_page_ids));
    for (index = 0u; index < update_count; ++index) {
        spy->last_update_page_ids[index] = updates[index].page_id;
    }
    if (spy->next_commit_status != BTREE_OK) {
        btree_status status = spy->next_commit_status;

        spy->next_commit_status = BTREE_OK;
        return status;
    }
    return spy->inner.commit_pages(spy->inner.context, updates, update_count);
}

static btree_storage spy_interface(spy_storage *spy) {
    btree_storage storage;

    storage.context = spy;
    storage.page_size = spy->inner.page_size;
    storage.read_page = spy_read_page;
    storage.page_count = spy_page_count;
    storage.commit_pages = spy_commit_pages;
    return storage;
}

static void spy_reset_observations(spy_storage *spy) {
    spy->read_count = 0u;
    spy->commit_count = 0u;
    spy->last_update_count = 0u;
    memset(spy->last_update_page_ids, 0, sizeof(spy->last_update_page_ids));
}

static btree_status stop_after_first(void *context, uint64_t key, uint64_t value) {
    uint64_t *seen = context;

    TEST_CHECK(key == 7u);
    TEST_CHECK(value == 71u);
    (*seen)++;
    return BTREE_STOPPED;
}

typedef struct scan_reentry_context {
    btree *tree;
    size_t callback_count;
    uint64_t nested_seen;
} scan_reentry_context;

static btree_status attempt_scan_reentry(void *context, uint64_t key, uint64_t value) {
    scan_reentry_context *reentry = context;
    bool inserted = false;
    bool removed = false;

    TEST_CHECK(reentry->callback_count == 0u);
    TEST_STATUS(test_btree_put_u64(reentry->tree, key, value + 1u, &inserted), BTREE_BUSY);
    TEST_STATUS(test_btree_delete_u64(reentry->tree, key, &removed), BTREE_BUSY);
    TEST_STATUS(
        test_btree_scan_u64(reentry->tree, key, key + 1u, stop_after_first, &reentry->nested_seen),
        BTREE_BUSY);
    reentry->callback_count++;
    return BTREE_STOPPED;
}

static void test_page_local_commits(void) {
    btree_mem *backend = NULL;
    spy_storage spy;
    btree_storage storage;
    btree *tree = NULL;
    bool inserted;
    uint64_t value;
    uint64_t seen = 0u;

    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    memset(&spy, 0, sizeof(spy));
    spy.inner = btree_mem_storage(backend);
    storage = spy_interface(&spy);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    TEST_CHECK(spy.commit_count == 1u);
    TEST_CHECK(spy.last_update_count == 2u);

    TEST_STATUS(test_btree_put_u64(tree, 7u, 70u, &inserted), BTREE_OK);
    TEST_CHECK(inserted);
    TEST_CHECK(spy.last_update_count == 2u);
    TEST_STATUS(test_btree_put_u64(tree, 7u, 71u, &inserted), BTREE_OK);
    TEST_CHECK(!inserted);
    TEST_CHECK(spy.last_update_count == 2u);
    {
        size_t commit_count = spy.commit_count;

        TEST_STATUS(test_btree_put_u64(tree, 7u, 71u, &inserted), BTREE_OK);
        TEST_CHECK(!inserted);
        TEST_CHECK(spy.commit_count == commit_count);
    }
    TEST_STATUS(test_btree_get_u64(tree, 7u, &value), BTREE_OK);
    TEST_CHECK(value == 71u);
    TEST_CHECK(spy.read_count > 0u);

    TEST_STATUS(test_btree_scan_u64(tree, 0u, 8u, stop_after_first, &seen), BTREE_STOPPED);
    TEST_CHECK(seen == 1u);

    spy.next_commit_status = BTREE_IO;
    TEST_STATUS(test_btree_put_u64(tree, 8u, 80u, &inserted), BTREE_IO);
    TEST_STATUS(test_btree_get_u64(tree, 8u, &value), BTREE_NOT_FOUND);
    TEST_STATUS(test_btree_get_u64(tree, 7u, &value), BTREE_OK);
    TEST_CHECK(value == 71u);

    spy.next_commit_status = BTREE_RECOVERY_REQUIRED;
    TEST_STATUS(test_btree_put_u64(tree, 9u, 90u, &inserted), BTREE_RECOVERY_REQUIRED);
    TEST_STATUS(test_btree_get_u64(tree, 7u, &value), BTREE_RECOVERY_REQUIRED);

    btree_close(tree);
    btree_mem_destroy(backend);
}

static void test_multilevel_existing_update_write_set(void) {
    btree_mem *backend = NULL;
    spy_storage spy;
    btree_storage storage;
    btree *tree = NULL;
    btree_stats before;
    btree_stats after;
    uint64_t key;
    uint64_t value;
    bool inserted;

    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    memset(&spy, 0, sizeof(spy));
    spy.inner = btree_mem_storage(backend);
    storage = spy_interface(&spy);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    for (key = 0u; key < MULTI_LEVEL_ITEM_COUNT; ++key) {
        TEST_STATUS(test_btree_put_u64(tree, key, key + 11u, &inserted), BTREE_OK);
        TEST_CHECK(inserted);
    }
    TEST_STATUS(btree_validate(tree, &before, NULL, 0u), BTREE_OK);
    TEST_CHECK(before.height >= 3u);

    spy_reset_observations(&spy);
    TEST_STATUS(
        test_btree_put_u64(tree, MULTI_LEVEL_ITEM_COUNT / 2u, UINT64_C(0xfeedface), &inserted),
        BTREE_OK);
    TEST_CHECK(!inserted);
    TEST_CHECK(spy.commit_count == 1u);
    TEST_CHECK(spy.last_update_count == 2u);
    TEST_CHECK(before.allocated_pages > spy.last_update_count);
    TEST_CHECK((spy.last_update_page_ids[0] == 0u && spy.last_update_page_ids[1] != 0u) ||
               (spy.last_update_page_ids[0] != 0u && spy.last_update_page_ids[1] == 0u));

    TEST_STATUS(btree_validate(tree, &after, NULL, 0u), BTREE_OK);
    TEST_CHECK(after.item_count == before.item_count);
    TEST_CHECK(after.allocated_pages == before.allocated_pages);
    TEST_CHECK(after.live_pages == before.live_pages);
    TEST_CHECK(after.free_pages == before.free_pages);
    TEST_CHECK(after.height == before.height);
    TEST_STATUS(test_btree_get_u64(tree, MULTI_LEVEL_ITEM_COUNT / 2u, &value), BTREE_OK);
    TEST_CHECK(value == UINT64_C(0xfeedface));

    btree_close(tree);
    btree_mem_destroy(backend);
}

static void test_scan_reentrancy_is_busy(void) {
    btree_mem *backend = NULL;
    btree_storage storage;
    btree *tree = NULL;
    scan_reentry_context reentry;
    uint64_t seen = 0u;
    uint64_t value;
    bool inserted;

    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    TEST_STATUS(test_btree_put_u64(tree, 7u, 71u, &inserted), BTREE_OK);
    TEST_CHECK(inserted);

    memset(&reentry, 0, sizeof(reentry));
    reentry.tree = tree;
    TEST_STATUS(test_btree_scan_u64(tree, 0u, 8u, attempt_scan_reentry, &reentry), BTREE_STOPPED);
    TEST_CHECK(reentry.callback_count == 1u);
    TEST_CHECK(reentry.nested_seen == 0u);

    TEST_STATUS(test_btree_get_u64(tree, 7u, &value), BTREE_OK);
    TEST_CHECK(value == 71u);
    test_validate(tree);
    TEST_STATUS(test_btree_scan_u64(tree, 0u, 8u, stop_after_first, &seen), BTREE_STOPPED);
    TEST_CHECK(seen == 1u);

    btree_close(tree);
    btree_mem_destroy(backend);
}

static void test_argument_contract(void) {
    btree_mem *backend = NULL;
    btree *tree = NULL;
    btree_storage storage;
    bool inserted;
    bool removed;
    uint64_t value;

    TEST_STATUS(btree_mem_create(511u, &backend), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_mem_create(513u, &backend), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_mem_create(512u, &backend), BTREE_OK);
    storage = btree_mem_storage(backend);
    TEST_STATUS(test_btree_create_u64(NULL, &tree), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_create_u64(&storage, NULL), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    TEST_STATUS(test_btree_get_u64(NULL, 0u, &value), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_get_u64(tree, 0u, NULL), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_put_u64(tree, 0u, 0u, NULL), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_delete_u64(tree, 0u, NULL), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_scan_u64(tree, 0u, 1u, NULL, NULL), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_validate(NULL, NULL, NULL, 0u), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(test_btree_put_u64(tree, 0u, 0u, &inserted), BTREE_OK);
    TEST_STATUS(test_btree_delete_u64(tree, 0u, &removed), BTREE_OK);
    TEST_CHECK(inserted && removed);
    btree_close(tree);
    btree_mem_destroy(backend);
}

static void test_supported_page_sizes(void) {
    static const uint32_t page_sizes[] = {512u, 4096u, 65536u};
    size_t size_index;

    for (size_index = 0u; size_index < sizeof(page_sizes) / sizeof(page_sizes[0]); ++size_index) {
        btree_mem *backend = NULL;
        btree_storage storage;
        btree *tree = NULL;
        btree_stats stats;
        uint64_t key;
        uint64_t item_count;

        TEST_STATUS(btree_mem_create(page_sizes[size_index], &backend), BTREE_OK);
        storage = btree_mem_storage(backend);
        TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
        TEST_STATUS(btree_validate(tree, &stats, NULL, 0u), BTREE_OK);
        TEST_CHECK(stats.page_size == page_sizes[size_index]);
        item_count = (uint64_t)stats.leaf_capacity + 3u;
        for (key = 0u; key < item_count; ++key) {
            bool inserted = false;

            TEST_STATUS(test_btree_put_u64(tree, key, key + 11u, &inserted), BTREE_OK);
            TEST_CHECK(inserted);
        }
        test_validate(tree);
        for (key = 0u; key < item_count; key += 2u) {
            bool removed = false;

            TEST_STATUS(test_btree_delete_u64(tree, key, &removed), BTREE_OK);
            TEST_CHECK(removed);
        }
        test_validate(tree);
        btree_close(tree);
        btree_mem_destroy(backend);
    }
}

int main(void) {
    test_page_local_commits();
    test_multilevel_existing_update_write_set();
    test_scan_reentrancy_is_busy();
    test_argument_contract();
    test_supported_page_sizes();
    return 0;
}
