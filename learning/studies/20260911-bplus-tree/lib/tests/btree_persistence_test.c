#include "btree_backends.h"
#include "test_support.h"

enum { PERSISTED_ITEMS = 1200 };

static void reopen(const char *path, btree_file **backend, btree **tree) {
    btree_close(*tree);
    *tree = NULL;
    TEST_STATUS(btree_file_close(*backend), BTREE_OK);
    *backend = NULL;
    TEST_STATUS(btree_file_open(path, backend), BTREE_OK);
    btree_storage storage = btree_file_storage(*backend);
    TEST_STATUS(test_btree_open_u64(&storage, tree), BTREE_OK);
    test_validate(*tree);
}

int main(void) {
    char path[256];
    btree_file *backend = NULL;
    btree_storage storage;
    btree *tree = NULL;
    uint64_t key;
    uint64_t value;
    bool inserted;
    bool removed;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(btree_file_create(path, 512u, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);

    for (key = 0u; key < PERSISTED_ITEMS; ++key) {
        TEST_STATUS(test_btree_put_u64(tree, key, key + 9000u, &inserted), BTREE_OK);
        TEST_CHECK(inserted);
        if ((key % 113u) == 0u) {
            reopen(path, &backend, &tree);
        }
    }

    TEST_STATUS(test_btree_put_u64(tree, 777u, UINT64_MAX, &inserted), BTREE_OK);
    TEST_CHECK(!inserted);
    reopen(path, &backend, &tree);
    TEST_STATUS(test_btree_get_u64(tree, 777u, &value), BTREE_OK);
    TEST_CHECK(value == UINT64_MAX);

    for (key = 0u; key < PERSISTED_ITEMS; key += 3u) {
        TEST_STATUS(test_btree_delete_u64(tree, key, &removed), BTREE_OK);
        TEST_CHECK(removed);
        if ((key % 117u) == 0u) {
            reopen(path, &backend, &tree);
        }
    }

    reopen(path, &backend, &tree);
    for (key = 0u; key < PERSISTED_ITEMS; ++key) {
        btree_status status = test_btree_get_u64(tree, key, &value);

        if ((key % 3u) == 0u) {
            TEST_CHECK(status == BTREE_NOT_FOUND);
        } else {
            TEST_CHECK(status == BTREE_OK);
            TEST_CHECK(value == (key == 777u ? UINT64_MAX : key + 9000u));
        }
    }

    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    test_remove_database(path);
    return 0;
}
