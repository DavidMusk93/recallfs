#include <rbt.h>
#include <rbt_backends.h>

#include <string.h>

int main(void) {
    static const char MESSAGE[] = "typed package record";
    const struct rbt_column columns[] = {
        {
            .id = 1u,
            .type = RBT_TYPE_U64,
            .flags = RBT_COLUMN_KEY,
            .max_size = 0u,
        },
        {
            .id = 2u,
            .type = RBT_TYPE_UTF8,
            .flags = 0u,
            .max_size = 64u,
        },
    };
    const struct rbt_schema schema = {
        .id = 1u,
        .columns = columns,
        .column_count = sizeof(columns) / sizeof(columns[0]),
    };
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    struct rbt_value values[] = {
        {
            .type = RBT_TYPE_U64,
            .is_null = false,
            .as.u64 = 42u,
        },
        {
            .type = RBT_TYPE_UTF8,
            .is_null = false,
            .as.bytes = {.data = MESSAGE, .size = sizeof(MESSAGE) - 1u},
        },
    };
    struct rbt_record record = {
        .values = values,
        .value_count = sizeof(values) / sizeof(values[0]),
    };
    struct rbt_record key = {
        .values = values,
        .value_count = 1u,
    };
    struct rbt_row *row = NULL;
    const struct rbt_value *actual = NULL;
    bool inserted = false;
    int result = 0;

    if (rbt_mem_create(512u, &memory) != 0 || rbt_mem_storage(memory, &storage) != 0 ||
        rbt_create(&config, &tree) != 0 || rbt_put(tree, &record, &inserted) != 0 || !inserted ||
        rbt_get(tree, &key, &row) != 0 || rbt_row_get(row, 1u, &actual) != 0 ||
        actual->type != RBT_TYPE_UTF8 || actual->is_null ||
        actual->as.bytes.size != sizeof(MESSAGE) - 1u ||
        memcmp(actual->as.bytes.data, MESSAGE, sizeof(MESSAGE) - 1u) != 0) {
        result = 1;
    }

    if (row != NULL && rbt_row_destroy(row) != 0) {
        result = 2;
    }
    if (tree != NULL && rbt_destroy(tree) != 0) {
        result = 3;
    }
    if (memory != NULL && rbt_mem_destroy(memory) != 0) {
        result = 4;
    }
    return result;
}
