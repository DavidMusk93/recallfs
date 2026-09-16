#include "odt_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    ODT_DEFAULT_MAX_SITES = 1000000,
    ODT_DEFAULT_MAX_FRAGMENTS = 1000000,
    ODT_DEFAULT_MAX_NODES = 65535,
    ODT_DEFAULT_MAX_DEPTH = 64,
};

#define ODT_DEFAULT_MAX_BUILD_BYTES (SIZE_C(512) * SIZE_C(1024) * SIZE_C(1024))
#define ODT_DEFAULT_MAX_BUILD_WORK UINT64_C(1000000000)
#define ODT_DEFAULT_MAX_ENCODED_BYTES UINT64_C(1073741824)
#define ODT_DEFAULT_MAX_ALLOCATION_BYTES (SIZE_C(1024) * SIZE_C(1024) * SIZE_C(1024))

#ifndef SIZE_C
#define SIZE_C(value) ((size_t)(value))
#endif

_Static_assert(sizeof(odt_allocator) <= UINT32_MAX, "odt_allocator size");
_Static_assert(sizeof(odt_limits) <= UINT32_MAX, "odt_limits size");
_Static_assert(sizeof(odt_build_options) <= UINT32_MAX, "odt_build_options size");
_Static_assert(sizeof(odt_encode_options) <= UINT32_MAX, "odt_encode_options size");
_Static_assert(sizeof(odt_load_limits) <= UINT32_MAX, "odt_load_limits size");

static bool odt_is_power_of_two(size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool odt_align_up(size_t value, size_t alignment, size_t *out_value) {
    const size_t remainder = value & (alignment - 1u);

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

static void *odt_default_allocate(void *context, size_t size, size_t alignment) {
    size_t aligned_size;

    (void)context;
    if (size == 0u || alignment < sizeof(void *) || !odt_is_power_of_two(alignment) ||
        !odt_align_up(size, alignment, &aligned_size)) {
        return NULL;
    }
    return aligned_alloc(alignment, aligned_size);
}

static void odt_default_deallocate(void *context, void *pointer, size_t size, size_t alignment) {
    (void)context;
    (void)size;
    (void)alignment;
    free(pointer);
}

static bool odt_versioned_struct_valid(uint32_t actual_size, size_t required_size,
                                       uint32_t actual_version, uint32_t required_version) {
    return required_size <= UINT32_MAX && actual_size >= (uint32_t)required_size &&
           actual_version == required_version;
}

const char *odt_status_string(odt_status status) {
    switch (status) {
    case ODT_OK:
        return "ok";
    case ODT_INVALID_ARGUMENT:
        return "invalid argument";
    case ODT_INVALID_DATA:
        return "invalid data";
    case ODT_LIMIT_EXCEEDED:
        return "configured limit exceeded";
    case ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT:
        return "unsupported numeric environment";
    case ODT_OUT_OF_MEMORY:
        return "out of memory";
    case ODT_IO_ERROR:
        return "I/O error";
    case ODT_COMMIT_UNKNOWN:
        return "commit state unknown";
    case ODT_CORRUPT_DATA:
        return "corrupt data";
    case ODT_UNSUPPORTED_FORMAT:
        return "unsupported format";
    case ODT_INTERNAL_ERROR:
        return "internal invariant failure";
    }
    return "unknown status";
}

odt_status odt_allocator_init(odt_allocator *out_allocator) {
    if (out_allocator == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    memset(out_allocator, 0, sizeof(*out_allocator));
    out_allocator->struct_size = (uint32_t)sizeof(*out_allocator);
    out_allocator->struct_version = ODT_ALLOCATOR_VERSION;
    out_allocator->allocate = odt_default_allocate;
    out_allocator->deallocate = odt_default_deallocate;
    return ODT_OK;
}

odt_status odt_limits_init(odt_limits *out_limits) {
    if (out_limits == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    memset(out_limits, 0, sizeof(*out_limits));
    out_limits->struct_size = (uint32_t)sizeof(*out_limits);
    out_limits->struct_version = ODT_LIMITS_VERSION;
    out_limits->max_sites = ODT_DEFAULT_MAX_SITES;
    out_limits->max_polygon_fragments = ODT_DEFAULT_MAX_FRAGMENTS;
    out_limits->max_internal_nodes = ODT_DEFAULT_MAX_NODES;
    out_limits->max_depth = ODT_DEFAULT_MAX_DEPTH;
    out_limits->max_build_bytes = ODT_DEFAULT_MAX_BUILD_BYTES;
    out_limits->max_build_work = ODT_DEFAULT_MAX_BUILD_WORK;
    return ODT_OK;
}

odt_status odt_build_options_init(odt_build_options *out_options) {
    if (out_options == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    memset(out_options, 0, sizeof(*out_options));
    out_options->struct_size = (uint32_t)sizeof(*out_options);
    out_options->struct_version = ODT_BUILD_OPTIONS_VERSION;
    return ODT_OK;
}

odt_status odt_encode_options_init(odt_encode_options *out_options) {
    if (out_options == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    memset(out_options, 0, sizeof(*out_options));
    out_options->struct_size = (uint32_t)sizeof(*out_options);
    out_options->struct_version = ODT_ENCODE_OPTIONS_VERSION;
    out_options->format_version = ODT_FORMAT_VERSION;
    return ODT_OK;
}

odt_status odt_load_limits_init(odt_load_limits *out_limits) {
    if (out_limits == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    memset(out_limits, 0, sizeof(*out_limits));
    out_limits->struct_size = (uint32_t)sizeof(*out_limits);
    out_limits->struct_version = ODT_LOAD_LIMITS_VERSION;
    out_limits->max_encoded_bytes = ODT_DEFAULT_MAX_ENCODED_BYTES;
    out_limits->max_sites = ODT_DEFAULT_MAX_SITES;
    out_limits->max_internal_nodes = ODT_DEFAULT_MAX_NODES;
    out_limits->max_leaves = ODT_DEFAULT_MAX_SITES;
    out_limits->max_depth = ODT_DEFAULT_MAX_DEPTH;
    out_limits->max_allocation_bytes = ODT_DEFAULT_MAX_ALLOCATION_BYTES;
    return ODT_OK;
}

odt_status odt_internal_allocator_validate(const odt_allocator *allocator) {
    if (allocator == NULL ||
        !odt_versioned_struct_valid(allocator->struct_size, sizeof(*allocator),
                                    allocator->struct_version, ODT_ALLOCATOR_VERSION) ||
        allocator->allocate == NULL || allocator->deallocate == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    return ODT_OK;
}

odt_status odt_internal_limits_validate(const odt_limits *limits) {
    if (limits == NULL ||
        !odt_versioned_struct_valid(limits->struct_size, sizeof(*limits), limits->struct_version,
                                    ODT_LIMITS_VERSION) ||
        limits->reserved_0 != 0u || limits->max_sites == 0u ||
        limits->max_polygon_fragments == 0u || limits->max_internal_nodes == 0u ||
        limits->max_depth == 0u || limits->max_build_bytes == 0u || limits->max_build_work == 0u) {
        return ODT_INVALID_ARGUMENT;
    }
    return ODT_OK;
}

odt_status odt_internal_build_options_validate(const odt_build_options *options) {
    if (options == NULL ||
        !odt_versioned_struct_valid(options->struct_size, sizeof(*options), options->struct_version,
                                    ODT_BUILD_OPTIONS_VERSION)) {
        return ODT_INVALID_ARGUMENT;
    }
    return ODT_OK;
}

odt_status odt_internal_encode_options_validate(const odt_encode_options *options) {
    if (options == NULL ||
        !odt_versioned_struct_valid(options->struct_size, sizeof(*options), options->struct_version,
                                    ODT_ENCODE_OPTIONS_VERSION) ||
        options->reserved_0 != 0u) {
        return ODT_INVALID_ARGUMENT;
    }
    if (options->format_version != ODT_FORMAT_VERSION) {
        return ODT_UNSUPPORTED_FORMAT;
    }
    return ODT_OK;
}

odt_status odt_internal_load_limits_validate(const odt_load_limits *limits) {
    if (limits == NULL ||
        !odt_versioned_struct_valid(limits->struct_size, sizeof(*limits), limits->struct_version,
                                    ODT_LOAD_LIMITS_VERSION) ||
        limits->reserved_0 != 0u || limits->max_encoded_bytes == 0u || limits->max_sites == 0u ||
        limits->max_internal_nodes == 0u || limits->max_leaves == 0u || limits->max_depth == 0u ||
        limits->max_allocation_bytes == 0u) {
        return ODT_INVALID_ARGUMENT;
    }
    return ODT_OK;
}

odt_status odt_internal_generation_allocate(const odt_allocator *allocator, size_t allocation_size,
                                            size_t allocation_alignment,
                                            odt_generation **out_generation) {
    odt_generation *generation;
    odt_status status;

    if (out_generation == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_generation = NULL;
    status = odt_internal_allocator_validate(allocator);
    if (status != ODT_OK || allocation_size < sizeof(*generation) ||
        allocation_alignment < _Alignof(odt_generation) ||
        !odt_is_power_of_two(allocation_alignment)) {
        return ODT_INVALID_ARGUMENT;
    }
    generation = allocator->allocate(allocator->context, allocation_size, allocation_alignment);
    if (generation == NULL) {
        return ODT_OUT_OF_MEMORY;
    }
    generation->allocator = *allocator;
    generation->allocation_size = allocation_size;
    generation->allocation_alignment = allocation_alignment;
    *out_generation = generation;
    return ODT_OK;
}

void odt_generation_destroy(odt_generation *generation) {
    odt_allocator allocator;
    size_t allocation_size;
    size_t allocation_alignment;

    if (generation == NULL) {
        return;
    }
    allocator = generation->allocator;
    allocation_size = generation->allocation_size;
    allocation_alignment = generation->allocation_alignment;
    allocator.deallocate(allocator.context, generation, allocation_size, allocation_alignment);
}
