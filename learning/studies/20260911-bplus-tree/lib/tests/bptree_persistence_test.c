#include "bptree_backends.h"
#include "test_support.h"

enum { PERSISTED_ITEMS = 1200 };

static void reopen(const char *path, bpt_file_backend **backend, bpt_tree **tree) {
    bpt_tree_close(*tree);
    *tree = NULL;
    TEST_STATUS(bpt_file_backend_close(*backend), BPT_OK);
    *backend = NULL;
    TEST_STATUS(bpt_file_backend_open(path, backend), BPT_OK);
    bpt_storage storage = bpt_file_backend_storage(*backend);
    TEST_STATUS(bpt_tree_open(&storage, tree), BPT_OK);
    test_validate(*tree);
}

int main(void) {
    char path[256];
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    uint64_t key;
    uint64_t value;
    bool inserted;
    bool removed;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);

    for (key = 0u; key < PERSISTED_ITEMS; ++key) {
        TEST_STATUS(bpt_tree_put(tree, key, key + 9000u, &inserted), BPT_OK);
        TEST_CHECK(inserted);
        if ((key % 113u) == 0u) {
            reopen(path, &backend, &tree);
        }
    }

    TEST_STATUS(bpt_tree_put(tree, 777u, UINT64_MAX, &inserted), BPT_OK);
    TEST_CHECK(!inserted);
    reopen(path, &backend, &tree);
    TEST_STATUS(bpt_tree_get(tree, 777u, &value), BPT_OK);
    TEST_CHECK(value == UINT64_MAX);

    for (key = 0u; key < PERSISTED_ITEMS; key += 3u) {
        TEST_STATUS(bpt_tree_delete(tree, key, &removed), BPT_OK);
        TEST_CHECK(removed);
        if ((key % 117u) == 0u) {
            reopen(path, &backend, &tree);
        }
    }

    reopen(path, &backend, &tree);
    for (key = 0u; key < PERSISTED_ITEMS; ++key) {
        bpt_status status = bpt_tree_get(tree, key, &value);

        if ((key % 3u) == 0u) {
            TEST_CHECK(status == BPT_NOT_FOUND);
        } else {
            TEST_CHECK(status == BPT_OK);
            TEST_CHECK(value == (key == 777u ? UINT64_MAX : key + 9000u));
        }
    }

    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
    return 0;
}
