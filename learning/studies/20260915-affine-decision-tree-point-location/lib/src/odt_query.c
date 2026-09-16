#include "odt_internal.h"

#include <math.h>
#include <stdint.h>

static const odt_site *odt_generation_sites(const odt_generation *generation) {
    return (const odt_site *)((const unsigned char *)generation + generation->sites_offset);
}

static const odt_internal_runtime_node *odt_generation_nodes(const odt_generation *generation) {
    return (const odt_internal_runtime_node *)((const unsigned char *)generation +
                                               generation->nodes_offset);
}

static const odt_internal_runtime_leaf *odt_generation_leaves(const odt_generation *generation) {
    return (const odt_internal_runtime_leaf *)((const unsigned char *)generation +
                                               generation->leaves_offset);
}

static bool odt_reference_valid(const odt_generation *generation, int32_t reference) {
    if (reference > 0) {
        return (uint64_t)reference <= (uint64_t)generation->node_count;
    }
    if (reference < 0) {
        return (uint64_t)(-(int64_t)reference) <= (uint64_t)generation->leaf_count;
    }
    return false;
}

odt_status odt_query(const odt_generation *generation, const odt_point *point,
                     odt_query_result *out_result, odt_query_stats *out_stats) {
    odt_query_result result;
    odt_query_stats stats = {0u, 0u};
    int32_t reference;
    odt_status status;

    if (generation == NULL || point == NULL || out_result == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    status = odt_internal_numeric_environment_check();
    if (status != ODT_OK) {
        return status;
    }
    if (!isfinite(point->x) || !isfinite(point->y)) {
        return ODT_INVALID_DATA;
    }
    if (point->x < generation->domain.min_x || point->x > generation->domain.max_x ||
        point->y < generation->domain.min_y || point->y > generation->domain.max_y) {
        result.kind = ODT_RESULT_OUTSIDE;
        result.status = ODT_OK;
        result.region_id = 0;
        *out_result = result;
        if (out_stats != NULL) {
            *out_stats = stats;
        }
        return ODT_OK;
    }
    reference = generation->root_reference;
    while (reference > 0) {
        const odt_internal_runtime_node *node;
        bool used_exact;
        int comparison;

        if (!odt_reference_valid(generation, reference) ||
            stats.comparisons >= (uint64_t)generation->node_count) {
            return ODT_INTERNAL_ERROR;
        }
        node = &odt_generation_nodes(generation)[(size_t)reference - 1u];
        status = odt_internal_runtime_bisector_compare(
            point, odt_generation_sites(generation), generation->site_count,
            generation->filter_scale_exponent, node, &comparison, &used_exact);
        if (status != ODT_OK) {
            return status;
        }
        reference = comparison <= 0 ? node->first_child : node->second_child;
        stats.comparisons += 1u;
        stats.exact_fallbacks += used_exact ? 1u : 0u;
    }
    if (!odt_reference_valid(generation, reference)) {
        return ODT_INTERNAL_ERROR;
    }
    {
        const size_t leaf_index = (size_t)(-(int64_t)reference) - 1u;
        const odt_internal_runtime_leaf *leaf = &odt_generation_leaves(generation)[leaf_index];

        if ((size_t)leaf->site_ordinal >= generation->site_count ||
            leaf->region_id != odt_generation_sites(generation)[leaf->site_ordinal].region_id) {
            return ODT_INTERNAL_ERROR;
        }
        result.kind = ODT_RESULT_REGION;
        result.status = ODT_OK;
        result.region_id = leaf->region_id;
    }
    *out_result = result;
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    return ODT_OK;
}

odt_status odt_generation_get_domain(const odt_generation *generation, odt_domain *out_domain) {
    if (generation == NULL || out_domain == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_domain = generation->domain;
    return ODT_OK;
}

odt_status odt_generation_get_site_count(const odt_generation *generation,
                                         uint64_t *out_site_count) {
    if (generation == NULL || out_site_count == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_site_count = (uint64_t)generation->site_count;
    return ODT_OK;
}

odt_status odt_generation_get_node_count(const odt_generation *generation,
                                         uint64_t *out_node_count) {
    if (generation == NULL || out_node_count == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_node_count = (uint64_t)generation->node_count;
    return ODT_OK;
}

odt_status odt_generation_get_leaf_count(const odt_generation *generation,
                                         uint64_t *out_leaf_count) {
    if (generation == NULL || out_leaf_count == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_leaf_count = (uint64_t)generation->leaf_count;
    return ODT_OK;
}

odt_status odt_generation_get_maximum_depth(const odt_generation *generation,
                                            uint32_t *out_maximum_depth) {
    if (generation == NULL || out_maximum_depth == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_maximum_depth = generation->build_stats.maximum_depth;
    return ODT_OK;
}

odt_status odt_generation_get_encoded_size(const odt_generation *generation,
                                           uint64_t *out_encoded_size) {
    if (generation == NULL || out_encoded_size == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    return ODT_UNSUPPORTED_FORMAT;
}

odt_status odt_generation_get_build_stats(const odt_generation *generation,
                                          odt_build_stats *out_stats) {
    if (generation == NULL || out_stats == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_stats = generation->build_stats;
    return ODT_OK;
}
