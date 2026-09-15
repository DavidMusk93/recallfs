#ifndef POINT_LOCATION_EXAMPLE_H
#define POINT_LOCATION_EXAMPLE_H

#include "odt.h"

enum point_location_region {
    POINT_LOCATION_OUTSIDE = -1,
    POINT_LOCATION_A = 0,
    POINT_LOCATION_B = 1,
    POINT_LOCATION_C = 2,
};

const struct odt_tree *point_location_example_tree(void);
const char *point_location_region_name(int32_t region);

#endif
