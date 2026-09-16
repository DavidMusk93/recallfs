#ifndef ODT_INTERNAL_H
#define ODT_INTERNAL_H

#include "odt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct odt_generation {
    odt_allocator allocator;
    size_t allocation_size;
    size_t allocation_alignment;
};

struct odt_builder;

typedef enum odt_internal_line_kind {
    ODT_INTERNAL_LINE_DOMAIN_MIN_X = 0,
    ODT_INTERNAL_LINE_DOMAIN_MAX_X = 1,
    ODT_INTERNAL_LINE_DOMAIN_MIN_Y = 2,
    ODT_INTERNAL_LINE_DOMAIN_MAX_Y = 3,
    ODT_INTERNAL_LINE_BISECTOR = 4
} odt_internal_line_kind;

typedef struct odt_internal_line_ref {
    odt_internal_line_kind kind;
    size_t first_ordinal;
    size_t second_ordinal;
} odt_internal_line_ref;

typedef struct odt_internal_vertex {
    odt_internal_line_ref first_line;
    odt_internal_line_ref second_line;
    odt_internal_line_ref incoming_line;
} odt_internal_vertex;

typedef struct odt_internal_cell {
    size_t site_ordinal;
    int32_t region_id;
    size_t vertex_count;
    size_t allocation_size;
    odt_internal_vertex *vertices;
    bool certified;
} odt_internal_cell;

typedef struct odt_internal_numeric_context {
    odt_domain domain;
    const odt_site *sites;
    size_t site_count;
    int filter_scale_exponent;
    bool filter_enabled;
} odt_internal_numeric_context;

typedef struct odt_internal_geometry {
    odt_allocator allocator;
    odt_domain domain;
    odt_site *sites;
    odt_internal_cell *cells;
    size_t site_count;
    size_t sites_allocation_size;
    size_t cells_allocation_size;
    uint64_t build_work;
    uint64_t exact_fallbacks;
    uint64_t vertex_count;
    size_t peak_build_bytes;
    odt_internal_numeric_context numeric;
} odt_internal_geometry;

odt_status odt_internal_allocator_validate(const odt_allocator *allocator);
odt_status odt_internal_limits_validate(const odt_limits *limits);
odt_status odt_internal_build_options_validate(const odt_build_options *options);
odt_status odt_internal_encode_options_validate(const odt_encode_options *options);
odt_status odt_internal_load_limits_validate(const odt_load_limits *limits);

odt_status odt_internal_generation_allocate(const odt_allocator *allocator, size_t allocation_size,
                                            size_t allocation_alignment,
                                            odt_generation **out_generation);

odt_status odt_internal_numeric_environment_check(void);
odt_status odt_internal_numeric_context_init(const odt_domain *domain, const odt_site *sites,
                                             size_t site_count,
                                             odt_internal_numeric_context *out_context);
odt_status odt_internal_compare_squared_distance(const odt_point *point, const odt_site *first,
                                                 size_t first_ordinal, const odt_site *second,
                                                 size_t second_ordinal, int *out_comparison);
odt_status odt_internal_compare_squared_distance_values(const odt_point *point,
                                                        const odt_site *first,
                                                        const odt_site *second,
                                                        int *out_comparison);
odt_status odt_internal_vertex_side(const odt_internal_numeric_context *context,
                                    const odt_internal_vertex *vertex,
                                    const odt_internal_line_ref *line, int *out_sign,
                                    bool *out_used_exact);
odt_status odt_internal_lines_parallel(const odt_internal_numeric_context *context,
                                       const odt_internal_line_ref *first,
                                       const odt_internal_line_ref *second, bool *out_parallel);

odt_status odt_internal_geometry_build(const odt_domain *domain, const odt_site *sites,
                                       size_t site_count, const odt_limits *limits,
                                       const odt_allocator *allocator,
                                       odt_internal_geometry **out_geometry);
void odt_internal_geometry_destroy(odt_internal_geometry *geometry);
odt_status odt_internal_geometry_cell_contains_point(const odt_internal_geometry *geometry,
                                                     size_t cell_index, const odt_point *point,
                                                     bool *out_contains);

#endif
