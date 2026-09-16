#ifndef ODT_INTERNAL_H
#define ODT_INTERNAL_H

#include "odt.h"

#include <stddef.h>

struct odt_generation {
    odt_allocator allocator;
    size_t allocation_size;
    size_t allocation_alignment;
};

struct odt_builder;

odt_status odt_internal_allocator_validate(const odt_allocator *allocator);
odt_status odt_internal_limits_validate(const odt_limits *limits);
odt_status odt_internal_build_options_validate(const odt_build_options *options);
odt_status odt_internal_encode_options_validate(const odt_encode_options *options);
odt_status odt_internal_load_limits_validate(const odt_load_limits *limits);

odt_status odt_internal_generation_allocate(const odt_allocator *allocator,
                                            size_t allocation_size,
                                            size_t allocation_alignment,
                                            odt_generation **out_generation);

#endif
