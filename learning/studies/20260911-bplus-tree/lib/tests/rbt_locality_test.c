#include "test_support.h"

enum {
    ITEM_COUNT = 190,
    PAYLOAD_SIZE = 4,
    MAX_COMMIT_OBSERVATIONS = 4,
    MAX_CAPTURED_PAGE_IDS = 256,
};

static const size_t LOCAL_WRITE_LIMIT = 2u;

struct commit_observation {
    size_t page_count;
    size_t captured_page_count;
    uint64_t page_ids[MAX_CAPTURED_PAGE_IDS];
};

struct storage_spy {
    struct rbt_storage backing;
    bool observing;
    bool capture_overflow;
    size_t read_page_calls;
    size_t commit_count;
    int forced_commit_result;
    struct commit_observation commits[MAX_COMMIT_OBSERVATIONS];
};

struct assertion_state {
    size_t failures;
};

static int spy_read_page(void *context, uint64_t page_id, void *data_out) {
    struct storage_spy *spy = context;

    if (spy->observing) {
        ++spy->read_page_calls;
    }
    return spy->backing.read_page(spy->backing.context, page_id, data_out);
}

static int spy_page_count(void *context, uint64_t *out_count) {
    struct storage_spy *spy = context;

    return spy->backing.page_count(spy->backing.context, out_count);
}

static int spy_commit_pages(void *context, const struct rbt_page_update *updates,
                            size_t update_count) {
    struct storage_spy *spy = context;

    if (spy->observing) {
        size_t commit_index = spy->commit_count;

        ++spy->commit_count;
        if (commit_index >= MAX_COMMIT_OBSERVATIONS) {
            spy->capture_overflow = true;
        } else {
            struct commit_observation *observation = &spy->commits[commit_index];
            size_t index;

            observation->page_count = update_count;
            observation->captured_page_count =
                update_count < MAX_CAPTURED_PAGE_IDS ? update_count : MAX_CAPTURED_PAGE_IDS;
            if (observation->captured_page_count != update_count) {
                spy->capture_overflow = true;
            }
            for (index = 0u; index < observation->captured_page_count; ++index) {
                observation->page_ids[index] = updates[index].page_id;
            }
        }
    }
    if (spy->forced_commit_result != 0) {
        return spy->forced_commit_result;
    }
    return spy->backing.commit_pages(spy->backing.context, updates, update_count);
}

static void spy_storage_init(struct storage_spy *spy, const struct rbt_storage *backing,
                             struct rbt_storage *out_storage) {
    memset(spy, 0, sizeof(*spy));
    spy->backing = *backing;
    *out_storage = *backing;
    out_storage->context = spy;
    out_storage->read_page = spy_read_page;
    out_storage->page_count = spy_page_count;
    out_storage->commit_pages = spy_commit_pages;
}

static void spy_reset(struct storage_spy *spy) {
    spy->observing = true;
    spy->capture_overflow = false;
    spy->read_page_calls = 0u;
    spy->commit_count = 0u;
    memset(spy->commits, 0, sizeof(spy->commits));
}

static void print_commit_observations(const struct storage_spy *spy) {
    size_t commit_limit =
        spy->commit_count < MAX_COMMIT_OBSERVATIONS ? spy->commit_count : MAX_COMMIT_OBSERVATIONS;
    size_t commit_index;

    for (commit_index = 0u; commit_index < commit_limit; ++commit_index) {
        const struct commit_observation *observation = &spy->commits[commit_index];
        size_t page_index;

        fprintf(stderr, " commit[%zu]={writes=%zu, page_ids=[", commit_index,
                observation->page_count);
        for (page_index = 0u; page_index < observation->captured_page_count; ++page_index) {
            fprintf(stderr, "%s%llu", page_index == 0u ? "" : ",",
                    (unsigned long long)observation->page_ids[page_index]);
        }
        fprintf(stderr, "]}");
    }
}

static void expect_local_commit(struct assertion_state *state, const char *operation,
                                const struct storage_spy *spy, size_t write_limit) {
    bool passed = !spy->capture_overflow && spy->commit_count == 1u &&
                  spy->commits[0].page_count <= write_limit;

    if (passed) {
        return;
    }
    fprintf(stderr,
            "%s: assertion failed: commit_count == 1 && write_count <= %zu; "
            "observed commit_count=%zu, read_page_calls=%zu;",
            operation, write_limit, spy->commit_count, spy->read_page_calls);
    print_commit_observations(spy);
    fprintf(stderr, "\n");
    ++state->failures;
}

static void expect_no_commit(struct assertion_state *state, const char *operation,
                             const struct storage_spy *spy) {
    if (!spy->capture_overflow && spy->commit_count == 0u) {
        return;
    }
    fprintf(stderr,
            "%s: assertion failed: commit_count == 0; observed commit_count=%zu, "
            "read_page_calls=%zu;",
            operation, spy->commit_count, spy->read_page_calls);
    print_commit_observations(spy);
    fprintf(stderr, "\n");
    ++state->failures;
}

static void make_payload(uint64_t key, unsigned char payload[PAYLOAD_SIZE]) {
    payload[0] = (unsigned char)key;
    payload[1] = (unsigned char)(key >> 8u);
    payload[2] = (unsigned char)(key ^ UINT64_C(0x5a));
    payload[3] = (unsigned char)((key * UINT64_C(17)) ^ UINT64_C(0xa5));
}

static struct rbt_record make_record(uint64_t key_number, uint64_t fixed_value,
                                     const unsigned char payload[PAYLOAD_SIZE],
                                     struct rbt_value key[1], struct rbt_value values[2]) {
    key[0] = rbt_test_u64(key_number);
    values[0] = rbt_test_u64(fixed_value);
    values[1] = rbt_test_bytes(payload, PAYLOAD_SIZE);
    return rbt_test_record(key, 1u, values, 2u);
}

static void expect_row(struct rbt *tree, uint64_t key_number, uint64_t fixed_value,
                       const unsigned char payload[PAYLOAD_SIZE]) {
    struct rbt_value key_value = rbt_test_u64(key_number);
    struct rbt_record key = rbt_test_key(&key_value, 1u);
    struct rbt_row *row = NULL;
    const struct rbt_value *actual_fixed = NULL;
    const struct rbt_value *actual_variable = NULL;

    RBT_TEST_OK(rbt_get(tree, &key, &row));
    RBT_TEST_OK(rbt_row_get_value(row, 0u, &actual_fixed));
    RBT_TEST_OK(rbt_row_get_value(row, 1u, &actual_variable));
    RBT_TEST_CHECK(actual_fixed->type == RBT_TYPE_U64);
    RBT_TEST_CHECK(!actual_fixed->is_null);
    RBT_TEST_CHECK(actual_fixed->as.u64 == fixed_value);
    RBT_TEST_CHECK(actual_variable->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!actual_variable->is_null);
    RBT_TEST_BYTES(&actual_variable->as.bytes, payload, PAYLOAD_SIZE);
    RBT_TEST_OK(rbt_row_destroy(row));
}

static void expect_missing(struct rbt *tree, uint64_t key_number) {
    struct rbt_value key_value = rbt_test_u64(key_number);
    struct rbt_record key = rbt_test_key(&key_value, 1u);
    struct rbt_row *row = NULL;

    RBT_TEST_ERRNO(rbt_get(tree, &key, &row), ENOENT);
    RBT_TEST_CHECK(row == NULL);
}

static void populate_tree(struct rbt *tree) {
    uint64_t index;

    for (index = 0u; index < ITEM_COUNT; ++index) {
        uint64_t key_number = index * 2u;
        unsigned char payload[PAYLOAD_SIZE];
        struct rbt_value key[1];
        struct rbt_value values[2];
        struct rbt_record record;
        bool inserted = false;

        make_payload(key_number, payload);
        record = make_record(key_number, key_number * UINT64_C(17) + 3u, payload, key, values);
        RBT_TEST_OK(rbt_put(tree, &record, &inserted));
        RBT_TEST_CHECK(inserted);
    }
}

static void test_mutation_locality(void) {
    static const unsigned char UPDATED_PAYLOAD[PAYLOAD_SIZE] = {'e', 'd', 'i', 't'};
    static const unsigned char INSERTED_PAYLOAD[PAYLOAD_SIZE] = {'n', 'e', 'w', '!'};
    struct rbt_column key_column = {
        .id = 1u,
        .type = RBT_TYPE_U64,
        .flags = 0u,
        .max_size = 0u,
    };
    struct rbt_column value_columns[2] = {
        {.id = 2u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u},
        {.id = 3u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = PAYLOAD_SIZE},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000003),
        .key_columns = &key_column,
        .key_column_count = 1u,
        .value_columns = value_columns,
        .value_column_count = 2u,
    };
    struct rbt_mem *memory = NULL;
    struct rbt_storage backing_storage;
    struct rbt_storage storage;
    struct storage_spy spy;
    struct assertion_state assertions = {0};
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    struct rbt_stats stats;
    uint64_t baseline_tree_pages;
    uint32_t baseline_height;
    struct rbt_value key[1];
    struct rbt_value values[2];
    struct rbt_record record;
    bool inserted = true;
    bool deleted = false;

    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &backing_storage));
    spy_storage_init(&spy, &backing_storage, &storage);
    RBT_TEST_OK(rbt_create(&config, &tree));
    populate_tree(tree);
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.item_count == ITEM_COUNT);
    RBT_TEST_CHECK(stats.height >= 3u);
    RBT_TEST_CHECK(stats.tree_pages > LOCAL_WRITE_LIMIT);
    baseline_tree_pages = stats.tree_pages;
    baseline_height = stats.height;

    record = make_record(200u, UINT64_C(9001), UPDATED_PAYLOAD, key, values);
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    expect_local_commit(&assertions, "existing-row update", &spy, LOCAL_WRITE_LIMIT);
    RBT_TEST_CHECK(spy.commits[0].page_count == 2u);
    expect_row(tree, 200u, UINT64_C(9001), UPDATED_PAYLOAD);
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.item_count == ITEM_COUNT);
    RBT_TEST_CHECK(stats.tree_pages == baseline_tree_pages);
    RBT_TEST_CHECK(stats.height == baseline_height);

    inserted = true;
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    expect_no_commit(&assertions, "same-value put", &spy);
    expect_row(tree, 200u, UINT64_C(9001), UPDATED_PAYLOAD);
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.item_count == ITEM_COUNT);
    RBT_TEST_CHECK(stats.tree_pages == baseline_tree_pages);
    RBT_TEST_CHECK(stats.height == baseline_height);

    record = make_record(375u, UINT64_C(6378), INSERTED_PAYLOAD, key, values);
    inserted = false;
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);
    expect_local_commit(&assertions, "non-splitting insert", &spy, LOCAL_WRITE_LIMIT);
    RBT_TEST_CHECK(spy.commits[0].page_count == 2u);
    expect_row(tree, 375u, UINT64_C(6378), INSERTED_PAYLOAD);
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.item_count == ITEM_COUNT + 1u);
    RBT_TEST_CHECK(stats.tree_pages == baseline_tree_pages);
    RBT_TEST_CHECK(stats.height == baseline_height);

    {
        struct rbt_value delete_key_value = rbt_test_u64(375u);
        struct rbt_record delete_key = rbt_test_key(&delete_key_value, 1u);

        spy_reset(&spy);
        RBT_TEST_OK(rbt_delete(tree, &delete_key, &deleted));
    }
    RBT_TEST_CHECK(deleted);
    expect_local_commit(&assertions, "non-rebalancing delete", &spy, LOCAL_WRITE_LIMIT);
    RBT_TEST_CHECK(spy.commits[0].page_count == 2u);
    expect_missing(tree, 375u);
    expect_row(tree, 200u, UINT64_C(9001), UPDATED_PAYLOAD);
    stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(stats.item_count == ITEM_COUNT);
    RBT_TEST_CHECK(stats.tree_pages == baseline_tree_pages);
    RBT_TEST_CHECK(stats.height == baseline_height);

    record = make_record(375u, UINT64_C(6378), INSERTED_PAYLOAD, key, values);
    inserted = true;
    spy_reset(&spy);
    spy.forced_commit_result = -EOWNERDEAD;
    RBT_TEST_ERRNO(rbt_put(tree, &record, &inserted), EOWNERDEAD);
    RBT_TEST_CHECK(!inserted);
    {
        struct rbt_value poisoned_key_value = rbt_test_u64(200u);
        struct rbt_record poisoned_key = rbt_test_key(&poisoned_key_value, 1u);
        struct rbt_row *row = NULL;

        RBT_TEST_ERRNO(rbt_get(tree, &poisoned_key, &row), EOWNERDEAD);
        RBT_TEST_CHECK(row == NULL);
    }

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_mem_destroy(memory));
    RBT_TEST_CHECK(assertions.failures == 0u);
}

int main(void) {
    test_mutation_locality();
    return 0;
}
