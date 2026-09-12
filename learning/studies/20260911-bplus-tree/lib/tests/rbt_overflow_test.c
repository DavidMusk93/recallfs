#include "test_support.h"

enum {
    LARGE_BYTES_SIZE = 4096,
    LARGE_TEXT_SIZE = 1536,
    LARGE_MUTATION_SIZE = 448 * 2048,
    KEY_COLUMN_ORDINAL = 0,
    SMALL_COLUMN_ORDINAL = 1,
    LARGE_COLUMN_ORDINAL = 2,
    TEXT_COLUMN_ORDINAL = 3,
};

static void fill_binary(unsigned char *data, size_t size) {
    size_t index;

    for (index = 0u; index < size; ++index) {
        data[index] = (unsigned char)((index * 37u + 11u) & 0xffu);
    }
}

static void fill_utf8(char *data, size_t size) {
    static const char alphabet[] = "overflow-pages-are-byte-exact/";
    size_t index;

    for (index = 0u; index < size; ++index) {
        data[index] = alphabet[index % (sizeof(alphabet) - 1u)];
    }
}

static struct rbt_record make_record(uint64_t key_number, const void *small_data, size_t small_size,
                                     const void *large_data, size_t large_size,
                                     const char *text_data, size_t text_size,
                                     struct rbt_value values[4]) {
    values[KEY_COLUMN_ORDINAL] = rbt_test_u64(key_number);
    values[SMALL_COLUMN_ORDINAL] = rbt_test_bytes(small_data, small_size);
    values[LARGE_COLUMN_ORDINAL] = rbt_test_bytes(large_data, large_size);
    values[TEXT_COLUMN_ORDINAL] = rbt_test_utf8(text_data, text_size);
    return rbt_test_record(values, 4u);
}

static void expect_row(struct rbt *tree, uint64_t key_number, const void *small_data,
                       size_t small_size, const void *large_data, size_t large_size,
                       const char *text_data, size_t text_size) {
    struct rbt_value key_value = rbt_test_u64(key_number);
    struct rbt_record key = rbt_test_key(&key_value, 1u);
    struct rbt_row *row = NULL;
    const struct rbt_value *actual_key = NULL;
    const struct rbt_value *actual_small = NULL;
    const struct rbt_value *actual_large = NULL;
    const struct rbt_value *actual_text = NULL;

    RBT_TEST_OK(rbt_get(tree, &key, &row));
    RBT_TEST_OK(rbt_row_get(row, KEY_COLUMN_ORDINAL, &actual_key));
    RBT_TEST_OK(rbt_row_get(row, SMALL_COLUMN_ORDINAL, &actual_small));
    RBT_TEST_OK(rbt_row_get(row, LARGE_COLUMN_ORDINAL, &actual_large));
    RBT_TEST_OK(rbt_row_get(row, TEXT_COLUMN_ORDINAL, &actual_text));

    RBT_TEST_CHECK(actual_key->type == RBT_TYPE_U64);
    RBT_TEST_CHECK(!actual_key->is_null);
    RBT_TEST_CHECK(actual_key->as.u64 == key_number);
    RBT_TEST_CHECK(actual_small->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(actual_large->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(actual_text->type == RBT_TYPE_UTF8);
    RBT_TEST_BYTES(&actual_small->as.bytes, small_data, small_size);
    RBT_TEST_BYTES(&actual_large->as.bytes, large_data, large_size);
    RBT_TEST_BYTES(&actual_text->as.bytes, text_data, text_size);
    RBT_TEST_OK(rbt_row_destroy(row));
}

static void expect_page_accounting(const struct rbt_stats *stats) {
    RBT_TEST_CHECK(stats->page_size == 512u);
    RBT_TEST_CHECK(stats->allocated_pages == stats->live_pages + stats->free_pages);
    RBT_TEST_CHECK(stats->live_pages ==
                   1u + stats->schema_pages + stats->tree_pages + stats->overflow_pages);
}

static void test_overflow_lifecycle(void) {
    static const unsigned char SMALL_BYTES[] = {'s', '\0', 'm', 'a', 'l', 'l'};
    static const unsigned char INLINE_BYTES[] = {'i', 'n', '\0', 'l', 'i', 'n', 'e'};
    static const char INLINE_TEXT[] = "ok";
    struct rbt_column columns[4] = {
        {.id = 1u, .type = RBT_TYPE_U64, .flags = RBT_COLUMN_KEY, .max_size = 0u},
        {.id = 2u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 32u},
        {.id = 3u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 8192u},
        {.id = 4u, .type = RBT_TYPE_UTF8, .flags = 0u, .max_size = 8192u},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000002),
        .columns = columns,
        .column_count = sizeof(columns) / sizeof(columns[0]),
    };
    unsigned char *large_bytes = malloc(LARGE_BYTES_SIZE);
    char *large_text = malloc(LARGE_TEXT_SIZE);
    char path[256];
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt_config create_config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    struct rbt_value values[4];
    struct rbt_record record;
    struct rbt_stats inline_stats;
    struct rbt_stats large_stats;
    struct rbt_stats shrink_stats;
    struct rbt_stats regrow_stats;
    struct rbt_stats delete_stats;
    struct rbt_stats reuse_stats;
    bool inserted = true;
    bool deleted = true;

    RBT_TEST_CHECK(large_bytes != NULL);
    RBT_TEST_CHECK(large_text != NULL);
    fill_binary(large_bytes, LARGE_BYTES_SIZE);
    fill_utf8(large_text, LARGE_TEXT_SIZE);

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_OK(rbt_file_create(path, 512u, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_create(&create_config, &tree));

    record = make_record(7u, SMALL_BYTES, sizeof(SMALL_BYTES), INLINE_BYTES, sizeof(INLINE_BYTES),
                         INLINE_TEXT, sizeof(INLINE_TEXT) - 1u, values);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);
    expect_row(tree, 7u, SMALL_BYTES, sizeof(SMALL_BYTES), INLINE_BYTES, sizeof(INLINE_BYTES),
               INLINE_TEXT, sizeof(INLINE_TEXT) - 1u);
    inline_stats = rbt_test_validate(tree);
    expect_page_accounting(&inline_stats);
    RBT_TEST_CHECK(inline_stats.item_count == 1u);
    RBT_TEST_CHECK(inline_stats.overflow_pages == 0u);

    record = make_record(7u, SMALL_BYTES, sizeof(SMALL_BYTES), large_bytes, LARGE_BYTES_SIZE,
                         large_text, LARGE_TEXT_SIZE, values);
    inserted = true;
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    expect_row(tree, 7u, SMALL_BYTES, sizeof(SMALL_BYTES), large_bytes, LARGE_BYTES_SIZE,
               large_text, LARGE_TEXT_SIZE);
    large_stats = rbt_test_validate(tree);
    expect_page_accounting(&large_stats);
    RBT_TEST_CHECK(large_stats.overflow_pages >= 9u);
    RBT_TEST_CHECK(large_stats.allocated_pages > inline_stats.allocated_pages);

    RBT_TEST_OK(rbt_destroy(tree));
    tree = NULL;
    RBT_TEST_OK(rbt_file_close(file));
    file = NULL;
    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    expect_row(tree, 7u, SMALL_BYTES, sizeof(SMALL_BYTES), large_bytes, LARGE_BYTES_SIZE,
               large_text, LARGE_TEXT_SIZE);

    record = make_record(7u, SMALL_BYTES, sizeof(SMALL_BYTES), INLINE_BYTES, sizeof(INLINE_BYTES),
                         INLINE_TEXT, sizeof(INLINE_TEXT) - 1u, values);
    inserted = true;
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    shrink_stats = rbt_test_validate(tree);
    expect_page_accounting(&shrink_stats);
    RBT_TEST_CHECK(shrink_stats.overflow_pages == 0u);
    RBT_TEST_CHECK(shrink_stats.free_pages >= large_stats.overflow_pages);

    record = make_record(7u, SMALL_BYTES, sizeof(SMALL_BYTES), large_bytes, LARGE_BYTES_SIZE,
                         large_text, LARGE_TEXT_SIZE, values);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    regrow_stats = rbt_test_validate(tree);
    expect_page_accounting(&regrow_stats);
    RBT_TEST_CHECK(regrow_stats.overflow_pages == large_stats.overflow_pages);
    RBT_TEST_CHECK(regrow_stats.allocated_pages <= large_stats.allocated_pages);

    {
        struct rbt_record delete_key = rbt_test_key(values, 1u);

        RBT_TEST_OK(rbt_delete(tree, &delete_key, &deleted));
        RBT_TEST_CHECK(deleted);
        deleted = true;
        RBT_TEST_ERRNO(rbt_delete(tree, &delete_key, &deleted), ENOENT);
        RBT_TEST_CHECK(!deleted);
    }
    delete_stats = rbt_test_validate(tree);
    expect_page_accounting(&delete_stats);
    RBT_TEST_CHECK(delete_stats.item_count == 0u);
    RBT_TEST_CHECK(delete_stats.overflow_pages == 0u);
    RBT_TEST_CHECK(delete_stats.free_pages >= regrow_stats.overflow_pages);

    record = make_record(99u, SMALL_BYTES, sizeof(SMALL_BYTES), large_bytes, LARGE_BYTES_SIZE,
                         large_text, LARGE_TEXT_SIZE, values);
    inserted = false;
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);
    reuse_stats = rbt_test_validate(tree);
    expect_page_accounting(&reuse_stats);
    RBT_TEST_CHECK(reuse_stats.overflow_pages == regrow_stats.overflow_pages);
    RBT_TEST_CHECK(reuse_stats.allocated_pages <= regrow_stats.allocated_pages);

    RBT_TEST_OK(rbt_destroy(tree));
    tree = NULL;
    RBT_TEST_OK(rbt_file_close(file));
    file = NULL;
    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    expect_row(tree, 99u, SMALL_BYTES, sizeof(SMALL_BYTES), large_bytes, LARGE_BYTES_SIZE,
               large_text, LARGE_TEXT_SIZE);
    reuse_stats = rbt_test_validate(tree);
    expect_page_accounting(&reuse_stats);

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));
    rbt_test_remove_database(path);
    free(large_text);
    free(large_bytes);
}

static void test_thousands_page_overflow_replacement(void) {
    struct rbt_column columns[2] = {
        {.id = 1u, .type = RBT_TYPE_U64, .flags = RBT_COLUMN_KEY, .max_size = 0u},
        {
            .id = 2u,
            .type = RBT_TYPE_BYTES,
            .flags = 0u,
            .max_size = LARGE_MUTATION_SIZE,
        },
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000005),
        .columns = columns,
        .column_count = sizeof(columns) / sizeof(columns[0]),
    };
    unsigned char *payload = malloc(LARGE_MUTATION_SIZE);
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    struct rbt_value values[2];
    struct rbt_record record;
    struct rbt_record key_record;
    struct rbt_row *row = NULL;
    const struct rbt_value *actual = NULL;
    struct rbt_stats stats;
    bool inserted = false;

    RBT_TEST_CHECK(payload != NULL);
    fill_binary(payload, LARGE_MUTATION_SIZE);
    values[0] = rbt_test_u64(42u);
    values[1] = rbt_test_bytes(payload, LARGE_MUTATION_SIZE);
    record = rbt_test_record(values, 2u);
    key_record = rbt_test_key(values, 1u);

    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));
    RBT_TEST_OK(rbt_create(&config, &tree));
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);

    payload[0] ^= 0xffu;
    inserted = true;
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    RBT_TEST_OK(rbt_get(tree, &key_record, &row));
    RBT_TEST_OK(rbt_row_get(row, 1u, &actual));
    RBT_TEST_BYTES(&actual->as.bytes, payload, LARGE_MUTATION_SIZE);
    RBT_TEST_OK(rbt_row_destroy(row));
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.overflow_pages == 2048u);

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_mem_destroy(memory));
    free(payload);
}

int main(void) {
    test_overflow_lifecycle();
    test_thousands_page_overflow_replacement();
    return 0;
}
