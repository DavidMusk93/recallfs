#include <btree.h>
#include <btree_backends.h>

#include <string.h>

int main(void) {
    static const unsigned char key[4] = {0u, 1u, 2u, 3u};
    static const unsigned char value[3] = {4u, 5u, 6u};
    unsigned char actual[sizeof(value)] = {0u};
    btree_options options;
    btree_mem *memory = NULL;
    btree_storage storage;
    btree *tree = NULL;
    bool inserted = false;
    int result = 0;

    btree_options_init(&options, sizeof(key), sizeof(value));
    if (btree_mem_create(4096u, &memory) != BTREE_OK) {
        return 1;
    }
    storage = btree_mem_storage(memory);
    if (btree_create(&storage, &options, &tree) != BTREE_OK ||
        btree_put(tree, key, value, &inserted) != BTREE_OK || !inserted ||
        btree_get(tree, key, actual) != BTREE_OK || memcmp(actual, value, sizeof(value)) != 0) {
        result = 2;
    }

    btree_close(tree);
    btree_mem_destroy(memory);
    return result;
}
