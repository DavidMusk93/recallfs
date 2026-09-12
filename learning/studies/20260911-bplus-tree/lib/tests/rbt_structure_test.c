#include "test_support.h"

enum {
    ITEM_COUNT = 512,
    OVERFLOW_SIZE = 700,
    VARIABLE_ITEM_COUNT = 180,
    VARIABLE_KEY_SIZE = 48,
};

enum insert_order {
    INSERT_ASCENDING,
    INSERT_DESCENDING,
    INSERT_SHUFFLED,
};

static void fill_overflow(unsigned char data[OVERFLOW_SIZE]) {
    size_t index;

    for (index = 0u; index < OVERFLOW_SIZE; ++index) {
        data[index] = (unsigned char)((index * 29u + 7u) & 0xffu);
    }
}

static struct rbt_record make_record(uint64_t number, const void *payload, size_t payload_size,
                                     struct rbt_value values[3]) {
    values[0] = rbt_test_u64(number);
    values[1] = rbt_test_u64(number * UINT64_C(17) + 3u);
    values[2] = rbt_test_bytes(payload, payload_size);
    return rbt_test_record(values, 3u);
}

static void expect_record(struct rbt *tree, uint64_t number, const void *payload,
                          size_t payload_size) {
    struct rbt_value key_value = rbt_test_u64(number);
    struct rbt_record key = rbt_test_record(&key_value, 1u);
    struct rbt_row *row = NULL;
    const struct rbt_value *number_value = NULL;
    const struct rbt_value *bytes_value = NULL;

    RBT_TEST_OK(rbt_get(tree, &key, &row));
    RBT_TEST_OK(rbt_row_get(row, 1u, &number_value));
    RBT_TEST_OK(rbt_row_get(row, 2u, &bytes_value));
    RBT_TEST_CHECK(number_value->as.u64 == number * UINT64_C(17) + 3u);
    RBT_TEST_BYTES(&bytes_value->as.bytes, payload, payload_size);
    RBT_TEST_OK(rbt_row_destroy(row));
}

static void shuffle(uint64_t *keys, size_t count) {
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    size_t index;

    for (index = count; index > 1u; --index) {
        size_t other;
        uint64_t temporary;

        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        other = (size_t)(state % index);
        temporary = keys[index - 1u];
        keys[index - 1u] = keys[other];
        keys[other] = temporary;
    }
}

static void make_order(enum insert_order order, uint64_t keys[ITEM_COUNT]) {
    size_t index;

    for (index = 0u; index < ITEM_COUNT; ++index) {
        keys[index] = order == INSERT_DESCENDING ? ITEM_COUNT - 1u - index : index;
    }
    if (order == INSERT_SHUFFLED) {
        shuffle(keys, ITEM_COUNT);
    }
}

static void run_order_case(enum insert_order order) {
    static const unsigned char SMALL_PAYLOAD[] = {'r', 'b', 't'};
    struct rbt_column columns[3] = {
        {.id = 1u, .type = RBT_TYPE_U64, .flags = RBT_COLUMN_KEY, .max_size = 0u},
        {.id = 2u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u},
        {.id = 3u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 1024u},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000004),
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
    unsigned char overflow[OVERFLOW_SIZE];
    uint64_t keys[ITEM_COUNT];
    struct rbt_stats full_stats;
    struct rbt_stats collapsed_stats;
    struct rbt_stats reused_stats;
    bool saw_reuse = false;
    size_t index;

    fill_overflow(overflow);
    make_order(order, keys);
    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));
    RBT_TEST_OK(rbt_create(&config, &tree));

    for (index = 0u; index < ITEM_COUNT; ++index) {
        uint64_t number = keys[index];
        const void *payload = number == 0u ? overflow : SMALL_PAYLOAD;
        size_t payload_size = number == 0u ? sizeof(overflow) : sizeof(SMALL_PAYLOAD);
        struct rbt_value values[3];
        struct rbt_record record = make_record(number, payload, payload_size, values);
        bool inserted = false;

        RBT_TEST_OK(rbt_put(tree, &record, &inserted));
        RBT_TEST_CHECK(inserted);
        if ((index % 31u) == 0u) {
            (void)rbt_test_validate(tree);
        }
    }
    full_stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(full_stats.item_count == ITEM_COUNT);
    RBT_TEST_CHECK(full_stats.height >= 3u);
    RBT_TEST_CHECK(full_stats.overflow_pages >= 2u);
    expect_record(tree, 0u, overflow, sizeof(overflow));

    for (index = 0u; index < ITEM_COUNT; ++index) {
        uint64_t number = keys[index];
        struct rbt_value key_value;
        struct rbt_record key;
        bool deleted = false;

        if (number == ITEM_COUNT / 2u) {
            continue;
        }
        key_value = rbt_test_u64(number);
        key = rbt_test_record(&key_value, 1u);
        RBT_TEST_OK(rbt_delete(tree, &key, &deleted));
        RBT_TEST_CHECK(deleted);
        if ((index % 29u) == 0u || number == 0u) {
            (void)rbt_test_validate(tree);
        }
    }
    collapsed_stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(collapsed_stats.item_count == 1u);
    RBT_TEST_CHECK(collapsed_stats.height == 1u);
    RBT_TEST_CHECK(collapsed_stats.overflow_pages == 0u);
    RBT_TEST_CHECK(collapsed_stats.free_pages > 0u);
    expect_record(tree, ITEM_COUNT / 2u, SMALL_PAYLOAD, sizeof(SMALL_PAYLOAD));

    for (index = 0u; index < ITEM_COUNT - 1u; ++index) {
        uint64_t number = ITEM_COUNT + index;
        struct rbt_value values[3];
        struct rbt_record record =
            make_record(number, SMALL_PAYLOAD, sizeof(SMALL_PAYLOAD), values);
        bool inserted = false;

        RBT_TEST_OK(rbt_put(tree, &record, &inserted));
        RBT_TEST_CHECK(inserted);
        if (!saw_reuse && index < 16u) {
            struct rbt_stats current = rbt_test_validate(tree);

            saw_reuse = current.allocated_pages == collapsed_stats.allocated_pages &&
                        current.free_pages < collapsed_stats.free_pages;
        }
    }
    RBT_TEST_CHECK(saw_reuse);
    reused_stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(reused_stats.height >= 3u);
    RBT_TEST_CHECK(reused_stats.item_count == ITEM_COUNT);

    for (index = 0u; index < ITEM_COUNT - 1u; ++index) {
        uint64_t number = ITEM_COUNT + index;
        struct rbt_value key_value = rbt_test_u64(number);
        struct rbt_record key = rbt_test_record(&key_value, 1u);
        bool deleted = false;

        RBT_TEST_OK(rbt_delete(tree, &key, &deleted));
        RBT_TEST_CHECK(deleted);
    }
    {
        struct rbt_value key_value = rbt_test_u64(ITEM_COUNT / 2u);
        struct rbt_record key = rbt_test_record(&key_value, 1u);
        bool deleted = false;

        RBT_TEST_OK(rbt_delete(tree, &key, &deleted));
        RBT_TEST_CHECK(deleted);
    }
    collapsed_stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(collapsed_stats.item_count == 0u);
    RBT_TEST_CHECK(collapsed_stats.height == 1u);

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_mem_destroy(memory));
}

static void make_variable_key(size_t index, char key[VARIABLE_KEY_SIZE]) {
    int length = snprintf(key, VARIABLE_KEY_SIZE, "%04zu-", index);
    size_t suffix = index % 29u;
    size_t position;

    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length + suffix < VARIABLE_KEY_SIZE);
    for (position = 0u; position < suffix; ++position) {
        key[(size_t)length + position] = (char)('a' + (int)(index % 26u));
    }
    key[(size_t)length + suffix] = '\0';
}

static void test_variable_separators(void) {
    struct rbt_column column = {
        .id = 1u,
        .type = RBT_TYPE_BYTES,
        .flags = RBT_COLUMN_KEY,
        .max_size = VARIABLE_KEY_SIZE,
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000005),
        .columns = &column,
        .column_count = 1u,
    };
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    uint64_t order[VARIABLE_ITEM_COUNT];
    size_t index;

    for (index = 0u; index < VARIABLE_ITEM_COUNT; ++index) {
        order[index] = index;
    }
    shuffle(order, VARIABLE_ITEM_COUNT);
    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));
    RBT_TEST_OK(rbt_create(&config, &tree));
    for (index = 0u; index < VARIABLE_ITEM_COUNT; ++index) {
        size_t number = (size_t)order[index];
        char key_data[VARIABLE_KEY_SIZE];
        struct rbt_value key_value;
        struct rbt_record record;
        bool inserted = false;

        make_variable_key(number, key_data);
        key_value = rbt_test_bytes(key_data, strlen(key_data));
        record = rbt_test_record(&key_value, 1u);
        RBT_TEST_OK(rbt_put(tree, &record, &inserted));
        RBT_TEST_CHECK(inserted);
        if ((index % 13u) == 0u) {
            (void)rbt_test_validate(tree);
        }
    }
    RBT_TEST_CHECK(rbt_test_validate(tree).height >= 3u);
    for (index = 0u; index < VARIABLE_ITEM_COUNT; ++index) {
        char key_data[VARIABLE_KEY_SIZE];
        struct rbt_value key_value;
        struct rbt_record key;
        bool deleted = false;

        make_variable_key(index, key_data);
        key_value = rbt_test_bytes(key_data, strlen(key_data));
        key = rbt_test_record(&key_value, 1u);
        RBT_TEST_OK(rbt_delete(tree, &key, &deleted));
        RBT_TEST_CHECK(deleted);
        if ((index % 11u) == 0u) {
            (void)rbt_test_validate(tree);
        }
    }
    RBT_TEST_CHECK(rbt_test_validate(tree).height == 1u);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_mem_destroy(memory));
}

int main(void) {
    run_order_case(INSERT_ASCENDING);
    run_order_case(INSERT_DESCENDING);
    run_order_case(INSERT_SHUFFLED);
    test_variable_separators();
    return 0;
}
