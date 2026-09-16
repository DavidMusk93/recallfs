#include "odt_internal.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct odt_query_address_range {
    uintptr_t first;
    uintptr_t last;
} odt_query_address_range;

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

static bool odt_query_range_init(const void *base, size_t count, size_t stride, size_t element_size,
                                 odt_query_address_range *out_range) {
    size_t last_offset;
    size_t span;
    uintptr_t first;

    if (base == NULL || count == 0u || stride < element_size || count - 1u > SIZE_MAX / stride) {
        return false;
    }
    last_offset = (count - 1u) * stride;
    if (last_offset > SIZE_MAX - element_size) {
        return false;
    }
    span = last_offset + element_size;
    first = (uintptr_t)base;
    if ((uintmax_t)(span - 1u) > (uintmax_t)(UINTPTR_MAX - first)) {
        return false;
    }
    out_range->first = first;
    out_range->last = first + (uintptr_t)(span - 1u);
    return true;
}

static bool odt_query_ranges_overlap(const odt_query_address_range *first,
                                     const odt_query_address_range *second) {
    return first->first <= second->last && second->first <= first->last;
}

static odt_status odt_query_evaluate(const odt_generation *generation, const odt_point *point,
                                     odt_query_result *out_result, odt_query_stats *out_stats) {
    const odt_site *sites;
    odt_query_result result;
    odt_query_stats stats = {0u, 0u};
    int32_t reference;
    odt_status status;

    if (!isfinite(point->x) || !isfinite(point->y)) {
        *out_stats = stats;
        return ODT_INVALID_DATA;
    }
    if (point->x < generation->domain.min_x || point->x > generation->domain.max_x ||
        point->y < generation->domain.min_y || point->y > generation->domain.max_y) {
        result.kind = ODT_RESULT_OUTSIDE;
        result.status = ODT_OK;
        result.region_id = 0;
        *out_result = result;
        *out_stats = stats;
        return ODT_OK;
    }
    sites = odt_generation_sites(generation);
    reference = generation->root_reference;
    while (reference > 0) {
        const odt_internal_runtime_node *node;
        bool used_exact;
        int comparison;

        if (!odt_reference_valid(generation, reference) ||
            stats.comparisons >= (uint64_t)generation->node_count) {
            *out_stats = stats;
            return ODT_INTERNAL_ERROR;
        }
        node = &odt_generation_nodes(generation)[(size_t)reference - 1u];
        status = odt_internal_runtime_bisector_compare(point, sites, generation->site_count,
                                                       generation->filter_scale_exponent, node,
                                                       &comparison, &used_exact);
        if (status != ODT_OK) {
            *out_stats = stats;
            return status;
        }
        reference = comparison <= 0 ? node->first_child : node->second_child;
        stats.comparisons += 1u;
        stats.exact_fallbacks += used_exact ? 1u : 0u;
    }
    if (!odt_reference_valid(generation, reference)) {
        *out_stats = stats;
        return ODT_INTERNAL_ERROR;
    }
    {
        const size_t leaf_index = (size_t)(-(int64_t)reference) - 1u;
        const odt_internal_runtime_leaf *leaf = &odt_generation_leaves(generation)[leaf_index];

        if ((size_t)leaf->site_ordinal >= generation->site_count ||
            leaf->region_id != sites[leaf->site_ordinal].region_id) {
            *out_stats = stats;
            return ODT_INTERNAL_ERROR;
        }
        result.kind = ODT_RESULT_REGION;
        result.status = ODT_OK;
        result.region_id = leaf->region_id;
    }
    *out_result = result;
    *out_stats = stats;
    return ODT_OK;
}

odt_status odt_query(const odt_generation *generation, const odt_point *point,
                     odt_query_result *out_result, odt_query_stats *out_stats) {
    odt_query_result result;
    odt_query_stats stats;
    odt_status status;

    if (generation == NULL || point == NULL || out_result == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    status = odt_internal_numeric_environment_check();
    if (status != ODT_OK) {
        return status;
    }
    status = odt_query_evaluate(generation, point, &result, &stats);
    if (status != ODT_OK) {
        return status;
    }
    *out_result = result;
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    return ODT_OK;
}

odt_status odt_query_batch(const odt_generation *generation, size_t count, const void *points,
                           size_t point_stride, void *out_results, size_t result_stride,
                           odt_batch_stats *out_stats) {
    odt_query_address_range point_range;
    odt_query_address_range result_range;
    odt_query_address_range stats_range;
    odt_batch_stats stats = {0u, 0u, 0u, 0u, 0u, 0u};
    const unsigned char *point_bytes = points;
    unsigned char *result_bytes = out_results;
    odt_status status;
    size_t index;

    if (generation == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if (count == 0u) {
        if (out_stats != NULL) {
            *out_stats = stats;
        }
        return ODT_OK;
    }
    if (!odt_query_range_init(points, count, point_stride, sizeof(odt_point), &point_range) ||
        !odt_query_range_init(out_results, count, result_stride, sizeof(odt_query_result),
                              &result_range) ||
        odt_query_ranges_overlap(&point_range, &result_range) ||
        (uintmax_t)count > (uintmax_t)UINT64_MAX ||
        (generation->node_count != 0u &&
         (uintmax_t)count > (uintmax_t)UINT64_MAX / (uintmax_t)generation->node_count)) {
        return ODT_INVALID_ARGUMENT;
    }
    if (out_stats != NULL) {
        if (!odt_query_range_init(out_stats, 1u, sizeof(*out_stats), sizeof(*out_stats),
                                  &stats_range) ||
            odt_query_ranges_overlap(&stats_range, &point_range) ||
            odt_query_ranges_overlap(&stats_range, &result_range)) {
            return ODT_INVALID_ARGUMENT;
        }
    }
    status = odt_internal_numeric_environment_check();
    if (status != ODT_OK) {
        return status;
    }

    stats.attempted = (uint64_t)count;
    for (index = 0u; index < count; ++index) {
        odt_point point;
        odt_query_result result;
        odt_query_stats point_stats;

        memcpy(&point, point_bytes + index * point_stride, sizeof(point));
        status = odt_query_evaluate(generation, &point, &result, &point_stats);
        stats.comparisons += point_stats.comparisons;
        stats.exact_fallbacks += point_stats.exact_fallbacks;
        if (status == ODT_OK && result.kind == ODT_RESULT_REGION) {
            stats.region_count += 1u;
        } else if (status == ODT_OK && result.kind == ODT_RESULT_OUTSIDE) {
            stats.outside_count += 1u;
        } else {
            result.kind = ODT_RESULT_ERROR;
            result.status = status == ODT_OK ? ODT_INTERNAL_ERROR : status;
            result.region_id = 0;
            stats.failed_count += 1u;
        }
        memcpy(result_bytes + index * result_stride, &result, sizeof(result));
    }
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
    return odt_internal_encoded_size(generation, out_encoded_size);
}

odt_status odt_generation_get_build_stats(const odt_generation *generation,
                                          odt_build_stats *out_stats) {
    if (generation == NULL || out_stats == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_stats = generation->build_stats;
    return ODT_OK;
}
