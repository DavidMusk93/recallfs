#include "odt.h"
#include "point_location_example.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct location_case {
    double x;
    double y;
    int32_t expected;
    size_t comparisons;
};

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_example_tree(void) {
    static const struct location_case cases[] = {
        {2.0, 2.0, POINT_LOCATION_A, 6},         {8.0, 3.0, POINT_LOCATION_B, 6},
        {4.0, 7.0, POINT_LOCATION_C, 6},         {5.0, 2.5, POINT_LOCATION_A, 6},
        {3.0, 4.5, POINT_LOCATION_A, 6},         {6.0, 5.0, POINT_LOCATION_B, 6},
        {0.0, 0.0, POINT_LOCATION_A, 6},         {10.0, 8.0, POINT_LOCATION_B, 6},
        {-0.01, 4.0, POINT_LOCATION_OUTSIDE, 1}, {10.01, 4.0, POINT_LOCATION_OUTSIDE, 2},
        {5.0, -0.01, POINT_LOCATION_OUTSIDE, 3}, {5.0, 8.01, POINT_LOCATION_OUTSIDE, 4},
    };
    const struct odt_tree *tree = point_location_example_tree();

    require(odt_tree_validate(tree) == ODT_OK, "example tree must validate");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct odt_result result = {0};
        require(odt_locate_2d(tree, cases[i].x, cases[i].y, &result) == ODT_OK,
                "example lookup must succeed");
        require(result.value == cases[i].expected, "example lookup returned the wrong region");
        require(result.comparisons == cases[i].comparisons,
                "example lookup returned the wrong comparison count");
    }

    struct odt_result result = {0};
    require(odt_locate_2d(tree, NAN, 1.0, &result) == ODT_NONFINITE_FEATURE,
            "NaN must be rejected instead of silently taking the below branch");
    require(odt_locate_2d(tree, 1.0, INFINITY, &result) == ODT_NONFINITE_FEATURE,
            "infinity must be rejected");
}

static void test_numeric_range_failure(void) {
    static const struct odt_node nodes[] = {
        {{DBL_MAX, DBL_MAX}, 0.0, -1, -2},
    };
    static const int32_t leaves[] = {7, 9};
    static const struct odt_tree tree = {nodes, 1, leaves, 2};
    struct odt_result result = {123, 456};

    require(odt_tree_validate(&tree) == ODT_OK, "finite DBL_MAX-scale tree must validate");
    require(odt_locate_2d(&tree, 1.0, 1.0, &result) == ODT_NUMERIC_RANGE,
            "non-finite affine score must be rejected");
    require(result.value == 123 && result.comparisons == 456,
            "numeric range failure must leave result unchanged");
}

static void test_validation_failures(void) {
    static const int32_t leaves[] = {7};
    static const struct odt_node zero_child[] = {
        {{1.0, 0.0}, 0.0, 0, -1},
    };
    static const struct odt_node unreachable_node[] = {
        {{1.0, 0.0}, 0.0, -1, -1},
        {{0.0, 1.0}, 0.0, -1, -1},
    };
    static const struct odt_node cycle[] = {
        {{1.0, 0.0}, 0.0, 2, -1},
        {{0.0, 1.0}, 0.0, 1, -1},
    };
    static const struct odt_node shared_node[] = {
        {{1.0, 0.0}, 0.0, 2, 3},
        {{0.0, 1.0}, 0.0, 4, -1},
        {{0.0, -1.0}, 0.0, 4, -1},
        {{1.0, 1.0}, 0.0, -1, -1},
    };
    static const struct odt_node nonfinite_weight[] = {
        {{INFINITY, 0.0}, 0.0, -1, -1},
    };
    static const struct odt_tree invalid_trees[] = {
        {zero_child, 1, leaves, 1},  {unreachable_node, 2, leaves, 1}, {cycle, 2, leaves, 1},
        {shared_node, 4, leaves, 1}, {nonfinite_weight, 1, leaves, 1},
    };

    for (size_t i = 0; i < sizeof(invalid_trees) / sizeof(invalid_trees[0]); ++i) {
        require(odt_tree_validate(&invalid_trees[i]) == ODT_INVALID_TREE,
                "malformed tree must be rejected");
    }
}

int main(void) {
    test_example_tree();
    test_numeric_range_failure();
    test_validation_failures();
    puts("odt tests passed: 12 location anchors and 8 failure checks");
    return EXIT_SUCCESS;
}
