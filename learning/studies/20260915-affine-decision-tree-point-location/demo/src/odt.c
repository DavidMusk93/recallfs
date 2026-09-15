#include "odt.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static int valid_reference(const struct odt_tree *tree, int32_t reference) {
    if (reference > 0) {
        return (uint64_t)reference <= tree->node_count;
    }
    if (reference < 0) {
        return (uint64_t)(-(int64_t)reference) <= tree->leaf_count;
    }
    return 0;
}

enum odt_status odt_tree_validate(const struct odt_tree *tree) {
    size_t *node_work = NULL;
    unsigned char *seen_leaves = NULL;
    size_t stack_size = 0;
    size_t visited_nodes = 0;
    enum odt_status status = ODT_INVALID_TREE;

    if (tree == NULL || tree->nodes == NULL || tree->leaf_values == NULL || tree->node_count == 0 ||
        tree->leaf_count == 0) {
        return ODT_INVALID_ARGUMENT;
    }
    if (tree->node_count > SIZE_MAX / sizeof(*node_work)) {
        return ODT_NO_MEMORY;
    }

    node_work = calloc(tree->node_count, sizeof(*node_work));
    seen_leaves = calloc(tree->leaf_count, sizeof(*seen_leaves));
    if (node_work == NULL || seen_leaves == NULL) {
        status = ODT_NO_MEMORY;
        goto out;
    }

    for (size_t i = 0; i < tree->node_count; ++i) {
        const struct odt_node *node = &tree->nodes[i];
        const int32_t references[] = {node->above, node->below};

        if (!isfinite(node->weights[0]) || !isfinite(node->weights[1]) ||
            !isfinite(node->threshold)) {
            goto out;
        }
        for (size_t branch = 0; branch < 2; ++branch) {
            int32_t reference = references[branch];
            if (!valid_reference(tree, reference)) {
                goto out;
            }
            if (reference > 0) {
                ++node_work[(size_t)reference - 1];
            }
        }
    }

    if (node_work[0] != 0) {
        goto out;
    }
    for (size_t i = 1; i < tree->node_count; ++i) {
        if (node_work[i] != 1) {
            goto out;
        }
    }

    node_work[stack_size++] = 0;
    while (stack_size > 0) {
        size_t node_index = node_work[--stack_size];
        const struct odt_node *node = &tree->nodes[node_index];
        const int32_t references[] = {node->above, node->below};

        ++visited_nodes;
        for (size_t branch = 0; branch < 2; ++branch) {
            int32_t reference = references[branch];
            if (reference > 0) {
                node_work[stack_size++] = (size_t)reference - 1;
            } else {
                seen_leaves[(size_t)(-(int64_t)reference) - 1] = 1;
            }
        }
    }

    if (visited_nodes != tree->node_count) {
        goto out;
    }
    for (size_t i = 0; i < tree->leaf_count; ++i) {
        if (seen_leaves[i] == 0) {
            goto out;
        }
    }
    status = ODT_OK;

out:
    free(seen_leaves);
    free(node_work);
    return status;
}

enum odt_status odt_locate_2d(const struct odt_tree *tree, double x, double y,
                              struct odt_result *result) {
    int32_t reference = 1;
    size_t comparisons = 0;

    if (tree == NULL || tree->nodes == NULL || tree->leaf_values == NULL || result == NULL ||
        tree->node_count == 0 || tree->leaf_count == 0) {
        return ODT_INVALID_ARGUMENT;
    }
    if (!isfinite(x) || !isfinite(y)) {
        return ODT_NONFINITE_FEATURE;
    }

    while (reference > 0) {
        if ((uint64_t)reference > tree->node_count || comparisons >= tree->node_count) {
            return ODT_INVALID_TREE;
        }
        const struct odt_node *node = &tree->nodes[(size_t)reference - 1];
        double score = node->weights[0] * x + node->weights[1] * y;
        if (!isfinite(score)) {
            return ODT_NUMERIC_RANGE;
        }
        reference = score >= node->threshold ? node->above : node->below;
        ++comparisons;
    }

    if (!valid_reference(tree, reference)) {
        return ODT_INVALID_TREE;
    }
    result->value = tree->leaf_values[(size_t)(-(int64_t)reference) - 1];
    result->comparisons = comparisons;
    return ODT_OK;
}
