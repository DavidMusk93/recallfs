#include "odt_test_support.h"

#include "odt_internal.h"

#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
    TEST_POINT_COUNT = 8,
    TEST_POINT_PADDING = 5,
    TEST_RESULT_PADDING = 7,
};

static const odt_domain test_domain = {-2.0, -2.0, 2.0, 2.0};
static const odt_site test_sites[] = {
    {-1.0, 0.0, 11},
    {1.0, 0.0, 22},
    {0.0, 1.5, 33},
};
static const odt_point test_points[TEST_POINT_COUNT] = {
    {-1.0, 0.0},  {1.0, 0.0}, {0.0, 0.0}, {0.0, 1.5},
    {-2.0, -2.0}, {3.0, 0.0}, {NAN, 0.0}, {0.0, INFINITY},
};

static const odt_query_result result_pattern = {
    ODT_RESULT_ERROR,
    ODT_CORRUPT_DATA,
    INT32_MIN,
};

static const odt_batch_stats stats_pattern = {
    UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
};

static uint64_t hash_bytes(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < size; ++index) {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void expect_result_equal(const odt_query_result *actual, const odt_query_result *expected) {
    ODT_TEST_CHECK(actual->kind == expected->kind);
    ODT_TEST_CHECK(actual->status == expected->status);
    ODT_TEST_CHECK(actual->region_id == expected->region_id);
}

static void expect_stats_equal(const odt_batch_stats *actual, const odt_batch_stats *expected) {
    ODT_TEST_CHECK(actual->attempted == expected->attempted);
    ODT_TEST_CHECK(actual->region_count == expected->region_count);
    ODT_TEST_CHECK(actual->outside_count == expected->outside_count);
    ODT_TEST_CHECK(actual->failed_count == expected->failed_count);
    ODT_TEST_CHECK(actual->comparisons == expected->comparisons);
    ODT_TEST_CHECK(actual->exact_fallbacks == expected->exact_fallbacks);
}

static void scalar_expectations(const odt_generation *generation, odt_query_result *out_results,
                                odt_batch_stats *out_stats) {
    size_t index;

    memset(out_stats, 0, sizeof(*out_stats));
    for (index = 0u; index < TEST_POINT_COUNT; ++index) {
        odt_query_result result = result_pattern;
        odt_query_stats stats = {0u, 0u};
        odt_status status = odt_query(generation, &test_points[index], &result, &stats);

        if (status == ODT_INVALID_DATA) {
            result.kind = ODT_RESULT_ERROR;
            result.status = status;
            result.region_id = 0;
        } else {
            ODT_TEST_CHECK(status == ODT_OK);
        }
        out_results[index] = result;
        out_stats->attempted += 1u;
        out_stats->comparisons += stats.comparisons;
        out_stats->exact_fallbacks += stats.exact_fallbacks;
        if (result.kind == ODT_RESULT_REGION) {
            out_stats->region_count += 1u;
        } else if (result.kind == ODT_RESULT_OUTSIDE) {
            out_stats->outside_count += 1u;
        } else {
            ODT_TEST_CHECK(result.kind == ODT_RESULT_ERROR);
            out_stats->failed_count += 1u;
        }
    }
}

static void expect_unchanged_rejection(const odt_generation *generation, size_t count,
                                       const void *points, size_t point_stride, void *results,
                                       size_t result_stride, odt_query_result *observed_result) {
    odt_batch_stats stats = stats_pattern;

    *observed_result = result_pattern;
    ODT_TEST_STATUS(
        odt_query_batch(generation, count, points, point_stride, results, result_stride, &stats),
        ODT_INVALID_ARGUMENT);
    expect_result_equal(observed_result, &result_pattern);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);
}

static void test_contiguous_and_padded_batches(void) {
    enum {
        POINT_STRIDE = sizeof(odt_point) + TEST_POINT_PADDING,
        RESULT_STRIDE = sizeof(odt_query_result) + TEST_RESULT_PADDING,
    };
    odt_test_allocator_state allocator_state = {.live = true};
    odt_allocator allocator;
    odt_generation *generation = NULL;
    odt_query_result expected[TEST_POINT_COUNT];
    odt_query_result contiguous[TEST_POINT_COUNT];
    odt_batch_stats expected_stats;
    odt_batch_stats contiguous_stats = stats_pattern;
    unsigned char padded_points[1u + TEST_POINT_COUNT * POINT_STRIDE];
    unsigned char padded_results[1u + TEST_POINT_COUNT * RESULT_STRIDE];
    unsigned char point_snapshot[sizeof(padded_points)];
    uint64_t generation_hash_before;
    size_t allocations_before;
    size_t index;

    odt_test_counting_allocator_init(&allocator, &allocator_state);
    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, &allocator, NULL, &generation),
                    ODT_OK);
    scalar_expectations(generation, expected, &expected_stats);
    allocations_before = allocator_state.allocate_calls;
    generation_hash_before = hash_bytes(generation, generation->allocation_size);

    memset(contiguous, 0xa5, sizeof(contiguous));
    ODT_TEST_STATUS(odt_query_batch(generation, TEST_POINT_COUNT, test_points, sizeof(odt_point),
                                    contiguous, sizeof(odt_query_result), &contiguous_stats),
                    ODT_OK);
    for (index = 0u; index < TEST_POINT_COUNT; ++index) {
        expect_result_equal(&contiguous[index], &expected[index]);
    }
    expect_stats_equal(&contiguous_stats, &expected_stats);
    contiguous[0] = result_pattern;
    ODT_TEST_STATUS(odt_query_batch(generation, 1u, test_points, sizeof(odt_point), contiguous,
                                    sizeof(odt_query_result), NULL),
                    ODT_OK);
    expect_result_equal(&contiguous[0], &expected[0]);

    memset(padded_points, 0x5a, sizeof(padded_points));
    memset(padded_results, 0xc3, sizeof(padded_results));
    for (index = 0u; index < TEST_POINT_COUNT; ++index) {
        memcpy(&padded_points[1u + index * POINT_STRIDE], &test_points[index], sizeof(odt_point));
    }
    memcpy(point_snapshot, padded_points, sizeof(padded_points));
    contiguous_stats = stats_pattern;
    ODT_TEST_STATUS(odt_query_batch(generation, TEST_POINT_COUNT, &padded_points[1], POINT_STRIDE,
                                    &padded_results[1], RESULT_STRIDE, &contiguous_stats),
                    ODT_OK);
    for (index = 0u; index < TEST_POINT_COUNT; ++index) {
        odt_query_result actual;
        size_t padding_index;

        memcpy(&actual, &padded_results[1u + index * RESULT_STRIDE], sizeof(actual));
        expect_result_equal(&actual, &expected[index]);
        for (padding_index = sizeof(actual); padding_index < RESULT_STRIDE; ++padding_index) {
            ODT_TEST_CHECK(padded_results[1u + index * RESULT_STRIDE + padding_index] == 0xc3u);
        }
    }
    ODT_TEST_CHECK(padded_results[0] == 0xc3u);
    expect_stats_equal(&contiguous_stats, &expected_stats);
    ODT_TEST_CHECK(memcmp(padded_points, point_snapshot, sizeof(padded_points)) == 0);
    ODT_TEST_CHECK(allocator_state.allocate_calls == allocations_before);
    ODT_TEST_CHECK(hash_bytes(generation, generation->allocation_size) == generation_hash_before);

    ODT_TEST_CHECK(expected_stats.attempted == TEST_POINT_COUNT);
    ODT_TEST_CHECK(expected_stats.region_count == 5u);
    ODT_TEST_CHECK(expected_stats.outside_count == 1u);
    ODT_TEST_CHECK(expected_stats.failed_count == 2u);
    ODT_TEST_CHECK(expected_stats.exact_fallbacks > 0u);
    ODT_TEST_CHECK(expected[6].kind == ODT_RESULT_ERROR);
    ODT_TEST_CHECK(expected[6].status == ODT_INVALID_DATA);
    ODT_TEST_CHECK(expected[6].region_id == 0);
    ODT_TEST_CHECK(expected[7].kind == ODT_RESULT_ERROR);
    ODT_TEST_CHECK(expected[7].status == ODT_INVALID_DATA);
    ODT_TEST_CHECK(expected[7].region_id == 0);

    odt_generation_destroy(generation);
    ODT_TEST_CHECK(allocator_state.live_allocations == 0u);
}

static void test_zero_count_semantics(void) {
    odt_generation *generation = NULL;
    odt_batch_stats stats = stats_pattern;

    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, NULL, NULL, &generation),
                    ODT_OK);
    ODT_TEST_STATUS(odt_query_batch(generation, 0u, NULL, 0u, NULL, 0u, &stats), ODT_OK);
    expect_stats_equal(&stats, &(odt_batch_stats){0u, 0u, 0u, 0u, 0u, 0u});
    ODT_TEST_STATUS(odt_query_batch(generation, 0u, NULL, 0u, NULL, 0u, NULL), ODT_OK);

    stats = stats_pattern;
    ODT_TEST_STATUS(odt_query_batch(NULL, 0u, NULL, 0u, NULL, 0u, &stats), ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);
    odt_generation_destroy(generation);
}

static void test_invalid_envelopes_are_transactional(void) {
    _Alignas(max_align_t) unsigned char overlap_storage[4u * sizeof(odt_point)];
    unsigned char overlap_snapshot[sizeof(overlap_storage)];
    odt_generation *generation = NULL;
    odt_query_result results[2] = {result_pattern, result_pattern};
    odt_batch_stats stats = stats_pattern;
    odt_point points[2] = {{-1.0, 0.0}, {1.0, 0.0}};
    odt_query_result observed;
    const size_t overflowing_count = SIZE_MAX / sizeof(odt_point) + 2u;
    const void *overflowing_points =
        (const void *)(UINTPTR_MAX - (uintptr_t)sizeof(odt_point) + (uintptr_t)1u);
    void *overflowing_results =
        (void *)(UINTPTR_MAX - (uintptr_t)sizeof(odt_query_result) + (uintptr_t)1u);

    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, NULL, NULL, &generation),
                    ODT_OK);

    expect_unchanged_rejection(generation, 1u, NULL, sizeof(odt_point), &observed,
                               sizeof(odt_query_result), &observed);
    expect_unchanged_rejection(generation, 1u, points, sizeof(odt_point), NULL,
                               sizeof(odt_query_result), &observed);
    expect_unchanged_rejection(generation, 1u, points, sizeof(odt_point) - 1u, &observed,
                               sizeof(odt_query_result), &observed);
    expect_unchanged_rejection(generation, 1u, points, sizeof(odt_point), &observed,
                               sizeof(odt_query_result) - 1u, &observed);
    expect_unchanged_rejection(generation, overflowing_count, points, sizeof(odt_point), results,
                               sizeof(odt_query_result), &results[0]);
    expect_unchanged_rejection(generation, 2u, overflowing_points, sizeof(odt_point), results,
                               sizeof(odt_query_result), &results[0]);

    stats = stats_pattern;
    results[0] = result_pattern;
    ODT_TEST_STATUS(odt_query_batch(generation, 2u, points, sizeof(odt_point), overflowing_results,
                                    sizeof(odt_query_result), &stats),
                    ODT_INVALID_ARGUMENT);
    expect_result_equal(&results[0], &result_pattern);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);

    memset(overlap_storage, 0x6d, sizeof(overlap_storage));
    memcpy(overlap_snapshot, overlap_storage, sizeof(overlap_storage));
    stats = stats_pattern;
    ODT_TEST_STATUS(odt_query_batch(generation, 2u, overlap_storage, sizeof(odt_point),
                                    &overlap_storage[sizeof(odt_point) - 1u],
                                    sizeof(odt_query_result), &stats),
                    ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(memcmp(overlap_storage, overlap_snapshot, sizeof(overlap_storage)) == 0);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);

    memset(overlap_storage, 0x7e, sizeof(overlap_storage));
    memcpy(overlap_snapshot, overlap_storage, sizeof(overlap_storage));
    stats = stats_pattern;
    ODT_TEST_STATUS(odt_query_batch(generation, 2u, overlap_storage, 2u * sizeof(odt_point),
                                    &overlap_storage[sizeof(odt_point)], 2u * sizeof(odt_point),
                                    &stats),
                    ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(memcmp(overlap_storage, overlap_snapshot, sizeof(overlap_storage)) == 0);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);

    memset(overlap_storage, 0x4c, sizeof(overlap_storage));
    memcpy(overlap_storage, points, sizeof(points));
    memcpy(overlap_snapshot, overlap_storage, sizeof(overlap_storage));
    ODT_TEST_STATUS(odt_query_batch(generation, 2u, overlap_storage, sizeof(odt_point),
                                    &overlap_storage[2u * sizeof(odt_point)],
                                    sizeof(odt_query_result),
                                    (odt_batch_stats *)&overlap_storage[sizeof(odt_point)]),
                    ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(memcmp(overlap_storage, overlap_snapshot, sizeof(overlap_storage)) == 0);

    odt_generation_destroy(generation);
}

static void test_numeric_environment_rejection_is_transactional(void) {
    odt_generation *generation = NULL;
    odt_query_result results[2] = {result_pattern, result_pattern};
    const odt_query_result expected_results[2] = {result_pattern, result_pattern};
    odt_batch_stats stats = stats_pattern;
    const odt_point points[2] = {{-1.0, 0.0}, {1.0, 0.0}};
    int original_round;

    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, NULL, NULL, &generation),
                    ODT_OK);
    original_round = fegetround();
    if (original_round != -1 && fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD) {
        ODT_TEST_STATUS(odt_query_batch(generation, 2u, points, sizeof(odt_point), results,
                                        sizeof(odt_query_result), &stats),
                        ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT);
        ODT_TEST_CHECK(memcmp(results, expected_results, sizeof(results)) == 0);
        ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);
        ODT_TEST_CHECK(fesetround(original_round) == 0);
    } else if (original_round != -1) {
        ODT_TEST_CHECK(fesetround(original_round) == 0);
    }
    odt_generation_destroy(generation);
}

int main(void) {
    test_contiguous_and_padded_batches();
    test_zero_count_semantics();
    test_invalid_envelopes_are_transactional();
    test_numeric_environment_rejection_is_transactional();
    return EXIT_SUCCESS;
}
