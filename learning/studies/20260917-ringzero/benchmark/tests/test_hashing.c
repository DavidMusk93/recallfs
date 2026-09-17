#include "hashing.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

static rz_selector *make_selector(rz_hash_kind kind, const rz_backend_id *backends,
                                  size_t backend_count) {
    const rz_selector_config config = {
        .kind = kind,
        .backends = backends,
        .backend_count = backend_count,
        .maglev_table_size = 4099,
        .ring_virtual_nodes = 256,
    };
    rz_selector *selector = NULL;
    CHECK(rz_selector_create(&config, &selector) == 0);
    CHECK(selector != NULL);
    return selector;
}

static size_t changed_keys(const rz_selector *before, const rz_selector *after,
                           uint64_t key_count) {
    size_t changed = 0;
    for (uint64_t key = 0; key < key_count; ++key) {
        uint64_t hash = rz_hash_key(key);
        if (rz_selector_select(before, hash) != rz_selector_select(after, hash)) {
            ++changed;
        }
    }
    return changed;
}

static void test_nsdi16_worked_example(void) {
    const rz_backend_id backends[] = {0, 1, 2};
    const size_t offsets[] = {3, 0, 3};
    const size_t skips[] = {4, 2, 1};
    const rz_backend_id expected[] = {1, 0, 1, 0, 2, 2, 0};
    rz_backend_id table[7] = {0};

    CHECK(rz_maglev_build_with_parameters(backends, offsets, skips, 3, 7, table) == 0);
    CHECK(memcmp(table, expected, sizeof(expected)) == 0);

    const rz_backend_id after_backends[] = {0, 2};
    const size_t after_offsets[] = {3, 3};
    const size_t after_skips[] = {4, 1};
    const rz_backend_id after_expected[] = {0, 0, 0, 0, 2, 2, 2};
    CHECK(rz_maglev_build_with_parameters(after_backends, after_offsets, after_skips, 2,
                                          7, table) == 0);
    CHECK(memcmp(table, after_expected, sizeof(after_expected)) == 0);
}

static void test_canonical_order_and_raw_order_sensitivity(void) {
    const rz_backend_id forward[] = {1, 2, 3, 4};
    const rz_backend_id reverse[] = {4, 3, 2, 1};
    rz_selector *left = make_selector(RZ_HASH_MAGLEV, forward, 4);
    rz_selector *right = make_selector(RZ_HASH_MAGLEV, reverse, 4);
    size_t left_size = 0;
    size_t right_size = 0;
    const rz_backend_id *left_table = rz_selector_maglev_table(left, &left_size);
    const rz_backend_id *right_table = rz_selector_maglev_table(right, &right_size);

    CHECK(left_size == 4099);
    CHECK(right_size == left_size);
    CHECK(memcmp(left_table, right_table, left_size * sizeof(*left_table)) == 0);

    rz_backend_id *raw_forward = calloc(left_size, sizeof(*raw_forward));
    rz_backend_id *raw_reverse = calloc(left_size, sizeof(*raw_reverse));
    CHECK(raw_forward != NULL);
    CHECK(raw_reverse != NULL);
    CHECK(rz_maglev_build_ordered(forward, 4, left_size, raw_forward) == 0);
    CHECK(rz_maglev_build_ordered(reverse, 4, left_size, raw_reverse) == 0);

    size_t changed = 0;
    for (size_t slot = 0; slot < left_size; ++slot) {
        changed += raw_forward[slot] != raw_reverse[slot];
    }
    CHECK(changed == 8);

    free(raw_reverse);
    free(raw_forward);
    rz_selector_destroy(right);
    rz_selector_destroy(left);
}

static void test_maglev_slot_balance(void) {
    rz_backend_id backends[32];
    for (size_t index = 0; index < 32; ++index) {
        backends[index] = (rz_backend_id)index;
    }
    rz_selector *selector = make_selector(RZ_HASH_MAGLEV, backends, 32);
    size_t table_size = 0;
    const rz_backend_id *table = rz_selector_maglev_table(selector, &table_size);
    size_t counts[32] = {0};
    for (size_t slot = 0; slot < table_size; ++slot) {
        CHECK(table[slot] < 32);
        ++counts[table[slot]];
    }
    size_t minimum = counts[0];
    size_t maximum = counts[0];
    size_t total = 0;
    for (size_t index = 0; index < 32; ++index) {
        if (counts[index] < minimum) {
            minimum = counts[index];
        }
        if (counts[index] > maximum) {
            maximum = counts[index];
        }
        total += counts[index];
    }
    CHECK(maximum - minimum == 1);
    CHECK(total == 4099);
    rz_selector_destroy(selector);
}

static bool contains(const rz_backend_id *backends, size_t backend_count,
                     rz_backend_id candidate) {
    for (size_t index = 0; index < backend_count; ++index) {
        if (backends[index] == candidate) {
            return true;
        }
    }
    return false;
}

static void test_all_selectors_return_configured_backends(void) {
    const rz_backend_id backends[] = {7, 11, 42, 99};
    const rz_hash_kind kinds[] = {
        RZ_HASH_MODULO, RZ_HASH_RING, RZ_HASH_RENDEZVOUS, RZ_HASH_JUMP, RZ_HASH_MAGLEV,
    };

    for (size_t kind_index = 0; kind_index < sizeof(kinds) / sizeof(kinds[0]);
         ++kind_index) {
        rz_selector *selector = make_selector(kinds[kind_index], backends,
                                              sizeof(backends) / sizeof(backends[0]));
        bool observed[4] = {false};
        for (uint64_t key = 0; key < 100000; ++key) {
            rz_backend_id selected = rz_selector_select(selector, rz_hash_key(key));
            CHECK(contains(backends, 4, selected));
            for (size_t index = 0; index < 4; ++index) {
                if (backends[index] == selected) {
                    observed[index] = true;
                }
            }
        }
        for (size_t index = 0; index < 4; ++index) {
            CHECK(observed[index]);
        }
        rz_selector_destroy(selector);
    }
}

static void test_membership_churn_bounds(void) {
    rz_backend_id before_ids[32];
    rz_backend_id after_add_ids[33];
    rz_backend_id after_remove_ids[31];
    for (size_t index = 0; index < 32; ++index) {
        before_ids[index] = (rz_backend_id)index;
        after_add_ids[index] = (rz_backend_id)index;
    }
    after_add_ids[32] = 32;
    for (size_t source = 0, destination = 0; source < 32; ++source) {
        if (source != 16) {
            after_remove_ids[destination++] = (rz_backend_id)source;
        }
    }

    rz_selector *modulo_before = make_selector(RZ_HASH_MODULO, before_ids, 32);
    rz_selector *modulo_after = make_selector(RZ_HASH_MODULO, after_add_ids, 33);
    CHECK(changed_keys(modulo_before, modulo_after, 100000) > 90000);
    rz_selector_destroy(modulo_after);
    rz_selector_destroy(modulo_before);

    rz_selector *maglev_before = make_selector(RZ_HASH_MAGLEV, before_ids, 32);
    rz_selector *maglev_after = make_selector(RZ_HASH_MAGLEV, after_add_ids, 33);
    size_t maglev_changed = changed_keys(maglev_before, maglev_after, 100000);
    CHECK(maglev_changed >= 2000);
    CHECK(maglev_changed < 8000);
    rz_selector_destroy(maglev_after);
    rz_selector_destroy(maglev_before);

    rz_selector *jump_before = make_selector(RZ_HASH_JUMP, before_ids, 32);
    rz_selector *jump_after = make_selector(RZ_HASH_JUMP, after_remove_ids, 31);
    size_t jump_changed = changed_keys(jump_before, jump_after, 100000);
    CHECK(jump_changed >= 45000);
    CHECK(jump_changed < 55000);
    rz_selector_destroy(jump_after);
    rz_selector_destroy(jump_before);
}

static void test_error_contracts(void) {
    const rz_backend_id one[] = {1};
    const rz_backend_id duplicate[] = {1, 1};
    rz_selector *selector = (rz_selector *)(uintptr_t)1;
    rz_selector_config config = {
        .kind = RZ_HASH_MODULO,
        .backends = NULL,
        .backend_count = 0,
        .maglev_table_size = 4099,
        .ring_virtual_nodes = 256,
    };

    CHECK(rz_selector_create(NULL, &selector) == EINVAL);
    CHECK(rz_selector_create(&config, NULL) == EINVAL);
    CHECK(rz_selector_create(&config, &selector) == EINVAL);
    CHECK(selector == NULL);

    config.backends = duplicate;
    config.backend_count = 2;
    CHECK(rz_selector_create(&config, &selector) == EEXIST);
    CHECK(selector == NULL);

    config.backends = one;
    config.backend_count = 1;
    config.kind = RZ_HASH_RING;
    config.ring_virtual_nodes = 0;
    CHECK(rz_selector_create(&config, &selector) == EINVAL);
    CHECK(selector == NULL);

    config.kind = RZ_HASH_MAGLEV;
    config.ring_virtual_nodes = 256;
    config.maglev_table_size = 4096;
    CHECK(rz_selector_create(&config, &selector) == EINVAL);
    CHECK(selector == NULL);

    config.kind = (rz_hash_kind)99;
    config.maglev_table_size = 4099;
    CHECK(rz_selector_create(&config, &selector) == EINVAL);
    CHECK(selector == NULL);
}

int main(void) {
    test_nsdi16_worked_example();
    test_canonical_order_and_raw_order_sensitivity();
    test_maglev_slot_balance();
    test_all_selectors_return_configured_backends();
    test_membership_churn_bounds();
    test_error_contracts();
    puts("PASS: 6 hashing suites");
    return 0;
}
