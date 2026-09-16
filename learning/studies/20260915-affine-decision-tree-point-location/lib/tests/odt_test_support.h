#ifndef ODT_TEST_SUPPORT_H
#define ODT_TEST_SUPPORT_H

#include "odt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define ODT_TEST_CHECK(condition)                                                        \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE);                                                          \
        }                                                                                \
    } while (0)

#define ODT_TEST_STATUS(expression, expected_status)                               \
    do {                                                                           \
        odt_status odt_test_actual_ = (expression);                                \
        odt_status odt_test_expected_ = (expected_status);                         \
        if (odt_test_actual_ != odt_test_expected_) {                              \
            fprintf(stderr,                                                       \
                    "%s:%d: %s returned %d (%s), expected %d (%s)\n", __FILE__,   \
                    __LINE__, #expression, (int)odt_test_actual_,                  \
                    odt_status_string(odt_test_actual_), (int)odt_test_expected_,  \
                    odt_status_string(odt_test_expected_));                        \
            exit(EXIT_FAILURE);                                                    \
        }                                                                          \
    } while (0)

typedef struct odt_test_allocator_state {
    bool live;
    bool fail_allocation;
    size_t allocate_calls;
    size_t deallocate_calls;
    size_t access_after_dead;
    size_t last_size;
    size_t last_alignment;
    void *active_pointer;
} odt_test_allocator_state;

void odt_test_counting_allocator_init(odt_allocator *out_allocator,
                                      odt_test_allocator_state *state);

odt_status odt_test_allocator_validate(const odt_allocator *allocator);
odt_status odt_test_limits_validate(const odt_limits *limits);
odt_status odt_test_build_options_validate(const odt_build_options *options);
odt_status odt_test_encode_options_validate(const odt_encode_options *options);
odt_status odt_test_load_limits_validate(const odt_load_limits *limits);

odt_status odt_test_generation_create(const odt_allocator *allocator, size_t size,
                                      size_t alignment, odt_generation **out_generation);

#endif
