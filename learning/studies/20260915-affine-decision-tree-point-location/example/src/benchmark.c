#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <odt.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    BENCH_SUCCESS = 0,
    BENCH_SEMANTIC_FAILURE = 1,
    BENCH_EXECUTION_FAILURE = 2,
    BENCH_LINE_CAPACITY = 1024,
    BENCH_SITE_COUNT = 1000,
    BENCH_COORD_LIMBS = 36,
    BENCH_DISTANCE_LIMBS = 72,
    BENCH_VARIANT_COUNT = 4,
};

static const odt_domain BENCH_DOMAIN = {0.0, 0.0, 12.0, 8.0};
static const uint64_t BENCH_HASH_OFFSET = UINT64_C(1469598103934665603);
static const uint64_t BENCH_HASH_PRIME = UINT64_C(1099511628211);
static volatile uint64_t benchmark_sink = UINT64_C(0x9e3779b97f4a7c15);

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

typedef enum line_status {
    LINE_OK,
    LINE_END,
    LINE_ERROR,
    LINE_TOO_LONG,
} line_status;

typedef struct benchmark_options {
    const char *sites_path;
    const char *queries_path;
    const char *snapshot_path;
    const char *output_path;
    size_t repetitions;
    size_t warmup;
    size_t tree_loops;
    size_t brute_loops;
    bool inject_mismatch;
    bool require_speedup;
    bool self_check_dce;
    bool self_check;
} benchmark_options;

typedef struct benchmark_dataset {
    odt_site *sites;
    size_t site_count;
    odt_point *queries;
    size_t query_count;
} benchmark_dataset;

typedef struct exact_uint {
    size_t used;
    uint32_t limbs[BENCH_COORD_LIMBS];
} exact_uint;

typedef struct exact_distance {
    size_t used;
    uint32_t limbs[BENCH_DISTANCE_LIMBS];
} exact_distance;

typedef struct exact_point {
    exact_uint x;
    exact_uint y;
} exact_point;

typedef struct exact_dataset {
    int common_exponent;
    exact_point *sites;
    exact_point *queries;
} exact_dataset;

typedef struct validation_stats {
    uint64_t checksum;
    uint64_t scalar_comparisons;
    uint64_t scalar_exact_fallbacks;
    uint64_t batch_comparisons;
    uint64_t batch_exact_fallbacks;
    uint64_t restored_comparisons;
    uint64_t restored_exact_fallbacks;
} validation_stats;

typedef struct measurement_summary {
    double median_ns_per_query;
    double median_queries_per_second;
    uint64_t minimum_sample_ns;
    uint64_t maximum_sample_ns;
} measurement_summary;

typedef struct speedup_summary {
    double median;
    double geometric_mean;
    double confidence_95_lower;
    double confidence_95_upper;
} speedup_summary;

typedef struct amortization_summary {
    uint64_t source_scalar_break_even_queries;
    uint64_t source_batch_break_even_queries;
    uint64_t restored_scalar_break_even_queries;
    double source_scalar_corpus_speedup;
    double source_batch_corpus_speedup;
    double restored_scalar_corpus_speedup;
} amortization_summary;

static void print_usage(FILE *stream, const char *program) {
    (void)fprintf(stream,
                  "usage: %s --sites PATH --queries PATH --snapshot PATH --output PATH "
                  "[--repetitions N] [--warmup N] [--tree-loops N] [--brute-loops N] "
                  "[--require-speedup] [--inject-mismatch] [--self-check-dce] [--self-check]\n",
                  program);
}

static bool set_once(const char **destination, const char *value) {
    if (*destination != NULL || value == NULL) {
        return false;
    }
    *destination = value;
    return true;
}

static bool parse_size(const char *text, bool allow_zero, size_t *out_value) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > (unsigned long long)SIZE_MAX ||
        (!allow_zero && value == 0u)) {
        return false;
    }
    *out_value = (size_t)value;
    return true;
}

static bool parse_options(int argc, char **argv, benchmark_options *out_options) {
    int index;

    *out_options = (benchmark_options){
        NULL, NULL, NULL, NULL, 9u, 2u, 128u, 1u, false, false, false, false,
    };
    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];

        if (strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            exit(BENCH_SUCCESS);
        }
        if (strcmp(argument, "--require-speedup") == 0) {
            out_options->require_speedup = true;
            continue;
        }
        if (strcmp(argument, "--inject-mismatch") == 0) {
            out_options->inject_mismatch = true;
            continue;
        }
        if (strcmp(argument, "--self-check-dce") == 0) {
            out_options->self_check_dce = true;
            continue;
        }
        if (strcmp(argument, "--self-check") == 0) {
            out_options->self_check = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        {
            const char *value = argv[++index];

            if (strcmp(argument, "--sites") == 0) {
                if (!set_once(&out_options->sites_path, value)) {
                    return false;
                }
            } else if (strcmp(argument, "--queries") == 0) {
                if (!set_once(&out_options->queries_path, value)) {
                    return false;
                }
            } else if (strcmp(argument, "--snapshot") == 0) {
                if (!set_once(&out_options->snapshot_path, value)) {
                    return false;
                }
            } else if (strcmp(argument, "--output") == 0) {
                if (!set_once(&out_options->output_path, value)) {
                    return false;
                }
            } else if (strcmp(argument, "--repetitions") == 0) {
                if (!parse_size(value, false, &out_options->repetitions)) {
                    return false;
                }
            } else if (strcmp(argument, "--warmup") == 0) {
                if (!parse_size(value, true, &out_options->warmup)) {
                    return false;
                }
            } else if (strcmp(argument, "--tree-loops") == 0) {
                if (!parse_size(value, false, &out_options->tree_loops)) {
                    return false;
                }
            } else if (strcmp(argument, "--brute-loops") == 0) {
                if (!parse_size(value, false, &out_options->brute_loops)) {
                    return false;
                }
            } else {
                return false;
            }
        }
    }
    if (out_options->self_check_dce || out_options->self_check) {
        return true;
    }
    return out_options->sites_path != NULL && out_options->queries_path != NULL &&
           out_options->snapshot_path != NULL && out_options->output_path != NULL &&
           out_options->repetitions >= 5u && out_options->repetitions <= 64u &&
           out_options->warmup <= 64u && out_options->tree_loops <= UINT32_MAX &&
           out_options->brute_loops <= UINT32_MAX;
}

static void consume_checksum(uint64_t checksum) {
#if !defined(ODT_BENCHMARK_DISABLE_OBSERVABLE_SINK)
    benchmark_sink += checksum | UINT64_C(1);
#else
    (void)checksum;
#endif
}

static bool verify_observable_sink(void) {
    const uint64_t before = benchmark_sink;

    consume_checksum(UINT64_C(0xd1b54a32d192ed03));
    if (benchmark_sink == before) {
        (void)fprintf(stderr, "error: observable benchmark checksum sink is disabled\n");
        return false;
    }
    return true;
}

static line_status read_line(FILE *stream, char *buffer, size_t capacity, size_t *line_number) {
    size_t length;

    if (fgets(buffer, (int)capacity, stream) == NULL) {
        return ferror(stream) ? LINE_ERROR : LINE_END;
    }
    *line_number += 1u;
    length = strlen(buffer);
    if (length == 0u) {
        return LINE_OK;
    }
    if (buffer[length - 1u] == '\n') {
        buffer[--length] = '\0';
    } else if (!feof(stream)) {
        return LINE_TOO_LONG;
    }
    if (length != 0u && buffer[length - 1u] == '\r') {
        buffer[length - 1u] = '\0';
    }
    return LINE_OK;
}

static size_t split_csv(char *line, char **fields, size_t capacity) {
    size_t count = 1u;
    char *cursor;

    fields[0] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == '"') {
            return 0u;
        }
        if (*cursor == ',') {
            if (count == capacity) {
                return capacity + 1u;
            }
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }
    return count;
}

static bool parse_u64(const char *text, uint64_t *out_value) {
    char *end = NULL;
    unsigned long long value;

    if (text[0] == '\0' || text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

static bool parse_i32(const char *text, int32_t *out_value) {
    char *end = NULL;
    long long value;

    if (text[0] == '\0' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out_value = (int32_t)value;
    return true;
}

static bool parse_binary64(const char *text, double *out_value) {
    char *end = NULL;
    double value;

    if (text[0] == '\0') {
        return false;
    }
    errno = 0;
    value = strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool grow_array(void **array, size_t *capacity, size_t count, size_t item_size) {
    size_t new_capacity;
    void *replacement;

    if (count < *capacity) {
        return true;
    }
    new_capacity = *capacity == 0u ? 64u : *capacity * 2u;
    if (new_capacity <= *capacity || new_capacity > SIZE_MAX / item_size) {
        return false;
    }
    replacement = realloc(*array, new_capacity * item_size);
    if (replacement == NULL) {
        return false;
    }
    *array = replacement;
    *capacity = new_capacity;
    return true;
}

static bool read_sites(const char *path, odt_site **out_sites, size_t *out_count) {
    static const char header[] = "ordinal,region_id,x_hex,y_hex";
    FILE *stream = fopen(path, "r");
    odt_site *sites = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t line_number = 0u;
    char line[BENCH_LINE_CAPACITY];
    line_status status;
    bool success = false;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open sites CSV %s: %s\n", path, strerror(errno));
        return false;
    }
    if (read_line(stream, line, sizeof(line), &line_number) != LINE_OK ||
        strcmp(line, header) != 0) {
        (void)fprintf(stderr, "error: invalid sites CSV header: %s\n", path);
        goto cleanup;
    }
    while ((status = read_line(stream, line, sizeof(line), &line_number)) == LINE_OK) {
        char *fields[4];
        uint64_t ordinal;
        int32_t region_id;
        double x;
        double y;

        if (split_csv(line, fields, 4u) != 4u || !parse_u64(fields[0], &ordinal) ||
            ordinal != (uint64_t)count || !parse_i32(fields[1], &region_id) ||
            !parse_binary64(fields[2], &x) || !parse_binary64(fields[3], &y) || !isfinite(x) ||
            !isfinite(y) || x < BENCH_DOMAIN.min_x || x > BENCH_DOMAIN.max_x ||
            y < BENCH_DOMAIN.min_y || y > BENCH_DOMAIN.max_y ||
            !grow_array((void **)&sites, &capacity, count, sizeof(*sites))) {
            (void)fprintf(stderr, "error: invalid sites CSV row %zu: %s\n", line_number, path);
            goto cleanup;
        }
        sites[count++] = (odt_site){x, y, region_id};
    }
    if (status == LINE_ERROR || status == LINE_TOO_LONG || count != BENCH_SITE_COUNT) {
        (void)fprintf(stderr, "error: incomplete sites CSV: %s\n", path);
        goto cleanup;
    }
    *out_sites = sites;
    *out_count = count;
    sites = NULL;
    success = true;

cleanup:
    free(sites);
    if (fclose(stream) != 0) {
        success = false;
    }
    return success;
}

static bool read_queries(const char *path, odt_point **out_queries, size_t *out_count) {
    static const char header[] =
        "query_id,class,x_hex,y_hex,site_a,site_b,expected_kind,expected_status,"
        "expected_region_id";
    FILE *stream = fopen(path, "r");
    odt_point *queries = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t line_number = 0u;
    char line[BENCH_LINE_CAPACITY];
    line_status status;
    bool success = false;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open queries CSV %s: %s\n", path, strerror(errno));
        return false;
    }
    if (read_line(stream, line, sizeof(line), &line_number) != LINE_OK ||
        strcmp(line, header) != 0) {
        (void)fprintf(stderr, "error: invalid queries CSV header: %s\n", path);
        goto cleanup;
    }
    while ((status = read_line(stream, line, sizeof(line), &line_number)) == LINE_OK) {
        char *fields[9];
        uint64_t query_id;
        double x;
        double y;

        if (split_csv(line, fields, 9u) != 9u || !parse_u64(fields[0], &query_id) ||
            query_id != (uint64_t)count || !parse_binary64(fields[2], &x) ||
            !parse_binary64(fields[3], &y) ||
            !grow_array((void **)&queries, &capacity, count, sizeof(*queries))) {
            (void)fprintf(stderr, "error: invalid queries CSV row %zu: %s\n", line_number, path);
            goto cleanup;
        }
        queries[count++] = (odt_point){x, y};
    }
    if (status == LINE_ERROR || status == LINE_TOO_LONG || count == 0u) {
        (void)fprintf(stderr, "error: incomplete queries CSV: %s\n", path);
        goto cleanup;
    }
    *out_queries = queries;
    *out_count = count;
    queries = NULL;
    success = true;

cleanup:
    free(queries);
    if (fclose(stream) != 0) {
        success = false;
    }
    return success;
}

static bool read_dataset(const benchmark_options *options, benchmark_dataset *out_dataset) {
    memset(out_dataset, 0, sizeof(*out_dataset));
    if (!read_sites(options->sites_path, &out_dataset->sites, &out_dataset->site_count) ||
        !read_queries(options->queries_path, &out_dataset->queries, &out_dataset->query_count)) {
        free(out_dataset->queries);
        free(out_dataset->sites);
        memset(out_dataset, 0, sizeof(*out_dataset));
        return false;
    }
    return true;
}

static bool binary64_lsb_exponent(double value, bool *out_nonzero, int *out_exponent) {
    uint64_t bits;
    uint64_t fraction;
    unsigned exponent_bits;

    if (!isfinite(value)) {
        return false;
    }
    memcpy(&bits, &value, sizeof(bits));
    fraction = bits & UINT64_C(0x000fffffffffffff);
    exponent_bits = (unsigned)((bits >> 52u) & UINT64_C(0x7ff));
    if ((bits >> 63u) != 0u && (fraction != 0u || exponent_bits != 0u)) {
        return false;
    }
    if (exponent_bits == 0u) {
        if (fraction == 0u) {
            *out_nonzero = false;
            *out_exponent = 0;
            return true;
        }
        *out_exponent = -1074;
    } else {
        *out_exponent = (int)exponent_bits - 1023 - 52;
    }
    *out_nonzero = true;
    return true;
}

static bool update_common_exponent(double value, int *common_exponent, bool *found_nonzero) {
    bool nonzero;
    int exponent;

    if (!binary64_lsb_exponent(value, &nonzero, &exponent)) {
        return false;
    }
    if (nonzero && (!*found_nonzero || exponent < *common_exponent)) {
        *common_exponent = exponent;
        *found_nonzero = true;
    }
    return true;
}

static bool exact_uint_from_binary64(double value, int common_exponent, exact_uint *out_value) {
    uint64_t bits;
    uint64_t significand;
    unsigned exponent_bits;
    int exponent;
    size_t shift;
    size_t bit;

    memset(out_value, 0, sizeof(*out_value));
    if (!isfinite(value) || value < 0.0) {
        return false;
    }
    memcpy(&bits, &value, sizeof(bits));
    exponent_bits = (unsigned)((bits >> 52u) & UINT64_C(0x7ff));
    significand = bits & UINT64_C(0x000fffffffffffff);
    if (exponent_bits == 0u) {
        if (significand == 0u) {
            return true;
        }
        exponent = -1074;
    } else {
        significand |= UINT64_C(0x0010000000000000);
        exponent = (int)exponent_bits - 1023 - 52;
    }
    if (exponent < common_exponent) {
        return false;
    }
    shift = (size_t)(exponent - common_exponent);
    if (shift > BENCH_COORD_LIMBS * 32u - 53u) {
        return false;
    }
    for (bit = 0u; bit < 53u; ++bit) {
        if ((significand & (UINT64_C(1) << bit)) != 0u) {
            const size_t destination = shift + bit;

            out_value->limbs[destination / 32u] |= UINT32_C(1) << (unsigned)(destination % 32u);
        }
    }
    out_value->used = (shift + 53u + 31u) / 32u;
    while (out_value->used != 0u && out_value->limbs[out_value->used - 1u] == 0u) {
        out_value->used -= 1u;
    }
    return true;
}

static int exact_uint_compare(const exact_uint *first, const exact_uint *second) {
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

static void exact_uint_subtract_magnitude(const exact_uint *first, const exact_uint *second,
                                          exact_uint *out_difference) {
    const exact_uint *larger = first;
    const exact_uint *smaller = second;
    uint64_t borrow = 0u;
    size_t index;

    if (exact_uint_compare(first, second) < 0) {
        larger = second;
        smaller = first;
    }
    memset(out_difference, 0, sizeof(*out_difference));
    for (index = 0u; index < larger->used; ++index) {
        const uint64_t larger_limb = larger->limbs[index];
        const uint64_t smaller_limb = index < smaller->used ? (uint64_t)smaller->limbs[index] : 0u;
        const uint64_t subtrahend = smaller_limb + borrow;

        if (larger_limb < subtrahend) {
            out_difference->limbs[index] =
                (uint32_t)(larger_limb + (UINT64_C(1) << 32u) - subtrahend);
            borrow = 1u;
        } else {
            out_difference->limbs[index] = (uint32_t)(larger_limb - subtrahend);
            borrow = 0u;
        }
    }
    out_difference->used = larger->used;
    while (out_difference->used != 0u && out_difference->limbs[out_difference->used - 1u] == 0u) {
        out_difference->used -= 1u;
    }
}

static void exact_uint_square(const exact_uint *value, exact_distance *out_square) {
    size_t first_index;

    memset(out_square, 0, sizeof(*out_square));
    for (first_index = 0u; first_index < value->used; ++first_index) {
        uint64_t carry = 0u;
        size_t second_index;

        for (second_index = 0u; second_index < value->used; ++second_index) {
            const size_t destination = first_index + second_index;
            const uint64_t product =
                (uint64_t)value->limbs[first_index] * (uint64_t)value->limbs[second_index];
            const uint64_t sum = (uint64_t)out_square->limbs[destination] + product + carry;

            out_square->limbs[destination] = (uint32_t)sum;
            carry = sum >> 32u;
        }
        out_square->limbs[first_index + value->used] = (uint32_t)carry;
    }
    out_square->used = value->used * 2u;
    while (out_square->used != 0u && out_square->limbs[out_square->used - 1u] == 0u) {
        out_square->used -= 1u;
    }
}

static void exact_distance_add(const exact_distance *first, const exact_distance *second,
                               exact_distance *out_sum) {
    const size_t count = first->used > second->used ? first->used : second->used;
    uint64_t carry = 0u;
    size_t index;

    memset(out_sum, 0, sizeof(*out_sum));
    for (index = 0u; index < count; ++index) {
        const uint64_t first_limb = index < first->used ? first->limbs[index] : 0u;
        const uint64_t second_limb = index < second->used ? second->limbs[index] : 0u;
        const uint64_t sum = first_limb + second_limb + carry;

        out_sum->limbs[index] = (uint32_t)sum;
        carry = sum >> 32u;
    }
    out_sum->used = count;
    if (carry != 0u) {
        out_sum->limbs[out_sum->used++] = (uint32_t)carry;
    }
}

static int exact_distance_compare(const exact_distance *first, const exact_distance *second) {
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

static void exact_squared_distance(const exact_point *point, const exact_point *site,
                                   exact_distance *out_distance) {
    exact_uint x_difference;
    exact_uint y_difference;
    exact_distance x_square;
    exact_distance y_square;

    exact_uint_subtract_magnitude(&point->x, &site->x, &x_difference);
    exact_uint_subtract_magnitude(&point->y, &site->y, &y_difference);
    exact_uint_square(&x_difference, &x_square);
    exact_uint_square(&y_difference, &y_square);
    exact_distance_add(&x_square, &y_square, out_distance);
}

static bool point_is_inside(const odt_point *point) {
    return isfinite(point->x) && isfinite(point->y) && point->x >= BENCH_DOMAIN.min_x &&
           point->x <= BENCH_DOMAIN.max_x && point->y >= BENCH_DOMAIN.min_y &&
           point->y <= BENCH_DOMAIN.max_y;
}

static bool exact_dataset_init(const benchmark_dataset *dataset, exact_dataset *out_exact) {
    int common_exponent = 0;
    bool found_nonzero = false;
    size_t index;

    memset(out_exact, 0, sizeof(*out_exact));
    for (index = 0u; index < dataset->site_count; ++index) {
        if (!update_common_exponent(dataset->sites[index].x, &common_exponent, &found_nonzero) ||
            !update_common_exponent(dataset->sites[index].y, &common_exponent, &found_nonzero)) {
            return false;
        }
    }
    for (index = 0u; index < dataset->query_count; ++index) {
        if (point_is_inside(&dataset->queries[index]) &&
            (!update_common_exponent(dataset->queries[index].x, &common_exponent, &found_nonzero) ||
             !update_common_exponent(dataset->queries[index].y, &common_exponent,
                                     &found_nonzero))) {
            return false;
        }
    }
    if (!found_nonzero || dataset->site_count > SIZE_MAX / sizeof(*out_exact->sites) ||
        dataset->query_count > SIZE_MAX / sizeof(*out_exact->queries)) {
        return false;
    }
    out_exact->sites = calloc(dataset->site_count, sizeof(*out_exact->sites));
    out_exact->queries = calloc(dataset->query_count, sizeof(*out_exact->queries));
    if (out_exact->sites == NULL || out_exact->queries == NULL) {
        free(out_exact->queries);
        free(out_exact->sites);
        memset(out_exact, 0, sizeof(*out_exact));
        return false;
    }
    out_exact->common_exponent = common_exponent;
    for (index = 0u; index < dataset->site_count; ++index) {
        if (!exact_uint_from_binary64(dataset->sites[index].x, common_exponent,
                                      &out_exact->sites[index].x) ||
            !exact_uint_from_binary64(dataset->sites[index].y, common_exponent,
                                      &out_exact->sites[index].y)) {
            return false;
        }
    }
    for (index = 0u; index < dataset->query_count; ++index) {
        if (point_is_inside(&dataset->queries[index]) &&
            (!exact_uint_from_binary64(dataset->queries[index].x, common_exponent,
                                       &out_exact->queries[index].x) ||
             !exact_uint_from_binary64(dataset->queries[index].y, common_exponent,
                                       &out_exact->queries[index].y))) {
            return false;
        }
    }
    return true;
}

static odt_query_result exact_brute_query(const benchmark_dataset *dataset,
                                          const exact_dataset *exact, size_t query_index) {
    const odt_point *point = &dataset->queries[query_index];
    size_t winner = 0u;
    exact_distance winner_distance;
    size_t site_index;

    if (!isfinite(point->x) || !isfinite(point->y)) {
        return (odt_query_result){ODT_RESULT_ERROR, ODT_INVALID_DATA, 0};
    }
    if (!point_is_inside(point)) {
        return (odt_query_result){ODT_RESULT_OUTSIDE, ODT_OK, 0};
    }
    exact_squared_distance(&exact->queries[query_index], &exact->sites[0], &winner_distance);
    for (site_index = 1u; site_index < dataset->site_count; ++site_index) {
        exact_distance distance;

        exact_squared_distance(&exact->queries[query_index], &exact->sites[site_index], &distance);
        if (exact_distance_compare(&distance, &winner_distance) < 0) {
            winner = site_index;
            winner_distance = distance;
        }
    }
    return (odt_query_result){ODT_RESULT_REGION, ODT_OK, dataset->sites[winner].region_id};
}

static odt_query_result scalar_query(const odt_generation *generation, const odt_point *point,
                                     odt_query_stats *out_stats) {
    odt_query_result result = {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, 0};
    odt_query_stats stats = {0u, 0u};
    const odt_status status = odt_query(generation, point, &result, &stats);

    if (status != ODT_OK) {
        result = (odt_query_result){ODT_RESULT_ERROR, status, 0};
        stats = (odt_query_stats){0u, 0u};
    }
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    return result;
}

static bool same_result(const odt_query_result *first, const odt_query_result *second) {
    return first->kind == second->kind && first->status == second->status &&
           first->region_id == second->region_id;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    unsigned shift;

    for (shift = 0u; shift < 32u; shift += 8u) {
        hash ^= (uint64_t)((value >> shift) & UINT32_C(0xff));
        hash *= BENCH_HASH_PRIME;
    }
    return hash;
}

static uint64_t hash_result(uint64_t hash, const odt_query_result *result) {
    uint32_t region_bits;

    memcpy(&region_bits, &result->region_id, sizeof(region_bits));
    hash = hash_u32(hash, (uint32_t)result->kind);
    hash = hash_u32(hash, (uint32_t)result->status);
    return hash_u32(hash, region_bits);
}

static uint64_t repeated_results_checksum(const odt_query_result *results, size_t result_count,
                                          size_t loops) {
    uint64_t hash = BENCH_HASH_OFFSET;
    size_t loop;

    for (loop = 0u; loop < loops; ++loop) {
        size_t index;

        for (index = 0u; index < result_count; ++index) {
            hash = hash_result(hash, &results[index]);
        }
    }
    return hash;
}

static bool benchmark_self_check(void) {
    static odt_site sites[] = {
        {0.0, 0.0, 11},
        {2.0, 0.0, 22},
        {1.0, 2.0, 33},
    };
    static odt_point queries[] = {
        {0.0, 0.0}, {2.0, 0.0}, {1.0, 0.0}, {1.0, 2.0}, {13.0, 0.0}, {NAN, 0.0},
    };
    static const odt_query_result expected[] = {
        {ODT_RESULT_REGION, ODT_OK, 11}, {ODT_RESULT_REGION, ODT_OK, 22},
        {ODT_RESULT_REGION, ODT_OK, 11}, {ODT_RESULT_REGION, ODT_OK, 33},
        {ODT_RESULT_OUTSIDE, ODT_OK, 0}, {ODT_RESULT_ERROR, ODT_INVALID_DATA, 0},
    };
    const benchmark_dataset dataset = {
        sites,
        sizeof(sites) / sizeof(sites[0]),
        queries,
        sizeof(queries) / sizeof(queries[0]),
    };
    exact_dataset exact;
    uint64_t checksum;
    uint64_t repeated_checksum;
    size_t index;
    bool success = false;

    memset(&exact, 0, sizeof(exact));
    if (!exact_dataset_init(&dataset, &exact)) {
        (void)fprintf(stderr, "error: benchmark self-check exact preprocessing failed\n");
        goto cleanup;
    }
    for (index = 0u; index < dataset.query_count; ++index) {
        const odt_query_result actual = exact_brute_query(&dataset, &exact, index);

        if (!same_result(&actual, &expected[index])) {
            (void)fprintf(stderr,
                          "error: benchmark self-check exact arithmetic failed at query %zu\n",
                          index);
            goto cleanup;
        }
    }
    checksum = repeated_results_checksum(expected, dataset.query_count, 1u);
    repeated_checksum = repeated_results_checksum(expected, dataset.query_count, 3u);
    if (checksum != UINT64_C(0x6c2e2a0370353c57) ||
        repeated_results_checksum(expected, dataset.query_count, 2u) !=
            UINT64_C(0x3739095e6a0a8783) ||
        repeated_checksum != UINT64_C(0x13489c27917f4057) ||
        repeated_results_checksum(expected, dataset.query_count, 2u) == repeated_checksum) {
        (void)fprintf(stderr, "error: benchmark self-check checksum logic failed\n");
        goto cleanup;
    }
    success = true;

cleanup:
    free(exact.queries);
    free(exact.sites);
    if (success) {
        (void)printf("benchmark_self_check exact_arithmetic=passed checksum=%016" PRIx64
                     " repeated_checksum=%016" PRIx64 "\n",
                     checksum, repeated_checksum);
    }
    return success;
}

static bool validate_paths(const benchmark_dataset *dataset, const exact_dataset *exact,
                           const odt_generation *source, const odt_generation *restored,
                           bool inject_mismatch, odt_query_result *batch_results,
                           validation_stats *out_stats) {
    odt_batch_stats batch_stats;
    uint64_t scalar_comparisons = 0u;
    uint64_t scalar_fallbacks = 0u;
    uint64_t restored_comparisons = 0u;
    uint64_t restored_fallbacks = 0u;
    uint64_t scalar_checksum = BENCH_HASH_OFFSET;
    uint64_t batch_checksum = BENCH_HASH_OFFSET;
    uint64_t restored_checksum = BENCH_HASH_OFFSET;
    uint64_t brute_checksum = BENCH_HASH_OFFSET;
    size_t index;

    if (odt_query_batch(source, dataset->query_count, dataset->queries, sizeof(*dataset->queries),
                        batch_results, sizeof(*batch_results), &batch_stats) != ODT_OK) {
        return false;
    }
    for (index = 0u; index < dataset->query_count; ++index) {
        odt_query_stats scalar_stats;
        odt_query_stats restored_stats;
        const odt_query_result scalar =
            scalar_query(source, &dataset->queries[index], &scalar_stats);
        const odt_query_result restored_result =
            scalar_query(restored, &dataset->queries[index], &restored_stats);
        odt_query_result brute = exact_brute_query(dataset, exact, index);

        if (inject_mismatch && index == 0u) {
            brute.region_id ^= INT32_C(1);
        }
        if (!same_result(&scalar, &batch_results[index]) ||
            !same_result(&scalar, &restored_result) || !same_result(&scalar, &brute)) {
            (void)fprintf(stderr, "error: semantic mismatch at query %zu\n", index);
            return false;
        }
        scalar_checksum = hash_result(scalar_checksum, &scalar);
        batch_checksum = hash_result(batch_checksum, &batch_results[index]);
        restored_checksum = hash_result(restored_checksum, &restored_result);
        brute_checksum = hash_result(brute_checksum, &brute);
        scalar_comparisons += scalar_stats.comparisons;
        scalar_fallbacks += scalar_stats.exact_fallbacks;
        restored_comparisons += restored_stats.comparisons;
        restored_fallbacks += restored_stats.exact_fallbacks;
    }
    if (scalar_checksum != batch_checksum || scalar_checksum != restored_checksum ||
        scalar_checksum != brute_checksum) {
        (void)fprintf(stderr, "error: classification checksums do not match\n");
        return false;
    }
    *out_stats = (validation_stats){
        scalar_checksum,         scalar_comparisons,          scalar_fallbacks,
        batch_stats.comparisons, batch_stats.exact_fallbacks, restored_comparisons,
        restored_fallbacks,
    };
    return true;
}

static BENCH_NOINLINE bool odt_benchmark_scalar_pass(const odt_generation *generation,
                                                     const odt_point *queries, size_t query_count,
                                                     size_t loops, uint64_t *out_checksum) {
    uint64_t hash = BENCH_HASH_OFFSET;
    size_t loop;

    for (loop = 0u; loop < loops; ++loop) {
        size_t index;

        for (index = 0u; index < query_count; ++index) {
            const odt_query_result result = scalar_query(generation, &queries[index], NULL);

            hash = hash_result(hash, &result);
        }
    }
    *out_checksum = hash;
    return true;
}

static BENCH_NOINLINE bool odt_benchmark_batch_pass(const odt_generation *generation,
                                                    const odt_point *queries, size_t query_count,
                                                    size_t loops, odt_query_result *results,
                                                    uint64_t *out_checksum) {
    uint64_t hash = BENCH_HASH_OFFSET;
    size_t loop;

    for (loop = 0u; loop < loops; ++loop) {
        size_t index;

        if (odt_query_batch(generation, query_count, queries, sizeof(*queries), results,
                            sizeof(*results), NULL) != ODT_OK) {
            return false;
        }
        for (index = 0u; index < query_count; ++index) {
            hash = hash_result(hash, &results[index]);
        }
    }
    *out_checksum = hash;
    return true;
}

static BENCH_NOINLINE bool odt_benchmark_brute_pass(const benchmark_dataset *dataset,
                                                    const exact_dataset *exact, size_t loops,
                                                    uint64_t *out_checksum) {
    uint64_t hash = BENCH_HASH_OFFSET;
    size_t loop;

    for (loop = 0u; loop < loops; ++loop) {
        size_t index;

        for (index = 0u; index < dataset->query_count; ++index) {
            const odt_query_result result = exact_brute_query(dataset, exact, index);

            hash = hash_result(hash, &result);
        }
    }
    *out_checksum = hash;
    return true;
}

static bool monotonic_now(uint64_t *out_ns) {
    struct timespec value;
#if defined(CLOCK_MONOTONIC_RAW)
    const clockid_t clock_id = CLOCK_MONOTONIC_RAW;
#else
    const clockid_t clock_id = CLOCK_MONOTONIC;
#endif

    if (clock_gettime(clock_id, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0) {
        return false;
    }
    *out_ns = (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
    return true;
}

static bool verify_timed_checksum(const char *variant, uint64_t actual, uint64_t expected) {
    if (actual == expected) {
        return true;
    }
    (void)fprintf(
        stderr, "error: %s timed checksum mismatch: expected %016" PRIx64 ", got %016" PRIx64 "\n",
        variant, expected, actual);
    return false;
}

static bool measure_scalar(const odt_generation *generation, const benchmark_dataset *dataset,
                           size_t loops, const char *variant, uint64_t expected_checksum,
                           uint64_t *out_elapsed) {
    uint64_t before;
    uint64_t after;
    uint64_t checksum;

    if (!monotonic_now(&before) ||
        !odt_benchmark_scalar_pass(generation, dataset->queries, dataset->query_count, loops,
                                   &checksum) ||
        !monotonic_now(&after) || after <= before) {
        return false;
    }
    if (!verify_timed_checksum(variant, checksum, expected_checksum)) {
        return false;
    }
    consume_checksum(checksum);
    *out_elapsed = after - before;
    return true;
}

static bool measure_batch(const odt_generation *generation, const benchmark_dataset *dataset,
                          size_t loops, odt_query_result *results, uint64_t expected_checksum,
                          uint64_t *out_elapsed) {
    uint64_t before;
    uint64_t after;
    uint64_t checksum;

    if (!monotonic_now(&before) ||
        !odt_benchmark_batch_pass(generation, dataset->queries, dataset->query_count, loops,
                                  results, &checksum) ||
        !monotonic_now(&after) || after <= before) {
        return false;
    }
    if (!verify_timed_checksum("source batch", checksum, expected_checksum)) {
        return false;
    }
    consume_checksum(checksum);
    *out_elapsed = after - before;
    return true;
}

static bool measure_brute(const benchmark_dataset *dataset, const exact_dataset *exact,
                          size_t loops, uint64_t expected_checksum, uint64_t *out_elapsed) {
    uint64_t before;
    uint64_t after;
    uint64_t checksum;

    if (!monotonic_now(&before) || !odt_benchmark_brute_pass(dataset, exact, loops, &checksum) ||
        !monotonic_now(&after) || after <= before) {
        return false;
    }
    if (!verify_timed_checksum("exact brute force", checksum, expected_checksum)) {
        return false;
    }
    consume_checksum(checksum);
    *out_elapsed = after - before;
    return true;
}

static int compare_double(const void *first, const void *second) {
    const double first_value = *(const double *)first;
    const double second_value = *(const double *)second;

    return (first_value > second_value) - (first_value < second_value);
}

static double median(double *values, size_t count) {
    qsort(values, count, sizeof(*values), compare_double);
    if ((count & 1u) != 0u) {
        return values[count / 2u];
    }
    return (values[count / 2u - 1u] + values[count / 2u]) / 2.0;
}

static measurement_summary summarize_measurements(const uint64_t *samples, size_t count,
                                                  size_t queries_per_sample) {
    measurement_summary summary;
    double per_query[64];
    size_t index;

    summary.minimum_sample_ns = UINT64_MAX;
    summary.maximum_sample_ns = 0u;
    for (index = 0u; index < count; ++index) {
        per_query[index] = (double)samples[index] / (double)queries_per_sample;
        if (samples[index] < summary.minimum_sample_ns) {
            summary.minimum_sample_ns = samples[index];
        }
        if (samples[index] > summary.maximum_sample_ns) {
            summary.maximum_sample_ns = samples[index];
        }
    }
    summary.median_ns_per_query = median(per_query, count);
    summary.median_queries_per_second = 1.0e9 / summary.median_ns_per_query;
    return summary;
}

static double t_critical_95(size_t degrees_of_freedom) {
    static const double values[] = {
        0.0,   12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179,  2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080,
        2.074, 2.069,  2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
    };

    if (degrees_of_freedom < sizeof(values) / sizeof(values[0])) {
        return values[degrees_of_freedom];
    }
    return 1.96;
}

static speedup_summary summarize_speedup(const uint64_t *tree_samples, size_t tree_loops,
                                         const uint64_t *brute_samples, size_t brute_loops,
                                         size_t count) {
    speedup_summary summary;
    double ratios[64];
    double log_sum = 0.0;
    double squared_deviation = 0.0;
    double mean_log;
    double standard_error;
    double margin;
    size_t index;

    for (index = 0u; index < count; ++index) {
        const double tree_per_query = (double)tree_samples[index] / (double)tree_loops;
        const double brute_per_query = (double)brute_samples[index] / (double)brute_loops;

        ratios[index] = brute_per_query / tree_per_query;
        log_sum += log(ratios[index]);
    }
    mean_log = log_sum / (double)count;
    for (index = 0u; index < count; ++index) {
        const double deviation = log(ratios[index]) - mean_log;

        squared_deviation += deviation * deviation;
    }
    standard_error = sqrt(squared_deviation / (double)(count - 1u)) / sqrt((double)count);
    margin = t_critical_95(count - 1u) * standard_error;
    summary.median = median(ratios, count);
    summary.geometric_mean = exp(mean_log);
    summary.confidence_95_lower = exp(mean_log - margin);
    summary.confidence_95_upper = exp(mean_log + margin);
    return summary;
}

static uint64_t break_even_query_count(uint64_t construction_ns, uint64_t baseline_preprocess_ns,
                                       double query_ns, double baseline_query_ns) {
    double net_construction_ns;
    double per_query_savings_ns;
    double result;

    if (construction_ns <= baseline_preprocess_ns) {
        return 0u;
    }
    per_query_savings_ns = baseline_query_ns - query_ns;
    if (per_query_savings_ns <= 0.0) {
        return UINT64_MAX;
    }
    net_construction_ns = (double)(construction_ns - baseline_preprocess_ns);
    result = ceil(net_construction_ns / per_query_savings_ns);
    return result >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)result;
}

static double total_cost_speedup(uint64_t construction_ns, double query_ns,
                                 uint64_t baseline_preprocess_ns, double baseline_query_ns,
                                 size_t query_count) {
    const double baseline_total =
        (double)baseline_preprocess_ns + baseline_query_ns * (double)query_count;
    const double candidate_total = (double)construction_ns + query_ns * (double)query_count;

    return baseline_total / candidate_total;
}

static amortization_summary summarize_amortization(uint64_t build_ns, uint64_t load_ns,
                                                   uint64_t exact_preprocess_ns, size_t query_count,
                                                   const measurement_summary *scalar,
                                                   const measurement_summary *batch,
                                                   const measurement_summary *restored,
                                                   const measurement_summary *brute) {
    amortization_summary summary;

    summary.source_scalar_break_even_queries = break_even_query_count(
        build_ns, exact_preprocess_ns, scalar->median_ns_per_query, brute->median_ns_per_query);
    summary.source_batch_break_even_queries = break_even_query_count(
        build_ns, exact_preprocess_ns, batch->median_ns_per_query, brute->median_ns_per_query);
    summary.restored_scalar_break_even_queries = break_even_query_count(
        load_ns, exact_preprocess_ns, restored->median_ns_per_query, brute->median_ns_per_query);
    summary.source_scalar_corpus_speedup =
        total_cost_speedup(build_ns, scalar->median_ns_per_query, exact_preprocess_ns,
                           brute->median_ns_per_query, query_count);
    summary.source_batch_corpus_speedup =
        total_cost_speedup(build_ns, batch->median_ns_per_query, exact_preprocess_ns,
                           brute->median_ns_per_query, query_count);
    summary.restored_scalar_corpus_speedup =
        total_cost_speedup(load_ns, restored->median_ns_per_query, exact_preprocess_ns,
                           brute->median_ns_per_query, query_count);
    return summary;
}

static bool run_measurements(const benchmark_options *options, const benchmark_dataset *dataset,
                             const exact_dataset *exact, const odt_generation *source,
                             const odt_generation *restored, odt_query_result *batch_results,
                             uint64_t *scalar_samples, uint64_t *batch_samples,
                             uint64_t *restored_samples, uint64_t *brute_samples) {
    const uint64_t expected_tree_checksum =
        repeated_results_checksum(batch_results, dataset->query_count, options->tree_loops);
    const uint64_t expected_brute_checksum =
        repeated_results_checksum(batch_results, dataset->query_count, options->brute_loops);
    size_t warmup;
    size_t repetition;

    for (warmup = 0u; warmup < options->warmup; ++warmup) {
        uint64_t ignored;

        if (!measure_scalar(source, dataset, options->tree_loops, "source scalar",
                            expected_tree_checksum, &ignored) ||
            !measure_batch(source, dataset, options->tree_loops, batch_results,
                           expected_tree_checksum, &ignored) ||
            !measure_scalar(restored, dataset, options->tree_loops, "restored scalar",
                            expected_tree_checksum, &ignored) ||
            !measure_brute(dataset, exact, options->brute_loops, expected_brute_checksum,
                           &ignored)) {
            return false;
        }
    }
    for (repetition = 0u; repetition < options->repetitions; ++repetition) {
        size_t offset;

        for (offset = 0u; offset < BENCH_VARIANT_COUNT; ++offset) {
            const size_t variant = (repetition + offset) % BENCH_VARIANT_COUNT;
            bool measured;

            if (variant == 0u) {
                measured = measure_scalar(source, dataset, options->tree_loops, "source scalar",
                                          expected_tree_checksum, &scalar_samples[repetition]);
            } else if (variant == 1u) {
                measured = measure_batch(source, dataset, options->tree_loops, batch_results,
                                         expected_tree_checksum, &batch_samples[repetition]);
            } else if (variant == 2u) {
                measured = measure_scalar(restored, dataset, options->tree_loops, "restored scalar",
                                          expected_tree_checksum, &restored_samples[repetition]);
            } else {
                measured = measure_brute(dataset, exact, options->brute_loops,
                                         expected_brute_checksum, &brute_samples[repetition]);
            }
            if (!measured) {
                return false;
            }
        }
    }
    return true;
}

static bool write_u64_array(FILE *stream, const uint64_t *values, size_t count) {
    size_t index;

    if (fputc('[', stream) == EOF) {
        return false;
    }
    for (index = 0u; index < count; ++index) {
        if ((index != 0u && fputc(',', stream) == EOF) ||
            fprintf(stream, "%" PRIu64, values[index]) < 0) {
            return false;
        }
    }
    return fputc(']', stream) != EOF;
}

static bool write_measurement(FILE *stream, const char *name, const uint64_t *samples,
                              size_t sample_count, size_t loops, const measurement_summary *summary,
                              bool trailing_comma) {
    if (fprintf(stream,
                "    \"%s\":{\"loops\":%zu,\"median_ns_per_query\":%.6f,"
                "\"median_queries_per_second\":%.6f,\"minimum_sample_ns\":%" PRIu64
                ",\"maximum_sample_ns\":%" PRIu64 ",\"samples_ns\":",
                name, loops, summary->median_ns_per_query, summary->median_queries_per_second,
                summary->minimum_sample_ns, summary->maximum_sample_ns) < 0 ||
        !write_u64_array(stream, samples, sample_count) ||
        fprintf(stream, "}%s\n", trailing_comma ? "," : "") < 0) {
        return false;
    }
    return true;
}

static bool
write_report(const benchmark_options *options, const benchmark_dataset *dataset,
             const exact_dataset *exact, const odt_build_stats *build_stats, uint64_t build_wall_ns,
             uint64_t encoded_size, uint64_t load_ns, uint64_t exact_preprocess_ns,
             const validation_stats *validation, const uint64_t *scalar_samples,
             const uint64_t *batch_samples, const uint64_t *restored_samples,
             const uint64_t *brute_samples, const measurement_summary *scalar_summary,
             const measurement_summary *batch_summary, const measurement_summary *restored_summary,
             const measurement_summary *brute_summary, const speedup_summary *scalar_speedup,
             const speedup_summary *batch_speedup, const amortization_summary *amortization,
             bool speedup_gate_passed) {
    FILE *stream = fopen(options->output_path, "w");
    bool success = false;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open benchmark output %s: %s\n", options->output_path,
                      strerror(errno));
        return false;
    }
    if (fprintf(stream,
                "{\n"
                "  \"schema\":\"odt-benchmark/v1\",\n"
                "  \"clock\":\"monotonic\",\n"
                "  \"inputs\":{\"site_count\":%zu,\"query_count\":%zu,"
                "\"domain\":[0.0,0.0,12.0,8.0]},\n"
                "  \"configuration\":{\"warmup\":%zu,\"repetitions\":%zu,"
                "\"tree_loops\":%zu,\"brute_loops\":%zu},\n"
                "  \"build\":{\"wall_ns\":%" PRIu64 ",\"reported_ns\":%" PRIu64
                ",\"peak_temporary_bytes\":%" PRIu64 ",\"node_count\":%" PRIu64
                ",\"leaf_count\":%" PRIu64 ",\"maximum_depth\":%" PRIu32
                ",\"fragment_count\":%" PRIu64 ",\"encoded_bytes\":%" PRIu64 "},\n"
                "  \"restore\":{\"load_ns\":%" PRIu64 "},\n"
                "  \"exact_baseline\":{\"common_binary_exponent\":%d,"
                "\"preprocess_ns\":%" PRIu64
                ",\"method\":\"common-scale unsigned fixed-limb squared distance\"},\n"
                "  \"validation\":{\"classification_checksum\":\"%016" PRIx64
                "\",\"scalar_comparisons\":%" PRIu64 ",\"scalar_exact_fallbacks\":%" PRIu64
                ",\"batch_comparisons\":%" PRIu64 ",\"batch_exact_fallbacks\":%" PRIu64
                ",\"restored_comparisons\":%" PRIu64 ",\"restored_exact_fallbacks\":%" PRIu64 "},\n"
                "  \"measurements\":{\n",
                dataset->site_count, dataset->query_count, options->warmup, options->repetitions,
                options->tree_loops, options->brute_loops, build_wall_ns,
                build_stats->build_duration_ns, build_stats->peak_build_bytes,
                build_stats->node_count, build_stats->leaf_count, build_stats->maximum_depth,
                build_stats->fragment_count, encoded_size, load_ns, exact->common_exponent,
                exact_preprocess_ns, validation->checksum, validation->scalar_comparisons,
                validation->scalar_exact_fallbacks, validation->batch_comparisons,
                validation->batch_exact_fallbacks, validation->restored_comparisons,
                validation->restored_exact_fallbacks) < 0 ||
        !write_measurement(stream, "scalar", scalar_samples, options->repetitions,
                           options->tree_loops, scalar_summary, true) ||
        !write_measurement(stream, "batch", batch_samples, options->repetitions,
                           options->tree_loops, batch_summary, true) ||
        !write_measurement(stream, "restored_scalar", restored_samples, options->repetitions,
                           options->tree_loops, restored_summary, true) ||
        !write_measurement(stream, "exact_brute_force", brute_samples, options->repetitions,
                           options->brute_loops, brute_summary, false) ||
        fprintf(stream,
                "  },\n"
                "  \"speedup\":{\"scalar\":{\"median\":%.6f,\"geometric_mean\":%.6f,"
                "\"confidence_95_lower\":%.6f,\"confidence_95_upper\":%.6f},"
                "\"batch\":{\"median\":%.6f,\"geometric_mean\":%.6f,"
                "\"confidence_95_lower\":%.6f,\"confidence_95_upper\":%.6f}},\n"
                "  \"amortization\":{\"baseline\":\"exact_brute_force_total_cost\","
                "\"canonical_query_count\":%zu,"
                "\"break_even_definition\":\"candidate setup plus N median queries <= exact "
                "preprocess plus N median queries\","
                "\"source_construction\":\"build.wall_ns\","
                "\"restored_construction\":\"restore.load_ns\","
                "\"source_scalar\":{\"break_even_queries\":%" PRIu64
                ",\"canonical_total_cost_speedup\":%.6f},"
                "\"source_batch\":{\"break_even_queries\":%" PRIu64
                ",\"canonical_total_cost_speedup\":%.6f},"
                "\"restored_scalar\":{\"break_even_queries\":%" PRIu64
                ",\"canonical_total_cost_speedup\":%.6f}},\n"
                "  \"gate\":{\"basis\":\"steady_state_query_only\",\"required\":%s,"
                "\"minimum_median\":2.0,"
                "\"minimum_confidence_95_lower\":1.0,\"passed\":%s},\n"
                "  \"observable_sink\":\"%016" PRIx64 "\"\n"
                "}\n",
                scalar_speedup->median, scalar_speedup->geometric_mean,
                scalar_speedup->confidence_95_lower, scalar_speedup->confidence_95_upper,
                batch_speedup->median, batch_speedup->geometric_mean,
                batch_speedup->confidence_95_lower, batch_speedup->confidence_95_upper,
                dataset->query_count, amortization->source_scalar_break_even_queries,
                amortization->source_scalar_corpus_speedup,
                amortization->source_batch_break_even_queries,
                amortization->source_batch_corpus_speedup,
                amortization->restored_scalar_break_even_queries,
                amortization->restored_scalar_corpus_speedup,
                options->require_speedup ? "true" : "false", speedup_gate_passed ? "true" : "false",
                benchmark_sink) < 0 ||
        fflush(stream) != 0) {
        goto cleanup;
    }
    success = true;

cleanup:
    if (fclose(stream) != 0) {
        success = false;
    }
    return success;
}

int main(int argc, char **argv) {
    benchmark_options options;
    benchmark_dataset dataset;
    exact_dataset exact;
    odt_generation *source = NULL;
    odt_generation *restored = NULL;
    odt_query_result *batch_results = NULL;
    uint64_t *scalar_samples = NULL;
    uint64_t *batch_samples = NULL;
    uint64_t *restored_samples = NULL;
    uint64_t *brute_samples = NULL;
    odt_build_stats build_stats;
    validation_stats validation;
    measurement_summary scalar_summary;
    measurement_summary batch_summary;
    measurement_summary restored_summary;
    measurement_summary brute_summary;
    speedup_summary scalar_speedup;
    speedup_summary batch_speedup;
    amortization_summary amortization;
    uint64_t build_before;
    uint64_t build_after;
    uint64_t exact_before;
    uint64_t exact_after;
    uint64_t load_before;
    uint64_t load_after;
    uint64_t encoded_size = 0u;
    size_t tree_queries_per_sample;
    size_t brute_queries_per_sample;
    bool speedup_gate_passed;
    int exit_code = BENCH_EXECUTION_FAILURE;

    memset(&dataset, 0, sizeof(dataset));
    memset(&exact, 0, sizeof(exact));
    if (!parse_options(argc, argv, &options)) {
        print_usage(stderr, argv[0]);
        return BENCH_EXECUTION_FAILURE;
    }
    if (!verify_observable_sink()) {
        return BENCH_SEMANTIC_FAILURE;
    }
    if (options.self_check_dce) {
        (void)printf("observable_sink=enabled\n");
        return BENCH_SUCCESS;
    }
    if (options.self_check) {
        return benchmark_self_check() ? BENCH_SUCCESS : BENCH_SEMANTIC_FAILURE;
    }
    if (!read_dataset(&options, &dataset) || dataset.query_count > SIZE_MAX / options.tree_loops ||
        dataset.query_count > SIZE_MAX / options.brute_loops) {
        goto cleanup;
    }
    if (!monotonic_now(&exact_before) || !exact_dataset_init(&dataset, &exact) ||
        !monotonic_now(&exact_after) || exact_after <= exact_before) {
        (void)fprintf(stderr, "error: cannot prepare exact brute-force baseline\n");
        goto cleanup;
    }
    if (!monotonic_now(&build_before) ||
        odt_build(&BENCH_DOMAIN, dataset.sites, dataset.site_count, NULL, NULL, NULL, &build_stats,
                  &source) != ODT_OK ||
        !monotonic_now(&build_after) || build_after <= build_before) {
        (void)fprintf(stderr, "error: cannot build benchmark generation\n");
        goto cleanup;
    }
    if (odt_save_file_atomic(source, options.snapshot_path, NULL) != ODT_OK ||
        odt_generation_get_encoded_size(source, &encoded_size) != ODT_OK ||
        !monotonic_now(&load_before) ||
        odt_load_file(options.snapshot_path, NULL, NULL, &restored) != ODT_OK ||
        !monotonic_now(&load_after) || load_after <= load_before) {
        (void)fprintf(stderr, "error: cannot persist and restore benchmark generation\n");
        goto cleanup;
    }
    batch_results = calloc(dataset.query_count, sizeof(*batch_results));
    scalar_samples = calloc(options.repetitions, sizeof(*scalar_samples));
    batch_samples = calloc(options.repetitions, sizeof(*batch_samples));
    restored_samples = calloc(options.repetitions, sizeof(*restored_samples));
    brute_samples = calloc(options.repetitions, sizeof(*brute_samples));
    if (batch_results == NULL || scalar_samples == NULL || batch_samples == NULL ||
        restored_samples == NULL || brute_samples == NULL) {
        (void)fprintf(stderr, "error: cannot allocate benchmark outputs\n");
        goto cleanup;
    }
    if (!validate_paths(&dataset, &exact, source, restored, options.inject_mismatch, batch_results,
                        &validation)) {
        exit_code = BENCH_SEMANTIC_FAILURE;
        goto cleanup;
    }
    if (!run_measurements(&options, &dataset, &exact, source, restored, batch_results,
                          scalar_samples, batch_samples, restored_samples, brute_samples)) {
        (void)fprintf(stderr, "error: benchmark measurement failed\n");
        goto cleanup;
    }
    tree_queries_per_sample = dataset.query_count * options.tree_loops;
    brute_queries_per_sample = dataset.query_count * options.brute_loops;
    scalar_summary =
        summarize_measurements(scalar_samples, options.repetitions, tree_queries_per_sample);
    batch_summary =
        summarize_measurements(batch_samples, options.repetitions, tree_queries_per_sample);
    restored_summary =
        summarize_measurements(restored_samples, options.repetitions, tree_queries_per_sample);
    brute_summary =
        summarize_measurements(brute_samples, options.repetitions, brute_queries_per_sample);
    scalar_speedup = summarize_speedup(scalar_samples, options.tree_loops, brute_samples,
                                       options.brute_loops, options.repetitions);
    batch_speedup = summarize_speedup(batch_samples, options.tree_loops, brute_samples,
                                      options.brute_loops, options.repetitions);
    amortization = summarize_amortization(
        build_after - build_before, load_after - load_before, exact_after - exact_before,
        dataset.query_count, &scalar_summary, &batch_summary, &restored_summary, &brute_summary);
    speedup_gate_passed = scalar_speedup.median >= 2.0 && batch_speedup.median >= 2.0 &&
                          scalar_speedup.confidence_95_lower > 1.0 &&
                          batch_speedup.confidence_95_lower > 1.0;
    if (!write_report(&options, &dataset, &exact, &build_stats, build_after - build_before,
                      encoded_size, load_after - load_before, exact_after - exact_before,
                      &validation, scalar_samples, batch_samples, restored_samples, brute_samples,
                      &scalar_summary, &batch_summary, &restored_summary, &brute_summary,
                      &scalar_speedup, &batch_speedup, &amortization, speedup_gate_passed)) {
        goto cleanup;
    }
    (void)printf("benchmark checksum=%016" PRIx64 " scalar=%.3f Mq/s batch=%.3f Mq/s "
                 "restored=%.3f Mq/s brute=%.3f Mq/s scalar_speedup=%.3fx "
                 "batch_speedup=%.3fx ci95_lower=%.3fx/%.3fx\n",
                 validation.checksum, scalar_summary.median_queries_per_second / 1.0e6,
                 batch_summary.median_queries_per_second / 1.0e6,
                 restored_summary.median_queries_per_second / 1.0e6,
                 brute_summary.median_queries_per_second / 1.0e6, scalar_speedup.median,
                 batch_speedup.median, scalar_speedup.confidence_95_lower,
                 batch_speedup.confidence_95_lower);
    if (options.require_speedup && !speedup_gate_passed) {
        (void)fprintf(stderr, "error: benchmark speedup gate failed\n");
        exit_code = BENCH_SEMANTIC_FAILURE;
    } else {
        exit_code = BENCH_SUCCESS;
    }

cleanup:
    free(brute_samples);
    free(restored_samples);
    free(batch_samples);
    free(scalar_samples);
    free(batch_results);
    odt_generation_destroy(restored);
    odt_generation_destroy(source);
    free(exact.queries);
    free(exact.sites);
    free(dataset.queries);
    free(dataset.sites);
    return exit_code;
}
