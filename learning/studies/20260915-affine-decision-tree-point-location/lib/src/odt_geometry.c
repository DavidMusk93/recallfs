#include "odt_internal.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct odt_build_memory {
    odt_allocator allocator;
    size_t limit;
    size_t live;
    size_t peak;
} odt_build_memory;

typedef struct odt_geometry_build_state {
    odt_build_memory memory;
    uint64_t work;
    uint64_t max_work;
    uint64_t exact_fallbacks;
} odt_geometry_build_state;

static bool odt_checked_add_size(size_t first, size_t second, size_t *out_value) {
    if (first > SIZE_MAX - second) {
        return false;
    }
    *out_value = first + second;
    return true;
}

static bool odt_checked_multiply_size(size_t first, size_t second, size_t *out_value) {
    if (first != 0u && second > SIZE_MAX / first) {
        return false;
    }
    *out_value = first * second;
    return true;
}

static bool odt_is_power_of_two_size(size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static size_t odt_allocation_alignment(size_t natural_alignment) {
    return natural_alignment < sizeof(void *) ? sizeof(void *) : natural_alignment;
}

static bool odt_allocation_charge(size_t size, size_t alignment, size_t *out_charge) {
    const size_t remainder = size & (alignment - 1u);

    if (size == 0u || !odt_is_power_of_two_size(alignment)) {
        return false;
    }
    if (remainder == 0u) {
        *out_charge = size;
        return true;
    }
    return odt_checked_add_size(size, alignment - remainder, out_charge);
}

static void *odt_build_allocate(odt_geometry_build_state *state, size_t size, size_t alignment,
                                odt_status *out_status) {
    size_t charge;
    void *pointer;

    if (!odt_allocation_charge(size, alignment, &charge)) {
        *out_status = ODT_LIMIT_EXCEEDED;
        return NULL;
    }
    if (state->memory.live > state->memory.limit ||
        charge > state->memory.limit - state->memory.live) {
        *out_status = ODT_LIMIT_EXCEEDED;
        return NULL;
    }
    pointer = state->memory.allocator.allocate(state->memory.allocator.context, size, alignment);
    if (pointer == NULL) {
        *out_status = ODT_OUT_OF_MEMORY;
        return NULL;
    }
    state->memory.live += charge;
    if (state->memory.live > state->memory.peak) {
        state->memory.peak = state->memory.live;
    }
    *out_status = ODT_OK;
    return pointer;
}

static void odt_build_deallocate(odt_geometry_build_state *state, void *pointer, size_t size,
                                 size_t alignment) {
    size_t charge;

    if (pointer == NULL) {
        return;
    }
    if (!odt_allocation_charge(size, alignment, &charge) || charge > state->memory.live) {
        return;
    }
    state->memory.live -= charge;
    state->memory.allocator.deallocate(state->memory.allocator.context, pointer, size, alignment);
}

static odt_status odt_consume_work(odt_geometry_build_state *state, uint64_t amount) {
    if (state->work > state->max_work || amount > state->max_work - state->work) {
        return ODT_LIMIT_EXCEEDED;
    }
    state->work += amount;
    return ODT_OK;
}

static int odt_line_ref_compare(const odt_internal_line_ref *first,
                                const odt_internal_line_ref *second) {
    if (first->kind != second->kind) {
        return first->kind < second->kind ? -1 : 1;
    }
    if (first->first_ordinal != second->first_ordinal) {
        return first->first_ordinal < second->first_ordinal ? -1 : 1;
    }
    if (first->second_ordinal != second->second_ordinal) {
        return first->second_ordinal < second->second_ordinal ? -1 : 1;
    }
    return 0;
}

static odt_internal_line_ref odt_domain_line(odt_internal_line_kind kind) {
    odt_internal_line_ref line = {
        kind,
        SIZE_MAX,
        SIZE_MAX,
    };

    return line;
}

static odt_internal_line_ref odt_bisector_line(size_t first_ordinal, size_t second_ordinal) {
    odt_internal_line_ref line = {
        ODT_INTERNAL_LINE_BISECTOR,
        first_ordinal < second_ordinal ? first_ordinal : second_ordinal,
        first_ordinal < second_ordinal ? second_ordinal : first_ordinal,
    };

    return line;
}

static odt_internal_vertex odt_vertex(odt_internal_line_ref first, odt_internal_line_ref second,
                                      odt_internal_line_ref incoming) {
    odt_internal_vertex vertex;

    if (odt_line_ref_compare(&first, &second) <= 0) {
        vertex.first_line = first;
        vertex.second_line = second;
    } else {
        vertex.first_line = second;
        vertex.second_line = first;
    }
    vertex.incoming_line = incoming;
    return vertex;
}

static int odt_vertex_compare(const odt_internal_vertex *first, const odt_internal_vertex *second) {
    int comparison = odt_line_ref_compare(&first->first_line, &second->first_line);

    if (comparison != 0) {
        return comparison;
    }
    comparison = odt_line_ref_compare(&first->second_line, &second->second_line);
    if (comparison != 0) {
        return comparison;
    }
    return odt_line_ref_compare(&first->incoming_line, &second->incoming_line);
}

static bool odt_line_applies_to_cell(const odt_internal_line_ref *line, size_t site_ordinal,
                                     size_t site_count) {
    if (line->kind >= ODT_INTERNAL_LINE_DOMAIN_MIN_X &&
        line->kind <= ODT_INTERNAL_LINE_DOMAIN_MAX_Y) {
        return line->first_ordinal == SIZE_MAX && line->second_ordinal == SIZE_MAX;
    }
    return line->kind == ODT_INTERNAL_LINE_BISECTOR && line->first_ordinal < line->second_ordinal &&
           line->second_ordinal < site_count &&
           (line->first_ordinal == site_ordinal || line->second_ordinal == site_ordinal);
}

static bool odt_sign_is_inside(const odt_internal_line_ref *line, size_t site_ordinal, int sign) {
    if (line->kind != ODT_INTERNAL_LINE_BISECTOR || site_ordinal == line->first_ordinal) {
        return sign <= 0;
    }
    return sign >= 0;
}

static odt_status odt_classify_vertex(odt_geometry_build_state *state,
                                      const odt_internal_numeric_context *numeric,
                                      const odt_internal_vertex *vertex,
                                      const odt_internal_line_ref *line, int *out_sign) {
    bool used_exact;
    odt_status status;

    status = odt_consume_work(state, 1u);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_internal_vertex_side(numeric, vertex, line, out_sign, &used_exact);
    if (status == ODT_OK && used_exact) {
        state->exact_fallbacks += 1u;
    }
    return status;
}

static odt_status odt_append_vertex(odt_geometry_build_state *state, odt_internal_vertex *vertices,
                                    size_t capacity, size_t *count, odt_internal_vertex vertex) {
    odt_status status = odt_consume_work(state, 1u);

    if (status != ODT_OK) {
        return status;
    }
    if (*count >= capacity) {
        return ODT_INTERNAL_ERROR;
    }
    vertices[*count] = vertex;
    *count += 1u;
    return ODT_OK;
}

static odt_status odt_intersection_vertex(const odt_internal_numeric_context *numeric,
                                          const odt_internal_line_ref *edge_line,
                                          const odt_internal_line_ref *clip_line,
                                          odt_internal_line_ref incoming_line,
                                          odt_internal_vertex *out_vertex) {
    bool parallel;
    odt_status status = odt_internal_lines_parallel(numeric, edge_line, clip_line, &parallel);

    if (status != ODT_OK) {
        return status;
    }
    if (parallel) {
        return ODT_INTERNAL_ERROR;
    }
    *out_vertex = odt_vertex(*edge_line, *clip_line, incoming_line);
    return ODT_OK;
}

static odt_status odt_clip_polygon(odt_geometry_build_state *state,
                                   const odt_internal_numeric_context *numeric, size_t site_ordinal,
                                   const odt_internal_line_ref *clip_line,
                                   const odt_internal_vertex *input, size_t input_count,
                                   odt_internal_vertex *output, size_t capacity,
                                   size_t *out_count) {
    size_t output_count = 0u;
    size_t index;
    int first_sign;
    int start_sign;
    odt_status status;

    status = odt_consume_work(state, 1u);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_classify_vertex(state, numeric, &input[0], clip_line, &first_sign);
    if (status != ODT_OK) {
        return status;
    }
    start_sign = first_sign;
    for (index = 0u; index < input_count; ++index) {
        const size_t end_index = (index + 1u) % input_count;
        const odt_internal_vertex *end = &input[end_index];
        const odt_internal_line_ref edge_line = end->incoming_line;
        int end_sign;
        bool start_inside;
        bool end_inside;

        if (end_index == 0u) {
            end_sign = first_sign;
        } else {
            status = odt_classify_vertex(state, numeric, end, clip_line, &end_sign);
            if (status != ODT_OK) {
                return status;
            }
        }
        start_inside = odt_sign_is_inside(clip_line, site_ordinal, start_sign);
        end_inside = odt_sign_is_inside(clip_line, site_ordinal, end_sign);

        if (start_inside && end_inside) {
            status = odt_append_vertex(state, output, capacity, &output_count, *end);
        } else if (start_inside) {
            if (start_sign == 0) {
                status = ODT_OK;
            } else {
                odt_internal_vertex intersection;

                status = odt_intersection_vertex(numeric, &edge_line, clip_line, edge_line,
                                                 &intersection);
                if (status == ODT_OK) {
                    status =
                        odt_append_vertex(state, output, capacity, &output_count, intersection);
                }
            }
        } else if (end_inside) {
            if (end_sign == 0) {
                odt_internal_vertex boundary_end = *end;

                boundary_end.incoming_line = *clip_line;
                status = odt_append_vertex(state, output, capacity, &output_count, boundary_end);
            } else {
                odt_internal_vertex intersection;

                status = odt_intersection_vertex(numeric, &edge_line, clip_line, *clip_line,
                                                 &intersection);
                if (status == ODT_OK) {
                    status =
                        odt_append_vertex(state, output, capacity, &output_count, intersection);
                }
                if (status == ODT_OK) {
                    status = odt_append_vertex(state, output, capacity, &output_count, *end);
                }
            }
        } else {
            status = ODT_OK;
        }
        if (status != ODT_OK) {
            return status;
        }
        start_sign = end_sign;
    }
    if (output_count < 3u) {
        return ODT_INTERNAL_ERROR;
    }
    *out_count = output_count;
    return ODT_OK;
}

static odt_status odt_vertex_satisfies_line(odt_geometry_build_state *state,
                                            const odt_internal_numeric_context *numeric,
                                            size_t site_ordinal, const odt_internal_vertex *vertex,
                                            const odt_internal_line_ref *line, bool *out_satisfies,
                                            int *out_sign) {
    int sign;
    odt_status status = odt_classify_vertex(state, numeric, vertex, line, &sign);

    if (status != ODT_OK) {
        return status;
    }
    *out_satisfies = odt_sign_is_inside(line, site_ordinal, sign);
    if (out_sign != NULL) {
        *out_sign = sign;
    }
    return ODT_OK;
}

static odt_status odt_certify_cell(odt_geometry_build_state *state,
                                   const odt_internal_numeric_context *numeric, size_t site_ordinal,
                                   const odt_internal_vertex *vertices, size_t vertex_count) {
    static const odt_internal_line_kind domain_kinds[] = {
        ODT_INTERNAL_LINE_DOMAIN_MIN_X,
        ODT_INTERNAL_LINE_DOMAIN_MAX_X,
        ODT_INTERNAL_LINE_DOMAIN_MIN_Y,
        ODT_INTERNAL_LINE_DOMAIN_MAX_Y,
    };
    const odt_internal_line_ref first_edge = vertices[0].incoming_line;
    bool positive = false;
    size_t vertex_index;

    if (vertex_count < 3u) {
        return ODT_INTERNAL_ERROR;
    }
    for (vertex_index = 0u; vertex_index < vertex_count; ++vertex_index) {
        const odt_internal_vertex *vertex = &vertices[vertex_index];
        const odt_internal_vertex *previous =
            &vertices[(vertex_index + vertex_count - 1u) % vertex_count];
        const odt_internal_line_ref edge = vertex->incoming_line;
        bool parallel;
        bool satisfies;
        int sign;
        size_t constraint_index;
        odt_status status;

        if (!odt_line_applies_to_cell(&edge, site_ordinal, numeric->site_count)) {
            return ODT_INTERNAL_ERROR;
        }
        status = odt_internal_lines_parallel(numeric, &vertex->first_line, &vertex->second_line,
                                             &parallel);
        if (status != ODT_OK || parallel) {
            return ODT_INTERNAL_ERROR;
        }
        status = odt_vertex_satisfies_line(state, numeric, site_ordinal, previous, &edge,
                                           &satisfies, &sign);
        if (status != ODT_OK || sign != 0) {
            return status == ODT_OK ? ODT_INTERNAL_ERROR : status;
        }
        status = odt_vertex_satisfies_line(state, numeric, site_ordinal, vertex, &edge, &satisfies,
                                           &sign);
        if (status != ODT_OK || sign != 0) {
            return status == ODT_OK ? ODT_INTERNAL_ERROR : status;
        }

        for (constraint_index = 0u;
             constraint_index < sizeof(domain_kinds) / sizeof(domain_kinds[0]);
             ++constraint_index) {
            odt_internal_line_ref constraint = odt_domain_line(domain_kinds[constraint_index]);

            status = odt_vertex_satisfies_line(state, numeric, site_ordinal, vertex, &constraint,
                                               &satisfies, NULL);
            if (status != ODT_OK || !satisfies) {
                return status == ODT_OK ? ODT_INTERNAL_ERROR : status;
            }
        }
        for (constraint_index = 0u; constraint_index < numeric->site_count; ++constraint_index) {
            odt_internal_line_ref constraint;

            if (constraint_index == site_ordinal) {
                continue;
            }
            constraint = odt_bisector_line(site_ordinal, constraint_index);
            status = odt_vertex_satisfies_line(state, numeric, site_ordinal, vertex, &constraint,
                                               &satisfies, NULL);
            if (status != ODT_OK || !satisfies) {
                return status == ODT_OK ? ODT_INTERNAL_ERROR : status;
            }
        }
        status = odt_vertex_satisfies_line(state, numeric, site_ordinal, vertex, &first_edge,
                                           &satisfies, &sign);
        if (status != ODT_OK || !satisfies) {
            return status == ODT_OK ? ODT_INTERNAL_ERROR : status;
        }
        if (sign != 0) {
            positive = true;
        }
    }
    return positive ? ODT_OK : ODT_INTERNAL_ERROR;
}

static void odt_initialize_rectangle(odt_internal_vertex *vertices) {
    const odt_internal_line_ref min_x = odt_domain_line(ODT_INTERNAL_LINE_DOMAIN_MIN_X);
    const odt_internal_line_ref max_x = odt_domain_line(ODT_INTERNAL_LINE_DOMAIN_MAX_X);
    const odt_internal_line_ref min_y = odt_domain_line(ODT_INTERNAL_LINE_DOMAIN_MIN_Y);
    const odt_internal_line_ref max_y = odt_domain_line(ODT_INTERNAL_LINE_DOMAIN_MAX_Y);

    vertices[0] = odt_vertex(min_x, min_y, min_x);
    vertices[1] = odt_vertex(max_x, min_y, min_y);
    vertices[2] = odt_vertex(max_x, max_y, max_x);
    vertices[3] = odt_vertex(min_x, max_y, max_y);
}

static size_t odt_canonical_vertex_start(const odt_internal_vertex *vertices, size_t vertex_count) {
    size_t best = 0u;
    size_t index;

    for (index = 1u; index < vertex_count; ++index) {
        if (odt_vertex_compare(&vertices[index], &vertices[best]) < 0) {
            best = index;
        }
    }
    return best;
}

static void odt_destroy_partial_geometry(odt_geometry_build_state *state,
                                         odt_internal_geometry *geometry) {
    const size_t vertex_alignment = odt_allocation_alignment(_Alignof(odt_internal_vertex));
    size_t index;

    if (geometry == NULL) {
        return;
    }
    if (geometry->cells != NULL) {
        for (index = 0u; index < geometry->site_count; ++index) {
            if (geometry->cells[index].vertices != NULL) {
                odt_build_deallocate(state, geometry->cells[index].vertices,
                                     geometry->cells[index].allocation_size, vertex_alignment);
            }
        }
        odt_build_deallocate(state, geometry->cells, geometry->cells_allocation_size,
                             odt_allocation_alignment(_Alignof(odt_internal_cell)));
    }
    if (geometry->sites != NULL) {
        odt_build_deallocate(state, geometry->sites, geometry->sites_allocation_size,
                             odt_allocation_alignment(_Alignof(odt_site)));
    }
    odt_build_deallocate(state, geometry, sizeof(*geometry),
                         odt_allocation_alignment(_Alignof(odt_internal_geometry)));
}

static odt_status odt_validate_geometry_input(const odt_domain *domain, const odt_site *sites,
                                              size_t site_count, const odt_limits *limits,
                                              odt_geometry_build_state *state) {
    size_t first;

    if (domain == NULL || (sites == NULL && site_count != 0u)) {
        return ODT_INVALID_ARGUMENT;
    }
    if (!isfinite(domain->min_x) || !isfinite(domain->min_y) || !isfinite(domain->max_x) ||
        !isfinite(domain->max_y) || !(domain->min_x < domain->max_x) ||
        !(domain->min_y < domain->max_y) || site_count == 0u) {
        return ODT_INVALID_DATA;
    }
    if (site_count > limits->max_sites || site_count > limits->max_polygon_fragments) {
        return ODT_LIMIT_EXCEEDED;
    }
    for (first = 0u; first < site_count; ++first) {
        size_t second;
        odt_status status = odt_consume_work(state, 1u);

        if (status != ODT_OK) {
            return status;
        }
        if (!isfinite(sites[first].x) || !isfinite(sites[first].y) ||
            sites[first].x < domain->min_x || sites[first].x > domain->max_x ||
            sites[first].y < domain->min_y || sites[first].y > domain->max_y) {
            return ODT_INVALID_DATA;
        }
        for (second = 0u; second < first; ++second) {
            status = odt_consume_work(state, 1u);
            if (status != ODT_OK) {
                return status;
            }
            if ((sites[first].x == sites[second].x && sites[first].y == sites[second].y) ||
                sites[first].region_id == sites[second].region_id) {
                return ODT_INVALID_DATA;
            }
        }
    }
    return ODT_OK;
}

odt_status odt_internal_geometry_build(const odt_domain *domain, const odt_site *sites,
                                       size_t site_count, const odt_limits *limits,
                                       const odt_allocator *allocator,
                                       odt_internal_geometry **out_geometry) {
    odt_limits default_limits;
    odt_allocator default_allocator;
    const odt_limits *effective_limits = limits;
    const odt_allocator *effective_allocator = allocator;
    odt_geometry_build_state state;
    odt_internal_geometry *geometry = NULL;
    odt_internal_vertex *first_buffer = NULL;
    odt_internal_vertex *second_buffer = NULL;
    size_t vertex_capacity;
    size_t vertex_buffer_size;
    size_t cell_index;
    odt_status status;

    if (out_geometry == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_geometry = NULL;
    status = odt_internal_numeric_environment_check();
    if (status != ODT_OK) {
        return status;
    }
    if (effective_limits == NULL) {
        status = odt_limits_init(&default_limits);
        if (status != ODT_OK) {
            return status;
        }
        effective_limits = &default_limits;
    }
    status = odt_internal_limits_validate(effective_limits);
    if (status != ODT_OK) {
        return status;
    }
    if (effective_allocator == NULL) {
        status = odt_allocator_init(&default_allocator);
        if (status != ODT_OK) {
            return status;
        }
        effective_allocator = &default_allocator;
    }
    status = odt_internal_allocator_validate(effective_allocator);
    if (status != ODT_OK) {
        return status;
    }
    memset(&state, 0, sizeof(state));
    state.memory.allocator = *effective_allocator;
    state.memory.limit = effective_limits->max_build_bytes;
    state.max_work = effective_limits->max_build_work;
    status = odt_validate_geometry_input(domain, sites, site_count, effective_limits, &state);
    if (status != ODT_OK) {
        return status;
    }
    if (!odt_checked_add_size(site_count, 4u, &vertex_capacity) ||
        !odt_checked_multiply_size(vertex_capacity, sizeof(odt_internal_vertex),
                                   &vertex_buffer_size)) {
        return ODT_LIMIT_EXCEEDED;
    }

    geometry =
        odt_build_allocate(&state, sizeof(*geometry),
                           odt_allocation_alignment(_Alignof(odt_internal_geometry)), &status);
    if (status != ODT_OK) {
        return status;
    }
    memset(geometry, 0, sizeof(*geometry));
    geometry->allocator = *effective_allocator;
    geometry->domain = *domain;
    geometry->site_count = site_count;
    if (!odt_checked_multiply_size(site_count, sizeof(*geometry->sites),
                                   &geometry->sites_allocation_size) ||
        !odt_checked_multiply_size(site_count, sizeof(*geometry->cells),
                                   &geometry->cells_allocation_size)) {
        status = ODT_LIMIT_EXCEEDED;
        goto fail;
    }
    geometry->sites = odt_build_allocate(&state, geometry->sites_allocation_size,
                                         odt_allocation_alignment(_Alignof(odt_site)), &status);
    if (status != ODT_OK) {
        goto fail;
    }
    memcpy(geometry->sites, sites, geometry->sites_allocation_size);
    geometry->cells =
        odt_build_allocate(&state, geometry->cells_allocation_size,
                           odt_allocation_alignment(_Alignof(odt_internal_cell)), &status);
    if (status != ODT_OK) {
        goto fail;
    }
    memset(geometry->cells, 0, geometry->cells_allocation_size);
    status = odt_internal_numeric_context_init(&geometry->domain, geometry->sites, site_count,
                                               &geometry->numeric);
    if (status != ODT_OK) {
        goto fail;
    }
    first_buffer =
        odt_build_allocate(&state, vertex_buffer_size,
                           odt_allocation_alignment(_Alignof(odt_internal_vertex)), &status);
    if (status != ODT_OK) {
        goto fail;
    }
    second_buffer =
        odt_build_allocate(&state, vertex_buffer_size,
                           odt_allocation_alignment(_Alignof(odt_internal_vertex)), &status);
    if (status != ODT_OK) {
        goto fail;
    }

    for (cell_index = 0u; cell_index < site_count; ++cell_index) {
        odt_internal_vertex *current = first_buffer;
        odt_internal_vertex *next = second_buffer;
        size_t current_count = 4u;
        size_t other_index;
        size_t canonical_start;
        size_t vertex_index;
        odt_internal_cell *cell = &geometry->cells[cell_index];

        odt_initialize_rectangle(current);
        for (other_index = 0u; other_index < site_count; ++other_index) {
            size_t next_count;
            odt_internal_vertex *swap;
            odt_internal_line_ref clip_line;

            if (other_index == cell_index) {
                continue;
            }
            clip_line = odt_bisector_line(cell_index, other_index);
            status = odt_clip_polygon(&state, &geometry->numeric, cell_index, &clip_line, current,
                                      current_count, next, vertex_capacity, &next_count);
            if (status != ODT_OK) {
                goto fail;
            }
            swap = current;
            current = next;
            next = swap;
            current_count = next_count;
        }
        status = odt_certify_cell(&state, &geometry->numeric, cell_index, current, current_count);
        if (status != ODT_OK) {
            goto fail;
        }
        if (!odt_checked_multiply_size(current_count, sizeof(*cell->vertices),
                                       &cell->allocation_size)) {
            status = ODT_LIMIT_EXCEEDED;
            goto fail;
        }
        cell->vertices =
            odt_build_allocate(&state, cell->allocation_size,
                               odt_allocation_alignment(_Alignof(odt_internal_vertex)), &status);
        if (status != ODT_OK) {
            goto fail;
        }
        canonical_start = odt_canonical_vertex_start(current, current_count);
        for (vertex_index = 0u; vertex_index < current_count; ++vertex_index) {
            cell->vertices[vertex_index] =
                current[(canonical_start + vertex_index) % current_count];
        }
        cell->site_ordinal = cell_index;
        cell->region_id = geometry->sites[cell_index].region_id;
        cell->vertex_count = current_count;
        cell->certified = true;
        if (geometry->vertex_count > UINT64_MAX - (uint64_t)current_count) {
            status = ODT_LIMIT_EXCEEDED;
            goto fail;
        }
        geometry->vertex_count += (uint64_t)current_count;
    }

    odt_build_deallocate(&state, second_buffer, vertex_buffer_size,
                         odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    second_buffer = NULL;
    odt_build_deallocate(&state, first_buffer, vertex_buffer_size,
                         odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    first_buffer = NULL;
    geometry->build_work = state.work;
    geometry->exact_fallbacks = state.exact_fallbacks;
    geometry->live_build_bytes = state.memory.live;
    geometry->peak_build_bytes = state.memory.peak;
    *out_geometry = geometry;
    return ODT_OK;

fail:
    odt_build_deallocate(&state, second_buffer, vertex_buffer_size,
                         odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    odt_build_deallocate(&state, first_buffer, vertex_buffer_size,
                         odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    odt_destroy_partial_geometry(&state, geometry);
    return status;
}

void odt_internal_geometry_destroy(odt_internal_geometry *geometry) {
    const size_t vertex_alignment = odt_allocation_alignment(_Alignof(odt_internal_vertex));
    odt_allocator allocator;
    size_t index;

    if (geometry == NULL) {
        return;
    }
    allocator = geometry->allocator;
    for (index = 0u; index < geometry->site_count; ++index) {
        if (geometry->cells[index].vertices != NULL) {
            allocator.deallocate(allocator.context, geometry->cells[index].vertices,
                                 geometry->cells[index].allocation_size, vertex_alignment);
        }
    }
    allocator.deallocate(allocator.context, geometry->cells, geometry->cells_allocation_size,
                         odt_allocation_alignment(_Alignof(odt_internal_cell)));
    allocator.deallocate(allocator.context, geometry->sites, geometry->sites_allocation_size,
                         odt_allocation_alignment(_Alignof(odt_site)));
    allocator.deallocate(allocator.context, geometry, sizeof(*geometry),
                         odt_allocation_alignment(_Alignof(odt_internal_geometry)));
}

static odt_status odt_point_line_side(const odt_internal_geometry *geometry, const odt_point *point,
                                      const odt_internal_line_ref *line, int *out_sign) {
    switch (line->kind) {
    case ODT_INTERNAL_LINE_DOMAIN_MIN_X:
        *out_sign =
            point->x < geometry->domain.min_x ? 1 : (point->x > geometry->domain.min_x ? -1 : 0);
        return ODT_OK;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_X:
        *out_sign =
            point->x < geometry->domain.max_x ? -1 : (point->x > geometry->domain.max_x ? 1 : 0);
        return ODT_OK;
    case ODT_INTERNAL_LINE_DOMAIN_MIN_Y:
        *out_sign =
            point->y < geometry->domain.min_y ? 1 : (point->y > geometry->domain.min_y ? -1 : 0);
        return ODT_OK;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_Y:
        *out_sign =
            point->y < geometry->domain.max_y ? -1 : (point->y > geometry->domain.max_y ? 1 : 0);
        return ODT_OK;
    case ODT_INTERNAL_LINE_BISECTOR:
        if (line->first_ordinal >= line->second_ordinal ||
            line->second_ordinal >= geometry->site_count) {
            return ODT_INTERNAL_ERROR;
        }
        return odt_internal_compare_squared_distance_values(
            point, &geometry->sites[line->first_ordinal], &geometry->sites[line->second_ordinal],
            out_sign);
    }
    return ODT_INTERNAL_ERROR;
}

odt_status odt_internal_geometry_cell_contains_point(const odt_internal_geometry *geometry,
                                                     size_t cell_index, const odt_point *point,
                                                     bool *out_contains) {
    const odt_internal_cell *cell;
    size_t edge_index;

    if (geometry == NULL || point == NULL || out_contains == NULL ||
        cell_index >= geometry->site_count) {
        return ODT_INVALID_ARGUMENT;
    }
    if (odt_internal_numeric_environment_check() != ODT_OK) {
        return ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT;
    }
    if (!isfinite(point->x) || !isfinite(point->y)) {
        return ODT_INVALID_DATA;
    }
    if (point->x < geometry->domain.min_x || point->x > geometry->domain.max_x ||
        point->y < geometry->domain.min_y || point->y > geometry->domain.max_y) {
        *out_contains = false;
        return ODT_OK;
    }
    cell = &geometry->cells[cell_index];
    for (edge_index = 0u; edge_index < cell->vertex_count; ++edge_index) {
        const odt_internal_line_ref *line = &cell->vertices[edge_index].incoming_line;
        int sign;
        odt_status status = odt_point_line_side(geometry, point, line, &sign);

        if (status != ODT_OK) {
            return status;
        }
        if (!odt_sign_is_inside(line, cell->site_ordinal, sign)) {
            *out_contains = false;
            return ODT_OK;
        }
    }
    *out_contains = true;
    return ODT_OK;
}

odt_status odt_internal_geometry_locate(const odt_internal_geometry *geometry,
                                        const odt_point *point, bool *out_found,
                                        size_t *out_site_ordinal) {
    bool found = false;
    size_t winner = 0u;
    size_t cell_index;

    if (geometry == NULL || point == NULL || out_found == NULL || out_site_ordinal == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    for (cell_index = 0u; cell_index < geometry->site_count; ++cell_index) {
        bool contains;
        odt_status status =
            odt_internal_geometry_cell_contains_point(geometry, cell_index, point, &contains);

        if (status != ODT_OK) {
            return status;
        }
        if (contains && (!found || cell_index < winner)) {
            found = true;
            winner = cell_index;
        }
    }
    *out_found = found;
    *out_site_ordinal = winner;
    return ODT_OK;
}
