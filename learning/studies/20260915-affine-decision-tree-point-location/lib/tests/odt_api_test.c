#include "odt_test_support.h"

#include <stdint.h>
#include <string.h>

static void test_status_values_and_strings(void) {
    static const struct {
        odt_status status;
        int value;
        const char *text;
    } cases[] = {
        {ODT_OK, 0, "ok"},
        {ODT_INVALID_ARGUMENT, 1, "invalid argument"},
        {ODT_INVALID_DATA, 2, "invalid data"},
        {ODT_LIMIT_EXCEEDED, 3, "configured limit exceeded"},
        {ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT, 4, "unsupported numeric environment"},
        {ODT_OUT_OF_MEMORY, 5, "out of memory"},
        {ODT_IO_ERROR, 6, "I/O error"},
        {ODT_COMMIT_UNKNOWN, 7, "commit state unknown"},
        {ODT_CORRUPT_DATA, 8, "corrupt data"},
        {ODT_UNSUPPORTED_FORMAT, 9, "unsupported format"},
        {ODT_INTERNAL_ERROR, 10, "internal invariant failure"},
    };
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ODT_TEST_CHECK((int)cases[index].status == cases[index].value);
        ODT_TEST_CHECK(strcmp(odt_status_string(cases[index].status), cases[index].text) == 0);
    }
    ODT_TEST_CHECK(strcmp(odt_status_string((odt_status)999), "unknown status") == 0);
}

static void test_initializers(void) {
    odt_allocator allocator;
    odt_limits limits;
    odt_build_options build_options;
    odt_encode_options encode_options;
    odt_load_limits load_limits;

    ODT_TEST_STATUS(odt_allocator_init(NULL), ODT_INVALID_ARGUMENT);
    ODT_TEST_STATUS(odt_limits_init(NULL), ODT_INVALID_ARGUMENT);
    ODT_TEST_STATUS(odt_build_options_init(NULL), ODT_INVALID_ARGUMENT);
    ODT_TEST_STATUS(odt_encode_options_init(NULL), ODT_INVALID_ARGUMENT);
    ODT_TEST_STATUS(odt_load_limits_init(NULL), ODT_INVALID_ARGUMENT);

    memset(&allocator, 0xa5, sizeof(allocator));
    ODT_TEST_STATUS(odt_allocator_init(&allocator), ODT_OK);
    ODT_TEST_CHECK(allocator.struct_size == sizeof(allocator));
    ODT_TEST_CHECK(allocator.struct_version == ODT_ALLOCATOR_VERSION);
    ODT_TEST_CHECK(allocator.context == NULL);
    ODT_TEST_CHECK(allocator.allocate != NULL);
    ODT_TEST_CHECK(allocator.deallocate != NULL);
    ODT_TEST_STATUS(odt_test_allocator_validate(&allocator), ODT_OK);

    memset(&limits, 0xa5, sizeof(limits));
    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    ODT_TEST_CHECK(limits.struct_size == sizeof(limits));
    ODT_TEST_CHECK(limits.struct_version == ODT_LIMITS_VERSION);
    ODT_TEST_CHECK(limits.max_sites >= 1000u);
    ODT_TEST_CHECK(limits.max_polygon_fragments >= 1000000u);
    ODT_TEST_CHECK(limits.max_internal_nodes >= 65535u);
    ODT_TEST_CHECK(limits.max_depth >= 64u);
    ODT_TEST_CHECK(limits.max_build_bytes >= 512u * 1024u * 1024u);
    ODT_TEST_CHECK(limits.max_build_work != 0u);
    ODT_TEST_STATUS(odt_test_limits_validate(&limits), ODT_OK);

    memset(&build_options, 0xa5, sizeof(build_options));
    ODT_TEST_STATUS(odt_build_options_init(&build_options), ODT_OK);
    ODT_TEST_CHECK(build_options.struct_size == sizeof(build_options));
    ODT_TEST_CHECK(build_options.struct_version == ODT_BUILD_OPTIONS_VERSION);
    ODT_TEST_STATUS(odt_test_build_options_validate(&build_options), ODT_OK);

    memset(&encode_options, 0xa5, sizeof(encode_options));
    ODT_TEST_STATUS(odt_encode_options_init(&encode_options), ODT_OK);
    ODT_TEST_CHECK(encode_options.struct_size == sizeof(encode_options));
    ODT_TEST_CHECK(encode_options.struct_version == ODT_ENCODE_OPTIONS_VERSION);
    ODT_TEST_CHECK(encode_options.format_version == ODT_FORMAT_VERSION);
    ODT_TEST_CHECK(encode_options.reserved_0 == 0u);
    ODT_TEST_STATUS(odt_test_encode_options_validate(&encode_options), ODT_OK);

    memset(&load_limits, 0xa5, sizeof(load_limits));
    ODT_TEST_STATUS(odt_load_limits_init(&load_limits), ODT_OK);
    ODT_TEST_CHECK(load_limits.struct_size == sizeof(load_limits));
    ODT_TEST_CHECK(load_limits.struct_version == ODT_LOAD_LIMITS_VERSION);
    ODT_TEST_CHECK(load_limits.max_encoded_bytes != 0u);
    ODT_TEST_CHECK(load_limits.max_sites >= 1000u);
    ODT_TEST_CHECK(load_limits.max_internal_nodes >= 65535u);
    ODT_TEST_CHECK(load_limits.max_leaves >= 1000u);
    ODT_TEST_CHECK(load_limits.max_depth >= 64u);
    ODT_TEST_CHECK(load_limits.max_allocation_bytes >= 512u * 1024u * 1024u);
    ODT_TEST_STATUS(odt_test_load_limits_validate(&load_limits), ODT_OK);
}

static void test_versioned_structure_validation(void) {
    odt_allocator allocator;
    odt_limits limits;
    odt_build_options build_options;
    odt_encode_options encode_options;
    odt_load_limits load_limits;

    ODT_TEST_STATUS(odt_allocator_init(&allocator), ODT_OK);
    allocator.struct_size = (uint32_t)(sizeof(allocator) - 1u);
    ODT_TEST_STATUS(odt_test_allocator_validate(&allocator), ODT_INVALID_ARGUMENT);
    allocator.struct_size = (uint32_t)sizeof(allocator);
    allocator.struct_version += 1u;
    ODT_TEST_STATUS(odt_test_allocator_validate(&allocator), ODT_INVALID_ARGUMENT);

    ODT_TEST_STATUS(odt_limits_init(&limits), ODT_OK);
    limits.struct_size = (uint32_t)(sizeof(limits) - 1u);
    ODT_TEST_STATUS(odt_test_limits_validate(&limits), ODT_INVALID_ARGUMENT);
    limits.struct_size = (uint32_t)sizeof(limits);
    limits.struct_version += 1u;
    ODT_TEST_STATUS(odt_test_limits_validate(&limits), ODT_INVALID_ARGUMENT);

    ODT_TEST_STATUS(odt_build_options_init(&build_options), ODT_OK);
    build_options.struct_size = (uint32_t)(sizeof(build_options) - 1u);
    ODT_TEST_STATUS(odt_test_build_options_validate(&build_options), ODT_INVALID_ARGUMENT);
    build_options.struct_size = (uint32_t)sizeof(build_options);
    build_options.struct_version += 1u;
    ODT_TEST_STATUS(odt_test_build_options_validate(&build_options), ODT_INVALID_ARGUMENT);

    ODT_TEST_STATUS(odt_encode_options_init(&encode_options), ODT_OK);
    encode_options.struct_size = (uint32_t)(sizeof(encode_options) - 1u);
    ODT_TEST_STATUS(odt_test_encode_options_validate(&encode_options), ODT_INVALID_ARGUMENT);
    encode_options.struct_size = (uint32_t)sizeof(encode_options);
    encode_options.struct_version += 1u;
    ODT_TEST_STATUS(odt_test_encode_options_validate(&encode_options), ODT_INVALID_ARGUMENT);
    ODT_TEST_STATUS(odt_encode_options_init(&encode_options), ODT_OK);
    encode_options.format_version += 1u;
    ODT_TEST_STATUS(odt_test_encode_options_validate(&encode_options), ODT_UNSUPPORTED_FORMAT);

    ODT_TEST_STATUS(odt_load_limits_init(&load_limits), ODT_OK);
    load_limits.struct_size = (uint32_t)(sizeof(load_limits) - 1u);
    ODT_TEST_STATUS(odt_test_load_limits_validate(&load_limits), ODT_INVALID_ARGUMENT);
    load_limits.struct_size = (uint32_t)sizeof(load_limits);
    load_limits.struct_version += 1u;
    ODT_TEST_STATUS(odt_test_load_limits_validate(&load_limits), ODT_INVALID_ARGUMENT);
}

static void test_default_allocator(void) {
    odt_allocator allocator;
    void *pointer;

    ODT_TEST_STATUS(odt_allocator_init(&allocator), ODT_OK);
    pointer = allocator.allocate(allocator.context, 37u, 64u);
    ODT_TEST_CHECK(pointer != NULL);
    ODT_TEST_CHECK((uintptr_t)pointer % 64u == 0u);
    allocator.deallocate(allocator.context, pointer, 37u, 64u);

    ODT_TEST_CHECK(allocator.allocate(allocator.context, 0u, 64u) == NULL);
    ODT_TEST_CHECK(allocator.allocate(allocator.context, 37u, 0u) == NULL);
    ODT_TEST_CHECK(allocator.allocate(allocator.context, 37u, 3u) == NULL);
}

static void test_generation_allocator_ownership(void) {
    odt_test_allocator_state state = {.live = true};
    odt_allocator allocator;
    odt_generation *generation = (odt_generation *)(uintptr_t)1u;
    const size_t allocation_size = 257u;
    const size_t allocation_alignment = 64u;

    odt_test_counting_allocator_init(&allocator, &state);
    ODT_TEST_STATUS(
        odt_test_generation_create(&allocator, allocation_size, allocation_alignment, &generation),
        ODT_OK);
    ODT_TEST_CHECK(generation != NULL);
    ODT_TEST_CHECK(state.allocate_calls == 1u);
    ODT_TEST_CHECK(state.deallocate_calls == 0u);
    ODT_TEST_CHECK(state.last_size == allocation_size);
    ODT_TEST_CHECK(state.last_alignment == allocation_alignment);

    odt_generation_destroy(generation);
    ODT_TEST_CHECK(state.allocate_calls == 1u);
    ODT_TEST_CHECK(state.deallocate_calls == 1u);
    ODT_TEST_CHECK(state.active_pointer == NULL);

    state.live = false;
    odt_generation_destroy(NULL);
    ODT_TEST_CHECK(state.access_after_dead == 0u);
}

static void test_generation_allocation_failures(void) {
    odt_test_allocator_state state = {.live = true, .fail_allocation = true};
    odt_allocator allocator;
    odt_generation *generation = (odt_generation *)(uintptr_t)1u;

    odt_test_counting_allocator_init(&allocator, &state);
    ODT_TEST_STATUS(odt_test_generation_create(&allocator, 257u, 64u, &generation),
                    ODT_OUT_OF_MEMORY);
    ODT_TEST_CHECK(generation == NULL);
    ODT_TEST_CHECK(state.allocate_calls == 1u);
    ODT_TEST_CHECK(state.deallocate_calls == 0u);

    generation = (odt_generation *)(uintptr_t)1u;
    ODT_TEST_STATUS(odt_test_generation_create(&allocator, 257u, 3u, &generation),
                    ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(generation == NULL);
    ODT_TEST_CHECK(state.allocate_calls == 1u);

    ODT_TEST_STATUS(odt_test_generation_create(&allocator, 257u, 64u, NULL), ODT_INVALID_ARGUMENT);
}

int main(void) {
    ODT_TEST_CHECK(ODT_VERSION_MAJOR == 1);
    ODT_TEST_CHECK(ODT_VERSION_MINOR == 0);
    ODT_TEST_CHECK(ODT_VERSION_PATCH == 0);
    ODT_TEST_CHECK(strcmp(ODT_VERSION_STRING, "1.0.0") == 0);
    ODT_TEST_CHECK(ODT_ABI_VERSION == 1u);
    ODT_TEST_CHECK(ODT_FORMAT_VERSION == 1u);

    test_status_values_and_strings();
    test_initializers();
    test_versioned_structure_validation();
    test_default_allocator();
    test_generation_allocator_ownership();
    test_generation_allocation_failures();
    return EXIT_SUCCESS;
}
