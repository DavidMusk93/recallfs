#include "odt_internal.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
    ODT_BINARY64_SIGNIFICAND_BITS = 53,
    ODT_BINARY64_TO_INTEGER_SHIFT = 1074,
    ODT_BINARY64_MAX_INTEGER_SHIFT = 2045,
    ODT_BINARY64_COORDINATE_BITS = ODT_BINARY64_SIGNIFICAND_BITS + ODT_BINARY64_MAX_INTEGER_SHIFT,
    ODT_EXACT_LINE_COEFFICIENT_BITS = 2 * ODT_BINARY64_COORDINATE_BITS + 1,
    ODT_EXACT_DETERMINANT_BITS = 3 * ODT_EXACT_LINE_COEFFICIENT_BITS + 3,
    ODT_EXACT_LIMB_BITS = 32,
    /*
     * Two guard limbs let schoolbook multiplication retain a sparse leading
     * limb while still covering the 12,594-bit determinant bound.
     */
    ODT_EXACT_LIMBS =
        (ODT_EXACT_DETERMINANT_BITS + ODT_EXACT_LIMB_BITS - 1) / ODT_EXACT_LIMB_BITS + 2,
    ODT_LINE_COMMON_SCALE_SHIFT = 2 * ODT_BINARY64_TO_INTEGER_SHIFT
};

typedef struct odt_bigint {
    int sign;
    size_t used;
    uint32_t limbs[ODT_EXACT_LIMBS];
} odt_bigint;

typedef struct odt_exact_line {
    odt_bigint a;
    odt_bigint b;
    odt_bigint c;
} odt_exact_line;

typedef struct odt_interval {
    double lower;
    double upper;
} odt_interval;

typedef struct odt_filter_line {
    odt_interval a;
    odt_interval b;
    odt_interval c;
} odt_filter_line;

_Static_assert(CHAR_BIT == 8, "odt exact arithmetic requires 8-bit bytes");
_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64, "odt requires uint64_t");
_Static_assert(ODT_EXACT_LIMBS *ODT_EXACT_LIMB_BITS >= ODT_EXACT_DETERMINANT_BITS,
               "exact determinant capacity");
_Static_assert(ODT_BINARY64_COORDINATE_BITS == 2098, "binary64 full-range coordinate capacity");

static void odt_big_zero(odt_bigint *value) {
    memset(value, 0, sizeof(*value));
}

static void odt_big_normalize(odt_bigint *value) {
    while (value->used != 0u && value->limbs[value->used - 1u] == 0u) {
        value->used -= 1u;
    }
    if (value->used == 0u) {
        value->sign = 0;
    }
}

static void odt_big_from_u64(uint64_t magnitude, int sign, odt_bigint *out_value) {
    odt_big_zero(out_value);
    if (magnitude == 0u) {
        return;
    }
    out_value->limbs[0] = (uint32_t)magnitude;
    out_value->limbs[1] = (uint32_t)(magnitude >> 32u);
    out_value->used = out_value->limbs[1] == 0u ? 1u : 2u;
    out_value->sign = sign;
}

static bool odt_big_shift_left(const odt_bigint *value, size_t shift, odt_bigint *out_value) {
    const size_t word_shift = shift / ODT_EXACT_LIMB_BITS;
    const unsigned bit_shift = (unsigned)(shift % ODT_EXACT_LIMB_BITS);
    size_t index;

    odt_big_zero(out_value);
    if (value->sign == 0) {
        return true;
    }
    if (word_shift >= ODT_EXACT_LIMBS ||
        value->used > ODT_EXACT_LIMBS - word_shift - (bit_shift == 0u ? 0u : 1u)) {
        return false;
    }
    for (index = 0u; index < value->used; ++index) {
        const uint64_t shifted = (uint64_t)value->limbs[index] << bit_shift;
        const size_t destination = index + word_shift;

        out_value->limbs[destination] |= (uint32_t)shifted;
        if (bit_shift != 0u) {
            out_value->limbs[destination + 1u] |= (uint32_t)(shifted >> 32u);
        }
    }
    out_value->used = value->used + word_shift + (bit_shift == 0u ? 0u : 1u);
    out_value->sign = value->sign;
    odt_big_normalize(out_value);
    return true;
}

static int odt_big_compare_magnitude(const odt_bigint *first, const odt_bigint *second) {
    size_t index;

    if (first->used != second->used) {
        return first->used < second->used ? -1 : 1;
    }
    index = first->used;
    while (index != 0u) {
        index -= 1u;
        if (first->limbs[index] != second->limbs[index]) {
            return first->limbs[index] < second->limbs[index] ? -1 : 1;
        }
    }
    return 0;
}

static bool odt_big_add_magnitude(const odt_bigint *first, const odt_bigint *second,
                                  odt_bigint *out_value) {
    const size_t count = first->used > second->used ? first->used : second->used;
    uint64_t carry = 0u;
    size_t index;

    odt_big_zero(out_value);
    for (index = 0u; index < count; ++index) {
        const uint64_t first_limb = index < first->used ? first->limbs[index] : 0u;
        const uint64_t second_limb = index < second->used ? second->limbs[index] : 0u;
        const uint64_t sum = first_limb + second_limb + carry;

        out_value->limbs[index] = (uint32_t)sum;
        carry = sum >> 32u;
    }
    if (carry != 0u) {
        if (count == ODT_EXACT_LIMBS) {
            return false;
        }
        out_value->limbs[count] = (uint32_t)carry;
        out_value->used = count + 1u;
    } else {
        out_value->used = count;
    }
    return true;
}

static void odt_big_subtract_magnitude(const odt_bigint *larger, const odt_bigint *smaller,
                                       odt_bigint *out_value) {
    uint64_t borrow = 0u;
    size_t index;

    odt_big_zero(out_value);
    for (index = 0u; index < larger->used; ++index) {
        const uint64_t larger_limb = larger->limbs[index];
        const uint64_t smaller_limb = index < smaller->used ? smaller->limbs[index] : 0u;
        const uint64_t subtrahend = smaller_limb + borrow;

        if (larger_limb < subtrahend) {
            out_value->limbs[index] = (uint32_t)((UINT64_C(1) << 32u) + larger_limb - subtrahend);
            borrow = 1u;
        } else {
            out_value->limbs[index] = (uint32_t)(larger_limb - subtrahend);
            borrow = 0u;
        }
    }
    out_value->used = larger->used;
    odt_big_normalize(out_value);
}

static bool odt_big_add(const odt_bigint *first, const odt_bigint *second, odt_bigint *out_value) {
    int comparison;

    if (first->sign == 0) {
        *out_value = *second;
        return true;
    }
    if (second->sign == 0) {
        *out_value = *first;
        return true;
    }
    if (first->sign == second->sign) {
        if (!odt_big_add_magnitude(first, second, out_value)) {
            return false;
        }
        out_value->sign = first->sign;
        return true;
    }
    comparison = odt_big_compare_magnitude(first, second);
    if (comparison == 0) {
        odt_big_zero(out_value);
    } else if (comparison > 0) {
        odt_big_subtract_magnitude(first, second, out_value);
        out_value->sign = first->sign;
    } else {
        odt_big_subtract_magnitude(second, first, out_value);
        out_value->sign = second->sign;
    }
    return true;
}

static bool odt_big_subtract(const odt_bigint *first, const odt_bigint *second,
                             odt_bigint *out_value) {
    odt_bigint negated = *second;

    negated.sign = -negated.sign;
    return odt_big_add(first, &negated, out_value);
}

static bool odt_big_multiply(const odt_bigint *first, const odt_bigint *second,
                             odt_bigint *out_value) {
    size_t first_index;

    odt_big_zero(out_value);
    if (first->sign == 0 || second->sign == 0) {
        return true;
    }
    if (first->used > ODT_EXACT_LIMBS - second->used) {
        return false;
    }
    for (first_index = 0u; first_index < first->used; ++first_index) {
        uint64_t carry = 0u;
        size_t second_index;

        for (second_index = 0u; second_index < second->used; ++second_index) {
            const size_t destination = first_index + second_index;
            const uint64_t product =
                (uint64_t)first->limbs[first_index] * (uint64_t)second->limbs[second_index];
            const uint64_t sum = (uint64_t)out_value->limbs[destination] + product + carry;

            out_value->limbs[destination] = (uint32_t)sum;
            carry = sum >> 32u;
        }
        if (first_index + second->used < ODT_EXACT_LIMBS) {
            out_value->limbs[first_index + second->used] = (uint32_t)carry;
        } else if (carry != 0u) {
            return false;
        }
    }
    out_value->used =
        first->used + second->used < ODT_EXACT_LIMBS ? first->used + second->used : ODT_EXACT_LIMBS;
    out_value->sign = first->sign * second->sign;
    odt_big_normalize(out_value);
    return true;
}

static bool odt_big_multiply_three(const odt_bigint *first, const odt_bigint *second,
                                   const odt_bigint *third, odt_bigint *out_value) {
    odt_bigint product;

    return odt_big_multiply(first, second, &product) &&
           odt_big_multiply(&product, third, out_value);
}

static bool odt_big_add_term(odt_bigint *accumulator, const odt_bigint *first,
                             const odt_bigint *second, const odt_bigint *third, int term_sign) {
    odt_bigint term;
    odt_bigint sum;

    if (!odt_big_multiply_three(first, second, third, &term)) {
        return false;
    }
    term.sign *= term_sign;
    if (!odt_big_add(accumulator, &term, &sum)) {
        return false;
    }
    *accumulator = sum;
    return true;
}

static bool odt_big_from_binary64(double value, odt_bigint *out_value) {
    uint64_t bits;
    uint64_t significand;
    unsigned exponent_bits;
    size_t shift;
    odt_bigint unshifted;

    memcpy(&bits, &value, sizeof(bits));
    exponent_bits = (unsigned)((bits >> 52u) & UINT64_C(0x7ff));
    if (exponent_bits == 0x7ffu) {
        return false;
    }
    significand = bits & UINT64_C(0x000fffffffffffff);
    if (exponent_bits != 0u) {
        significand |= UINT64_C(0x0010000000000000);
        shift = (size_t)(exponent_bits - 1u);
    } else {
        shift = 0u;
    }
    odt_big_from_u64(significand, (bits >> 63u) == 0u ? 1 : -1, &unshifted);
    return odt_big_shift_left(&unshifted, shift, out_value);
}

static bool odt_exact_squared_axis(double point, double site, odt_bigint *out_square) {
    odt_bigint point_integer;
    odt_bigint site_integer;
    odt_bigint difference;

    return odt_big_from_binary64(point, &point_integer) &&
           odt_big_from_binary64(site, &site_integer) &&
           odt_big_subtract(&point_integer, &site_integer, &difference) &&
           odt_big_multiply(&difference, &difference, out_square);
}

static bool odt_exact_squared_distance(const odt_point *point, const odt_site *site,
                                       odt_bigint *out_distance) {
    odt_bigint x_square;
    odt_bigint y_square;

    return odt_exact_squared_axis(point->x, site->x, &x_square) &&
           odt_exact_squared_axis(point->y, site->y, &y_square) &&
           odt_big_add(&x_square, &y_square, out_distance);
}

static bool odt_line_ref_equal(const odt_internal_line_ref *first,
                               const odt_internal_line_ref *second) {
    return first->kind == second->kind && first->first_ordinal == second->first_ordinal &&
           first->second_ordinal == second->second_ordinal;
}

static bool odt_exact_domain_line(const odt_internal_numeric_context *context,
                                  const odt_internal_line_ref *reference,
                                  odt_exact_line *out_line) {
    odt_bigint one;
    odt_bigint coordinate;
    odt_bigint scaled_coordinate;
    double coordinate_value;
    int a_sign = 0;
    int b_sign = 0;
    int c_sign = 1;

    switch (reference->kind) {
    case ODT_INTERNAL_LINE_DOMAIN_MIN_X:
        coordinate_value = context->domain.min_x;
        a_sign = -1;
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_X:
        coordinate_value = context->domain.max_x;
        a_sign = 1;
        c_sign = -1;
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MIN_Y:
        coordinate_value = context->domain.min_y;
        b_sign = -1;
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_Y:
        coordinate_value = context->domain.max_y;
        b_sign = 1;
        c_sign = -1;
        break;
    case ODT_INTERNAL_LINE_BISECTOR:
        return false;
    }

    odt_big_from_u64(1u, 1, &one);
    odt_big_zero(&out_line->a);
    odt_big_zero(&out_line->b);
    if (a_sign != 0) {
        if (!odt_big_shift_left(&one, ODT_LINE_COMMON_SCALE_SHIFT, &out_line->a)) {
            return false;
        }
        out_line->a.sign = a_sign;
    }
    if (b_sign != 0) {
        if (!odt_big_shift_left(&one, ODT_LINE_COMMON_SCALE_SHIFT, &out_line->b)) {
            return false;
        }
        out_line->b.sign = b_sign;
    }
    if (!odt_big_from_binary64(coordinate_value, &coordinate) ||
        !odt_big_shift_left(&coordinate, ODT_BINARY64_TO_INTEGER_SHIFT, &scaled_coordinate)) {
        return false;
    }
    scaled_coordinate.sign *= c_sign;
    out_line->c = scaled_coordinate;
    return true;
}

static bool odt_exact_bisector_line(const odt_internal_numeric_context *context,
                                    const odt_internal_line_ref *reference,
                                    odt_exact_line *out_line) {
    const odt_site *first;
    const odt_site *second;
    odt_bigint first_x;
    odt_bigint first_y;
    odt_bigint second_x;
    odt_bigint second_y;
    odt_bigint difference;
    odt_bigint first_x_square;
    odt_bigint first_y_square;
    odt_bigint second_x_square;
    odt_bigint second_y_square;
    odt_bigint first_norm;
    odt_bigint second_norm;

    if (reference->first_ordinal >= reference->second_ordinal ||
        reference->second_ordinal >= context->site_count) {
        return false;
    }
    first = &context->sites[reference->first_ordinal];
    second = &context->sites[reference->second_ordinal];
    if (!odt_big_from_binary64(first->x, &first_x) || !odt_big_from_binary64(first->y, &first_y) ||
        !odt_big_from_binary64(second->x, &second_x) ||
        !odt_big_from_binary64(second->y, &second_y) ||
        !odt_big_subtract(&second_x, &first_x, &difference) ||
        !odt_big_shift_left(&difference, ODT_BINARY64_TO_INTEGER_SHIFT + 1u, &out_line->a) ||
        !odt_big_subtract(&second_y, &first_y, &difference) ||
        !odt_big_shift_left(&difference, ODT_BINARY64_TO_INTEGER_SHIFT + 1u, &out_line->b) ||
        !odt_big_multiply(&first_x, &first_x, &first_x_square) ||
        !odt_big_multiply(&first_y, &first_y, &first_y_square) ||
        !odt_big_multiply(&second_x, &second_x, &second_x_square) ||
        !odt_big_multiply(&second_y, &second_y, &second_y_square) ||
        !odt_big_add(&first_x_square, &first_y_square, &first_norm) ||
        !odt_big_add(&second_x_square, &second_y_square, &second_norm) ||
        !odt_big_subtract(&first_norm, &second_norm, &out_line->c)) {
        return false;
    }
    return true;
}

static bool odt_exact_line_build(const odt_internal_numeric_context *context,
                                 const odt_internal_line_ref *reference, odt_exact_line *out_line) {
    if (reference->kind == ODT_INTERNAL_LINE_BISECTOR) {
        return odt_exact_bisector_line(context, reference, out_line);
    }
    return odt_exact_domain_line(context, reference, out_line);
}

static bool odt_exact_line_denominator(const odt_exact_line *first, const odt_exact_line *second,
                                       odt_bigint *out_denominator) {
    odt_bigint first_product;
    odt_bigint second_product;

    return odt_big_multiply(&first->a, &second->b, &first_product) &&
           odt_big_multiply(&first->b, &second->a, &second_product) &&
           odt_big_subtract(&first_product, &second_product, out_denominator);
}

static bool odt_exact_line_determinant(const odt_exact_line *first, const odt_exact_line *second,
                                       const odt_exact_line *third, odt_bigint *out_determinant) {
    odt_bigint determinant;

    odt_big_zero(&determinant);
    if (!odt_big_add_term(&determinant, &first->a, &second->b, &third->c, 1) ||
        !odt_big_add_term(&determinant, &first->b, &second->c, &third->a, 1) ||
        !odt_big_add_term(&determinant, &first->c, &second->a, &third->b, 1) ||
        !odt_big_add_term(&determinant, &first->c, &second->b, &third->a, -1) ||
        !odt_big_add_term(&determinant, &first->b, &second->a, &third->c, -1) ||
        !odt_big_add_term(&determinant, &first->a, &second->c, &third->b, -1)) {
        return false;
    }
    *out_determinant = determinant;
    return true;
}

static double odt_rounded_add(double first, double second) {
    volatile double result = first + second;

    return result;
}

static double odt_rounded_subtract(double first, double second) {
    volatile double result = first - second;

    return result;
}

static double odt_rounded_multiply(double first, double second) {
    volatile double result = first * second;

    return result;
}

static odt_interval odt_interval_point(double value) {
    odt_interval result = {value, value};

    return result;
}

static bool odt_interval_expand(double lower, double upper, odt_interval *out_interval) {
    if (!isfinite(lower) || !isfinite(upper)) {
        return false;
    }
    out_interval->lower = lower == 0.0 ? -DBL_TRUE_MIN : nextafter(lower, -INFINITY);
    out_interval->upper = upper == 0.0 ? DBL_TRUE_MIN : nextafter(upper, INFINITY);
    return true;
}

static bool odt_interval_add(odt_interval first, odt_interval second, odt_interval *out_interval) {
    return odt_interval_expand(odt_rounded_add(first.lower, second.lower),
                               odt_rounded_add(first.upper, second.upper), out_interval);
}

static bool odt_interval_subtract(odt_interval first, odt_interval second,
                                  odt_interval *out_interval) {
    return odt_interval_expand(odt_rounded_subtract(first.lower, second.upper),
                               odt_rounded_subtract(first.upper, second.lower), out_interval);
}

static bool odt_interval_multiply(odt_interval first, odt_interval second,
                                  odt_interval *out_interval) {
    const double products[4] = {
        odt_rounded_multiply(first.lower, second.lower),
        odt_rounded_multiply(first.lower, second.upper),
        odt_rounded_multiply(first.upper, second.lower),
        odt_rounded_multiply(first.upper, second.upper),
    };
    double lower = products[0];
    double upper = products[0];
    size_t index;

    for (index = 1u; index < 4u; ++index) {
        if (products[index] < lower) {
            lower = products[index];
        }
        if (products[index] > upper) {
            upper = products[index];
        }
    }
    return odt_interval_expand(lower, upper, out_interval);
}

static int odt_interval_sign(odt_interval interval) {
    if (interval.lower > 0.0) {
        return 1;
    }
    if (interval.upper < 0.0) {
        return -1;
    }
    return 0;
}

static bool odt_filter_scaled_value(const odt_internal_numeric_context *context, double value,
                                    double *out_value) {
    double scaled;
    double restored;

    scaled = scalbn(value, context->filter_scale_exponent);
    restored = scalbn(scaled, -context->filter_scale_exponent);
    if (!isfinite(scaled) || restored != value || (value != 0.0 && scaled == 0.0)) {
        return false;
    }
    *out_value = scaled;
    return true;
}

static bool odt_filter_domain_line(const odt_internal_numeric_context *context,
                                   const odt_internal_line_ref *reference,
                                   odt_filter_line *out_line) {
    double coordinate;

    out_line->a = odt_interval_point(0.0);
    out_line->b = odt_interval_point(0.0);
    switch (reference->kind) {
    case ODT_INTERNAL_LINE_DOMAIN_MIN_X:
        out_line->a = odt_interval_point(-1.0);
        if (!odt_filter_scaled_value(context, context->domain.min_x, &coordinate)) {
            return false;
        }
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_X:
        out_line->a = odt_interval_point(1.0);
        if (!odt_filter_scaled_value(context, -context->domain.max_x, &coordinate)) {
            return false;
        }
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MIN_Y:
        out_line->b = odt_interval_point(-1.0);
        if (!odt_filter_scaled_value(context, context->domain.min_y, &coordinate)) {
            return false;
        }
        break;
    case ODT_INTERNAL_LINE_DOMAIN_MAX_Y:
        out_line->b = odt_interval_point(1.0);
        if (!odt_filter_scaled_value(context, -context->domain.max_y, &coordinate)) {
            return false;
        }
        break;
    case ODT_INTERNAL_LINE_BISECTOR:
        return false;
    }
    out_line->c = odt_interval_point(coordinate);
    return true;
}

static bool odt_filter_bisector_line(const odt_internal_numeric_context *context,
                                     const odt_internal_line_ref *reference,
                                     odt_filter_line *out_line) {
    const odt_site *first;
    const odt_site *second;
    double first_x;
    double first_y;
    double second_x;
    double second_y;
    odt_interval difference;
    odt_interval first_x_square;
    odt_interval first_y_square;
    odt_interval second_x_square;
    odt_interval second_y_square;
    odt_interval first_norm;
    odt_interval second_norm;
    const odt_interval two = {2.0, 2.0};

    if (reference->first_ordinal >= reference->second_ordinal ||
        reference->second_ordinal >= context->site_count) {
        return false;
    }
    first = &context->sites[reference->first_ordinal];
    second = &context->sites[reference->second_ordinal];
    if (!odt_filter_scaled_value(context, first->x, &first_x) ||
        !odt_filter_scaled_value(context, first->y, &first_y) ||
        !odt_filter_scaled_value(context, second->x, &second_x) ||
        !odt_filter_scaled_value(context, second->y, &second_y) ||
        !odt_interval_subtract(odt_interval_point(second_x), odt_interval_point(first_x),
                               &difference) ||
        !odt_interval_multiply(two, difference, &out_line->a) ||
        !odt_interval_subtract(odt_interval_point(second_y), odt_interval_point(first_y),
                               &difference) ||
        !odt_interval_multiply(two, difference, &out_line->b) ||
        !odt_interval_multiply(odt_interval_point(first_x), odt_interval_point(first_x),
                               &first_x_square) ||
        !odt_interval_multiply(odt_interval_point(first_y), odt_interval_point(first_y),
                               &first_y_square) ||
        !odt_interval_multiply(odt_interval_point(second_x), odt_interval_point(second_x),
                               &second_x_square) ||
        !odt_interval_multiply(odt_interval_point(second_y), odt_interval_point(second_y),
                               &second_y_square) ||
        !odt_interval_add(first_x_square, first_y_square, &first_norm) ||
        !odt_interval_add(second_x_square, second_y_square, &second_norm) ||
        !odt_interval_subtract(first_norm, second_norm, &out_line->c)) {
        return false;
    }
    return true;
}

static bool odt_filter_line_build(const odt_internal_numeric_context *context,
                                  const odt_internal_line_ref *reference,
                                  odt_filter_line *out_line) {
    if (!context->filter_enabled) {
        return false;
    }
    if (reference->kind == ODT_INTERNAL_LINE_BISECTOR) {
        return odt_filter_bisector_line(context, reference, out_line);
    }
    return odt_filter_domain_line(context, reference, out_line);
}

static bool odt_interval_multiply_three(odt_interval first, odt_interval second, odt_interval third,
                                        odt_interval *out_value) {
    odt_interval product;

    return odt_interval_multiply(first, second, &product) &&
           odt_interval_multiply(product, third, out_value);
}

static bool odt_filter_line_denominator(const odt_filter_line *first, const odt_filter_line *second,
                                        odt_interval *out_denominator) {
    odt_interval first_product;
    odt_interval second_product;

    return odt_interval_multiply(first->a, second->b, &first_product) &&
           odt_interval_multiply(first->b, second->a, &second_product) &&
           odt_interval_subtract(first_product, second_product, out_denominator);
}

static bool odt_filter_add_term(odt_interval *accumulator, odt_interval first, odt_interval second,
                                odt_interval third, bool subtract) {
    odt_interval term;
    odt_interval result;

    if (!odt_interval_multiply_three(first, second, third, &term)) {
        return false;
    }
    if (subtract) {
        if (!odt_interval_subtract(*accumulator, term, &result)) {
            return false;
        }
    } else if (!odt_interval_add(*accumulator, term, &result)) {
        return false;
    }
    *accumulator = result;
    return true;
}

static bool odt_filter_line_determinant(const odt_filter_line *first, const odt_filter_line *second,
                                        const odt_filter_line *third,
                                        odt_interval *out_determinant) {
    odt_interval determinant = {0.0, 0.0};

    if (!odt_filter_add_term(&determinant, first->a, second->b, third->c, false) ||
        !odt_filter_add_term(&determinant, first->b, second->c, third->a, false) ||
        !odt_filter_add_term(&determinant, first->c, second->a, third->b, false) ||
        !odt_filter_add_term(&determinant, first->c, second->b, third->a, true) ||
        !odt_filter_add_term(&determinant, first->b, second->a, third->c, true) ||
        !odt_filter_add_term(&determinant, first->a, second->c, third->b, true)) {
        return false;
    }
    *out_determinant = determinant;
    return true;
}

odt_status odt_internal_numeric_environment_check(void) {
    volatile double minimum_normal = DBL_MIN;
    volatile double half_minimum = minimum_normal * 0.5;

    if (CHAR_BIT != 8 || sizeof(double) != sizeof(uint64_t) || FLT_RADIX != 2 ||
        DBL_MANT_DIG != 53 || DBL_MIN_EXP != -1021 || DBL_MAX_EXP != 1024 || FLT_EVAL_METHOD != 0 ||
        fegetround() != FE_TONEAREST || DBL_TRUE_MIN != scalbn(1.0, -1074) || half_minimum == 0.0) {
        return ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT;
    }
    return ODT_OK;
}

odt_status odt_internal_numeric_context_init(const odt_domain *domain, const odt_site *sites,
                                             size_t site_count,
                                             odt_internal_numeric_context *out_context) {
    double maximum = 0.0;
    int maximum_exponent;
    size_t index;

    if (domain == NULL || sites == NULL || site_count == 0u || out_context == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    out_context->domain = *domain;
    out_context->sites = sites;
    out_context->site_count = site_count;
    maximum = fmax(maximum, fabs(domain->min_x));
    maximum = fmax(maximum, fabs(domain->min_y));
    maximum = fmax(maximum, fabs(domain->max_x));
    maximum = fmax(maximum, fabs(domain->max_y));
    for (index = 0u; index < site_count; ++index) {
        maximum = fmax(maximum, fabs(sites[index].x));
        maximum = fmax(maximum, fabs(sites[index].y));
    }
    (void)frexp(maximum, &maximum_exponent);
    out_context->filter_scale_exponent = -maximum_exponent;
    out_context->filter_enabled = true;
    {
        const double values[4] = {
            domain->min_x,
            domain->min_y,
            domain->max_x,
            domain->max_y,
        };

        for (index = 0u; index < 4u; ++index) {
            double scaled;

            if (!odt_filter_scaled_value(out_context, values[index], &scaled)) {
                out_context->filter_enabled = false;
            }
        }
    }
    for (index = 0u; index < site_count; ++index) {
        double scaled;

        if (!odt_filter_scaled_value(out_context, sites[index].x, &scaled) ||
            !odt_filter_scaled_value(out_context, sites[index].y, &scaled)) {
            out_context->filter_enabled = false;
        }
    }
    return ODT_OK;
}

odt_status odt_internal_compare_squared_distance_values(const odt_point *point,
                                                        const odt_site *first,
                                                        const odt_site *second,
                                                        int *out_comparison) {
    odt_bigint first_distance;
    odt_bigint second_distance;

    if (point == NULL || first == NULL || second == NULL || out_comparison == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if (!isfinite(point->x) || !isfinite(point->y) || !isfinite(first->x) || !isfinite(first->y) ||
        !isfinite(second->x) || !isfinite(second->y)) {
        return ODT_INVALID_DATA;
    }
    if (!odt_exact_squared_distance(point, first, &first_distance) ||
        !odt_exact_squared_distance(point, second, &second_distance)) {
        return ODT_INTERNAL_ERROR;
    }
    *out_comparison = odt_big_compare_magnitude(&first_distance, &second_distance);
    return ODT_OK;
}

odt_status odt_internal_compare_squared_distance(const odt_point *point, const odt_site *first,
                                                 size_t first_ordinal, const odt_site *second,
                                                 size_t second_ordinal, int *out_comparison) {
    int comparison;
    odt_status status =
        odt_internal_compare_squared_distance_values(point, first, second, &comparison);

    if (status != ODT_OK) {
        return status;
    }
    if (comparison == 0) {
        if (first_ordinal < second_ordinal) {
            comparison = -1;
        } else if (first_ordinal > second_ordinal) {
            comparison = 1;
        }
    }
    *out_comparison = comparison;
    return ODT_OK;
}

odt_status odt_internal_runtime_filter_init(const odt_internal_numeric_context *context,
                                            const odt_internal_line_ref *line,
                                            odt_internal_runtime_filter *out_filter,
                                            bool *out_enabled) {
    odt_filter_line filter;

    if (context == NULL || line == NULL || out_filter == NULL || out_enabled == NULL ||
        line->kind != ODT_INTERNAL_LINE_BISECTOR) {
        return ODT_INVALID_ARGUMENT;
    }
    memset(out_filter, 0, sizeof(*out_filter));
    if (!odt_filter_line_build(context, line, &filter)) {
        *out_enabled = false;
        return ODT_OK;
    }
    out_filter->a_lower = filter.a.lower;
    out_filter->a_upper = filter.a.upper;
    out_filter->b_lower = filter.b.lower;
    out_filter->b_upper = filter.b.upper;
    out_filter->c_lower = filter.c.lower;
    out_filter->c_upper = filter.c.upper;
    *out_enabled = true;
    return ODT_OK;
}

odt_status odt_internal_runtime_bisector_compare(const odt_point *point, const odt_site *sites,
                                                 size_t site_count, int filter_scale_exponent,
                                                 const odt_internal_runtime_node *node,
                                                 int *out_comparison, bool *out_used_exact) {
    int comparison = 0;

    if (point == NULL || sites == NULL || node == NULL || out_comparison == NULL ||
        out_used_exact == NULL || node->first_ordinal >= node->second_ordinal ||
        (size_t)node->second_ordinal >= site_count) {
        return ODT_INVALID_ARGUMENT;
    }
    if (!isfinite(point->x) || !isfinite(point->y)) {
        return ODT_INVALID_DATA;
    }
    if (node->filter_enabled != 0u) {
        odt_internal_numeric_context context;
        odt_filter_line filter;
        odt_interval x_term;
        odt_interval y_term;
        odt_interval score;
        double scaled_x;
        double scaled_y;

        memset(&context, 0, sizeof(context));
        context.filter_scale_exponent = filter_scale_exponent;
        context.filter_enabled = true;
        filter.a.lower = node->filter.a_lower;
        filter.a.upper = node->filter.a_upper;
        filter.b.lower = node->filter.b_lower;
        filter.b.upper = node->filter.b_upper;
        filter.c.lower = node->filter.c_lower;
        filter.c.upper = node->filter.c_upper;
        if (odt_filter_scaled_value(&context, point->x, &scaled_x) &&
            odt_filter_scaled_value(&context, point->y, &scaled_y) &&
            odt_interval_multiply(filter.a, odt_interval_point(scaled_x), &x_term) &&
            odt_interval_multiply(filter.b, odt_interval_point(scaled_y), &y_term) &&
            odt_interval_add(x_term, y_term, &score) && odt_interval_add(score, filter.c, &score)) {
            comparison = odt_interval_sign(score);
            if (comparison != 0) {
                *out_comparison = comparison;
                *out_used_exact = false;
                return ODT_OK;
            }
        }
    }
    {
        odt_status status = odt_internal_compare_squared_distance(
            point, &sites[node->first_ordinal], node->first_ordinal, &sites[node->second_ordinal],
            node->second_ordinal, &comparison);

        if (status != ODT_OK) {
            return status;
        }
    }
    *out_comparison = comparison;
    *out_used_exact = true;
    return ODT_OK;
}

odt_status odt_internal_vertex_side(const odt_internal_numeric_context *context,
                                    const odt_internal_vertex *vertex,
                                    const odt_internal_line_ref *line, int *out_sign,
                                    bool *out_used_exact) {
    odt_filter_line first_filter;
    odt_filter_line second_filter;
    odt_filter_line side_filter;
    odt_interval denominator_interval;
    odt_interval determinant_interval;
    int denominator_sign;
    int determinant_sign;
    odt_exact_line first_exact;
    odt_exact_line second_exact;
    odt_exact_line side_exact;
    odt_bigint denominator;
    odt_bigint determinant;

    if (context == NULL || vertex == NULL || line == NULL || out_sign == NULL ||
        out_used_exact == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if (odt_line_ref_equal(&vertex->first_line, line) ||
        odt_line_ref_equal(&vertex->second_line, line)) {
        *out_sign = 0;
        *out_used_exact = true;
        return ODT_OK;
    }
    if (odt_filter_line_build(context, &vertex->first_line, &first_filter) &&
        odt_filter_line_build(context, &vertex->second_line, &second_filter) &&
        odt_filter_line_build(context, line, &side_filter) &&
        odt_filter_line_denominator(&first_filter, &second_filter, &denominator_interval) &&
        odt_filter_line_determinant(&first_filter, &second_filter, &side_filter,
                                    &determinant_interval)) {
        denominator_sign = odt_interval_sign(denominator_interval);
        determinant_sign = odt_interval_sign(determinant_interval);
        if (denominator_sign != 0 && determinant_sign != 0) {
            *out_sign = denominator_sign * determinant_sign;
            *out_used_exact = false;
            return ODT_OK;
        }
    }
    if (!odt_exact_line_build(context, &vertex->first_line, &first_exact) ||
        !odt_exact_line_build(context, &vertex->second_line, &second_exact) ||
        !odt_exact_line_build(context, line, &side_exact) ||
        !odt_exact_line_denominator(&first_exact, &second_exact, &denominator) ||
        denominator.sign == 0 ||
        !odt_exact_line_determinant(&first_exact, &second_exact, &side_exact, &determinant)) {
        return ODT_INTERNAL_ERROR;
    }
    *out_sign = determinant.sign * denominator.sign;
    *out_used_exact = true;
    return ODT_OK;
}

odt_status odt_internal_lines_parallel(const odt_internal_numeric_context *context,
                                       const odt_internal_line_ref *first,
                                       const odt_internal_line_ref *second, bool *out_parallel) {
    odt_exact_line first_exact;
    odt_exact_line second_exact;
    odt_bigint denominator;

    if (context == NULL || first == NULL || second == NULL || out_parallel == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if (!odt_exact_line_build(context, first, &first_exact) ||
        !odt_exact_line_build(context, second, &second_exact) ||
        !odt_exact_line_denominator(&first_exact, &second_exact, &denominator)) {
        return ODT_INTERNAL_ERROR;
    }
    *out_parallel = denominator.sign == 0;
    return ODT_OK;
}
