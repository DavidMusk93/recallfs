#ifndef ODT_TEST_SUPPORT_H
#define ODT_TEST_SUPPORT_H

#include "odt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define ODT_TEST_CHECK(condition)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define ODT_TEST_STATUS(expression, expected_status)                                               \
    do {                                                                                           \
        odt_status odt_test_actual_ = (expression);                                                \
        odt_status odt_test_expected_ = (expected_status);                                         \
        if (odt_test_actual_ != odt_test_expected_) {                                              \
            fprintf(stderr, "%s:%d: %s returned %d (%s), expected %d (%s)\n", __FILE__, __LINE__,  \
                    #expression, (int)odt_test_actual_, odt_status_string(odt_test_actual_),       \
                    (int)odt_test_expected_, odt_status_string(odt_test_expected_));               \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

typedef struct odt_test_allocator_state {
    bool live;
    bool fail_allocation;
    size_t fail_at_call;
    size_t allocate_calls;
    size_t deallocate_calls;
    size_t access_after_dead;
    size_t last_size;
    size_t last_alignment;
    void *active_pointer;
    size_t live_allocations;
    size_t peak_live_allocations;
    void *pointers[64];
    size_t sizes[64];
    size_t alignments[64];
} odt_test_allocator_state;

void odt_test_counting_allocator_init(odt_allocator *out_allocator,
                                      odt_test_allocator_state *state);

odt_status odt_test_allocator_validate(const odt_allocator *allocator);
odt_status odt_test_limits_validate(const odt_limits *limits);
odt_status odt_test_build_options_validate(const odt_build_options *options);
odt_status odt_test_encode_options_validate(const odt_encode_options *options);
odt_status odt_test_load_limits_validate(const odt_load_limits *limits);

odt_status odt_test_generation_create(const odt_allocator *allocator, size_t size, size_t alignment,
                                      odt_generation **out_generation);

typedef struct odt_test_geometry odt_test_geometry;

typedef enum odt_test_line_kind {
    ODT_TEST_LINE_DOMAIN_MIN_X = 0,
    ODT_TEST_LINE_DOMAIN_MAX_X = 1,
    ODT_TEST_LINE_DOMAIN_MIN_Y = 2,
    ODT_TEST_LINE_DOMAIN_MAX_Y = 3,
    ODT_TEST_LINE_BISECTOR = 4
} odt_test_line_kind;

typedef struct odt_test_geometry_stats {
    uint64_t build_work;
    uint64_t exact_fallbacks;
    uint64_t cell_count;
    uint64_t vertex_count;
    uint64_t peak_build_bytes;
} odt_test_geometry_stats;

odt_status odt_test_numeric_environment_check(void);
odt_status odt_test_compare_squared_distance(const odt_point *point, const odt_site *first,
                                             size_t first_ordinal, const odt_site *second,
                                             size_t second_ordinal, int *out_comparison);
odt_status odt_test_geometry_build(const odt_domain *domain, const odt_site *sites,
                                   size_t site_count, const odt_limits *limits,
                                   const odt_allocator *allocator,
                                   odt_test_geometry **out_geometry);
void odt_test_geometry_destroy(odt_test_geometry *geometry);
odt_status odt_test_geometry_get_stats(const odt_test_geometry *geometry,
                                       odt_test_geometry_stats *out_stats);
odt_status odt_test_geometry_get_cell(const odt_test_geometry *geometry, size_t cell_index,
                                      size_t *out_site_ordinal, int32_t *out_region_id,
                                      size_t *out_vertex_count);
odt_status odt_test_geometry_cell_contains_point(const odt_test_geometry *geometry,
                                                 size_t cell_index, const odt_point *point,
                                                 bool *out_contains);
odt_status odt_test_geometry_cell_is_certified(const odt_test_geometry *geometry, size_t cell_index,
                                               bool *out_certified);
odt_status odt_test_geometry_get_edge(const odt_test_geometry *geometry, size_t cell_index,
                                      size_t edge_index, odt_test_line_kind *out_kind,
                                      size_t *out_first_ordinal, size_t *out_second_ordinal);

#endif
