#ifndef ODT_H
#define ODT_H

#include <stddef.h>
#include <stdint.h>

enum odt_status {
    ODT_OK = 0,
    ODT_INVALID_ARGUMENT = -1,
    ODT_INVALID_TREE = -2,
    ODT_NONFINITE_FEATURE = -3,
    ODT_NO_MEMORY = -4,
    ODT_NUMERIC_RANGE = -5,
};

struct odt_node {
    double weights[2];
    double threshold;
    int32_t above;
    int32_t below;
};

struct odt_tree {
    const struct odt_node *nodes;
    size_t node_count;
    const int32_t *leaf_values;
    size_t leaf_count;
};

struct odt_result {
    int32_t value;
    size_t comparisons;
};

enum odt_status odt_tree_validate(const struct odt_tree *tree);
/*
 * The evaluator contract is the binary64 computation
 * weights[0] * x + weights[1] * y >= threshold, not exact real arithmetic.
 * A non-finite computed score returns ODT_NUMERIC_RANGE. Any failure leaves
 * result unchanged.
 */
enum odt_status odt_locate_2d(const struct odt_tree *tree, double x, double y,
                              struct odt_result *result);

#endif
