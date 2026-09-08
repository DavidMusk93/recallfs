#include "fast_time.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static hms_time reference_hms(uint32_t day_second) {
    hms_time result = {
        .hour = day_second / 3600U,
        .minute = (day_second % 3600U) / 60U,
        .second = day_second % 60U,
    };
    return result;
}

static int equal_hms(hms_time left, hms_time right) {
    return left.hour == right.hour && left.minute == right.minute &&
           left.second == right.second;
}

static int check_value(const char *name, hms_converter converter,
                       uint32_t day_second) {
    hms_time expected = reference_hms(day_second);
    hms_time actual = converter(day_second);

    if (equal_hms(expected, actual)) {
        return 1;
    }

    fprintf(stderr,
            "%s mismatch at %" PRIu32 ": expected %" PRIu32 ":%02" PRIu32
            ":%02" PRIu32 ", got %" PRIu32 ":%02" PRIu32 ":%02" PRIu32
            "\n",
            name, day_second, expected.hour, expected.minute, expected.second,
            actual.hour, actual.minute, actual.second);
    return 0;
}

static int check_range(const char *name, hms_converter converter,
                       uint32_t inclusive_max) {
    for (uint32_t value = 0; value <= inclusive_max; ++value) {
        if (!check_value(name, converter, value)) {
            return 0;
        }
    }
    return 1;
}

static int expect_first_out_of_range_mismatch(const char *name,
                                               hms_converter converter,
                                               uint32_t first_invalid) {
    hms_time expected = reference_hms(first_invalid);
    hms_time actual = converter(first_invalid);

    if (!equal_hms(expected, actual)) {
        return 1;
    }

    fprintf(stderr, "%s unexpectedly remained exact at %" PRIu32 "\n", name,
            first_invalid);
    return 0;
}

static int check_full_range_samples(const char *name, hms_converter converter) {
    static const uint32_t samples[] = {
        0U,          1U,          59U,         60U,
        3599U,       3600U,       86399U,      86400U,
        2255818U,    2257198U,    UINT32_MAX - 1U,
        UINT32_MAX,
    };

    for (size_t index = 0; index < sizeof(samples) / sizeof(samples[0]);
         ++index) {
        if (!check_value(name, converter, samples[index])) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    struct {
        const char *name;
        hms_converter converter;
    } daily_variants[] = {
        {"traditional", hms_traditional},
        {"parallel_div", hms_parallel_div},
        {"parallel_fixed", hms_parallel_fixed},
        {"hi_low", hms_hi_low},
        {"base64", hms_base64},
    };

    for (size_t index = 0;
         index < sizeof(daily_variants) / sizeof(daily_variants[0]); ++index) {
        if (!check_range(daily_variants[index].name,
                         daily_variants[index].converter, 86399U)) {
            return 1;
        }
    }

    if (!check_full_range_samples("traditional", hms_traditional) ||
        !check_full_range_samples("parallel_div", hms_parallel_div) ||
        !check_range("parallel_fixed", hms_parallel_fixed, 2257198U) ||
        !check_range("hi_low", hms_hi_low, 2255818U) ||
        !check_range("base64", hms_base64, 2257198U)) {
        return 1;
    }

    if (!expect_first_out_of_range_mismatch("parallel_fixed",
                                            hms_parallel_fixed, 2257199U) ||
        !expect_first_out_of_range_mismatch("hi_low", hms_hi_low, 2255819U) ||
        !expect_first_out_of_range_mismatch("base64", hms_base64, 2257199U)) {
        return 1;
    }

    puts("FIL-C correctness passed: 5 variants, exhaustive declared ranges");
    return 0;
}
