#include "odt_test_support.h"

#include "odt_internal.h"

#include <stdint.h>

static bool odt_test_is_power_of_two(size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool odt_test_align_up(size_t value, size_t alignment, size_t *out_value) {
    size_t remainder;

    if (!odt_test_is_power_of_two(alignment)) {
        return false;
    }
    remainder = value & (alignment - 1u);
    if (remainder == 0u) {
        *out_value = value;
        return true;
    }
    if (value > SIZE_MAX - (alignment - remainder)) {
        return false;
    }
    *out_value = value + alignment - remainder;
    return true;
}

static void *odt_test_allocate(void *context, size_t size, size_t alignment) {
    odt_test_allocator_state *state = context;
    size_t aligned_size;
    void *pointer;

    if (!state->live) {
        state->access_after_dead += 1u;
        return NULL;
    }
    state->allocate_calls += 1u;
    state->last_size = size;
    state->last_alignment = alignment;
    if (state->fail_allocation ||
        (state->fail_at_call != 0u && state->allocate_calls == state->fail_at_call) ||
        !odt_test_align_up(size, alignment, &aligned_size)) {
        return NULL;
    }
    pointer = aligned_alloc(alignment, aligned_size);
    if (pointer != NULL) {
        const size_t index = state->live_allocations;

        ODT_TEST_CHECK(index < sizeof(state->pointers) / sizeof(state->pointers[0]));
        state->pointers[index] = pointer;
        state->sizes[index] = size;
        state->alignments[index] = alignment;
        state->live_allocations += 1u;
        if (state->live_allocations > state->peak_live_allocations) {
            state->peak_live_allocations = state->live_allocations;
        }
        if (state->live_allocations == 1u) {
            state->active_pointer = pointer;
        }
    }
    return pointer;
}

static void odt_test_deallocate(void *context, void *pointer, size_t size, size_t alignment) {
    odt_test_allocator_state *state = context;
    size_t index;

    if (!state->live) {
        state->access_after_dead += 1u;
        return;
    }
    for (index = 0u; index < state->live_allocations; ++index) {
        if (state->pointers[index] == pointer) {
            break;
        }
    }
    ODT_TEST_CHECK(index < state->live_allocations);
    ODT_TEST_CHECK(size == state->sizes[index]);
    ODT_TEST_CHECK(alignment == state->alignments[index]);
    state->live_allocations -= 1u;
    state->pointers[index] = state->pointers[state->live_allocations];
    state->sizes[index] = state->sizes[state->live_allocations];
    state->alignments[index] = state->alignments[state->live_allocations];
    state->pointers[state->live_allocations] = NULL;
    state->deallocate_calls += 1u;
    state->active_pointer = state->live_allocations == 0u ? NULL : state->pointers[0];
    free(pointer);
}

void odt_test_counting_allocator_init(odt_allocator *out_allocator,
                                      odt_test_allocator_state *state) {
    ODT_TEST_STATUS(odt_allocator_init(out_allocator), ODT_OK);
    out_allocator->context = state;
    out_allocator->allocate = odt_test_allocate;
    out_allocator->deallocate = odt_test_deallocate;
}

odt_status odt_test_allocator_validate(const odt_allocator *allocator) {
    return odt_internal_allocator_validate(allocator);
}

odt_status odt_test_limits_validate(const odt_limits *limits) {
    return odt_internal_limits_validate(limits);
}

odt_status odt_test_build_options_validate(const odt_build_options *options) {
    return odt_internal_build_options_validate(options);
}

odt_status odt_test_encode_options_validate(const odt_encode_options *options) {
    return odt_internal_encode_options_validate(options);
}

odt_status odt_test_load_limits_validate(const odt_load_limits *limits) {
    return odt_internal_load_limits_validate(limits);
}

odt_status odt_test_generation_create(const odt_allocator *allocator, size_t size, size_t alignment,
                                      odt_generation **out_generation) {
    return odt_internal_generation_allocate(allocator, size, alignment, out_generation);
}

odt_status odt_test_numeric_environment_check(void) {
    return odt_internal_numeric_environment_check();
}

odt_status odt_test_compare_squared_distance(const odt_point *point, const odt_site *first,
                                             size_t first_ordinal, const odt_site *second,
                                             size_t second_ordinal, int *out_comparison) {
    odt_status status = odt_internal_numeric_environment_check();

    if (status != ODT_OK) {
        return status;
    }
    return odt_internal_compare_squared_distance(point, first, first_ordinal, second,
                                                 second_ordinal, out_comparison);
}

odt_status odt_test_geometry_build(const odt_domain *domain, const odt_site *sites,
                                   size_t site_count, const odt_limits *limits,
                                   const odt_allocator *allocator,
                                   odt_test_geometry **out_geometry) {
    odt_internal_geometry *geometry = NULL;
    odt_status status;

    if (out_geometry == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_geometry = NULL;
    status = odt_internal_geometry_build(domain, sites, site_count, limits, allocator, &geometry);
    if (status == ODT_OK) {
        *out_geometry = (odt_test_geometry *)geometry;
    }
    return status;
}

void odt_test_geometry_destroy(odt_test_geometry *geometry) {
    odt_internal_geometry_destroy((odt_internal_geometry *)geometry);
}

odt_status odt_test_geometry_get_stats(const odt_test_geometry *geometry,
                                       odt_test_geometry_stats *out_stats) {
    const odt_internal_geometry *internal = (const odt_internal_geometry *)geometry;

    if (internal == NULL || out_stats == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    out_stats->build_work = internal->build_work;
    out_stats->exact_fallbacks = internal->exact_fallbacks;
    out_stats->cell_count = (uint64_t)internal->site_count;
    out_stats->vertex_count = internal->vertex_count;
    out_stats->peak_build_bytes = (uint64_t)internal->peak_build_bytes;
    return ODT_OK;
}

odt_status odt_test_geometry_get_cell(const odt_test_geometry *geometry, size_t cell_index,
                                      size_t *out_site_ordinal, int32_t *out_region_id,
                                      size_t *out_vertex_count) {
    const odt_internal_geometry *internal = (const odt_internal_geometry *)geometry;
    const odt_internal_cell *cell;

    if (internal == NULL || cell_index >= internal->site_count || out_site_ordinal == NULL ||
        out_region_id == NULL || out_vertex_count == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    cell = &internal->cells[cell_index];
    *out_site_ordinal = cell->site_ordinal;
    *out_region_id = cell->region_id;
    *out_vertex_count = cell->vertex_count;
    return ODT_OK;
}

odt_status odt_test_geometry_cell_contains_point(const odt_test_geometry *geometry,
                                                 size_t cell_index, const odt_point *point,
                                                 bool *out_contains) {
    return odt_internal_geometry_cell_contains_point((const odt_internal_geometry *)geometry,
                                                     cell_index, point, out_contains);
}

odt_status odt_test_geometry_cell_is_certified(const odt_test_geometry *geometry, size_t cell_index,
                                               bool *out_certified) {
    const odt_internal_geometry *internal = (const odt_internal_geometry *)geometry;

    if (internal == NULL || cell_index >= internal->site_count || out_certified == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_certified = internal->cells[cell_index].certified;
    return ODT_OK;
}

odt_status odt_test_geometry_get_edge(const odt_test_geometry *geometry, size_t cell_index,
                                      size_t edge_index, odt_test_line_kind *out_kind,
                                      size_t *out_first_ordinal, size_t *out_second_ordinal) {
    const odt_internal_geometry *internal = (const odt_internal_geometry *)geometry;
    const odt_internal_cell *cell;
    odt_internal_line_ref line;

    if (internal == NULL || cell_index >= internal->site_count || out_kind == NULL ||
        out_first_ordinal == NULL || out_second_ordinal == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    cell = &internal->cells[cell_index];
    if (edge_index >= cell->vertex_count) {
        return ODT_INVALID_ARGUMENT;
    }
    line = cell->vertices[edge_index].incoming_line;
    *out_kind = (odt_test_line_kind)line.kind;
    *out_first_ordinal = line.first_ordinal;
    *out_second_ordinal = line.second_ordinal;
    return ODT_OK;
}
