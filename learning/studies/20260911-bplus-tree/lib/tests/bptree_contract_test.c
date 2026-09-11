#include "bptree_backends.h"
#include "test_support.h"

enum { SPY_MAX_UPDATE_PAGE_IDS = 128, MULTI_LEVEL_ITEM_COUNT = 512 };

typedef struct spy_storage {
    bpt_storage inner;
    size_t read_count;
    size_t commit_count;
    size_t last_update_count;
    uint64_t last_update_page_ids[SPY_MAX_UPDATE_PAGE_IDS];
    bpt_status next_commit_status;
} spy_storage;

static bpt_status spy_read_page(void *context, uint64_t page_id, void *data_out) {
    spy_storage *spy = context;

    spy->read_count++;
    return spy->inner.read_page(spy->inner.context, page_id, data_out);
}

static bpt_status spy_page_count(void *context, uint64_t *count_out) {
    spy_storage *spy = context;

    return spy->inner.page_count(spy->inner.context, count_out);
}

static bpt_status spy_commit_pages(void *context, const bpt_page_update *updates,
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
    if (spy->next_commit_status != BPT_OK) {
        bpt_status status = spy->next_commit_status;

        spy->next_commit_status = BPT_OK;
        return status;
    }
    return spy->inner.commit_pages(spy->inner.context, updates, update_count);
}

static bpt_storage spy_interface(spy_storage *spy) {
    bpt_storage storage;

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

static bpt_status stop_after_first(void *context, uint64_t key, uint64_t value) {
    uint64_t *seen = context;

    TEST_CHECK(key == 7u);
    TEST_CHECK(value == 71u);
    (*seen)++;
    return BPT_STOPPED;
}

typedef struct scan_reentry_context {
    bpt_tree *tree;
    size_t callback_count;
    uint64_t nested_seen;
} scan_reentry_context;

static bpt_status attempt_scan_reentry(void *context, uint64_t key, uint64_t value) {
    scan_reentry_context *reentry = context;
    bool inserted = false;
    bool removed = false;

    TEST_CHECK(reentry->callback_count == 0u);
    TEST_STATUS(bpt_tree_put(reentry->tree, key, value + 1u, &inserted), BPT_BUSY);
    TEST_STATUS(bpt_tree_delete(reentry->tree, key, &removed), BPT_BUSY);
    TEST_STATUS(
        bpt_tree_scan(reentry->tree, key, key + 1u, stop_after_first, &reentry->nested_seen),
        BPT_BUSY);
    reentry->callback_count++;
    return BPT_STOPPED;
}

static void test_page_local_commits(void) {
    bpt_memory_backend *backend = NULL;
    spy_storage spy;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    bool inserted;
    uint64_t value;
    uint64_t seen = 0u;

    TEST_STATUS(bpt_memory_backend_create(512u, &backend), BPT_OK);
    memset(&spy, 0, sizeof(spy));
    spy.inner = bpt_memory_backend_storage(backend);
    storage = spy_interface(&spy);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_CHECK(spy.commit_count == 1u);
    TEST_CHECK(spy.last_update_count == 2u);

    TEST_STATUS(bpt_tree_put(tree, 7u, 70u, &inserted), BPT_OK);
    TEST_CHECK(inserted);
    TEST_CHECK(spy.last_update_count == 2u);
    TEST_STATUS(bpt_tree_put(tree, 7u, 71u, &inserted), BPT_OK);
    TEST_CHECK(!inserted);
    TEST_CHECK(spy.last_update_count == 2u);
    {
        size_t commit_count = spy.commit_count;

        TEST_STATUS(bpt_tree_put(tree, 7u, 71u, &inserted), BPT_OK);
        TEST_CHECK(!inserted);
        TEST_CHECK(spy.commit_count == commit_count);
    }
    TEST_STATUS(bpt_tree_get(tree, 7u, &value), BPT_OK);
    TEST_CHECK(value == 71u);
    TEST_CHECK(spy.read_count > 0u);

    TEST_STATUS(bpt_tree_scan(tree, 0u, 8u, stop_after_first, &seen), BPT_STOPPED);
    TEST_CHECK(seen == 1u);

    spy.next_commit_status = BPT_IO;
    TEST_STATUS(bpt_tree_put(tree, 8u, 80u, &inserted), BPT_IO);
    TEST_STATUS(bpt_tree_get(tree, 8u, &value), BPT_NOT_FOUND);
    TEST_STATUS(bpt_tree_get(tree, 7u, &value), BPT_OK);
    TEST_CHECK(value == 71u);

    spy.next_commit_status = BPT_RECOVERY_REQUIRED;
    TEST_STATUS(bpt_tree_put(tree, 9u, 90u, &inserted), BPT_RECOVERY_REQUIRED);
    TEST_STATUS(bpt_tree_get(tree, 7u, &value), BPT_RECOVERY_REQUIRED);

    bpt_tree_close(tree);
    bpt_memory_backend_destroy(backend);
}

static void test_multilevel_existing_update_write_set(void) {
    bpt_memory_backend *backend = NULL;
    spy_storage spy;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    bpt_stats before;
    bpt_stats after;
    uint64_t key;
    uint64_t value;
    bool inserted;

    TEST_STATUS(bpt_memory_backend_create(512u, &backend), BPT_OK);
    memset(&spy, 0, sizeof(spy));
    spy.inner = bpt_memory_backend_storage(backend);
    storage = spy_interface(&spy);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    for (key = 0u; key < MULTI_LEVEL_ITEM_COUNT; ++key) {
        TEST_STATUS(bpt_tree_put(tree, key, key + 11u, &inserted), BPT_OK);
        TEST_CHECK(inserted);
    }
    TEST_STATUS(bpt_tree_validate(tree, &before, NULL, 0u), BPT_OK);
    TEST_CHECK(before.height >= 3u);

    spy_reset_observations(&spy);
    TEST_STATUS(bpt_tree_put(tree, MULTI_LEVEL_ITEM_COUNT / 2u, UINT64_C(0xfeedface), &inserted),
                BPT_OK);
    TEST_CHECK(!inserted);
    TEST_CHECK(spy.commit_count == 1u);
    TEST_CHECK(spy.last_update_count == 2u);
    TEST_CHECK(before.allocated_pages > spy.last_update_count);
    TEST_CHECK((spy.last_update_page_ids[0] == 0u && spy.last_update_page_ids[1] != 0u) ||
               (spy.last_update_page_ids[0] != 0u && spy.last_update_page_ids[1] == 0u));

    TEST_STATUS(bpt_tree_validate(tree, &after, NULL, 0u), BPT_OK);
    TEST_CHECK(after.item_count == before.item_count);
    TEST_CHECK(after.allocated_pages == before.allocated_pages);
    TEST_CHECK(after.live_pages == before.live_pages);
    TEST_CHECK(after.free_pages == before.free_pages);
    TEST_CHECK(after.height == before.height);
    TEST_STATUS(bpt_tree_get(tree, MULTI_LEVEL_ITEM_COUNT / 2u, &value), BPT_OK);
    TEST_CHECK(value == UINT64_C(0xfeedface));

    bpt_tree_close(tree);
    bpt_memory_backend_destroy(backend);
}

static void test_scan_reentrancy_is_busy(void) {
    bpt_memory_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    scan_reentry_context reentry;
    uint64_t seen = 0u;
    uint64_t value;
    bool inserted;

    TEST_STATUS(bpt_memory_backend_create(512u, &backend), BPT_OK);
    storage = bpt_memory_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_put(tree, 7u, 71u, &inserted), BPT_OK);
    TEST_CHECK(inserted);

    memset(&reentry, 0, sizeof(reentry));
    reentry.tree = tree;
    TEST_STATUS(bpt_tree_scan(tree, 0u, 8u, attempt_scan_reentry, &reentry), BPT_STOPPED);
    TEST_CHECK(reentry.callback_count == 1u);
    TEST_CHECK(reentry.nested_seen == 0u);

    TEST_STATUS(bpt_tree_get(tree, 7u, &value), BPT_OK);
    TEST_CHECK(value == 71u);
    test_validate(tree);
    TEST_STATUS(bpt_tree_scan(tree, 0u, 8u, stop_after_first, &seen), BPT_STOPPED);
    TEST_CHECK(seen == 1u);

    bpt_tree_close(tree);
    bpt_memory_backend_destroy(backend);
}

static void test_argument_contract(void) {
    bpt_memory_backend *backend = NULL;
    bpt_tree *tree = NULL;
    bpt_storage storage;
    bool inserted;
    bool removed;
    uint64_t value;

    TEST_STATUS(bpt_memory_backend_create(511u, &backend), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_memory_backend_create(513u, &backend), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_memory_backend_create(512u, &backend), BPT_OK);
    storage = bpt_memory_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(NULL, &tree), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_create(&storage, NULL), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(NULL, 0u, &value), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_get(tree, 0u, NULL), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_put(tree, 0u, 0u, NULL), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_delete(tree, 0u, NULL), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_scan(tree, 0u, 1u, NULL, NULL), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_validate(NULL, NULL, NULL, 0u), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_tree_put(tree, 0u, 0u, &inserted), BPT_OK);
    TEST_STATUS(bpt_tree_delete(tree, 0u, &removed), BPT_OK);
    TEST_CHECK(inserted && removed);
    bpt_tree_close(tree);
    bpt_memory_backend_destroy(backend);
}

static void test_supported_page_sizes(void) {
    static const uint32_t page_sizes[] = {512u, 4096u, 65536u};
    size_t size_index;

    for (size_index = 0u; size_index < sizeof(page_sizes) / sizeof(page_sizes[0]); ++size_index) {
        bpt_memory_backend *backend = NULL;
        bpt_storage storage;
        bpt_tree *tree = NULL;
        bpt_stats stats;
        uint64_t key;
        uint64_t item_count;

        TEST_STATUS(bpt_memory_backend_create(page_sizes[size_index], &backend), BPT_OK);
        storage = bpt_memory_backend_storage(backend);
        TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
        TEST_STATUS(bpt_tree_validate(tree, &stats, NULL, 0u), BPT_OK);
        TEST_CHECK(stats.page_size == page_sizes[size_index]);
        item_count = (uint64_t)stats.leaf_capacity + 3u;
        for (key = 0u; key < item_count; ++key) {
            bool inserted = false;

            TEST_STATUS(bpt_tree_put(tree, key, key + 11u, &inserted), BPT_OK);
            TEST_CHECK(inserted);
        }
        test_validate(tree);
        for (key = 0u; key < item_count; key += 2u) {
            bool removed = false;

            TEST_STATUS(bpt_tree_delete(tree, key, &removed), BPT_OK);
            TEST_CHECK(removed);
        }
        test_validate(tree);
        bpt_tree_close(tree);
        bpt_memory_backend_destroy(backend);
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
