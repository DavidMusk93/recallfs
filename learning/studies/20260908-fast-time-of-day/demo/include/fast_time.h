#ifndef FAST_TIME_H
#define FAST_TIME_H

#include <stdint.h>

typedef struct {
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} hms_time;

typedef hms_time (*hms_converter)(uint32_t day_second);

hms_time hms_traditional(uint32_t day_second);
hms_time hms_parallel_div(uint32_t day_second);
hms_time hms_parallel_fixed(uint32_t day_second);
hms_time hms_hi_low(uint32_t day_second);
hms_time hms_base64(uint32_t day_second);

#endif
