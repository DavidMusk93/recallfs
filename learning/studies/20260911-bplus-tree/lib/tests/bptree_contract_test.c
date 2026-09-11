#include "bptree_backends.h"
#include "test_support.h"

typedef struct spy_storage {
    bpt_storage inner;
    size_t read_count;
    size_t commit_count;
    size_t last_update_count;
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

    spy->commit_count++;
    spy->last_update_count = update_count;
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

static bpt_status stop_after_first(void *context, uint64_t key, uint64_t value) {
    uint64_t *seen = context;

    TEST_CHECK(key == 7u);
    TEST_CHECK(value == 71u);
    (*seen)++;
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
    test_argument_contract();
    test_supported_page_sizes();
    return 0;
}
