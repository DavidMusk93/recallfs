#include "odt_test_support.h"

#include "odt_internal.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct test_line {
    long double a;
    long double b;
    long double c;
} test_line;

static const odt_internal_runtime_node *generation_nodes(const odt_generation *generation) {
    return (const odt_internal_runtime_node *)((const unsigned char *)generation +
                                               generation->nodes_offset);
}

static test_line line_from_reference(const odt_internal_geometry *geometry,
                                     odt_internal_line_ref reference) {
    test_line line = {0.0L, 0.0L, 0.0L};

    switch (reference.kind) {
    case ODT_INTERNAL_LINE_DOMAIN_MIN_X:
        line.a = -1.0L;
        line.c = (long double)geometry->domain.min_x;
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_X:
        line.a = 1.0L;
        line.c = -(long double)geometry->domain.max_x;
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MIN_Y:
        line.b = -1.0L;
        line.c = (long double)geometry->domain.min_y;
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_Y:
        line.b = 1.0L;
        line.c = -(long double)geometry->domain.max_y;
        break;
    case ODT_INTERNAL_LINE_BISECTOR: {
        const odt_site *first = &geometry->sites[reference.first_ordinal];
        const odt_site *second = &geometry->sites[reference.second_ordinal];
        const long double first_x = (long double)first->x;
        const long double first_y = (long double)first->y;
        const long double second_x = (long double)second->x;
        const long double second_y = (long double)second->y;

        line.a = 2.0L * (second_x - first_x);
        line.b = 2.0L * (second_y - first_y);
        line.c = first_x * first_x + first_y * first_y - second_x * second_x - second_y * second_y;
        break;
    }
    }
    return line;
}

static odt_point vertex_point(const odt_internal_geometry *geometry,
                              const odt_internal_vertex *vertex) {
    const test_line first = line_from_reference(geometry, vertex->first_line);
    const test_line second = line_from_reference(geometry, vertex->second_line);
    const long double denominator = first.a * second.b - second.a * first.b;
    odt_point point = {
        (double)((first.b * second.c - second.b * first.c) / denominator),
        (double)((first.c * second.a - second.c * first.a) / denominator),
    };

    point.x = fmax(geometry->domain.min_x, fmin(geometry->domain.max_x, point.x));
    point.y = fmax(geometry->domain.min_y, fmin(geometry->domain.max_y, point.y));
    return point;
}

static size_t brute_force_winner(const odt_point *point, const odt_site *sites, size_t site_count) {
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

static void expect_differential(const odt_generation *generation,
                                const odt_internal_geometry *geometry, const odt_site *sites,
                                size_t site_count, const odt_point *point) {
    const size_t winner = brute_force_winner(point, sites, site_count);
    odt_query_result result;
    size_t slow_winner = SIZE_MAX;
    bool found = false;

    ODT_TEST_STATUS(odt_internal_geometry_locate(geometry, point, &found, &slow_winner), ODT_OK);
    ODT_TEST_CHECK(found);
    ODT_TEST_CHECK(slow_winner == winner);
    ODT_TEST_STATUS(odt_query(generation, point, &result, NULL), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.region_id == sites[winner].region_id);
}

static void expect_region(const odt_generation *generation, odt_point point, int32_t region_id) {
    odt_query_result result = {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, 0};
    odt_query_stats stats = {UINT64_MAX, UINT64_MAX};

    ODT_TEST_STATUS(odt_query(generation, &point, &result, &stats), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.status == ODT_OK);
    ODT_TEST_CHECK(result.region_id == region_id);
    ODT_TEST_CHECK(stats.comparisons <= 64u);
    ODT_TEST_CHECK(stats.exact_fallbacks <= stats.comparisons);
}

static void test_reference_queries_and_ties(void) {
    static const odt_domain domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
    };
    static const odt_domain tie_domain = {-2.0, -2.0, 2.0, 2.0};
    static const odt_site tie_sites[] = {
        {-1.0, 0.0, 11},
        {1.0, 0.0, 22},
    };
    odt_generation *generation = NULL;
    odt_generation *tie_generation = NULL;
    odt_query_result result;
    odt_query_stats stats;
    odt_point point;

    ODT_TEST_STATUS(odt_build(&domain, sites, 3u, NULL, NULL, NULL, NULL, &generation), ODT_OK);
    expect_region(generation, (odt_point){2.0, 2.0}, 101);
    expect_region(generation, (odt_point){8.0, 3.0}, -7);
    expect_region(generation, (odt_point){4.0, 7.0}, 42);
    expect_region(generation, (odt_point){0.0, 0.0}, 101);
    expect_region(generation, (odt_point){10.0, 8.0}, -7);

    point = (odt_point){nextafter(0.0, -INFINITY), 4.0};
    ODT_TEST_STATUS(odt_query(generation, &point, &result, &stats), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_OUTSIDE);
    ODT_TEST_CHECK(result.status == ODT_OK);
    ODT_TEST_CHECK(result.region_id == 0);
    ODT_TEST_CHECK(stats.comparisons == 0u);
    ODT_TEST_CHECK(stats.exact_fallbacks == 0u);

    point = (odt_point){5.0, nextafter(8.0, INFINITY)};
    ODT_TEST_STATUS(odt_query(generation, &point, &result, NULL), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_OUTSIDE);
    odt_generation_destroy(generation);

    ODT_TEST_STATUS(odt_build(&tie_domain, tie_sites, 2u, NULL, NULL, NULL, NULL, &tie_generation),
                    ODT_OK);
    point = (odt_point){0.0, 0.0};
    ODT_TEST_STATUS(odt_query(tie_generation, &point, &result, &stats), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.region_id == 11);
    ODT_TEST_CHECK(stats.exact_fallbacks == 1u);
    expect_region(tie_generation, (odt_point){nextafter(0.0, -INFINITY), 0.0}, 11);
    expect_region(tie_generation, (odt_point){nextafter(0.0, INFINITY), 0.0}, 22);
    odt_generation_destroy(tie_generation);
}

static void test_differential_queries(void) {
    static const odt_domain domain = {-10.0, -8.0, 11.0, 9.0};
    static const odt_site sites[] = {
        {-8.0, -6.0, 101}, {-4.5, 3.0, 102}, {-1.0, -2.0, 103}, {0.0, 7.5, 104},
        {2.0, 1.0, 105},   {5.5, -5.0, 106}, {8.0, 4.0, 107},   {10.0, -1.0, 108},
    };
    uint64_t state = UINT64_C(0x6a09e667f3bcc909);
    odt_generation *generation = NULL;
    odt_internal_geometry *geometry = NULL;
    size_t index;

    ODT_TEST_STATUS(odt_build(&domain, sites, sizeof(sites) / sizeof(sites[0]), NULL, NULL, NULL,
                              NULL, &generation),
                    ODT_OK);
    ODT_TEST_STATUS(odt_internal_geometry_build(&domain, sites, sizeof(sites) / sizeof(sites[0]),
                                                NULL, NULL, &geometry),
                    ODT_OK);
    for (index = 0u; index < 4096u; ++index) {
        odt_point point;

        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        point.x = -10.0 + 21.0 * ((double)(uint32_t)(state >> 32u) / 4294967296.0);
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        point.y = -8.0 + 17.0 * ((double)(uint32_t)(state >> 32u) / 4294967296.0);
        expect_differential(generation, geometry, sites, sizeof(sites) / sizeof(sites[0]), &point);
    }
    for (index = 0u; index < geometry->site_count; ++index) {
        const odt_internal_cell *cell = &geometry->cells[index];
        size_t vertex_index;

        for (vertex_index = 0u; vertex_index < cell->vertex_count; ++vertex_index) {
            const odt_point point = vertex_point(geometry, &cell->vertices[vertex_index]);

            expect_differential(generation, geometry, sites, sizeof(sites) / sizeof(sites[0]),
                                &point);
        }
    }
    for (index = 0u; index < generation->node_count; ++index) {
        const odt_internal_runtime_node *node = &generation_nodes(generation)[index];
        const odt_site *first = &sites[node->first_ordinal];
        const odt_site *second = &sites[node->second_ordinal];
        odt_point points[3];
        size_t point_index;

        points[0].x = first->x / 2.0 + second->x / 2.0;
        points[0].y = first->y / 2.0 + second->y / 2.0;
        points[1] = points[0];
        points[2] = points[0];
        if (fabs(first->x - second->x) >= fabs(first->y - second->y)) {
            points[1].x = nextafter(points[0].x, -INFINITY);
            points[2].x = nextafter(points[0].x, INFINITY);
        } else {
            points[1].y = nextafter(points[0].y, -INFINITY);
            points[2].y = nextafter(points[0].y, INFINITY);
        }
        for (point_index = 0u; point_index < 3u; ++point_index) {
            expect_differential(generation, geometry, sites, sizeof(sites) / sizeof(sites[0]),
                                &points[point_index]);
        }
    }
    odt_internal_geometry_destroy(geometry);
    odt_generation_destroy(generation);
}

static void test_query_failure_semantics_and_no_allocations(void) {
    static const odt_domain domain = {0.0, 0.0, 1.0, 1.0};
    static const odt_site sites[] = {
        {0.25, 0.25, 1},
        {0.75, 0.75, 2},
    };
    odt_test_allocator_state allocator_state = {.live = true};
    odt_allocator allocator;
    odt_generation *generation = NULL;
    const odt_query_result result_pattern = {
        ODT_RESULT_ERROR,
        ODT_CORRUPT_DATA,
        INT32_MIN,
    };
    const odt_query_stats stats_pattern = {UINT64_MAX, UINT64_MAX};
    odt_query_result result;
    odt_query_stats stats;
    odt_point point = {0.25, 0.25};
    size_t allocations_before;
    int original_round;

    odt_test_counting_allocator_init(&allocator, &allocator_state);
    ODT_TEST_STATUS(odt_build(&domain, sites, 2u, NULL, NULL, &allocator, NULL, &generation),
                    ODT_OK);
    allocations_before = allocator_state.allocate_calls;
    result = result_pattern;
    stats = stats_pattern;
    ODT_TEST_STATUS(odt_query(generation, &point, &result, &stats), ODT_OK);
    ODT_TEST_CHECK(allocator_state.allocate_calls == allocations_before);
    ODT_TEST_CHECK(allocator_state.deallocate_calls + 1u == allocator_state.allocate_calls);

    result = result_pattern;
    stats = stats_pattern;
    ODT_TEST_STATUS(odt_query(NULL, &point, &result, &stats), ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(memcmp(&result, &result_pattern, sizeof(result)) == 0);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);
    ODT_TEST_STATUS(odt_query(generation, NULL, &result, &stats), ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(memcmp(&result, &result_pattern, sizeof(result)) == 0);
    ODT_TEST_STATUS(odt_query(generation, &point, NULL, &stats), ODT_INVALID_ARGUMENT);

    point.x = NAN;
    ODT_TEST_STATUS(odt_query(generation, &point, &result, &stats), ODT_INVALID_DATA);
    ODT_TEST_CHECK(memcmp(&result, &result_pattern, sizeof(result)) == 0);
    ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);

    original_round = fegetround();
    if (original_round != -1 && fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD) {
        point = (odt_point){0.25, 0.25};
        ODT_TEST_STATUS(odt_query(generation, &point, &result, &stats),
                        ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT);
        ODT_TEST_CHECK(memcmp(&result, &result_pattern, sizeof(result)) == 0);
        ODT_TEST_CHECK(memcmp(&stats, &stats_pattern, sizeof(stats)) == 0);
        ODT_TEST_CHECK(fesetround(original_round) == 0);
    } else if (original_round != -1) {
        ODT_TEST_CHECK(fesetround(original_round) == 0);
    }

    odt_generation_destroy(generation);
    ODT_TEST_CHECK(allocator_state.live_allocations == 0u);
}

static void test_one_site_zero_comparisons(void) {
    const odt_domain domain = {-1.0, -1.0, 1.0, 1.0};
    const odt_site site = {0.0, 0.0, 77};
    const odt_point point = {0.25, -0.5};
    odt_generation *generation = NULL;
    odt_query_result result;
    odt_query_stats stats;

    ODT_TEST_STATUS(odt_build(&domain, &site, 1u, NULL, NULL, NULL, NULL, &generation), ODT_OK);
    ODT_TEST_STATUS(odt_query(generation, &point, &result, &stats), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.region_id == 77);
    ODT_TEST_CHECK(stats.comparisons == 0u);
    ODT_TEST_CHECK(stats.exact_fallbacks == 0u);
    odt_generation_destroy(generation);
}

static void test_full_range_exact_fallbacks(void) {
    const odt_domain skewed_domain = {
        -DBL_MAX / 4.0,
        -4.0 * DBL_TRUE_MIN,
        DBL_MAX / 4.0,
        4.0 * DBL_TRUE_MIN,
    };
    const odt_site skewed_sites[] = {
        {-DBL_MAX / 8.0, -DBL_TRUE_MIN, 31},
        {DBL_MAX / 8.0, DBL_TRUE_MIN, 32},
    };
    const odt_domain subnormal_domain = {0.0, 0.0, 4.0 * DBL_TRUE_MIN, 4.0 * DBL_TRUE_MIN};
    const odt_site subnormal_sites[] = {
        {DBL_TRUE_MIN, DBL_TRUE_MIN, 41},
        {3.0 * DBL_TRUE_MIN, 3.0 * DBL_TRUE_MIN, 42},
    };
    odt_generation *generation = NULL;
    odt_query_result result;
    odt_query_stats stats;

    ODT_TEST_STATUS(
        odt_build(&skewed_domain, skewed_sites, 2u, NULL, NULL, NULL, NULL, &generation), ODT_OK);
    ODT_TEST_STATUS(odt_query(generation, &(odt_point){0.0, 0.0}, &result, &stats), ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.region_id == 31);
    ODT_TEST_CHECK(stats.exact_fallbacks == 1u);
    odt_generation_destroy(generation);

    ODT_TEST_STATUS(
        odt_build(&subnormal_domain, subnormal_sites, 2u, NULL, NULL, NULL, NULL, &generation),
        ODT_OK);
    ODT_TEST_STATUS(odt_query(generation, &(odt_point){2.0 * DBL_TRUE_MIN, 2.0 * DBL_TRUE_MIN},
                              &result, &stats),
                    ODT_OK);
    ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
    ODT_TEST_CHECK(result.region_id == 41);
    ODT_TEST_CHECK(stats.exact_fallbacks == 1u);
    odt_generation_destroy(generation);
}

int main(void) {
    test_reference_queries_and_ties();
    test_differential_queries();
    test_query_failure_semantics_and_no_allocations();
    test_one_site_zero_comparisons();
    test_full_range_exact_fallbacks();
    return EXIT_SUCCESS;
}
