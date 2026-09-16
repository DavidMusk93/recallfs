#include "odt_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ODT_GENERATION_ALIGNMENT = 64,
};

typedef struct odt_builder_memory {
    odt_allocator allocator;
    size_t limit;
    size_t live;
    size_t peak;
} odt_builder_memory;

typedef struct odt_builder_fragment {
    size_t site_ordinal;
    size_t vertex_count;
    size_t allocation_size;
    odt_internal_vertex *vertices;
} odt_builder_fragment;

typedef struct odt_builder_fragment_set {
    size_t count;
    size_t allocation_size;
    odt_builder_fragment *items;
} odt_builder_fragment_set;

typedef struct odt_builder_task {
    odt_builder_fragment_set fragments;
    size_t depth;
    size_t parent_node;
    bool second_branch;
    bool root;
} odt_builder_task;

typedef struct odt_candidate_score {
    odt_internal_line_ref line;
    size_t first_region_count;
    size_t second_region_count;
    size_t first_fragment_count;
    size_t second_fragment_count;
    size_t new_fragment_count;
    size_t total_child_fragment_count;
} odt_candidate_score;

typedef struct odt_verify_stack_entry {
    int32_t reference;
    size_t parent_node;
    uint32_t depth;
    bool second_branch;
    bool root;
} odt_verify_stack_entry;

typedef struct odt_verify_path_step {
    size_t node_index;
    bool second_branch;
} odt_verify_path_step;

typedef struct odt_builder {
    odt_builder_memory memory;
    const odt_limits *limits;
    const odt_internal_geometry *geometry;
    odt_internal_line_ref *candidates;
    size_t candidate_count;
    size_t candidates_allocation_size;
    size_t *first_marks;
    size_t *second_marks;
    size_t *region_marks;
    size_t marks_allocation_size;
    size_t mark_epoch;
    odt_internal_runtime_node *nodes;
    size_t node_count;
    size_t node_capacity;
    size_t nodes_allocation_size;
    odt_internal_runtime_leaf *leaves;
    size_t leaf_count;
    size_t leaf_capacity;
    size_t leaves_allocation_size;
    odt_builder_task *tasks;
    size_t task_count;
    size_t task_capacity;
    size_t tasks_allocation_size;
    uint64_t work;
    uint64_t fragment_count;
    uint32_t maximum_depth;
    int32_t root_reference;
} odt_builder;

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

static bool odt_is_power_of_two(size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static size_t odt_allocation_alignment(size_t natural_alignment) {
    return natural_alignment < sizeof(void *) ? sizeof(void *) : natural_alignment;
}

static bool odt_align_up(size_t value, size_t alignment, size_t *out_value) {
    const size_t remainder = value & (alignment - 1u);

    if (!odt_is_power_of_two(alignment)) {
        return false;
    }
    if (remainder == 0u) {
        *out_value = value;
        return true;
    }
    return odt_checked_add_size(value, alignment - remainder, out_value);
}

static bool odt_allocation_charge(size_t size, size_t alignment, size_t *out_charge) {
    return size != 0u && odt_align_up(size, alignment, out_charge);
}

static void *odt_builder_allocate(odt_builder *builder, size_t size, size_t alignment,
                                  odt_status *out_status) {
    size_t charge;
    void *pointer;

    if (!odt_allocation_charge(size, alignment, &charge)) {
        *out_status = ODT_LIMIT_EXCEEDED;
        return NULL;
    }
    if (builder->memory.live > builder->memory.limit ||
        charge > builder->memory.limit - builder->memory.live) {
        *out_status = ODT_LIMIT_EXCEEDED;
        return NULL;
    }
    pointer =
        builder->memory.allocator.allocate(builder->memory.allocator.context, size, alignment);
    if (pointer == NULL) {
        *out_status = ODT_OUT_OF_MEMORY;
        return NULL;
    }
    builder->memory.live += charge;
    if (builder->memory.live > builder->memory.peak) {
        builder->memory.peak = builder->memory.live;
    }
    *out_status = ODT_OK;
    return pointer;
}

static void odt_builder_deallocate(odt_builder *builder, void *pointer, size_t size,
                                   size_t alignment) {
    size_t charge;

    if (pointer == NULL) {
        return;
    }
    if (!odt_allocation_charge(size, alignment, &charge) || charge > builder->memory.live) {
        return;
    }
    builder->memory.live -= charge;
    builder->memory.allocator.deallocate(builder->memory.allocator.context, pointer, size,
                                         alignment);
}

static odt_status odt_builder_consume_work(odt_builder *builder, uint64_t amount) {
    if (builder->work > builder->limits->max_build_work ||
        amount > builder->limits->max_build_work - builder->work) {
        return ODT_LIMIT_EXCEEDED;
    }
    builder->work += amount;
    return ODT_OK;
}

static odt_status odt_builder_emit_fragment(odt_builder *builder) {
    odt_status status = odt_builder_consume_work(builder, 1u);

    if (status != ODT_OK) {
        return status;
    }
    if (builder->fragment_count >= (uint64_t)builder->limits->max_polygon_fragments) {
        return ODT_LIMIT_EXCEEDED;
    }
    builder->fragment_count += 1u;
    return ODT_OK;
}

static int odt_line_compare(const odt_internal_line_ref *first,
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

static int odt_line_qsort_compare(const void *first, const void *second) {
    return odt_line_compare((const odt_internal_line_ref *)first,
                            (const odt_internal_line_ref *)second);
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

    if (odt_line_compare(&first, &second) <= 0) {
        vertex.first_line = first;
        vertex.second_line = second;
    } else {
        vertex.first_line = second;
        vertex.second_line = first;
    }
    vertex.incoming_line = incoming;
    return vertex;
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

static odt_status odt_classify_fragment(odt_builder *builder, const odt_builder_fragment *fragment,
                                        const odt_internal_line_ref *line, bool *out_first,
                                        bool *out_second) {
    bool first = false;
    bool second = false;
    size_t vertex_index;

    for (vertex_index = 0u; vertex_index < fragment->vertex_count; ++vertex_index) {
        bool used_exact;
        int sign;
        odt_status status = odt_builder_consume_work(builder, 1u);

        if (status != ODT_OK) {
            return status;
        }
        status =
            odt_internal_vertex_side(&builder->geometry->numeric, &fragment->vertices[vertex_index],
                                     line, &sign, &used_exact);
        if (status != ODT_OK) {
            return status;
        }
        (void)used_exact;
        first = first || sign < 0;
        second = second || sign > 0;
    }
    if (!first && !second) {
        return ODT_INTERNAL_ERROR;
    }
    *out_first = first;
    *out_second = second;
    return ODT_OK;
}

static bool odt_side_inside(int sign, bool keep_second) {
    return keep_second ? sign >= 0 : sign <= 0;
}

static odt_status odt_append_vertex(odt_internal_vertex *vertices, size_t capacity, size_t *count,
                                    odt_internal_vertex vertex) {
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

static odt_status odt_clip_vertices(odt_builder *builder, const odt_internal_vertex *input,
                                    size_t input_count, const odt_internal_line_ref *line,
                                    bool keep_second, odt_internal_vertex *output, size_t capacity,
                                    size_t *out_count) {
    size_t output_count = 0u;
    size_t index;
    int first_sign;
    int start_sign;
    bool used_exact;
    odt_status status = odt_builder_consume_work(builder, 1u);

    if (status != ODT_OK) {
        return status;
    }
    status = odt_builder_consume_work(builder, 1u);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_internal_vertex_side(&builder->geometry->numeric, &input[0], line, &first_sign,
                                      &used_exact);
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
            status = odt_builder_consume_work(builder, 1u);
            if (status != ODT_OK) {
                return status;
            }
            status = odt_internal_vertex_side(&builder->geometry->numeric, end, line, &end_sign,
                                              &used_exact);
            if (status != ODT_OK) {
                return status;
            }
        }
        start_inside = odt_side_inside(start_sign, keep_second);
        end_inside = odt_side_inside(end_sign, keep_second);
        if (start_inside && end_inside) {
            status = odt_append_vertex(output, capacity, &output_count, *end);
        } else if (start_inside) {
            if (start_sign == 0) {
                status = ODT_OK;
            } else {
                odt_internal_vertex intersection;

                status = odt_intersection_vertex(&builder->geometry->numeric, &edge_line, line,
                                                 edge_line, &intersection);
                if (status == ODT_OK) {
                    status = odt_append_vertex(output, capacity, &output_count, intersection);
                }
            }
        } else if (end_inside) {
            if (end_sign == 0) {
                odt_internal_vertex boundary_end = *end;

                boundary_end.incoming_line = *line;
                status = odt_append_vertex(output, capacity, &output_count, boundary_end);
            } else {
                odt_internal_vertex intersection;

                status = odt_intersection_vertex(&builder->geometry->numeric, &edge_line, line,
                                                 *line, &intersection);
                if (status == ODT_OK) {
                    status = odt_append_vertex(output, capacity, &output_count, intersection);
                }
                if (status == ODT_OK) {
                    status = odt_append_vertex(output, capacity, &output_count, *end);
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

static void odt_fragment_destroy(odt_builder *builder, odt_builder_fragment *fragment) {
    if (fragment->vertices != NULL) {
        odt_builder_deallocate(builder, fragment->vertices, fragment->allocation_size,
                               odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    }
    memset(fragment, 0, sizeof(*fragment));
}

static void odt_fragment_set_destroy(odt_builder *builder, odt_builder_fragment_set *set) {
    size_t index;

    if (set->items == NULL) {
        return;
    }
    for (index = 0u; index < set->count; ++index) {
        odt_fragment_destroy(builder, &set->items[index]);
    }
    odt_builder_deallocate(builder, set->items, set->allocation_size,
                           odt_allocation_alignment(_Alignof(odt_builder_fragment)));
    memset(set, 0, sizeof(*set));
}

static odt_status odt_fragment_set_allocate(odt_builder *builder, size_t count,
                                            odt_builder_fragment_set *out_set) {
    odt_status status;

    memset(out_set, 0, sizeof(*out_set));
    if (count == 0u ||
        !odt_checked_multiply_size(count, sizeof(*out_set->items), &out_set->allocation_size)) {
        return ODT_INTERNAL_ERROR;
    }
    out_set->items =
        odt_builder_allocate(builder, out_set->allocation_size,
                             odt_allocation_alignment(_Alignof(odt_builder_fragment)), &status);
    if (status != ODT_OK) {
        return status;
    }
    memset(out_set->items, 0, out_set->allocation_size);
    out_set->count = count;
    return ODT_OK;
}

static odt_status odt_fragment_copy(odt_builder *builder, size_t site_ordinal,
                                    const odt_internal_vertex *vertices, size_t vertex_count,
                                    odt_builder_fragment *out_fragment) {
    odt_status status;

    memset(out_fragment, 0, sizeof(*out_fragment));
    if (vertex_count < 3u ||
        !odt_checked_multiply_size(vertex_count, sizeof(*out_fragment->vertices),
                                   &out_fragment->allocation_size)) {
        return ODT_INTERNAL_ERROR;
    }
    out_fragment->vertices =
        odt_builder_allocate(builder, out_fragment->allocation_size,
                             odt_allocation_alignment(_Alignof(odt_internal_vertex)), &status);
    if (status != ODT_OK) {
        return status;
    }
    memcpy(out_fragment->vertices, vertices, out_fragment->allocation_size);
    out_fragment->site_ordinal = site_ordinal;
    out_fragment->vertex_count = vertex_count;
    return ODT_OK;
}

static odt_status odt_fragment_clip(odt_builder *builder, const odt_builder_fragment *fragment,
                                    const odt_internal_line_ref *line, bool keep_second,
                                    odt_builder_fragment *out_fragment) {
    size_t capacity;
    size_t allocation_size;
    size_t output_count;
    odt_internal_vertex *vertices;
    odt_status status;

    memset(out_fragment, 0, sizeof(*out_fragment));
    if (!odt_checked_add_size(fragment->vertex_count, 1u, &capacity) ||
        !odt_checked_multiply_size(capacity, sizeof(*vertices), &allocation_size)) {
        return ODT_LIMIT_EXCEEDED;
    }
    vertices = odt_builder_allocate(
        builder, allocation_size, odt_allocation_alignment(_Alignof(odt_internal_vertex)), &status);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_clip_vertices(builder, fragment->vertices, fragment->vertex_count, line,
                               keep_second, vertices, capacity, &output_count);
    if (status != ODT_OK) {
        odt_builder_deallocate(builder, vertices, allocation_size,
                               odt_allocation_alignment(_Alignof(odt_internal_vertex)));
        return status;
    }
    out_fragment->site_ordinal = fragment->site_ordinal;
    out_fragment->vertex_count = output_count;
    out_fragment->allocation_size = allocation_size;
    out_fragment->vertices = vertices;
    return ODT_OK;
}

static odt_status odt_builder_initial_fragments(odt_builder *builder,
                                                odt_builder_fragment_set *out_set) {
    size_t index;
    odt_status status = odt_fragment_set_allocate(builder, builder->geometry->site_count, out_set);

    if (status != ODT_OK) {
        return status;
    }
    for (index = 0u; index < builder->geometry->site_count; ++index) {
        const odt_internal_cell *cell = &builder->geometry->cells[index];

        if (!cell->certified || cell->site_ordinal != index) {
            status = ODT_INTERNAL_ERROR;
            break;
        }
        status = odt_fragment_copy(builder, index, cell->vertices, cell->vertex_count,
                                   &out_set->items[index]);
        if (status != ODT_OK) {
            break;
        }
    }
    if (status != ODT_OK) {
        odt_fragment_set_destroy(builder, out_set);
    }
    return status;
}

static odt_status odt_builder_extract_candidates(odt_builder *builder) {
    size_t capacity;
    size_t index;
    size_t candidate_count = 0u;
    odt_status status;

    if (builder->geometry->vertex_count > SIZE_MAX) {
        return ODT_LIMIT_EXCEEDED;
    }
    capacity = (size_t)builder->geometry->vertex_count;
    if (capacity == 0u || !odt_checked_multiply_size(capacity, sizeof(*builder->candidates),
                                                     &builder->candidates_allocation_size)) {
        return ODT_INTERNAL_ERROR;
    }
    builder->candidates =
        odt_builder_allocate(builder, builder->candidates_allocation_size,
                             odt_allocation_alignment(_Alignof(odt_internal_line_ref)), &status);
    if (status != ODT_OK) {
        return status;
    }
    for (index = 0u; index < builder->geometry->site_count; ++index) {
        const odt_internal_cell *cell = &builder->geometry->cells[index];
        size_t edge_index;

        for (edge_index = 0u; edge_index < cell->vertex_count; ++edge_index) {
            const odt_internal_line_ref line = cell->vertices[edge_index].incoming_line;

            if (line.kind == ODT_INTERNAL_LINE_BISECTOR) {
                if (candidate_count >= capacity) {
                    return ODT_INTERNAL_ERROR;
                }
                builder->candidates[candidate_count] = line;
                candidate_count += 1u;
            }
        }
    }
    if (candidate_count == 0u && builder->geometry->site_count != 1u) {
        return ODT_INTERNAL_ERROR;
    }
    qsort(builder->candidates, candidate_count, sizeof(*builder->candidates),
          odt_line_qsort_compare);
    builder->candidate_count = 0u;
    for (index = 0u; index < candidate_count; ++index) {
        const odt_internal_line_ref line = builder->candidates[index];

        if (builder->candidate_count == 0u ||
            odt_line_compare(&line, &builder->candidates[builder->candidate_count - 1u]) != 0) {
            const odt_point first_point = {
                builder->geometry->sites[line.first_ordinal].x,
                builder->geometry->sites[line.first_ordinal].y,
            };
            const odt_point second_point = {
                builder->geometry->sites[line.second_ordinal].x,
                builder->geometry->sites[line.second_ordinal].y,
            };
            int first_sign;
            int second_sign;

            status = odt_internal_compare_squared_distance_values(
                &first_point, &builder->geometry->sites[line.first_ordinal],
                &builder->geometry->sites[line.second_ordinal], &first_sign);
            if (status != ODT_OK) {
                return status;
            }
            status = odt_internal_compare_squared_distance_values(
                &second_point, &builder->geometry->sites[line.first_ordinal],
                &builder->geometry->sites[line.second_ordinal], &second_sign);
            if (status != ODT_OK) {
                return status;
            }
            if (first_sign >= 0 || second_sign <= 0) {
                return ODT_INTERNAL_ERROR;
            }
            builder->candidates[builder->candidate_count] = line;
            builder->candidate_count += 1u;
        }
    }
    return ODT_OK;
}

static odt_status odt_builder_allocate_marks(odt_builder *builder) {
    size_t one_size;
    size_t all_size;
    odt_status status;

    if (!odt_checked_multiply_size(builder->geometry->site_count, sizeof(*builder->first_marks),
                                   &one_size) ||
        !odt_checked_multiply_size(one_size, 3u, &all_size)) {
        return ODT_LIMIT_EXCEEDED;
    }
    builder->first_marks = odt_builder_allocate(
        builder, all_size, odt_allocation_alignment(_Alignof(size_t)), &status);
    if (status != ODT_OK) {
        return status;
    }
    memset(builder->first_marks, 0, all_size);
    builder->second_marks = builder->first_marks + builder->geometry->site_count;
    builder->region_marks = builder->second_marks + builder->geometry->site_count;
    builder->marks_allocation_size = all_size;
    builder->mark_epoch = 1u;
    return ODT_OK;
}

static size_t odt_builder_next_epoch(odt_builder *builder) {
    builder->mark_epoch += 1u;
    if (builder->mark_epoch == 0u) {
        memset(builder->first_marks, 0, builder->marks_allocation_size);
        builder->mark_epoch = 1u;
    }
    return builder->mark_epoch;
}

static bool odt_score_better(const odt_candidate_score *candidate,
                             const odt_candidate_score *best) {
    const size_t candidate_largest = candidate->first_region_count > candidate->second_region_count
                                         ? candidate->first_region_count
                                         : candidate->second_region_count;
    const size_t best_largest = best->first_region_count > best->second_region_count
                                    ? best->first_region_count
                                    : best->second_region_count;
    if (candidate_largest != best_largest) {
        return candidate_largest < best_largest;
    }
    if (candidate->new_fragment_count != best->new_fragment_count) {
        return candidate->new_fragment_count < best->new_fragment_count;
    }
    if (candidate->total_child_fragment_count != best->total_child_fragment_count) {
        return candidate->total_child_fragment_count < best->total_child_fragment_count;
    }
    return odt_line_compare(&candidate->line, &best->line) < 0;
}

static odt_status odt_evaluate_candidate(odt_builder *builder,
                                         const odt_builder_fragment_set *fragments,
                                         const odt_internal_line_ref *line,
                                         odt_candidate_score *out_score, bool *out_admissible) {
    const size_t epoch = odt_builder_next_epoch(builder);
    bool has_first_site = false;
    bool has_second_site = false;
    size_t index;
    odt_status status = odt_builder_consume_work(builder, 1u);

    if (status != ODT_OK) {
        return status;
    }
    memset(out_score, 0, sizeof(*out_score));
    out_score->line = *line;
    for (index = 0u; index < fragments->count; ++index) {
        const odt_builder_fragment *fragment = &fragments->items[index];
        bool first;
        bool second;

        has_first_site = has_first_site || fragment->site_ordinal == line->first_ordinal;
        has_second_site = has_second_site || fragment->site_ordinal == line->second_ordinal;
        status = odt_classify_fragment(builder, fragment, line, &first, &second);
        if (status != ODT_OK) {
            return status;
        }
        if (first) {
            out_score->first_fragment_count += 1u;
            if (builder->first_marks[fragment->site_ordinal] != epoch) {
                builder->first_marks[fragment->site_ordinal] = epoch;
                out_score->first_region_count += 1u;
            }
        }
        if (second) {
            out_score->second_fragment_count += 1u;
            if (builder->second_marks[fragment->site_ordinal] != epoch) {
                builder->second_marks[fragment->site_ordinal] = epoch;
                out_score->second_region_count += 1u;
            }
        }
        if (first && second) {
            out_score->new_fragment_count += 1u;
        }
    }
    if (!odt_checked_add_size(out_score->first_fragment_count, out_score->second_fragment_count,
                              &out_score->total_child_fragment_count)) {
        return ODT_LIMIT_EXCEEDED;
    }
    *out_admissible = has_first_site && has_second_site && out_score->first_region_count != 0u &&
                      out_score->second_region_count != 0u;
    return ODT_OK;
}

static odt_status odt_select_candidate(odt_builder *builder,
                                       const odt_builder_fragment_set *fragments,
                                       odt_candidate_score *out_score) {
    odt_candidate_score best;
    const size_t membership_epoch = odt_builder_next_epoch(builder);
    bool found = false;
    size_t index;

    memset(&best, 0, sizeof(best));
    for (index = 0u; index < fragments->count; ++index) {
        builder->region_marks[fragments->items[index].site_ordinal] = membership_epoch;
    }
    for (index = 0u; index < builder->candidate_count; ++index) {
        odt_candidate_score candidate;
        bool admissible;
        const odt_internal_line_ref *line = &builder->candidates[index];
        odt_status status;

        if (builder->region_marks[line->first_ordinal] != membership_epoch ||
            builder->region_marks[line->second_ordinal] != membership_epoch) {
            continue;
        }
        status = odt_evaluate_candidate(builder, fragments, line, &candidate, &admissible);

        if (status != ODT_OK) {
            return status;
        }
        if (admissible && (!found || odt_score_better(&candidate, &best))) {
            best = candidate;
            found = true;
        }
    }
    if (!found) {
        return ODT_INTERNAL_ERROR;
    }
    *out_score = best;
    return ODT_OK;
}

static odt_status odt_split_fragments(odt_builder *builder, odt_builder_fragment_set *fragments,
                                      const odt_candidate_score *score,
                                      odt_builder_fragment_set *out_first,
                                      odt_builder_fragment_set *out_second) {
    size_t first_index = 0u;
    size_t second_index = 0u;
    size_t index;
    odt_status status = odt_fragment_set_allocate(builder, score->first_fragment_count, out_first);

    memset(out_second, 0, sizeof(*out_second));
    if (status != ODT_OK) {
        return status;
    }
    status = odt_fragment_set_allocate(builder, score->second_fragment_count, out_second);
    if (status != ODT_OK) {
        odt_fragment_set_destroy(builder, out_first);
        return status;
    }
    for (index = 0u; index < fragments->count; ++index) {
        odt_builder_fragment *fragment = &fragments->items[index];
        bool first;
        bool second;

        status = odt_classify_fragment(builder, fragment, &score->line, &first, &second);
        if (status != ODT_OK) {
            goto fail;
        }
        if (first && second) {
            odt_builder_fragment first_fragment;
            odt_builder_fragment second_fragment;

            status = odt_builder_emit_fragment(builder);
            if (status == ODT_OK) {
                status = odt_builder_emit_fragment(builder);
            }
            if (status == ODT_OK) {
                status = odt_fragment_clip(builder, fragment, &score->line, false, &first_fragment);
            }
            if (status == ODT_OK) {
                status = odt_fragment_clip(builder, fragment, &score->line, true, &second_fragment);
                if (status != ODT_OK) {
                    odt_fragment_destroy(builder, &first_fragment);
                }
            }
            if (status != ODT_OK) {
                goto fail;
            }
            out_first->items[first_index] = first_fragment;
            out_second->items[second_index] = second_fragment;
            first_index += 1u;
            second_index += 1u;
            odt_fragment_destroy(builder, fragment);
        } else {
            odt_builder_fragment *destination;

            status = odt_builder_emit_fragment(builder);
            if (status != ODT_OK) {
                goto fail;
            }
            if (first) {
                destination = &out_first->items[first_index];
                first_index += 1u;
            } else {
                destination = &out_second->items[second_index];
                second_index += 1u;
            }
            *destination = *fragment;
            memset(fragment, 0, sizeof(*fragment));
        }
    }
    if (first_index != out_first->count || second_index != out_second->count) {
        status = ODT_INTERNAL_ERROR;
        goto fail;
    }
    return ODT_OK;

fail:
    odt_fragment_set_destroy(builder, out_second);
    odt_fragment_set_destroy(builder, out_first);
    return status;
}

static odt_status odt_reserve_array(odt_builder *builder, void **array, size_t *capacity,
                                    size_t *allocation_size, size_t needed, size_t maximum,
                                    size_t element_size, size_t alignment) {
    size_t new_capacity;
    size_t new_size;
    void *new_array;
    odt_status status;

    if (needed <= *capacity) {
        return ODT_OK;
    }
    if (needed > maximum) {
        return ODT_LIMIT_EXCEEDED;
    }
    new_capacity = *capacity == 0u ? 8u : *capacity;
    while (new_capacity < needed) {
        if (new_capacity > maximum / 2u) {
            new_capacity = maximum;
            break;
        }
        new_capacity *= 2u;
    }
    if (new_capacity > maximum) {
        new_capacity = maximum;
    }
    if (!odt_checked_multiply_size(new_capacity, element_size, &new_size)) {
        return ODT_LIMIT_EXCEEDED;
    }
    new_array = odt_builder_allocate(builder, new_size, alignment, &status);
    if (status != ODT_OK) {
        return status;
    }
    if (*array != NULL) {
        memcpy(new_array, *array, *allocation_size);
        odt_builder_deallocate(builder, *array, *allocation_size, alignment);
    }
    *array = new_array;
    *capacity = new_capacity;
    *allocation_size = new_size;
    return ODT_OK;
}

static odt_status odt_append_node(odt_builder *builder, const odt_internal_line_ref *line,
                                  size_t *out_index) {
    odt_internal_runtime_node *node;
    bool filter_enabled;
    odt_status status;

    if (builder->node_count >= builder->limits->max_internal_nodes ||
        builder->node_count >= (size_t)INT32_MAX) {
        return ODT_LIMIT_EXCEEDED;
    }
    status = odt_reserve_array(builder, (void **)&builder->nodes, &builder->node_capacity,
                               &builder->nodes_allocation_size, builder->node_count + 1u,
                               builder->limits->max_internal_nodes < (size_t)INT32_MAX
                                   ? builder->limits->max_internal_nodes
                                   : (size_t)INT32_MAX,
                               sizeof(*builder->nodes),
                               odt_allocation_alignment(_Alignof(odt_internal_runtime_node)));
    if (status != ODT_OK) {
        return status;
    }
    node = &builder->nodes[builder->node_count];
    memset(node, 0, sizeof(*node));
    if (line->first_ordinal > UINT32_MAX || line->second_ordinal > UINT32_MAX) {
        return ODT_LIMIT_EXCEEDED;
    }
    node->first_ordinal = (uint32_t)line->first_ordinal;
    node->second_ordinal = (uint32_t)line->second_ordinal;
    status = odt_internal_runtime_filter_init(&builder->geometry->numeric, line, &node->filter,
                                              &filter_enabled);
    if (status != ODT_OK) {
        return status;
    }
    node->filter_enabled = filter_enabled ? 1u : 0u;
    *out_index = builder->node_count;
    builder->node_count += 1u;
    return ODT_OK;
}

static odt_status odt_append_leaf(odt_builder *builder, size_t site_ordinal,
                                  int32_t *out_reference) {
    size_t maximum;
    odt_status status;

    if (builder->limits->max_internal_nodes >= (size_t)INT32_MAX) {
        maximum = (size_t)INT32_MAX;
    } else {
        maximum = builder->limits->max_internal_nodes + 1u;
    }
    if (builder->leaf_count >= maximum) {
        return ODT_LIMIT_EXCEEDED;
    }
    status = odt_reserve_array(builder, (void **)&builder->leaves, &builder->leaf_capacity,
                               &builder->leaves_allocation_size, builder->leaf_count + 1u, maximum,
                               sizeof(*builder->leaves),
                               odt_allocation_alignment(_Alignof(odt_internal_runtime_leaf)));
    if (status != ODT_OK) {
        return status;
    }
    if (site_ordinal > UINT32_MAX) {
        return ODT_LIMIT_EXCEEDED;
    }
    builder->leaves[builder->leaf_count].site_ordinal = (uint32_t)site_ordinal;
    builder->leaves[builder->leaf_count].region_id =
        builder->geometry->sites[site_ordinal].region_id;
    builder->leaf_count += 1u;
    *out_reference = -(int32_t)builder->leaf_count;
    return ODT_OK;
}

static odt_status odt_reserve_tasks(odt_builder *builder, size_t needed) {
    size_t maximum;

    if (builder->limits->max_internal_nodes >= SIZE_MAX - 1u) {
        maximum = SIZE_MAX;
    } else {
        maximum = builder->limits->max_internal_nodes + 1u;
    }
    return odt_reserve_array(builder, (void **)&builder->tasks, &builder->task_capacity,
                             &builder->tasks_allocation_size, needed, maximum,
                             sizeof(*builder->tasks),
                             odt_allocation_alignment(_Alignof(odt_builder_task)));
}

static void odt_assign_reference(odt_builder *builder, const odt_builder_task *task,
                                 int32_t reference) {
    if (task->root) {
        builder->root_reference = reference;
    } else if (task->second_branch) {
        builder->nodes[task->parent_node].second_child = reference;
    } else {
        builder->nodes[task->parent_node].first_child = reference;
    }
}

static bool odt_fragments_single_site(const odt_builder_fragment_set *fragments,
                                      size_t *out_site_ordinal) {
    const size_t site_ordinal = fragments->items[0].site_ordinal;
    size_t index;

    for (index = 1u; index < fragments->count; ++index) {
        if (fragments->items[index].site_ordinal != site_ordinal) {
            return false;
        }
    }
    *out_site_ordinal = site_ordinal;
    return true;
}

static odt_status odt_build_tree(odt_builder *builder,
                                 odt_builder_fragment_set *initial_fragments) {
    odt_builder_task initial_task;
    odt_status status = odt_reserve_tasks(builder, 1u);

    if (status != ODT_OK) {
        return status;
    }
    memset(&initial_task, 0, sizeof(initial_task));
    initial_task.fragments = *initial_fragments;
    initial_task.parent_node = SIZE_MAX;
    initial_task.root = true;
    memset(initial_fragments, 0, sizeof(*initial_fragments));
    builder->tasks[builder->task_count] = initial_task;
    builder->task_count += 1u;

    while (builder->task_count != 0u) {
        odt_builder_task task = builder->tasks[builder->task_count - 1u];
        size_t site_ordinal;

        builder->task_count -= 1u;
        if (odt_fragments_single_site(&task.fragments, &site_ordinal)) {
            int32_t reference;

            status = odt_append_leaf(builder, site_ordinal, &reference);
            if (status == ODT_OK) {
                odt_assign_reference(builder, &task, reference);
                if (task.depth > builder->maximum_depth) {
                    builder->maximum_depth = (uint32_t)task.depth;
                }
            }
            odt_fragment_set_destroy(builder, &task.fragments);
            if (status != ODT_OK) {
                return status;
            }
        } else {
            odt_candidate_score score;
            odt_builder_fragment_set first;
            odt_builder_fragment_set second;
            size_t node_index;
            odt_builder_task first_task;
            odt_builder_task second_task;

            if (task.depth >= builder->limits->max_depth) {
                odt_fragment_set_destroy(builder, &task.fragments);
                return ODT_LIMIT_EXCEEDED;
            }
            status = odt_select_candidate(builder, &task.fragments, &score);
            if (status == ODT_OK) {
                status = odt_append_node(builder, &score.line, &node_index);
            }
            if (status == ODT_OK) {
                status = odt_reserve_tasks(builder, builder->task_count + 2u);
            }
            if (status == ODT_OK) {
                status = odt_split_fragments(builder, &task.fragments, &score, &first, &second);
            }
            if (status != ODT_OK) {
                odt_fragment_set_destroy(builder, &task.fragments);
                return status;
            }
            odt_assign_reference(builder, &task, (int32_t)(node_index + 1u));
            memset(&first_task, 0, sizeof(first_task));
            memset(&second_task, 0, sizeof(second_task));
            first_task.fragments = first;
            first_task.depth = task.depth + 1u;
            first_task.parent_node = node_index;
            second_task.fragments = second;
            second_task.depth = task.depth + 1u;
            second_task.parent_node = node_index;
            second_task.second_branch = true;
            builder->tasks[builder->task_count] = second_task;
            builder->task_count += 1u;
            builder->tasks[builder->task_count] = first_task;
            builder->task_count += 1u;
            odt_fragment_set_destroy(builder, &task.fragments);
        }
    }
    return builder->root_reference == 0 ? ODT_INTERNAL_ERROR : ODT_OK;
}

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

static odt_status odt_equality_face_is_excluded(
    odt_builder *builder, const odt_generation *generation, const odt_verify_path_step *steps,
    size_t step_count, const odt_internal_line_ref *equality_line,
    const odt_internal_vertex *vertices, size_t vertex_count, bool *out_excluded) {
    const odt_internal_runtime_node *nodes = odt_generation_nodes(generation);
    size_t step_index;

    *out_excluded = false;
    for (step_index = 0u; step_index < step_count; ++step_index) {
        const odt_internal_runtime_node *node = &nodes[steps[step_index].node_index];
        const odt_internal_line_ref strict_line =
            odt_bisector_line(node->first_ordinal, node->second_ordinal);
        bool excludes_face = steps[step_index].second_branch;
        bool saw_equality = false;
        size_t vertex_index;

        if (!excludes_face) {
            continue;
        }
        for (vertex_index = 0u; vertex_index < vertex_count; ++vertex_index) {
            bool used_exact;
            int equality_sign;
            int strict_sign;
            odt_status status = odt_builder_consume_work(builder, 1u);

            if (status != ODT_OK) {
                return status;
            }
            status = odt_internal_vertex_side(&builder->geometry->numeric, &vertices[vertex_index],
                                              equality_line, &equality_sign, &used_exact);
            if (status != ODT_OK) {
                return status;
            }
            if (equality_sign != 0) {
                continue;
            }
            saw_equality = true;
            status = odt_builder_consume_work(builder, 1u);
            if (status != ODT_OK) {
                return status;
            }
            status = odt_internal_vertex_side(&builder->geometry->numeric, &vertices[vertex_index],
                                              &strict_line, &strict_sign, &used_exact);
            if (status != ODT_OK) {
                return status;
            }
            if (strict_sign != 0) {
                excludes_face = false;
                break;
            }
        }
        if (saw_equality && excludes_face) {
            *out_excluded = true;
            return ODT_OK;
        }
    }
    return ODT_OK;
}

static odt_status odt_verify_leaf_polygon(odt_builder *builder, const odt_generation *generation,
                                          const odt_verify_path_step *steps, size_t step_count,
                                          size_t site_ordinal, const odt_internal_vertex *vertices,
                                          size_t vertex_count) {
    odt_internal_line_ref first_edge;
    bool has_positive_area = false;
    size_t other_ordinal;

    if (vertex_count < 3u || site_ordinal >= generation->site_count) {
        return ODT_INTERNAL_ERROR;
    }
    first_edge = vertices[0].incoming_line;
    for (other_ordinal = 0u; other_ordinal < vertex_count; ++other_ordinal) {
        bool used_exact;
        int sign;
        odt_status status = odt_builder_consume_work(builder, 1u);

        if (status != ODT_OK) {
            return status;
        }
        status = odt_internal_vertex_side(&builder->geometry->numeric, &vertices[other_ordinal],
                                          &first_edge, &sign, &used_exact);
        if (status != ODT_OK) {
            return status;
        }
        (void)used_exact;
        has_positive_area = has_positive_area || sign != 0;
    }
    if (!has_positive_area) {
        return ODT_INTERNAL_ERROR;
    }
    for (other_ordinal = 0u; other_ordinal < generation->site_count; ++other_ordinal) {
        odt_internal_line_ref line;
        bool saw_equality = false;
        size_t vertex_index;

        if (other_ordinal == site_ordinal) {
            continue;
        }
        line = odt_bisector_line(site_ordinal, other_ordinal);
        for (vertex_index = 0u; vertex_index < vertex_count; ++vertex_index) {
            bool used_exact;
            int sign;
            bool wins;
            odt_status status = odt_builder_consume_work(builder, 1u);

            if (status != ODT_OK) {
                return status;
            }
            status = odt_internal_vertex_side(&builder->geometry->numeric, &vertices[vertex_index],
                                              &line, &sign, &used_exact);
            if (status != ODT_OK) {
                return status;
            }
            (void)used_exact;
            wins = site_ordinal == line.first_ordinal ? sign <= 0 : sign >= 0;
            if (!wins) {
                return ODT_INTERNAL_ERROR;
            }
            saw_equality = saw_equality || sign == 0;
        }
        if (other_ordinal < site_ordinal && saw_equality) {
            bool excluded;
            odt_status status = odt_equality_face_is_excluded(
                builder, generation, steps, step_count, &line, vertices, vertex_count, &excluded);

            if (status != ODT_OK) {
                return status;
            }
            if (!excluded) {
                return ODT_INTERNAL_ERROR;
            }
        }
    }
    return ODT_OK;
}

static odt_status odt_verify_generation_paths(odt_builder *builder,
                                              const odt_generation *generation) {
    const odt_internal_runtime_node *nodes = odt_generation_nodes(generation);
    const odt_internal_runtime_leaf *leaves = odt_generation_leaves(generation);
    size_t *node_parent = NULL;
    size_t *leaf_parent = NULL;
    unsigned char *node_branch = NULL;
    unsigned char *leaf_branch = NULL;
    odt_verify_stack_entry *stack = NULL;
    odt_verify_path_step *steps = NULL;
    odt_internal_vertex *first_vertices = NULL;
    odt_internal_vertex *second_vertices = NULL;
    size_t node_parent_size = 0u;
    size_t leaf_parent_size = 0u;
    size_t node_branch_size = 0u;
    size_t leaf_branch_size = 0u;
    size_t stack_size = 0u;
    size_t steps_size = 0u;
    size_t vertices_size = 0u;
    size_t stack_count = 0u;
    size_t visited_nodes = 0u;
    size_t visited_leaves = 0u;
    size_t vertex_capacity;
    size_t index;
    odt_status status = ODT_OK;

    if (generation->site_count == 0u || generation->leaf_count == 0u ||
        !odt_reference_valid(generation, generation->root_reference) ||
        (generation->node_count != 0u && generation->root_reference != 1) ||
        generation->node_count > (size_t)INT32_MAX || generation->leaf_count > (size_t)INT32_MAX) {
        return ODT_INTERNAL_ERROR;
    }
    if (!odt_checked_multiply_size(generation->node_count, sizeof(*node_parent),
                                   &node_parent_size) ||
        !odt_checked_multiply_size(generation->leaf_count, sizeof(*leaf_parent),
                                   &leaf_parent_size) ||
        !odt_checked_multiply_size(generation->node_count, sizeof(*node_branch),
                                   &node_branch_size) ||
        !odt_checked_multiply_size(generation->leaf_count, sizeof(*leaf_branch),
                                   &leaf_branch_size) ||
        !odt_checked_add_size(generation->node_count, generation->leaf_count, &stack_size) ||
        !odt_checked_multiply_size(stack_size, sizeof(*stack), &stack_size) ||
        !odt_checked_multiply_size(generation->build_stats.maximum_depth, sizeof(*steps),
                                   &steps_size) ||
        !odt_checked_add_size((size_t)generation->build_stats.maximum_depth, 4u,
                              &vertex_capacity) ||
        !odt_checked_multiply_size(vertex_capacity, sizeof(*first_vertices), &vertices_size)) {
        return ODT_LIMIT_EXCEEDED;
    }
    if (leaf_parent_size == 0u || leaf_branch_size == 0u || stack_size == 0u ||
        vertices_size == 0u) {
        return ODT_INTERNAL_ERROR;
    }
#define ODT_VERIFY_ALLOCATE(pointer, size_value, type)                                             \
    do {                                                                                           \
        if ((size_value) != 0u) {                                                                  \
            (pointer) = odt_builder_allocate(builder, (size_value),                                \
                                             odt_allocation_alignment(_Alignof(type)), &status);   \
            if (status != ODT_OK) {                                                                \
                goto out;                                                                          \
            }                                                                                      \
        }                                                                                          \
    } while (0)

    ODT_VERIFY_ALLOCATE(node_parent, node_parent_size, size_t);
    ODT_VERIFY_ALLOCATE(leaf_parent, leaf_parent_size, size_t);
    ODT_VERIFY_ALLOCATE(node_branch, node_branch_size, unsigned char);
    ODT_VERIFY_ALLOCATE(leaf_branch, leaf_branch_size, unsigned char);
    ODT_VERIFY_ALLOCATE(stack, stack_size, odt_verify_stack_entry);
    ODT_VERIFY_ALLOCATE(steps, steps_size, odt_verify_path_step);
    ODT_VERIFY_ALLOCATE(first_vertices, vertices_size, odt_internal_vertex);
    ODT_VERIFY_ALLOCATE(second_vertices, vertices_size, odt_internal_vertex);
#undef ODT_VERIFY_ALLOCATE

    if (node_parent_size != 0u) {
        memset(node_parent, 0xff, node_parent_size);
        memset(node_branch, 0xff, node_branch_size);
    }
    memset(leaf_parent, 0xff, leaf_parent_size);
    memset(leaf_branch, 0xff, leaf_branch_size);
    stack[stack_count++] = (odt_verify_stack_entry){
        generation->root_reference, SIZE_MAX, 0u, false, true,
    };
    while (stack_count != 0u) {
        const odt_verify_stack_entry entry = stack[--stack_count];

        if (!odt_reference_valid(generation, entry.reference) ||
            entry.depth > generation->build_stats.maximum_depth) {
            status = ODT_INTERNAL_ERROR;
            goto out;
        }
        if (entry.reference > 0) {
            const size_t node_index = (size_t)entry.reference - 1u;
            const odt_internal_runtime_node *node = &nodes[node_index];

            if (node_branch[node_index] != UCHAR_MAX || (entry.root && node_index != 0u) ||
                node->first_ordinal >= node->second_ordinal ||
                (size_t)node->second_ordinal >= generation->site_count ||
                !odt_reference_valid(generation, node->first_child) ||
                !odt_reference_valid(generation, node->second_child) ||
                stack_count > generation->node_count + generation->leaf_count - 2u) {
                status = ODT_INTERNAL_ERROR;
                goto out;
            }
            node_parent[node_index] = entry.parent_node;
            node_branch[node_index] = entry.root ? 2u : (entry.second_branch ? 1u : 0u);
            visited_nodes += 1u;
            stack[stack_count++] = (odt_verify_stack_entry){
                node->second_child, node_index, entry.depth + 1u, true, false,
            };
            stack[stack_count++] = (odt_verify_stack_entry){
                node->first_child, node_index, entry.depth + 1u, false, false,
            };
        } else {
            const size_t leaf_index = (size_t)(-(int64_t)entry.reference) - 1u;

            if (leaf_branch[leaf_index] != UCHAR_MAX) {
                status = ODT_INTERNAL_ERROR;
                goto out;
            }
            leaf_parent[leaf_index] = entry.parent_node;
            leaf_branch[leaf_index] = entry.root ? 2u : (entry.second_branch ? 1u : 0u);
            if ((size_t)leaves[leaf_index].site_ordinal >= generation->site_count ||
                leaves[leaf_index].region_id !=
                    odt_generation_sites(generation)[leaves[leaf_index].site_ordinal].region_id) {
                status = ODT_INTERNAL_ERROR;
                goto out;
            }
            visited_leaves += 1u;
        }
    }
    if (visited_nodes != generation->node_count || visited_leaves != generation->leaf_count) {
        status = ODT_INTERNAL_ERROR;
        goto out;
    }

    for (index = 0u; index < generation->leaf_count; ++index) {
        odt_internal_vertex *current = first_vertices;
        odt_internal_vertex *next = second_vertices;
        size_t current_count = 4u;
        size_t step_count = 0u;
        size_t path_length;
        size_t parent = leaf_parent[index];
        bool branch = leaf_branch[index] != 0u;

        while (parent != SIZE_MAX) {
            if (step_count >= generation->build_stats.maximum_depth) {
                status = ODT_INTERNAL_ERROR;
                goto out;
            }
            steps[step_count].node_index = parent;
            steps[step_count].second_branch = branch;
            step_count += 1u;
            branch = node_branch[parent] != 0u;
            parent = node_parent[parent];
        }
        path_length = step_count;
        odt_initialize_rectangle(current);
        while (step_count != 0u) {
            const odt_verify_path_step step = steps[step_count - 1u];
            const odt_internal_runtime_node *node = &nodes[step.node_index];
            const odt_internal_line_ref line =
                odt_bisector_line(node->first_ordinal, node->second_ordinal);
            size_t next_count;
            odt_internal_vertex *swap;

            status = odt_clip_vertices(builder, current, current_count, &line, step.second_branch,
                                       next, vertex_capacity, &next_count);
            if (status != ODT_OK) {
                goto out;
            }
            swap = current;
            current = next;
            next = swap;
            current_count = next_count;
            step_count -= 1u;
        }
        status = odt_verify_leaf_polygon(builder, generation, steps, path_length,
                                         leaves[index].site_ordinal, current, current_count);
        if (status != ODT_OK) {
            goto out;
        }
    }

out:
    odt_builder_deallocate(builder, second_vertices, vertices_size,
                           odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    odt_builder_deallocate(builder, first_vertices, vertices_size,
                           odt_allocation_alignment(_Alignof(odt_internal_vertex)));
    odt_builder_deallocate(builder, steps, steps_size,
                           odt_allocation_alignment(_Alignof(odt_verify_path_step)));
    odt_builder_deallocate(builder, stack, stack_size,
                           odt_allocation_alignment(_Alignof(odt_verify_stack_entry)));
    odt_builder_deallocate(builder, leaf_branch, leaf_branch_size,
                           odt_allocation_alignment(_Alignof(unsigned char)));
    odt_builder_deallocate(builder, node_branch, node_branch_size,
                           odt_allocation_alignment(_Alignof(unsigned char)));
    odt_builder_deallocate(builder, leaf_parent, leaf_parent_size,
                           odt_allocation_alignment(_Alignof(size_t)));
    odt_builder_deallocate(builder, node_parent, node_parent_size,
                           odt_allocation_alignment(_Alignof(size_t)));
    return status;
}

static odt_status odt_generation_layout(size_t site_count, size_t node_count, size_t leaf_count,
                                        size_t *out_sites_offset, size_t *out_nodes_offset,
                                        size_t *out_leaves_offset, size_t *out_size) {
    size_t offset = sizeof(odt_generation);
    size_t section_size;

    if (!odt_align_up(offset, _Alignof(odt_site), &offset)) {
        return ODT_LIMIT_EXCEEDED;
    }
    *out_sites_offset = offset;
    if (!odt_checked_multiply_size(site_count, sizeof(odt_site), &section_size) ||
        !odt_checked_add_size(offset, section_size, &offset) ||
        !odt_align_up(offset, _Alignof(odt_internal_runtime_node), &offset)) {
        return ODT_LIMIT_EXCEEDED;
    }
    *out_nodes_offset = offset;
    if (!odt_checked_multiply_size(node_count, sizeof(odt_internal_runtime_node), &section_size) ||
        !odt_checked_add_size(offset, section_size, &offset) ||
        !odt_align_up(offset, _Alignof(odt_internal_runtime_leaf), &offset)) {
        return ODT_LIMIT_EXCEEDED;
    }
    *out_leaves_offset = offset;
    if (!odt_checked_multiply_size(leaf_count, sizeof(odt_internal_runtime_leaf), &section_size) ||
        !odt_checked_add_size(offset, section_size, &offset) ||
        !odt_align_up(offset, ODT_GENERATION_ALIGNMENT, &offset)) {
        return ODT_LIMIT_EXCEEDED;
    }
    *out_size = offset;
    return ODT_OK;
}

static odt_status odt_compile_generation(odt_builder *builder, odt_generation **out_generation) {
    odt_generation *generation = NULL;
    size_t sites_offset;
    size_t nodes_offset;
    size_t leaves_offset;
    size_t allocation_size;
    size_t charge;
    odt_status status = odt_generation_layout(builder->geometry->site_count, builder->node_count,
                                              builder->leaf_count, &sites_offset, &nodes_offset,
                                              &leaves_offset, &allocation_size);

    *out_generation = NULL;
    if (status != ODT_OK) {
        return status;
    }
    if (!odt_allocation_charge(allocation_size, ODT_GENERATION_ALIGNMENT, &charge) ||
        builder->memory.live > builder->memory.limit ||
        charge > builder->memory.limit - builder->memory.live) {
        return ODT_LIMIT_EXCEEDED;
    }
    status = odt_internal_generation_allocate(&builder->memory.allocator, allocation_size,
                                              ODT_GENERATION_ALIGNMENT, &generation);
    if (status != ODT_OK) {
        return status;
    }
    builder->memory.live += charge;
    if (builder->memory.live > builder->memory.peak) {
        builder->memory.peak = builder->memory.live;
    }
    generation->domain = builder->geometry->domain;
    generation->site_count = builder->geometry->site_count;
    generation->node_count = builder->node_count;
    generation->leaf_count = builder->leaf_count;
    generation->sites_offset = sites_offset;
    generation->nodes_offset = nodes_offset;
    generation->leaves_offset = leaves_offset;
    generation->root_reference = builder->root_reference;
    generation->filter_scale_exponent = builder->geometry->numeric.filter_scale_exponent;
    generation->filter_enabled = builder->geometry->numeric.filter_enabled ? 1u : 0u;
    generation->build_stats.site_count = (uint64_t)generation->site_count;
    generation->build_stats.node_count = (uint64_t)generation->node_count;
    generation->build_stats.leaf_count = (uint64_t)generation->leaf_count;
    generation->build_stats.fragment_count = builder->fragment_count;
    generation->build_stats.maximum_depth = builder->maximum_depth;
    memcpy((unsigned char *)generation + sites_offset, builder->geometry->sites,
           generation->site_count * sizeof(odt_site));
    if (generation->node_count != 0u) {
        memcpy((unsigned char *)generation + nodes_offset, builder->nodes,
               generation->node_count * sizeof(odt_internal_runtime_node));
    }
    memcpy((unsigned char *)generation + leaves_offset, builder->leaves,
           generation->leaf_count * sizeof(odt_internal_runtime_leaf));
    status = odt_verify_generation_paths(builder, generation);
    if (status != ODT_OK) {
        odt_generation_destroy(generation);
        builder->memory.live -= charge;
        return status;
    }
    *out_generation = generation;
    return ODT_OK;
}

static void odt_builder_destroy(odt_builder *builder) {
    size_t index;

    for (index = 0u; index < builder->task_count; ++index) {
        odt_fragment_set_destroy(builder, &builder->tasks[index].fragments);
    }
    odt_builder_deallocate(builder, builder->tasks, builder->tasks_allocation_size,
                           odt_allocation_alignment(_Alignof(odt_builder_task)));
    odt_builder_deallocate(builder, builder->leaves, builder->leaves_allocation_size,
                           odt_allocation_alignment(_Alignof(odt_internal_runtime_leaf)));
    odt_builder_deallocate(builder, builder->nodes, builder->nodes_allocation_size,
                           odt_allocation_alignment(_Alignof(odt_internal_runtime_node)));
    odt_builder_deallocate(builder, builder->first_marks, builder->marks_allocation_size,
                           odt_allocation_alignment(_Alignof(size_t)));
    odt_builder_deallocate(builder, builder->candidates, builder->candidates_allocation_size,
                           odt_allocation_alignment(_Alignof(odt_internal_line_ref)));
}

static uint64_t odt_now_nanoseconds(void) {
    struct timespec value;

    if (timespec_get(&value, TIME_UTC) != TIME_UTC || value.tv_sec < 0 || value.tv_nsec < 0) {
        return 0u;
    }
    if ((uint64_t)value.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

odt_status odt_build(const odt_domain *domain, const odt_site *sites, size_t site_count,
                     const odt_build_options *options, const odt_limits *limits,
                     const odt_allocator *allocator, odt_build_stats *out_stats,
                     odt_generation **out_generation) {
    odt_build_options default_options;
    odt_limits default_limits;
    odt_allocator default_allocator;
    const odt_build_options *effective_options = options;
    const odt_limits *effective_limits = limits;
    const odt_allocator *effective_allocator = allocator;
    odt_internal_geometry *geometry = NULL;
    odt_builder_fragment_set fragments;
    odt_builder builder;
    odt_generation *generation = NULL;
    uint64_t start_time;
    uint64_t end_time;
    odt_status status;

    if (out_generation == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_generation = NULL;
    start_time = odt_now_nanoseconds();
    status = odt_internal_numeric_environment_check();
    if (status != ODT_OK) {
        return status;
    }
    if (effective_options == NULL) {
        status = odt_build_options_init(&default_options);
        if (status != ODT_OK) {
            return status;
        }
        effective_options = &default_options;
    }
    status = odt_internal_build_options_validate(effective_options);
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
    if (site_count > UINT32_MAX) {
        return ODT_LIMIT_EXCEEDED;
    }
    status = odt_internal_geometry_build(domain, sites, site_count, effective_limits,
                                         effective_allocator, &geometry);
    if (status != ODT_OK) {
        return status;
    }
    memset(&builder, 0, sizeof(builder));
    memset(&fragments, 0, sizeof(fragments));
    builder.memory.allocator = *effective_allocator;
    builder.memory.limit = effective_limits->max_build_bytes;
    builder.memory.live = geometry->live_build_bytes;
    builder.memory.peak = geometry->peak_build_bytes;
    builder.limits = effective_limits;
    builder.geometry = geometry;
    builder.work = geometry->build_work;
    builder.fragment_count = (uint64_t)geometry->site_count;

    status = odt_builder_extract_candidates(&builder);
    if (status == ODT_OK) {
        status = odt_builder_allocate_marks(&builder);
    }
    if (status == ODT_OK) {
        status = odt_builder_initial_fragments(&builder, &fragments);
    }
    if (status == ODT_OK) {
        status = odt_build_tree(&builder, &fragments);
    }
    if (status == ODT_OK) {
        status = odt_compile_generation(&builder, &generation);
    }
    odt_fragment_set_destroy(&builder, &fragments);
    odt_builder_destroy(&builder);
    if (status != ODT_OK) {
        odt_internal_geometry_destroy(geometry);
        return status;
    }
    generation->build_stats.build_work = builder.work;
    generation->build_stats.peak_build_bytes = (uint64_t)builder.memory.peak;
    odt_internal_geometry_destroy(geometry);
    end_time = odt_now_nanoseconds();
    generation->build_stats.build_duration_ns = end_time >= start_time ? end_time - start_time : 0u;
    if (out_stats != NULL) {
        *out_stats = generation->build_stats;
    }
    *out_generation = generation;
    return ODT_OK;
}
