#include "point_location_example.h"

/*
 * Internal references are one-based. Negative references select leaf values:
 * -1 = outside, -2 = A, -3 = B, -4 = C.
 *
 * The first four nodes accept the closed rectangle [0, 10] x [0, 8]. The last
 * three nodes implement a nearest-site tournament for A=(2,2), B=(8,3), and
 * C=(4,7). A comparison against a pair's perpendicular bisector chooses the
 * closer site; equality chooses the lower region ID.
 */
static const struct odt_node nodes[] = {
    {{1.0, 0.0}, 0.0, 2, -1},   {{-1.0, 0.0}, -10.0, 3, -1}, {{0.0, 1.0}, 0.0, 4, -1},
    {{0.0, -1.0}, -8.0, 5, -1}, {{-6.0, -1.0}, -32.5, 6, 7}, {{-2.0, -5.0}, -28.5, -2, -4},
    {{4.0, -4.0}, 4.0, -3, -4},
};

static const int32_t leaf_values[] = {
    POINT_LOCATION_OUTSIDE,
    POINT_LOCATION_A,
    POINT_LOCATION_B,
    POINT_LOCATION_C,
};

static const struct odt_tree tree = {
    nodes,
    sizeof(nodes) / sizeof(nodes[0]),
    leaf_values,
    sizeof(leaf_values) / sizeof(leaf_values[0]),
};

const struct odt_tree *point_location_example_tree(void) {
    return &tree;
}

const char *point_location_region_name(int32_t region) {
    switch (region) {
    case POINT_LOCATION_OUTSIDE:
        return "outside";
    case POINT_LOCATION_A:
        return "A";
    case POINT_LOCATION_B:
        return "B";
    case POINT_LOCATION_C:
        return "C";
    default:
        return "unknown";
    }
}
