#include "test_support.h"

enum {
    ITEM_COUNT = 190,
    PAYLOAD_SIZE = 4,
    OVERFLOW_VALUE_SIZE = 1024,
    EXPANDED_OVERFLOW_VALUE_SIZE = 1600,
    OVERFLOW_CHAIN_PAGES = 3,
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
    bool oversized_page_count;
    bool lease_active;
    size_t acquire_calls;
    size_t release_calls;
    size_t read_page_calls;
    size_t commit_count;
    int forced_commit_result;
    struct commit_observation commits[MAX_COMMIT_OBSERVATIONS];
};

struct assertion_state {
    size_t failures;
};

struct balancing_observation {
    bool seen;
    size_t writes;
    size_t limit;
    uint32_t height;
};

static int spy_acquire(void *context) {
    struct storage_spy *spy = context;
    int result;

    ++spy->acquire_calls;
    RBT_TEST_CHECK(!spy->lease_active);
    result = spy->backing.acquire(spy->backing.context);
    if (result == 0) {
        spy->lease_active = true;
    }
    return result;
}

static void spy_release(void *context) {
    struct storage_spy *spy = context;

    RBT_TEST_CHECK(spy->lease_active);
    ++spy->release_calls;
    spy->lease_active = false;
    spy->backing.release(spy->backing.context);
}

static int spy_read_page(void *context, uint64_t page_id, void *data_out) {
    struct storage_spy *spy = context;

    RBT_TEST_CHECK(spy->lease_active);
    if (spy->observing) {
        ++spy->read_page_calls;
    }
    return spy->backing.read_page(spy->backing.context, page_id, data_out);
}

static int spy_page_count(void *context, uint64_t *out_count) {
    struct storage_spy *spy = context;

    RBT_TEST_CHECK(spy->lease_active);
    if (spy->oversized_page_count) {
        *out_count = UINT64_MAX;
        return 0;
    }
    return spy->backing.page_count(spy->backing.context, out_count);
}

static int spy_commit_pages(void *context, const struct rbt_page_update *updates,
                            size_t update_count) {
    struct storage_spy *spy = context;

    RBT_TEST_CHECK(spy->lease_active);
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
    out_storage->acquire = spy_acquire;
    out_storage->release = spy_release;
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

static void expect_exact_commit(struct assertion_state *state, const char *operation,
                                const struct storage_spy *spy, const uint64_t *expected_page_ids,
                                size_t expected_page_count) {
    bool passed = !spy->capture_overflow && spy->commit_count == 1u &&
                  spy->commits[0].page_count == expected_page_count &&
                  spy->commits[0].captured_page_count == expected_page_count;
    size_t index;

    for (index = 0u; passed && index < expected_page_count; ++index) {
        passed = spy->commits[0].page_ids[index] == expected_page_ids[index];
    }
    if (passed) {
        return;
    }
    fprintf(stderr,
            "%s: assertion failed: exact commit page IDs; expected writes=%zu, "
            "observed commit_count=%zu, read_page_calls=%zu;",
            operation, expected_page_count, spy->commit_count, spy->read_page_calls);
    print_commit_observations(spy);
    fprintf(stderr, "\n");
    ++state->failures;
}

static size_t balancing_write_limit(uint32_t height) {
    /* Metadata plus at most two path/sibling pages per level and split/root slack. */
    return 2u * (size_t)height + 4u;
}

static void capture_balancing_commit(const struct storage_spy *spy, const struct rbt_stats *before,
                                     struct balancing_observation *observation) {
    size_t write_limit = balancing_write_limit(before->height);
    bool includes_metadata = false;
    size_t index;

    RBT_TEST_CHECK(!spy->capture_overflow);
    RBT_TEST_CHECK(spy->commit_count == 1u);
    RBT_TEST_CHECK(spy->commits[0].captured_page_count == spy->commits[0].page_count);
    RBT_TEST_CHECK(spy->commits[0].page_count <= write_limit);
    RBT_TEST_CHECK(spy->commits[0].page_count < before->live_pages);
    for (index = 0u; index < spy->commits[0].page_count; ++index) {
        size_t other;

        includes_metadata = includes_metadata || spy->commits[0].page_ids[index] == 0u;
        for (other = index + 1u; other < spy->commits[0].page_count; ++other) {
            RBT_TEST_CHECK(spy->commits[0].page_ids[index] != spy->commits[0].page_ids[other]);
        }
    }
    RBT_TEST_CHECK(includes_metadata);
    observation->seen = true;
    observation->writes = spy->commits[0].page_count;
    observation->limit = write_limit;
    observation->height = before->height;
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

static void fill_overflow_payload(unsigned char *data, size_t size, unsigned char seed) {
    size_t index;

    for (index = 0u; index < size; ++index) {
        data[index] = (unsigned char)(seed + (unsigned char)(index * 37u));
    }
}

static struct rbt_record make_overflow_locality_record(uint64_t scalar, const unsigned char *first,
                                                       size_t first_size,
                                                       const unsigned char *second,
                                                       size_t second_size, struct rbt_value key[1],
                                                       struct rbt_value values[3]) {
    key[0] = rbt_test_u64(7u);
    values[0] = rbt_test_u64(scalar);
    values[1] = rbt_test_bytes(first, first_size);
    values[2] = rbt_test_bytes(second, second_size);
    return rbt_test_record(key, 1u, values, 3u);
}

static void expect_overflow_locality_row(struct rbt *tree, uint64_t scalar,
                                         const unsigned char *first, size_t first_size,
                                         const unsigned char *second, size_t second_size) {
    struct rbt_value key_value = rbt_test_u64(7u);
    struct rbt_record key = rbt_test_key(&key_value, 1u);
    struct rbt_row *row = NULL;
    const struct rbt_value *actual_scalar = NULL;
    const struct rbt_value *actual_first = NULL;
    const struct rbt_value *actual_second = NULL;

    RBT_TEST_OK(rbt_get(tree, &key, &row));
    RBT_TEST_OK(rbt_row_get_value(row, 0u, &actual_scalar));
    RBT_TEST_OK(rbt_row_get_value(row, 1u, &actual_first));
    RBT_TEST_OK(rbt_row_get_value(row, 2u, &actual_second));
    RBT_TEST_CHECK(actual_scalar->type == RBT_TYPE_U64);
    RBT_TEST_CHECK(!actual_scalar->is_null);
    RBT_TEST_CHECK(actual_scalar->as.u64 == scalar);
    RBT_TEST_CHECK(actual_first->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!actual_first->is_null);
    RBT_TEST_BYTES(&actual_first->as.bytes, first, first_size);
    RBT_TEST_CHECK(actual_second->type == RBT_TYPE_BYTES);
    RBT_TEST_CHECK(!actual_second->is_null);
    RBT_TEST_BYTES(&actual_second->as.bytes, second, second_size);
    RBT_TEST_OK(rbt_row_destroy(row));
}

static void test_unchanged_overflow_locality(void) {
    struct rbt_column key_column = {
        .id = 1u,
        .type = RBT_TYPE_U64,
        .flags = 0u,
        .max_size = 0u,
    };
    struct rbt_column value_columns[3] = {
        {.id = 2u, .type = RBT_TYPE_U64, .flags = 0u, .max_size = 0u},
        {.id = 3u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 2048u},
        {.id = 4u, .type = RBT_TYPE_BYTES, .flags = 0u, .max_size = 2048u},
    };
    struct rbt_schema schema = {
        .id = UINT64_C(0x7262740000000011),
        .key_columns = &key_column,
        .key_column_count = 1u,
        .value_columns = value_columns,
        .value_column_count = 3u,
    };
    unsigned char first[OVERFLOW_VALUE_SIZE];
    unsigned char changed_first[OVERFLOW_VALUE_SIZE];
    unsigned char second[OVERFLOW_VALUE_SIZE];
    unsigned char expanded_second[EXPANDED_OVERFLOW_VALUE_SIZE];
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
    struct rbt_value key[1];
    struct rbt_value values[3];
    struct rbt_record record;
    struct rbt_stats committed_stats;
    struct rbt_stats reopened_stats;
    uint64_t leaf_page_id;
    uint64_t first_chain[OVERFLOW_CHAIN_PAGES];
    uint64_t second_chain[OVERFLOW_CHAIN_PAGES];
    uint64_t expected_scalar_pages[2];
    uint64_t expected_first_update_pages[2u + OVERFLOW_CHAIN_PAGES];
    uint64_t expected_failed_pages[3u + OVERFLOW_CHAIN_PAGES];
    size_t index;
    bool inserted = false;

    fill_overflow_payload(first, sizeof(first), 11u);
    fill_overflow_payload(changed_first, sizeof(changed_first), 73u);
    fill_overflow_payload(second, sizeof(second), 149u);
    fill_overflow_payload(expanded_second, sizeof(expanded_second), 211u);

    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &backing_storage));
    spy_storage_init(&spy, &backing_storage, &storage);
    RBT_TEST_OK(rbt_create(&config, &tree));

    record = make_overflow_locality_record(100u, first, sizeof(first), second, sizeof(second), key,
                                           values);
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(inserted);
    RBT_TEST_CHECK(spy.commit_count == 1u);
    RBT_TEST_CHECK(spy.commits[0].page_count == 2u + 2u * OVERFLOW_CHAIN_PAGES);
    leaf_page_id = spy.commits[0].page_ids[1u];
    for (index = 0u; index < OVERFLOW_CHAIN_PAGES; ++index) {
        first_chain[index] = spy.commits[0].page_ids[2u + index];
        second_chain[index] = spy.commits[0].page_ids[2u + OVERFLOW_CHAIN_PAGES + index];
    }

    expected_scalar_pages[0] = 0u;
    expected_scalar_pages[1] = leaf_page_id;
    record = make_overflow_locality_record(101u, first, sizeof(first), second, sizeof(second), key,
                                           values);
    inserted = true;
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    expect_exact_commit(&assertions, "scalar update beside overflow columns", &spy,
                        expected_scalar_pages,
                        sizeof(expected_scalar_pages) / sizeof(expected_scalar_pages[0]));
    expect_overflow_locality_row(tree, 101u, first, sizeof(first), second, sizeof(second));

    inserted = true;
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    expect_no_commit(&assertions, "overflow row no-op", &spy);

    expected_first_update_pages[0] = 0u;
    expected_first_update_pages[1] = leaf_page_id;
    for (index = 0u; index < OVERFLOW_CHAIN_PAGES; ++index) {
        expected_first_update_pages[2u + index] = first_chain[index];
    }
    record = make_overflow_locality_record(101u, changed_first, sizeof(changed_first), second,
                                           sizeof(second), key, values);
    inserted = true;
    spy_reset(&spy);
    RBT_TEST_OK(rbt_put(tree, &record, &inserted));
    RBT_TEST_CHECK(!inserted);
    expect_exact_commit(
        &assertions, "single overflow column update", &spy, expected_first_update_pages,
        sizeof(expected_first_update_pages) / sizeof(expected_first_update_pages[0]));
    for (index = 0u; index < OVERFLOW_CHAIN_PAGES; ++index) {
        size_t page_index;

        for (page_index = 0u; page_index < spy.commits[0].page_count; ++page_index) {
            RBT_TEST_CHECK(spy.commits[0].page_ids[page_index] != second_chain[index]);
        }
    }
    expect_overflow_locality_row(tree, 101u, changed_first, sizeof(changed_first), second,
                                 sizeof(second));
    committed_stats = rbt_test_validate(tree);

    expected_failed_pages[0] = 0u;
    expected_failed_pages[1] = leaf_page_id;
    for (index = 0u; index < OVERFLOW_CHAIN_PAGES; ++index) {
        expected_failed_pages[2u + index] = second_chain[index];
    }
    expected_failed_pages[2u + OVERFLOW_CHAIN_PAGES] = committed_stats.allocated_pages;
    record = make_overflow_locality_record(102u, changed_first, sizeof(changed_first),
                                           expanded_second, sizeof(expanded_second), key, values);
    inserted = true;
    spy_reset(&spy);
    spy.forced_commit_result = -EIO;
    RBT_TEST_ERRNO(rbt_put(tree, &record, &inserted), EIO);
    RBT_TEST_CHECK(!inserted);
    expect_exact_commit(&assertions, "failed expanded overflow update", &spy, expected_failed_pages,
                        sizeof(expected_failed_pages) / sizeof(expected_failed_pages[0]));

    RBT_TEST_OK(rbt_destroy(tree));
    tree = NULL;
    spy.forced_commit_result = 0;
    spy_reset(&spy);
    RBT_TEST_OK(rbt_open(&storage, &tree));
    expect_overflow_locality_row(tree, 101u, changed_first, sizeof(changed_first), second,
                                 sizeof(second));
    reopened_stats = rbt_test_validate(tree);
    RBT_TEST_CHECK(reopened_stats.allocated_pages == committed_stats.allocated_pages);
    RBT_TEST_CHECK(reopened_stats.live_pages == committed_stats.live_pages);
    RBT_TEST_CHECK(reopened_stats.free_pages == committed_stats.free_pages);

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_CHECK(spy.acquire_calls == 2u);
    RBT_TEST_CHECK(spy.release_calls == 2u);
    RBT_TEST_CHECK(!spy.lease_active);
    RBT_TEST_OK(rbt_mem_destroy(memory));
    RBT_TEST_CHECK(assertions.failures == 0u);
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

    spy_reset(&spy);
    spy.oversized_page_count = true;
    RBT_TEST_ERRNO(rbt_validate(tree, NULL, NULL, 0u), EBADMSG);
    RBT_TEST_CHECK(spy.read_page_calls == 0u);
    spy.oversized_page_count = false;

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
    RBT_TEST_CHECK(spy.acquire_calls == 1u);
    RBT_TEST_CHECK(spy.release_calls == 1u);
    RBT_TEST_CHECK(!spy.lease_active);
    RBT_TEST_OK(rbt_mem_destroy(memory));
    RBT_TEST_CHECK(assertions.failures == 0u);
}

static void test_balancing_locality(void) {
    static const unsigned char PAYLOAD[PAYLOAD_SIZE] = {'t', 'r', 'e', 'e'};
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
        .id = UINT64_C(0x7262740000000013),
        .key_columns = &key_column,
        .key_column_count = 1u,
        .value_columns = value_columns,
        .value_column_count = 2u,
    };
    struct rbt_mem *memory = NULL;
    struct rbt_storage backing_storage;
    struct rbt_storage storage;
    struct storage_spy spy;
    struct rbt_config config = {
        .storage = &storage,
        .schema = &schema,
    };
    struct rbt *tree = NULL;
    struct rbt_stats before;
    struct rbt_stats after;
    struct balancing_observation leaf_split = {0};
    struct balancing_observation internal_split = {0};
    struct balancing_observation borrow = {0};
    struct balancing_observation merge = {0};
    struct balancing_observation root_collapse = {0};
    uint64_t inserted_count = 0u;
    uint64_t number;

    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &backing_storage));
    spy_storage_init(&spy, &backing_storage, &storage);
    RBT_TEST_OK(rbt_create(&config, &tree));
    before = rbt_test_validate(tree);

    for (number = 0u; number < ITEM_COUNT && !internal_split.seen; ++number) {
        struct rbt_value key[1];
        struct rbt_value values[2];
        struct rbt_record record =
            make_record(number, number * UINT64_C(17) + 3u, PAYLOAD, key, values);
        bool inserted = false;

        spy_reset(&spy);
        RBT_TEST_OK(rbt_put(tree, &record, &inserted));
        RBT_TEST_CHECK(inserted);
        after = rbt_test_validate(tree);
        inserted_count = number + 1u;
        if (!leaf_split.seen && after.tree_pages > before.tree_pages &&
            after.height == before.height && before.height >= 2u &&
            spy.commits[0].page_count < before.live_pages) {
            capture_balancing_commit(&spy, &before, &leaf_split);
        }
        if (after.height > before.height && before.height >= 2u) {
            capture_balancing_commit(&spy, &before, &internal_split);
        }
        before = after;
    }
    RBT_TEST_CHECK(leaf_split.seen);
    RBT_TEST_CHECK(internal_split.seen);
    RBT_TEST_CHECK(before.height >= 3u);

    for (number = 0u; number < inserted_count; ++number) {
        uint64_t odd_count = inserted_count / 2u;
        uint64_t highest_odd =
            (inserted_count - 1u) - (((inserted_count - 1u) & 1u) == 0u ? 1u : 0u);
        uint64_t highest_even = (inserted_count - 1u) & ~UINT64_C(1);
        uint64_t key_number = number < odd_count ? highest_odd - number * 2u
                                                 : highest_even - (number - odd_count) * 2u;
        struct rbt_value key_value = rbt_test_u64(key_number);
        struct rbt_record key = rbt_test_key(&key_value, 1u);
        bool deleted = false;

        spy_reset(&spy);
        RBT_TEST_OK(rbt_delete(tree, &key, &deleted));
        RBT_TEST_CHECK(deleted);
        after = rbt_test_validate(tree);
        if (!borrow.seen && after.height == before.height &&
            after.tree_pages == before.tree_pages && spy.commits[0].page_count >= 4u) {
            capture_balancing_commit(&spy, &before, &borrow);
        }
        if (!merge.seen && after.height == before.height && after.tree_pages < before.tree_pages) {
            capture_balancing_commit(&spy, &before, &merge);
        }
        if (!root_collapse.seen && after.height < before.height) {
            capture_balancing_commit(&spy, &before, &root_collapse);
        }
        before = after;
    }
    RBT_TEST_CHECK(borrow.seen);
    RBT_TEST_CHECK(merge.seen);
    RBT_TEST_CHECK(root_collapse.seen);
    RBT_TEST_CHECK(before.item_count == 0u);
    RBT_TEST_CHECK(before.height == 1u);
    fprintf(stderr,
            "balancing locality: leaf_split=%zu/%zu@h%u internal_split=%zu/%zu@h%u "
            "borrow=%zu/%zu@h%u merge=%zu/%zu@h%u root_collapse=%zu/%zu@h%u\n",
            leaf_split.writes, leaf_split.limit, leaf_split.height, internal_split.writes,
            internal_split.limit, internal_split.height, borrow.writes, borrow.limit, borrow.height,
            merge.writes, merge.limit, merge.height, root_collapse.writes, root_collapse.limit,
            root_collapse.height);

    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_CHECK(spy.acquire_calls == 1u);
    RBT_TEST_CHECK(spy.release_calls == 1u);
    RBT_TEST_CHECK(!spy.lease_active);
    RBT_TEST_OK(rbt_mem_destroy(memory));
}

int main(void) {
    test_unchanged_overflow_locality();
    test_mutation_locality();
    test_balancing_locality();
    return 0;
}
