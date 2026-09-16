#include "odt_test_support.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct exact_case {
    uint64_t point_x;
    uint64_t point_y;
    uint64_t first_x;
    uint64_t first_y;
    uint64_t second_x;
    uint64_t second_y;
    int expected;
} exact_case;

static double double_from_bits(uint64_t bits) {
    double value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void expect_comparison(const odt_point *point, const odt_site *first, size_t first_ordinal,
                              const odt_site *second, size_t second_ordinal, int expected) {
    int comparison = 99;

    ODT_TEST_STATUS(odt_test_compare_squared_distance(point, first, first_ordinal, second,
                                                      second_ordinal, &comparison),
                    ODT_OK);
    ODT_TEST_CHECK(comparison == expected);
}

static void test_exact_squared_distance_edges(void) {
    const double tiny = DBL_TRUE_MIN;
    odt_point point;
    odt_site first;
    odt_site second;
    int comparison = 99;

    point = (odt_point){0.0, 0.0};
    first = (odt_site){-1.0, 0.0, 10};
    second = (odt_site){1.0, -0.0, 20};
    expect_comparison(&point, &first, 0u, &second, 1u, -1);
    expect_comparison(&point, &first, 2u, &second, 1u, 1);

    first = (odt_site){-0.0, 0.0, 10};
    second = (odt_site){0.0, -0.0, 20};
    expect_comparison(&point, &first, 4u, &second, 9u, -1);

    point = (odt_point){tiny, -tiny};
    first = (odt_site){0.0, -tiny, 10};
    second = (odt_site){2.0 * tiny, -tiny, 20};
    expect_comparison(&point, &first, 0u, &second, 1u, -1);

    point = (odt_point){DBL_MAX, -DBL_MAX};
    first = (odt_site){DBL_MAX, 0.0, 10};
    second = (odt_site){-DBL_MAX, -DBL_MAX, 20};
    expect_comparison(&point, &first, 0u, &second, 1u, -1);

    point = (odt_point){0x1.0000000000001p+500, 0x1.0p-500};
    first = (odt_site){0x1.0p+500, -0x1.0p-500, 10};
    second = (odt_site){0x1.0000000000002p+500, 0.0, 20};
    expect_comparison(&point, &first, 0u, &second, 1u, 1);

    ODT_TEST_STATUS(odt_test_compare_squared_distance(NULL, &first, 0u, &second, 1u, &comparison),
                    ODT_INVALID_ARGUMENT);
    point.x = INFINITY;
    ODT_TEST_STATUS(odt_test_compare_squared_distance(&point, &first, 0u, &second, 1u, &comparison),
                    ODT_INVALID_DATA);
}

static void test_fraction_generated_raw_bit_corpus(void) {
    static const exact_case cases[] = {
        {UINT64_C(0x0aff37b0a9993823), UINT64_C(0x61ea5cd6aeb9ff44), UINT64_C(0x28bf92e8f129e72d),
         UINT64_C(0xa4c4c25edec23e4a), UINT64_C(0x5e9974481bc955e9), UINT64_C(0x3dc165d08123c0d7),
         -1},
        {UINT64_C(0xc30f3acfddc60450), UINT64_C(0x701cb1ea97062032), UINT64_C(0x92b3c87ba970c295),
         UINT64_C(0x803ccbc49a645bd1), UINT64_C(0x8fc54d6fbc318d2c), UINT64_C(0x88efc1ba9cd237f8),
         -1},
        {UINT64_C(0x3fa245063e3aba91), UINT64_C(0x6f8f67562c4ec107), UINT64_C(0x8a60572244d28db5),
         UINT64_C(0x740d6931a3732f61), UINT64_C(0x696f668acc472e0f), UINT64_C(0x6aba2a66a308d6cb),
         1},
        {UINT64_C(0xd0e3125d87d44760), UINT64_C(0xeb639b5667b388b6), UINT64_C(0xbf85685220bddc3a),
         UINT64_C(0x0bd4c68a7c0c902d), UINT64_C(0xca2014ccce292113), UINT64_C(0xd32208c32ea74199),
         1},
        {UINT64_C(0xf26bf58479f6c213), UINT64_C(0x4cd9261998c4f225), UINT64_C(0x87852b6dae1ba114),
         UINT64_C(0xb6938e80e56b8cfa), UINT64_C(0x30a17d057aa91a25), UINT64_C(0xdc1b048507cbd566),
         -1},
        {UINT64_C(0xf2647d8a135a236e), UINT64_C(0x5d7c498a0c68b695), UINT64_C(0xd91b05b30656a414),
         UINT64_C(0xfd408d19530874ab), UINT64_C(0x7a26446ff2a30623), UINT64_C(0x68394383426f97bc),
         1},
        {UINT64_C(0x9b9de663e733b909), UINT64_C(0x0cb44fd2ffab5e38), UINT64_C(0x88ae9c7b726c4c45),
         UINT64_C(0x4dac2a5f96546a91), UINT64_C(0x1f435bd9b3c5b757), UINT64_C(0x1dd7b7649ce07d91),
         1},
        {UINT64_C(0xc00d52090db3b939), UINT64_C(0x9f320ce3938cff56), UINT64_C(0x47c61066373ac322),
         UINT64_C(0x7e6da63f94c149b2), UINT64_C(0x2108d7f0e56af251), UINT64_C(0x8eb3d7740489cf8b),
         1},
        {UINT64_C(0x1350a3bd18257b3b), UINT64_C(0xc48605d257381815), UINT64_C(0xab84bc9438dc0112),
         UINT64_C(0xa92407a81e1f9859), UINT64_C(0x7aef8b662aa4b436), UINT64_C(0x2a75be86aa3cb9d5),
         -1},
        {UINT64_C(0xeb6727f1d98c5e71), UINT64_C(0xe21f549fd878bf45), UINT64_C(0x02397237fed3195e),
         UINT64_C(0x5a8b9b310eadbfcc), UINT64_C(0xef7972df609536a6), UINT64_C(0x5e8246b762aa7b7b),
         -1},
        {UINT64_C(0xbc86f6f68b507aa3), UINT64_C(0xf2977caebf5fc2dd), UINT64_C(0x6fecc1f372997cc6),
         UINT64_C(0x707c662732819e92), UINT64_C(0x2676c8f46279f719), UINT64_C(0xbb7a03bfa73ebc7e),
         1},
        {UINT64_C(0xf4a3a258a12710be), UINT64_C(0xf25fd20f75e62ebe), UINT64_C(0x69a68259795baf35),
         UINT64_C(0x47445f092de82002), UINT64_C(0x18a94380f81653d8), UINT64_C(0x2dec04a9b436a67e),
         1},
        {UINT64_C(0x067effa39ffebdc7), UINT64_C(0x8c136e971d5d8a28), UINT64_C(0x32e76e287c40bb6c),
         UINT64_C(0xfcc87721df7582ac), UINT64_C(0xbe036a6243d14e73), UINT64_C(0x32f51c9bdc3a5c62),
         1},
        {UINT64_C(0x9f3eb78d540912a3), UINT64_C(0xa99efe6af121153a), UINT64_C(0xd2705386f1330eed),
         UINT64_C(0xbcc41772b8f11a1f), UINT64_C(0x1b68d4ea2d93fa84), UINT64_C(0xe0d7afce1dc410d3),
         -1},
        {UINT64_C(0x6527107afd14f61c), UINT64_C(0xf9f27d7e93379db1), UINT64_C(0x47c7f4ed228ef7da),
         UINT64_C(0x0e902ccc73bf3b54), UINT64_C(0xbe64b0bc83e4681e), UINT64_C(0xa789bc0b4f2fa65c),
         -1},
        {UINT64_C(0xabe22c09d9a48252), UINT64_C(0x7ba2afc1658f7942), UINT64_C(0x84c3eac8d191ad92),
         UINT64_C(0x155d15a977f011a7), UINT64_C(0x47ac14ec64b079f0), UINT64_C(0x7b92d8dfb6caf440),
         1},
        {UINT64_C(0x009f926ebd96bbdb), UINT64_C(0x82e6756ddf79af6d), UINT64_C(0x16551e0fbb85e78d),
         UINT64_C(0x2b3c8b9af1b54ca7), UINT64_C(0xe67859d30c511f20), UINT64_C(0xc0ee7ae4ba12f66b),
         -1},
        {UINT64_C(0x89fd2dc25a3b2c9f), UINT64_C(0x26896dcf70d1ee29), UINT64_C(0xfdd357f33e3df820),
         UINT64_C(0x7a7e738b3cde33bc), UINT64_C(0xbb755a88c731e6e6), UINT64_C(0x21333e0803ef3200),
         1},
        {UINT64_C(0xd21045c5b0bce022), UINT64_C(0x18de34793aeb60a1), UINT64_C(0x222948dc18b57dc0),
         UINT64_C(0xe9c95d9b1ada8b0b), UINT64_C(0x2763063e83da7cd4), UINT64_C(0xbfe5fe5b68174ada),
         1},
        {UINT64_C(0xef8b6364ab0bcd2e), UINT64_C(0x568ee9b708427878), UINT64_C(0x1410f2dbc82083a9),
         UINT64_C(0x49eb5e17c1987120), UINT64_C(0x5c5d3086a31e4c75), UINT64_C(0x24da20dac7986de8),
         -1},
        {UINT64_C(0x4d544e14e438fdf3), UINT64_C(0x1aaf552502585887), UINT64_C(0x9ff2c77918c50081),
         UINT64_C(0xaa90e5c0a09a34d0), UINT64_C(0xcad3f50b762cada0), UINT64_C(0x7a14fdc266b6e3a1),
         -1},
        {UINT64_C(0x9f4735eacc01e808), UINT64_C(0x444f3131e809004a), UINT64_C(0x004b79c9ebd45987),
         UINT64_C(0xc5fd55e67b6ed2a7), UINT64_C(0x32a21c783db2351f), UINT64_C(0x389858e36fab4160),
         1},
        {UINT64_C(0x3490ee2c9f6f35d6), UINT64_C(0xf9c75ea6b159588f), UINT64_C(0xf82f826ff51414cf),
         UINT64_C(0xb381b169058d8c13), UINT64_C(0x931b441a77ebce82), UINT64_C(0x5ded4ebfd890efc6),
         1},
        {UINT64_C(0xb691ac0b6ab932ed), UINT64_C(0x10af30af22d081f1), UINT64_C(0x2027552c5c8db7cb),
         UINT64_C(0xa4f25bf7e960d753), UINT64_C(0x7d53dbf36898cf7a), UINT64_C(0x44de8ff71f8828df),
         -1},
    };
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        odt_point point = {
            double_from_bits(cases[index].point_x),
            double_from_bits(cases[index].point_y),
        };
        odt_site first = {
            double_from_bits(cases[index].first_x),
            double_from_bits(cases[index].first_y),
            10,
        };
        odt_site second = {
            double_from_bits(cases[index].second_x),
            double_from_bits(cases[index].second_y),
            20,
        };

        expect_comparison(&point, &first, 2u, &second, 7u, cases[index].expected);
        expect_comparison(&point, &second, 7u, &first, 2u, -cases[index].expected);
    }
}

static void check_geometry(const odt_domain *domain, const odt_site *sites, size_t site_count,
                           bool require_exact_fallback) {
    odt_test_geometry *geometry = NULL;
    odt_test_geometry_stats stats;
    bool saw_bisector = site_count == 1u;
    size_t cell_index;

    ODT_TEST_STATUS(odt_test_geometry_build(domain, sites, site_count, NULL, NULL, &geometry),
                    ODT_OK);
    ODT_TEST_CHECK(geometry != NULL);
    ODT_TEST_STATUS(odt_test_geometry_get_stats(geometry, &stats), ODT_OK);
    ODT_TEST_CHECK(stats.cell_count == site_count);
    ODT_TEST_CHECK(stats.vertex_count >= site_count * 3u);
    ODT_TEST_CHECK(stats.build_work != 0u);
    ODT_TEST_CHECK(stats.peak_build_bytes != 0u);
    if (require_exact_fallback) {
        ODT_TEST_CHECK(stats.exact_fallbacks != 0u);
    }

    for (cell_index = 0u; cell_index < site_count; ++cell_index) {
        size_t site_ordinal = SIZE_MAX;
        int32_t region_id = 0;
        size_t vertex_count = 0u;
        size_t edge_index;
        bool contains = false;
        bool certified = false;
        odt_point site_point = {sites[cell_index].x, sites[cell_index].y};

        ODT_TEST_STATUS(odt_test_geometry_get_cell(geometry, cell_index, &site_ordinal, &region_id,
                                                   &vertex_count),
                        ODT_OK);
        ODT_TEST_CHECK(site_ordinal == cell_index);
        ODT_TEST_CHECK(region_id == sites[cell_index].region_id);
        ODT_TEST_CHECK(vertex_count >= 3u);
        ODT_TEST_STATUS(
            odt_test_geometry_cell_contains_point(geometry, cell_index, &site_point, &contains),
            ODT_OK);
        ODT_TEST_CHECK(contains);
        ODT_TEST_STATUS(odt_test_geometry_cell_is_certified(geometry, cell_index, &certified),
                        ODT_OK);
        ODT_TEST_CHECK(certified);

        for (edge_index = 0u; edge_index < vertex_count; ++edge_index) {
            odt_test_line_kind kind = (odt_test_line_kind)99;
            size_t first_ordinal = SIZE_MAX;
            size_t second_ordinal = SIZE_MAX;

            ODT_TEST_STATUS(odt_test_geometry_get_edge(geometry, cell_index, edge_index, &kind,
                                                       &first_ordinal, &second_ordinal),
                            ODT_OK);
            ODT_TEST_CHECK(kind >= ODT_TEST_LINE_DOMAIN_MIN_X);
            ODT_TEST_CHECK(kind <= ODT_TEST_LINE_BISECTOR);
            if (kind == ODT_TEST_LINE_BISECTOR) {
                ODT_TEST_CHECK(first_ordinal < second_ordinal);
                ODT_TEST_CHECK(first_ordinal == cell_index || second_ordinal == cell_index);
                saw_bisector = true;
            } else {
                ODT_TEST_CHECK(first_ordinal == SIZE_MAX);
                ODT_TEST_CHECK(second_ordinal == SIZE_MAX);
            }
        }
    }
    ODT_TEST_CHECK(saw_bisector);
    odt_test_geometry_destroy(geometry);
}

static void test_reference_and_degenerate_geometry(void) {
    static const odt_domain reference_domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site reference_sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
    };
    static const odt_domain unit_domain = {0.0, 0.0, 1.0, 1.0};
    static const odt_site one_site[] = {{0.5, 0.5, 1}};
    static const odt_site collinear_sites[] = {
        {0.125, 0.5, 1},
        {0.5, 0.5, 2},
        {0.875, 0.5, 3},
    };
    static const odt_site boundary_sites[] = {
        {0.0, 0.0, 1},
        {1.0, 0.0, 2},
        {1.0, 1.0, 3},
        {0.0, 1.0, 4},
    };
    const odt_site near_sites[] = {
        {1.0, 1.0, 1},
        {nextafter(1.0, INFINITY), 1.0, 2},
    };
    const odt_domain near_domain = {
        0.0,
        0.0,
        nextafter(1.0, INFINITY),
        2.0,
    };
    const odt_domain subnormal_domain = {0.0, 0.0, 4.0 * DBL_TRUE_MIN, 4.0 * DBL_TRUE_MIN};
    const odt_site subnormal_sites[] = {
        {DBL_TRUE_MIN, DBL_TRUE_MIN, 1},
        {3.0 * DBL_TRUE_MIN, 3.0 * DBL_TRUE_MIN, 2},
    };
    const odt_domain skewed_domain = {
        -DBL_MAX / 4.0,
        -4.0 * DBL_TRUE_MIN,
        DBL_MAX / 4.0,
        4.0 * DBL_TRUE_MIN,
    };
    const odt_site skewed_sites[] = {
        {-DBL_MAX / 8.0, -DBL_TRUE_MIN, 1},
        {DBL_MAX / 8.0, DBL_TRUE_MIN, 2},
    };
    const odt_domain extreme_domain = {
        -DBL_MAX,
        -DBL_MAX,
        DBL_MAX,
        DBL_MAX,
    };
    const odt_site extreme_sites[] = {
        {DBL_MAX, 0.0, 1},
        {0.0, 0.0, 2},
        {0.0, DBL_MAX / 2.0, 3},
    };

    check_geometry(&reference_domain, reference_sites,
                   sizeof(reference_sites) / sizeof(reference_sites[0]), false);
    check_geometry(&unit_domain, one_site, 1u, false);
    check_geometry(&unit_domain, collinear_sites,
                   sizeof(collinear_sites) / sizeof(collinear_sites[0]), false);
    check_geometry(&unit_domain, boundary_sites, sizeof(boundary_sites) / sizeof(boundary_sites[0]),
                   true);
    check_geometry(&near_domain, near_sites, sizeof(near_sites) / sizeof(near_sites[0]), true);
    check_geometry(&subnormal_domain, subnormal_sites,
                   sizeof(subnormal_sites) / sizeof(subnormal_sites[0]), true);
    check_geometry(&skewed_domain, skewed_sites, sizeof(skewed_sites) / sizeof(skewed_sites[0]),
                   true);
    check_geometry(&extreme_domain, extreme_sites, sizeof(extreme_sites) / sizeof(extreme_sites[0]),
                   true);
}

static void test_reference_partition_membership(void) {
    static const odt_domain domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
    };
    static const odt_point points[] = {
        {0.0, 0.0}, {10.0, 8.0}, {2.0, 2.0}, {8.0, 3.0},  {4.0, 7.0},
        {5.0, 1.0}, {9.0, 7.0},  {1.0, 7.0}, {5.0, 4.25},
    };
    const odt_point tie = {5.0, 2.5};
    odt_test_geometry *geometry = NULL;
    odt_test_geometry *repeat = NULL;
    size_t point_index;

    ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, NULL, NULL, &geometry), ODT_OK);
    ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, NULL, NULL, &repeat), ODT_OK);
    for (point_index = 0u; point_index < 3u; ++point_index) {
        size_t first_ordinal;
        size_t second_ordinal;
        int32_t first_region;
        int32_t second_region;
        size_t first_count;
        size_t second_count;
        size_t edge_index;

        ODT_TEST_STATUS(odt_test_geometry_get_cell(geometry, point_index, &first_ordinal,
                                                   &first_region, &first_count),
                        ODT_OK);
        ODT_TEST_STATUS(odt_test_geometry_get_cell(repeat, point_index, &second_ordinal,
                                                   &second_region, &second_count),
                        ODT_OK);
        ODT_TEST_CHECK(first_ordinal == second_ordinal);
        ODT_TEST_CHECK(first_region == second_region);
        ODT_TEST_CHECK(first_count == second_count);
        for (edge_index = 0u; edge_index < first_count; ++edge_index) {
            odt_test_line_kind first_kind;
            odt_test_line_kind second_kind;
            size_t first_a;
            size_t first_b;
            size_t second_a;
            size_t second_b;

            ODT_TEST_STATUS(odt_test_geometry_get_edge(geometry, point_index, edge_index,
                                                       &first_kind, &first_a, &first_b),
                            ODT_OK);
            ODT_TEST_STATUS(odt_test_geometry_get_edge(repeat, point_index, edge_index,
                                                       &second_kind, &second_a, &second_b),
                            ODT_OK);
            ODT_TEST_CHECK(first_kind == second_kind);
            ODT_TEST_CHECK(first_a == second_a);
            ODT_TEST_CHECK(first_b == second_b);
        }
    }
    odt_test_geometry_destroy(repeat);
    for (point_index = 0u; point_index < sizeof(points) / sizeof(points[0]); ++point_index) {
        size_t winner = 0u;
        size_t site_index;
        size_t containing_cells = 0u;

        for (site_index = 1u; site_index < 3u; ++site_index) {
            int comparison;

            ODT_TEST_STATUS(odt_test_compare_squared_distance(&points[point_index], &sites[winner],
                                                              winner, &sites[site_index],
                                                              site_index, &comparison),
                            ODT_OK);
            if (comparison > 0) {
                winner = site_index;
            }
        }
        for (site_index = 0u; site_index < 3u; ++site_index) {
            bool contains = false;

            ODT_TEST_STATUS(odt_test_geometry_cell_contains_point(geometry, site_index,
                                                                  &points[point_index], &contains),
                            ODT_OK);
            containing_cells += contains ? 1u : 0u;
            if (site_index == winner) {
                ODT_TEST_CHECK(contains);
            }
        }
        ODT_TEST_CHECK(containing_cells == 1u);
    }
    {
        bool first_contains = false;
        bool second_contains = false;
        bool third_contains = true;

        ODT_TEST_STATUS(odt_test_geometry_cell_contains_point(geometry, 0u, &tie, &first_contains),
                        ODT_OK);
        ODT_TEST_STATUS(odt_test_geometry_cell_contains_point(geometry, 1u, &tie, &second_contains),
                        ODT_OK);
        ODT_TEST_STATUS(odt_test_geometry_cell_contains_point(geometry, 2u, &tie, &third_contains),
                        ODT_OK);
        ODT_TEST_CHECK(first_contains);
        ODT_TEST_CHECK(second_contains);
        ODT_TEST_CHECK(!third_contains);
    }
    odt_test_geometry_destroy(geometry);
}

static void expect_invalid_geometry(const odt_domain *domain, const odt_site *sites,
                                    size_t site_count, odt_status expected) {
    odt_test_geometry *geometry = (odt_test_geometry *)(uintptr_t)1u;

    ODT_TEST_STATUS(odt_test_geometry_build(domain, sites, site_count, NULL, NULL, &geometry),
                    expected);
    ODT_TEST_CHECK(geometry == NULL);
}

static void test_geometry_validation(void) {
    const odt_domain valid_domain = {0.0, 0.0, 1.0, 1.0};
    const odt_domain inverted_domain = {1.0, 0.0, 0.0, 1.0};
    const odt_domain infinite_domain = {0.0, 0.0, INFINITY, 1.0};
    const odt_site valid_sites[] = {{0.25, 0.25, 1}, {0.75, 0.75, 2}};
    const odt_site outside_sites[] = {{0.25, 0.25, 1}, {2.0, 0.75, 2}};
    const odt_site coincident_sites[] = {{-0.0, 0.0, 1}, {0.0, -0.0, 2}};
    const odt_site duplicate_ids[] = {{0.25, 0.25, 7}, {0.75, 0.75, 7}};
    const odt_site nonfinite_sites[] = {{0.25, NAN, 1}};
    odt_limits limits;
    odt_test_geometry *geometry = (odt_test_geometry *)(uintptr_t)1u;

    expect_invalid_geometry(&inverted_domain, valid_sites, 2u, ODT_INVALID_DATA);
    expect_invalid_geometry(&infinite_domain, valid_sites, 2u, ODT_INVALID_DATA);
    expect_invalid_geometry(&valid_domain, NULL, 0u, ODT_INVALID_DATA);
    expect_invalid_geometry(&valid_domain, outside_sites, 2u, ODT_INVALID_DATA);
    expect_invalid_geometry(&valid_domain, coincident_sites, 2u, ODT_INVALID_DATA);
    expect_invalid_geometry(&valid_domain, duplicate_ids, 2u, ODT_INVALID_DATA);
    expect_invalid_geometry(&valid_domain, nonfinite_sites, 1u, ODT_INVALID_DATA);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_sites = 1u;
    ODT_TEST_STATUS(
        odt_test_geometry_build(&valid_domain, valid_sites, 2u, &limits, NULL, &geometry),
        ODT_LIMIT_EXCEEDED);
    ODT_TEST_CHECK(geometry == NULL);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_polygon_fragments = 1u;
    geometry = (odt_test_geometry *)(uintptr_t)1u;
    ODT_TEST_STATUS(
        odt_test_geometry_build(&valid_domain, valid_sites, 2u, &limits, NULL, &geometry),
        ODT_LIMIT_EXCEEDED);
    ODT_TEST_CHECK(geometry == NULL);

    ODT_TEST_STATUS(odt_test_geometry_build(&valid_domain, valid_sites, 2u, NULL, NULL, NULL),
                    ODT_INVALID_ARGUMENT);
}

static void test_numeric_environment(void) {
    const odt_domain domain = {0.0, 0.0, 1.0, 1.0};
    const odt_site site = {0.5, 0.5, 1};
    int original_round = fegetround();

    ODT_TEST_STATUS(odt_test_numeric_environment_check(), ODT_OK);
    if (original_round != -1 && fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD) {
        const odt_point point = {0.25, 0.25};
        const odt_site other = {0.75, 0.75, 2};
        odt_test_geometry *geometry = (odt_test_geometry *)(uintptr_t)1u;
        int comparison = 99;

        ODT_TEST_STATUS(odt_test_numeric_environment_check(), ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT);
        ODT_TEST_STATUS(
            odt_test_compare_squared_distance(&point, &site, 0u, &other, 1u, &comparison),
            ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT);
        ODT_TEST_CHECK(comparison == 99);
        ODT_TEST_STATUS(odt_test_geometry_build(&domain, &site, 1u, NULL, NULL, &geometry),
                        ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT);
        ODT_TEST_CHECK(geometry == NULL);
        ODT_TEST_CHECK(fesetround(original_round) == 0);
        ODT_TEST_STATUS(odt_test_numeric_environment_check(), ODT_OK);
    } else if (original_round != -1) {
        ODT_TEST_CHECK(fesetround(original_round) == 0);
    }
}

static void test_resource_limits_and_allocation_failures(void) {
    static const odt_domain domain = {0.0, 0.0, 10.0, 8.0};
    static const odt_site sites[] = {
        {2.0, 2.0, 101},
        {8.0, 3.0, -7},
        {4.0, 7.0, 42},
    };
    odt_test_geometry *geometry = NULL;
    odt_test_geometry_stats stats;
    odt_limits limits;
    size_t successful_allocation_count;
    size_t fail_at;

    ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, NULL, NULL, &geometry), ODT_OK);
    ODT_TEST_STATUS(odt_test_geometry_get_stats(geometry, &stats), ODT_OK);
    odt_test_geometry_destroy(geometry);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_build_work = stats.build_work - 1u;
    geometry = (odt_test_geometry *)(uintptr_t)1u;
    ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, &limits, NULL, &geometry),
                    ODT_LIMIT_EXCEEDED);
    ODT_TEST_CHECK(geometry == NULL);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.max_build_bytes = (size_t)stats.peak_build_bytes - 1u;
    geometry = (odt_test_geometry *)(uintptr_t)1u;
    ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, &limits, NULL, &geometry),
                    ODT_LIMIT_EXCEEDED);
    ODT_TEST_CHECK(geometry == NULL);

    {
        odt_test_allocator_state state = {.live = true};
        odt_allocator allocator;

        odt_test_counting_allocator_init(&allocator, &state);
        ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, NULL, &allocator, &geometry),
                        ODT_OK);
        successful_allocation_count = state.allocate_calls;
        ODT_TEST_CHECK(successful_allocation_count >= 6u);
        ODT_TEST_CHECK(state.live_allocations != 0u);
        odt_test_geometry_destroy(geometry);
        ODT_TEST_CHECK(state.live_allocations == 0u);
        ODT_TEST_CHECK(state.allocate_calls == state.deallocate_calls);
    }

    for (fail_at = 1u; fail_at <= successful_allocation_count; ++fail_at) {
        odt_test_allocator_state state = {
            .live = true,
            .fail_at_call = fail_at,
        };
        odt_allocator allocator;

        odt_test_counting_allocator_init(&allocator, &state);
        geometry = (odt_test_geometry *)(uintptr_t)1u;
        ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, NULL, &allocator, &geometry),
                        ODT_OUT_OF_MEMORY);
        ODT_TEST_CHECK(geometry == NULL);
        ODT_TEST_CHECK(state.live_allocations == 0u);
        ODT_TEST_CHECK(state.deallocate_calls + 1u == state.allocate_calls);

        state.fail_at_call = 0u;
        ODT_TEST_STATUS(odt_test_geometry_build(&domain, sites, 3u, NULL, &allocator, &geometry),
                        ODT_OK);
        odt_test_geometry_destroy(geometry);
        ODT_TEST_CHECK(state.live_allocations == 0u);
    }
}

int main(void) {
    test_exact_squared_distance_edges();
    test_fraction_generated_raw_bit_corpus();
    test_reference_and_degenerate_geometry();
    test_reference_partition_membership();
    test_geometry_validation();
    test_numeric_environment();
    test_resource_limits_and_allocation_failures();
    return EXIT_SUCCESS;
}
