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
        !odt_test_align_up(size, alignment, &aligned_size)) {
        return NULL;
    }
    pointer = aligned_alloc(alignment, aligned_size);
    if (pointer != NULL) {
        ODT_TEST_CHECK(state->active_pointer == NULL);
        state->active_pointer = pointer;
    }
    return pointer;
}

static void odt_test_deallocate(void *context, void *pointer, size_t size,
                                size_t alignment) {
    odt_test_allocator_state *state = context;

    if (!state->live) {
        state->access_after_dead += 1u;
        return;
    }
    ODT_TEST_CHECK(pointer == state->active_pointer);
    ODT_TEST_CHECK(size == state->last_size);
    ODT_TEST_CHECK(alignment == state->last_alignment);
    state->deallocate_calls += 1u;
    state->active_pointer = NULL;
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

odt_status odt_test_generation_create(const odt_allocator *allocator, size_t size,
                                      size_t alignment, odt_generation **out_generation) {
    return odt_internal_generation_allocate(allocator, size, alignment, out_generation);
}
