#include "fast_time.h"

#include <stdint.h>

enum {
    MINUTE_RECIPROCAL_32 = 71582789U,
    HOUR_RECIPROCAL_32 = 1193047U,
};

hms_time hms_traditional(uint32_t day_second) {
    uint32_t hour = day_second / 3600U;
    uint32_t hour_remainder = day_second % 3600U;
    uint32_t minute = hour_remainder / 60U;
    uint32_t second = hour_remainder % 60U;

    hms_time result = {
        .hour = hour,
        .minute = minute,
        .second = second,
    };
    return result;
}

hms_time hms_parallel_div(uint32_t day_second) {
    uint32_t total_minutes = day_second / 60U;
    uint32_t hour = day_second / 3600U;

    hms_time result = {
        .hour = hour,
        .minute = total_minutes - hour * 60U,
        .second = day_second - total_minutes * 60U,
    };
    return result;
}

hms_time hms_parallel_fixed(uint32_t day_second) {
    uint32_t total_minutes =
        (uint32_t)(((uint64_t)day_second * MINUTE_RECIPROCAL_32) >> 32);
    uint32_t hour =
        (uint32_t)(((uint64_t)day_second * HOUR_RECIPROCAL_32) >> 32);

    hms_time result = {
        .hour = hour,
        .minute = total_minutes - hour * 60U,
        .second = day_second - total_minutes * 60U,
    };
    return result;
}

hms_time hms_hi_low(uint32_t day_second) {
    /*
     * The high product half is the quotient. The low half tracks fractional
     * progress toward the next quotient; multiplying it by 60 recovers the
     * base-60 remainder without a second division.
     */
    uint64_t hour_product = (uint64_t)day_second * HOUR_RECIPROCAL_32;
    uint32_t minute_progress = (uint32_t)hour_product;
    uint32_t second_progress =
        (uint32_t)((uint64_t)day_second * MINUTE_RECIPROCAL_32);

    hms_time result = {
        .hour = (uint32_t)(hour_product >> 32),
        .minute = (uint32_t)(((uint64_t)minute_progress * 60U) >> 32),
        .second = (uint32_t)(((uint64_t)second_progress * 60U) >> 32),
    };
    return result;
}

hms_time hms_base64(uint32_t day_second) {
    uint32_t total_minutes =
        (uint32_t)(((uint64_t)day_second * MINUTE_RECIPROCAL_32) >> 32);
    uint32_t hour =
        (uint32_t)(((uint64_t)day_second * HOUR_RECIPROCAL_32) >> 32);

    /*
     * For D=60 and c=4:
     * x mod D = (x + c * floor(x / D)) mod (D + c).
     * Modulo 64 is a mask, and x + 4*y maps to one add-with-shift on common
     * scalar ISAs.
     */
    hms_time result = {
        .hour = hour,
        .minute = (total_minutes + (hour << 2)) & 63U,
        .second = (day_second + (total_minutes << 2)) & 63U,
    };
    return result;
}
