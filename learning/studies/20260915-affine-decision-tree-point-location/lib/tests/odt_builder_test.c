#include "odt_test_support.h"

#include "odt_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef ODT_TEST_SKIP_ARCHITECTURE_GATE
enum {
    ODT_ARCHITECTURE_SITE_COUNT = 1000,
    ODT_ARCHITECTURE_QUERY_COUNT = 4096,
};

typedef struct pcg32_state {
    uint64_t state;
    uint64_t increment;
} pcg32_state;

static uint32_t pcg32_next(pcg32_state *generator) {
    const uint64_t old_state = generator->state;
    const uint32_t xor_shifted = (uint32_t)(((old_state >> 18u) ^ old_state) >> 27u);
    const uint32_t rotation = (uint32_t)(old_state >> 59u);

    generator->state = old_state * UINT64_C(6364136223846793005) + generator->increment;
    return (xor_shifted >> rotation) | (xor_shifted << ((0u - rotation) & 31u));
}

static void pcg32_seed(pcg32_state *generator, uint64_t seed) {
    generator->state = 0u;
    generator->increment = (seed << 1u) | 1u;
    (void)pcg32_next(generator);
    generator->state += seed;
    (void)pcg32_next(generator);
}

static double pcg32_unit(pcg32_state *generator) {
    return ((double)pcg32_next(generator) + 0.5) / 4294967296.0;
}

static void generate_canonical_sites(odt_site *sites) {
    pcg32_state generator;
    size_t index;

    pcg32_seed(&generator, UINT64_C(20260916));
    for (index = 0u; index < ODT_ARCHITECTURE_SITE_COUNT; ++index) {
        const size_t column = index % 40u;
        const size_t row = index / 40u;
        const double grid_x = ((double)column + 0.1 + 0.8 * pcg32_unit(&generator)) / 40.0;
        const double grid_y = ((double)row + 0.1 + 0.8 * pcg32_unit(&generator)) / 25.0;

        sites[index].x = 12.0 * grid_x * grid_x;
        sites[index].y = 8.0 * grid_y * (2.0 - grid_y);
        sites[index].region_id = (int32_t)index + 1;
    }
}
#endif

static const odt_site *generation_sites(const odt_generation *generation) {
    return (const odt_site *)((const unsigned char *)generation + generation->sites_offset);
}

static const odt_internal_runtime_node *generation_nodes(const odt_generation *generation) {
    return (const odt_internal_runtime_node *)((const unsigned char *)generation +
                                               generation->nodes_offset);
}

static const odt_internal_runtime_leaf *generation_leaves(const odt_generation *generation) {
    return (const odt_internal_runtime_leaf *)((const unsigned char *)generation +
                                               generation->leaves_offset);
}

static void expect_build_limit(const odt_domain *domain, const odt_site *sites, size_t site_count,
                               const odt_limits *limits) {
    odt_test_allocator_state allocator_state = {.live = true};
    odt_allocator allocator;
    odt_build_stats unchanged;
    odt_build_stats stats;
    odt_generation *generation = (odt_generation *)(uintptr_t)1u;

    memset(&unchanged, 0xa5, sizeof(unchanged));
    stats = unchanged;
    odt_test_counting_allocator_init(&allocator, &allocator_state);
    ODT_TEST_STATUS(
        odt_build(domain, sites, site_count, NULL, limits, &allocator, &stats, &generation),
        ODT_LIMIT_EXCEEDED);
    ODT_TEST_CHECK(generation == NULL);
    ODT_TEST_CHECK(memcmp(&stats, &unchanged, sizeof(stats)) == 0);
    ODT_TEST_CHECK(allocator_state.live_allocations == 0u);
}

static void test_reference_build_and_metadata(void) {
    static const odt_domain domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
    };
    odt_test_allocator_state allocator_state = {.live = true};
    odt_allocator allocator;
    odt_build_stats stats;
    odt_build_stats copied_stats;
    odt_domain copied_domain;
    odt_generation *generation = NULL;
    uint64_t count = 0u;
    uint64_t encoded_size = UINT64_MAX;
    uint32_t depth = 0u;

    odt_test_counting_allocator_init(&allocator, &allocator_state);
    ODT_TEST_STATUS(odt_build(&domain, sites, 3u, NULL, NULL, &allocator, &stats, &generation),
                    ODT_OK);
    ODT_TEST_CHECK(generation != NULL);
    ODT_TEST_CHECK(stats.site_count == 3u);
    ODT_TEST_CHECK(stats.node_count != 0u);
    ODT_TEST_CHECK(stats.leaf_count != 0u);
    ODT_TEST_CHECK(stats.fragment_count >= 3u);
    ODT_TEST_CHECK(stats.build_work != 0u);
    ODT_TEST_CHECK(stats.peak_build_bytes >= generation->allocation_size);
    ODT_TEST_CHECK(stats.build_duration_ns != 0u);
    ODT_TEST_CHECK(stats.maximum_depth != 0u);
    ODT_TEST_CHECK(allocator_state.live_allocations == 1u);
    ODT_TEST_CHECK(allocator_state.active_pointer == generation);

    ODT_TEST_STATUS(odt_generation_get_domain(generation, &copied_domain), ODT_OK);
    ODT_TEST_CHECK(memcmp(&copied_domain, &domain, sizeof(domain)) == 0);
    ODT_TEST_STATUS(odt_generation_get_site_count(generation, &count), ODT_OK);
    ODT_TEST_CHECK(count == stats.site_count);
    ODT_TEST_STATUS(odt_generation_get_node_count(generation, &count), ODT_OK);
    ODT_TEST_CHECK(count == stats.node_count);
    ODT_TEST_STATUS(odt_generation_get_leaf_count(generation, &count), ODT_OK);
    ODT_TEST_CHECK(count == stats.leaf_count);
    ODT_TEST_STATUS(odt_generation_get_maximum_depth(generation, &depth), ODT_OK);
    ODT_TEST_CHECK(depth == stats.maximum_depth);
    ODT_TEST_STATUS(odt_generation_get_encoded_size(generation, &encoded_size),
                    ODT_UNSUPPORTED_FORMAT);
    ODT_TEST_CHECK(encoded_size == UINT64_MAX);
    ODT_TEST_STATUS(odt_generation_get_build_stats(generation, &copied_stats), ODT_OK);
    ODT_TEST_CHECK(memcmp(&copied_stats, &stats, sizeof(stats)) == 0);

    ODT_TEST_CHECK(generation->allocation_alignment >= _Alignof(max_align_t));
    ODT_TEST_CHECK(generation->allocation_size % generation->allocation_alignment == 0u);
    ODT_TEST_CHECK(generation->sites_offset % _Alignof(odt_site) == 0u);
    ODT_TEST_CHECK(generation->nodes_offset % _Alignof(odt_internal_runtime_node) == 0u);
    ODT_TEST_CHECK(generation->leaves_offset % _Alignof(odt_internal_runtime_leaf) == 0u);
    ODT_TEST_CHECK(generation->sites_offset < generation->nodes_offset);
    ODT_TEST_CHECK(generation->nodes_offset <= generation->leaves_offset);
    ODT_TEST_CHECK(generation->leaves_offset < generation->allocation_size);

    odt_generation_destroy(generation);
    ODT_TEST_CHECK(allocator_state.live_allocations == 0u);
    ODT_TEST_CHECK(allocator_state.allocate_calls == allocator_state.deallocate_calls);
}

static void test_deterministic_packed_program(void) {
    static const odt_domain domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
        {7.0, 6.0, 88},
    };
    odt_generation *first = NULL;
    odt_generation *second = NULL;
    odt_build_stats first_stats;
    odt_build_stats second_stats;

    ODT_TEST_STATUS(odt_build(&domain, sites, 4u, NULL, NULL, NULL, &first_stats, &first), ODT_OK);
    ODT_TEST_STATUS(odt_build(&domain, sites, 4u, NULL, NULL, NULL, &second_stats, &second),
                    ODT_OK);
    ODT_TEST_CHECK(first != second);
    first_stats.build_duration_ns = 0u;
    second_stats.build_duration_ns = 0u;
    ODT_TEST_CHECK(memcmp(&first_stats, &second_stats, sizeof(first_stats)) == 0);
    ODT_TEST_CHECK(first->root_reference == second->root_reference);
    ODT_TEST_CHECK(first->site_count == second->site_count);
    ODT_TEST_CHECK(first->node_count == second->node_count);
    ODT_TEST_CHECK(first->leaf_count == second->leaf_count);
    ODT_TEST_CHECK(memcmp(generation_sites(first), generation_sites(second),
                          first->site_count * sizeof(odt_site)) == 0);
    ODT_TEST_CHECK(memcmp(generation_nodes(first), generation_nodes(second),
                          first->node_count * sizeof(odt_internal_runtime_node)) == 0);
    ODT_TEST_CHECK(memcmp(generation_leaves(first), generation_leaves(second),
                          first->leaf_count * sizeof(odt_internal_runtime_leaf)) == 0);
    odt_generation_destroy(second);
    odt_generation_destroy(first);
}

static void test_limits_and_failpoint_cleanup(void) {
    static const odt_domain domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
    };
    odt_build_stats stats;
    odt_generation *generation = NULL;
    odt_limits limits;
    size_t successful_allocation_count;
    size_t fail_at;

    ODT_TEST_STATUS(odt_build(&domain, sites, 3u, NULL, NULL, NULL, &stats, &generation), ODT_OK);
    odt_generation_destroy(generation);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_sites = 2u;
    expect_build_limit(&domain, sites, 3u, &limits);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_internal_nodes = (size_t)stats.node_count - 1u;
    expect_build_limit(&domain, sites, 3u, &limits);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_depth = stats.maximum_depth - 1u;
    expect_build_limit(&domain, sites, 3u, &limits);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_polygon_fragments = (size_t)stats.fragment_count - 1u;
    expect_build_limit(&domain, sites, 3u, &limits);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_build_work = stats.build_work - 1u;
    expect_build_limit(&domain, sites, 3u, &limits);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_build_bytes = (size_t)stats.peak_build_bytes - 1u;
    expect_build_limit(&domain, sites, 3u, &limits);

    {
        odt_test_allocator_state state = {.live = true};
        odt_allocator allocator;

        odt_test_counting_allocator_init(&allocator, &state);
        ODT_TEST_STATUS(odt_build(&domain, sites, 3u, NULL, NULL, &allocator, NULL, &generation),
                        ODT_OK);
        successful_allocation_count = state.allocate_calls;
        odt_generation_destroy(generation);
        ODT_TEST_CHECK(state.live_allocations == 0u);
    }

    for (fail_at = 1u; fail_at <= successful_allocation_count; ++fail_at) {
        odt_test_allocator_state state = {
            .live = true,
            .fail_at_call = fail_at,
        };
        odt_allocator allocator;

        generation = (odt_generation *)(uintptr_t)1u;
        odt_test_counting_allocator_init(&allocator, &state);
        ODT_TEST_STATUS(odt_build(&domain, sites, 3u, NULL, NULL, &allocator, NULL, &generation),
                        ODT_OUT_OF_MEMORY);
        ODT_TEST_CHECK(generation == NULL);
        ODT_TEST_CHECK(state.live_allocations == 0u);
        ODT_TEST_CHECK(state.deallocate_calls + 1u == state.allocate_calls);
    }
}

#ifndef ODT_TEST_SKIP_ARCHITECTURE_GATE
static int compare_u32(const void *first, const void *second) {
    const uint32_t first_value = *(const uint32_t *)first;
    const uint32_t second_value = *(const uint32_t *)second;

    return first_value < second_value ? -1 : (first_value > second_value ? 1 : 0);
}

static size_t exact_winner(const odt_point *point, const odt_site *sites, size_t site_count) {
    size_t winner = 0u;
    size_t index;

    for (index = 1u; index < site_count; ++index) {
        int comparison;

        ODT_TEST_STATUS(odt_internal_compare_squared_distance(point, &sites[winner], winner,
                                                              &sites[index], index, &comparison),
                        ODT_OK);
        if (comparison > 0) {
            winner = index;
        }
    }
    return winner;
}

static void test_architecture_gate(void) {
    const odt_domain domain = {0.0, 0.0, 12.0, 8.0};
    odt_site sites[ODT_ARCHITECTURE_SITE_COUNT];
    uint32_t comparisons[ODT_ARCHITECTURE_QUERY_COUNT];
    pcg32_state generator;
    odt_build_stats build_stats;
    odt_generation *generation = NULL;
    uint64_t comparison_total = 0u;
    uint64_t exact_total = 0u;
    size_t query_index;

    generate_canonical_sites(sites);
    ODT_TEST_STATUS(odt_build(&domain, sites, ODT_ARCHITECTURE_SITE_COUNT, NULL, NULL, NULL,
                              &build_stats, &generation),
                    ODT_OK);
    pcg32_seed(&generator, UINT64_C(20260917));
    for (query_index = 0u; query_index < ODT_ARCHITECTURE_QUERY_COUNT; ++query_index) {
        const odt_point point = {
            12.0 * pcg32_unit(&generator),
            8.0 * pcg32_unit(&generator),
        };
        const size_t winner = exact_winner(&point, sites, ODT_ARCHITECTURE_SITE_COUNT);
        odt_query_result result;
        odt_query_stats query_stats;

        ODT_TEST_STATUS(odt_query(generation, &point, &result, &query_stats), ODT_OK);
        ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
        ODT_TEST_CHECK(result.region_id == sites[winner].region_id);
        ODT_TEST_CHECK(query_stats.comparisons <= UINT32_MAX);
        comparisons[query_index] = (uint32_t)query_stats.comparisons;
        comparison_total += query_stats.comparisons;
        exact_total += query_stats.exact_fallbacks;
    }
    qsort(comparisons, ODT_ARCHITECTURE_QUERY_COUNT, sizeof(comparisons[0]), compare_u32);

    printf("architecture_gate sites=%" PRIu64 " nodes=%" PRIu64 " leaves=%" PRIu64 " depth=%" PRIu32
           " fragments=%" PRIu64 " work=%" PRIu64 " peak_bytes=%" PRIu64 " build_ns=%" PRIu64
           " mean_comparisons=%.3f p99=%" PRIu32 " exact_fallback_rate=%.6f\n",
           build_stats.site_count, build_stats.node_count, build_stats.leaf_count,
           build_stats.maximum_depth, build_stats.fragment_count, build_stats.build_work,
           build_stats.peak_build_bytes, build_stats.build_duration_ns,
           (double)comparison_total / (double)ODT_ARCHITECTURE_QUERY_COUNT,
           comparisons[(ODT_ARCHITECTURE_QUERY_COUNT * 99u + 99u) / 100u - 1u],
           comparison_total == 0u ? 0.0 : (double)exact_total / (double)comparison_total);

    ODT_TEST_CHECK(build_stats.maximum_depth <= 64u);
    ODT_TEST_CHECK(build_stats.node_count <= 65535u);
    ODT_TEST_CHECK(build_stats.fragment_count <= UINT64_C(1000000));
    ODT_TEST_CHECK(build_stats.peak_build_bytes <= UINT64_C(512) * 1024u * 1024u);
    ODT_TEST_CHECK(comparison_total <= UINT64_C(32) * ODT_ARCHITECTURE_QUERY_COUNT);
    ODT_TEST_CHECK(comparisons[(ODT_ARCHITECTURE_QUERY_COUNT * 99u + 99u) / 100u - 1u] <= 64u);
    ODT_TEST_CHECK(exact_total * 100u < comparison_total);
    odt_generation_destroy(generation);
}
#endif

int main(void) {
    test_reference_build_and_metadata();
    test_deterministic_packed_program();
    test_limits_and_failpoint_cleanup();
#ifndef ODT_TEST_SKIP_ARCHITECTURE_GATE
    test_architecture_gate();
#endif
    return EXIT_SUCCESS;
}
