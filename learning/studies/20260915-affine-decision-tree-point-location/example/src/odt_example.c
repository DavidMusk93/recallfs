#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <odt.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ODT_EXAMPLE_SUCCESS = 0,
    ODT_EXAMPLE_SEMANTIC_MISMATCH = 1,
    ODT_EXAMPLE_EXECUTION_ERROR = 2,
    ODT_EXAMPLE_SITE_COUNT = 1000,
    ODT_EXAMPLE_LINE_CAPACITY = 1024,
    ODT_EXAMPLE_CLASS_CAPACITY = 32,
    ODT_EXAMPLE_HEX_CAPACITY = 32,
};

static const char *const ODT_EXAMPLE_RESULT_SCHEMA = "odt-example-result/v1";
static const char *const ODT_EXAMPLE_SUMMARY_SCHEMA = "odt-example-summary/v1";
static const char *const ODT_EXAMPLE_QUERY_CLASSES[] = {
    "random", "boundary", "site", "exact_bisector", "outside", "nonfinite",
};
static const odt_domain ODT_EXAMPLE_DOMAIN = {0.0, 0.0, 12.0, 8.0};

typedef enum output_mode {
    OUTPUT_HUMAN,
    OUTPUT_MACHINE,
} output_mode;

typedef enum line_status {
    LINE_OK,
    LINE_END,
    LINE_ERROR,
    LINE_TOO_LONG,
} line_status;

typedef struct command_options {
    output_mode mode;
    const char *sites_path;
    const char *queries_path;
    const char *results_path;
    const char *summary_path;
    const char *snapshot_path;
} command_options;

typedef struct expected_result {
    bool present;
    odt_query_result value;
} expected_result;

typedef struct query_record {
    uint64_t query_id;
    char query_class[ODT_EXAMPLE_CLASS_CAPACITY];
    char x_hex[ODT_EXAMPLE_HEX_CAPACITY];
    char y_hex[ODT_EXAMPLE_HEX_CAPACITY];
    odt_point point;
    expected_result expected;
} query_record;

typedef struct scalar_result {
    odt_query_result value;
    odt_query_stats stats;
} scalar_result;

typedef struct result_totals {
    uint64_t region_count;
    uint64_t outside_count;
    uint64_t error_count;
    uint64_t comparisons;
    uint64_t exact_fallbacks;
} result_totals;

typedef struct mismatch_totals {
    uint64_t scalar_batch;
    uint64_t scalar_restored;
    uint64_t expectation;
} mismatch_totals;

static void print_usage(FILE *stream, const char *program) {
    (void)fprintf(stream,
                  "usage: %s --mode human|machine --sites PATH --queries PATH "
                  "--results PATH --summary PATH --snapshot PATH\n",
                  program);
}

static bool set_once(const char **destination, const char *value) {
    if (*destination != NULL || value == NULL) {
        return false;
    }
    *destination = value;
    return true;
}

static bool parse_options(int argc, char **argv, command_options *out_options) {
    bool mode_set = false;
    int index;

    memset(out_options, 0, sizeof(*out_options));
    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value;

        if (strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            exit(ODT_EXAMPLE_SUCCESS);
        }
        if (index + 1 >= argc) {
            return false;
        }
        value = argv[++index];
        if (strcmp(argument, "--mode") == 0) {
            if (mode_set) {
                return false;
            }
            if (strcmp(value, "human") == 0) {
                out_options->mode = OUTPUT_HUMAN;
            } else if (strcmp(value, "machine") == 0) {
                out_options->mode = OUTPUT_MACHINE;
            } else {
                return false;
            }
            mode_set = true;
        } else if (strcmp(argument, "--sites") == 0) {
            if (!set_once(&out_options->sites_path, value)) {
                return false;
            }
        } else if (strcmp(argument, "--queries") == 0) {
            if (!set_once(&out_options->queries_path, value)) {
                return false;
            }
        } else if (strcmp(argument, "--results") == 0) {
            if (!set_once(&out_options->results_path, value)) {
                return false;
            }
        } else if (strcmp(argument, "--summary") == 0) {
            if (!set_once(&out_options->summary_path, value)) {
                return false;
            }
        } else if (strcmp(argument, "--snapshot") == 0) {
            if (!set_once(&out_options->snapshot_path, value)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return mode_set && out_options->sites_path != NULL && out_options->queries_path != NULL &&
           out_options->results_path != NULL && out_options->summary_path != NULL &&
           out_options->snapshot_path != NULL;
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

static size_t split_csv(char *line, char **fields, size_t field_capacity) {
    size_t count = 1u;
    char *cursor;

    fields[0] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == '"') {
            return 0u;
        }
        if (*cursor == ',') {
            if (count == field_capacity) {
                return field_capacity + 1u;
            }
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }
    return count;
}

static bool parse_u64(const char *text, uint64_t *out_value) {
    char canonical[32];
    char *end = NULL;
    unsigned long long value;
    int length;

    if (text[0] == '\0' || text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    length = snprintf(canonical, sizeof(canonical), "%llu", value);
    if (length < 0 || (size_t)length >= sizeof(canonical) || strcmp(canonical, text) != 0) {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

static bool parse_i32(const char *text, int32_t *out_value) {
    char canonical[32];
    char *end = NULL;
    long long value;
    int length;

    if (text[0] == '\0' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    length = snprintf(canonical, sizeof(canonical), "%lld", value);
    if (length < 0 || (size_t)length >= sizeof(canonical) || strcmp(canonical, text) != 0) {
        return false;
    }
    *out_value = (int32_t)value;
    return true;
}

static bool format_binary64(double value, char *buffer, size_t capacity) {
    uint64_t bits;
    uint64_t fraction;
    uint64_t exponent_bits;
    const char *sign;
    int length;

    if (isnan(value)) {
        return snprintf(buffer, capacity, "nan") == 3;
    }
    if (isinf(value)) {
        const char *text = signbit(value) ? "-inf" : "inf";

        length = snprintf(buffer, capacity, "%s", text);
        return length >= 0 && (size_t)length < capacity;
    }
    memcpy(&bits, &value, sizeof(bits));
    sign = (bits >> 63u) == 0u ? "" : "-";
    exponent_bits = (bits >> 52u) & UINT64_C(0x7ff);
    fraction = bits & UINT64_C(0x000fffffffffffff);
    if (exponent_bits == 0u) {
        if (fraction == 0u) {
            length = snprintf(buffer, capacity, "%s0x0.0p+0", sign);
        } else {
            length = snprintf(buffer, capacity, "%s0x0.%013" PRIx64 "p-1022", sign, fraction);
        }
    } else {
        const int exponent = (int)exponent_bits - 1023;

        length = snprintf(buffer, capacity, "%s0x1.%013" PRIx64 "p%+d", sign, fraction, exponent);
    }
    return length >= 0 && (size_t)length < capacity;
}

static bool parse_binary64(const char *text, double *out_value) {
    char canonical[ODT_EXAMPLE_HEX_CAPACITY];
    char *end = NULL;
    double value;

    if (text[0] == '\0') {
        return false;
    }
    errno = 0;
    value = strtod(text, &end);
    if (end == text || *end != '\0' || !format_binary64(value, canonical, sizeof(canonical)) ||
        strcmp(canonical, text) != 0) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool valid_query_class(const char *text) {
    size_t index;
    size_t length = strlen(text);

    if (length == 0u || length >= ODT_EXAMPLE_CLASS_CAPACITY) {
        return false;
    }
    for (index = 0u; index < length; ++index) {
        const unsigned char byte = (unsigned char)text[index];

        if (byte != (unsigned char)'_' && !isalnum((int)byte)) {
            return false;
        }
    }
    for (index = 0u;
         index < sizeof(ODT_EXAMPLE_QUERY_CLASSES) / sizeof(ODT_EXAMPLE_QUERY_CLASSES[0]);
         ++index) {
        if (strcmp(text, ODT_EXAMPLE_QUERY_CLASSES[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool grow_array(void **array, size_t *capacity, size_t count, size_t item_size) {
    size_t new_capacity;
    void *new_array;

    if (count < *capacity) {
        return true;
    }
    new_capacity = *capacity == 0u ? 64u : *capacity * 2u;
    if (new_capacity <= *capacity || new_capacity > SIZE_MAX / item_size) {
        return false;
    }
    new_array = realloc(*array, new_capacity * item_size);
    if (new_array == NULL) {
        return false;
    }
    *array = new_array;
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
    char line[ODT_EXAMPLE_LINE_CAPACITY];
    bool success = false;
    line_status status;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open sites CSV %s: %s\n", path, strerror(errno));
        return false;
    }
    if (read_line(stream, line, sizeof(line), &line_number) != LINE_OK ||
        strcmp(line, header) != 0) {
        (void)fprintf(stderr, "error: invalid site CSV header: %s\n", path);
        goto cleanup;
    }
    while ((status = read_line(stream, line, sizeof(line), &line_number)) == LINE_OK) {
        char *fields[4];
        uint64_t ordinal;
        int32_t region_id;
        double x;
        double y;
        size_t prior;

        if (split_csv(line, fields, 4u) != 4u || !parse_u64(fields[0], &ordinal) ||
            ordinal != (uint64_t)count || !parse_i32(fields[1], &region_id) ||
            region_id != (int32_t)count || !parse_binary64(fields[2], &x) ||
            !parse_binary64(fields[3], &y) || !isfinite(x) || !isfinite(y) ||
            x < ODT_EXAMPLE_DOMAIN.min_x || x > ODT_EXAMPLE_DOMAIN.max_x ||
            y < ODT_EXAMPLE_DOMAIN.min_y || y > ODT_EXAMPLE_DOMAIN.max_y) {
            (void)fprintf(stderr, "error: invalid site CSV row %zu: %s\n", line_number, path);
            goto cleanup;
        }
        for (prior = 0u; prior < count; ++prior) {
            if (sites[prior].x == x && sites[prior].y == y) {
                (void)fprintf(stderr, "error: duplicate site at row %zu: %s\n", line_number, path);
                goto cleanup;
            }
        }
        if (!grow_array((void **)&sites, &capacity, count, sizeof(*sites))) {
            (void)fprintf(stderr, "error: cannot allocate site input\n");
            goto cleanup;
        }
        sites[count++] = (odt_site){x, y, region_id};
    }
    if (status == LINE_ERROR || status == LINE_TOO_LONG) {
        (void)fprintf(stderr, "error: cannot read sites CSV row %zu: %s\n", line_number + 1u, path);
        goto cleanup;
        goto cleanup;
    }
    if (count != ODT_EXAMPLE_SITE_COUNT) {
        (void)fprintf(stderr, "error: expected %d sites, found %zu: %s\n", ODT_EXAMPLE_SITE_COUNT,
                      count, path);
        goto cleanup;
    }
    *out_sites = sites;
    *out_count = count;
    sites = NULL;
    success = true;

cleanup:
    free(sites);
    if (fclose(stream) != 0) {
        (void)fprintf(stderr, "error: cannot close sites CSV %s: %s\n", path, strerror(errno));
        success = false;
    }
    return success;
}

static bool parse_status(const char *text, odt_status *out_status) {
    int value;

    for (value = (int)ODT_OK; value <= (int)ODT_INTERNAL_ERROR; ++value) {
        if (strcmp(text, odt_status_string((odt_status)value)) == 0) {
            *out_status = (odt_status)value;
            return true;
        }
    }
    return false;
}

static bool parse_kind(const char *text, odt_result_kind *out_kind) {
    if (strcmp(text, "region") == 0) {
        *out_kind = ODT_RESULT_REGION;
    } else if (strcmp(text, "outside") == 0) {
        *out_kind = ODT_RESULT_OUTSIDE;
    } else if (strcmp(text, "error") == 0) {
        *out_kind = ODT_RESULT_ERROR;
    } else {
        return false;
    }
    return true;
}

static bool valid_expected_result(const odt_query_result *result) {
    if (result->kind == ODT_RESULT_REGION) {
        return result->status == ODT_OK;
    }
    if (result->kind == ODT_RESULT_OUTSIDE) {
        return result->status == ODT_OK && result->region_id == 0;
    }
    return result->kind == ODT_RESULT_ERROR && result->status != ODT_OK && result->region_id == 0;
}

static bool read_queries(const char *path, query_record **out_queries, size_t *out_count) {
    static const char header[] =
        "query_id,class,x_hex,y_hex,site_a,site_b,expected_kind,expected_status,"
        "expected_region_id";
    FILE *stream = fopen(path, "r");
    query_record *queries = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t line_number = 0u;
    char line[ODT_EXAMPLE_LINE_CAPACITY];
    bool success = false;
    line_status status;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open queries CSV %s: %s\n", path, strerror(errno));
        return false;
    }
    if (read_line(stream, line, sizeof(line), &line_number) != LINE_OK ||
        strcmp(line, header) != 0) {
        (void)fprintf(stderr, "error: invalid query CSV header: %s\n", path);
        goto cleanup;
    }
    while ((status = read_line(stream, line, sizeof(line), &line_number)) == LINE_OK) {
        char *fields[9];
        uint64_t query_id;
        uint64_t site_a = 0u;
        uint64_t site_b = 0u;
        double x;
        double y;
        expected_result expected = {false, {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, 0}};
        bool has_site_ordinals;

        if (split_csv(line, fields, 9u) != 9u || !parse_u64(fields[0], &query_id) ||
            query_id != (uint64_t)count || !valid_query_class(fields[1]) ||
            strlen(fields[2]) >= ODT_EXAMPLE_HEX_CAPACITY ||
            strlen(fields[3]) >= ODT_EXAMPLE_HEX_CAPACITY || !parse_binary64(fields[2], &x) ||
            !parse_binary64(fields[3], &y)) {
            (void)fprintf(stderr, "error: invalid query CSV row %zu: %s\n", line_number, path);
            goto cleanup;
        }
        if ((fields[4][0] == '\0') != (fields[5][0] == '\0')) {
            (void)fprintf(stderr, "error: partial site ordinals at query row %zu: %s\n",
                          line_number, path);
            goto cleanup;
        }
        has_site_ordinals = fields[4][0] != '\0';
        if (has_site_ordinals &&
            (!parse_u64(fields[4], &site_a) || !parse_u64(fields[5], &site_b) ||
             site_a >= ODT_EXAMPLE_SITE_COUNT || site_b >= ODT_EXAMPLE_SITE_COUNT)) {
            (void)fprintf(stderr, "error: invalid site ordinals at query row %zu: %s\n",
                          line_number, path);
            goto cleanup;
        }
        if ((strcmp(fields[1], "site") == 0 && (!has_site_ordinals || site_a != site_b)) ||
            (strcmp(fields[1], "exact_bisector") == 0 &&
             (!has_site_ordinals || site_a == site_b)) ||
            ((strcmp(fields[1], "site") != 0 && strcmp(fields[1], "exact_bisector") != 0) &&
             has_site_ordinals)) {
            (void)fprintf(stderr, "error: inconsistent site ordinals at query row %zu: %s\n",
                          line_number, path);
            goto cleanup;
        }
        if ((fields[6][0] == '\0') != (fields[7][0] == '\0') ||
            (fields[6][0] == '\0') != (fields[8][0] == '\0')) {
            (void)fprintf(stderr, "error: partial expectation at query row %zu: %s\n", line_number,
                          path);
            goto cleanup;
        }
        if (fields[6][0] != '\0') {
            if (!parse_kind(fields[6], &expected.value.kind) ||
                !parse_status(fields[7], &expected.value.status) ||
                !parse_i32(fields[8], &expected.value.region_id) ||
                !valid_expected_result(&expected.value)) {
                (void)fprintf(stderr, "error: invalid expectation at query row %zu: %s\n",
                              line_number, path);
                goto cleanup;
            }
            expected.present = true;
        }
        if (!grow_array((void **)&queries, &capacity, count, sizeof(*queries))) {
            (void)fprintf(stderr, "error: cannot allocate query input\n");
            goto cleanup;
        }
        queries[count].query_id = query_id;
        (void)snprintf(queries[count].query_class, sizeof(queries[count].query_class), "%s",
                       fields[1]);
        (void)snprintf(queries[count].x_hex, sizeof(queries[count].x_hex), "%s", fields[2]);
        (void)snprintf(queries[count].y_hex, sizeof(queries[count].y_hex), "%s", fields[3]);
        queries[count].point = (odt_point){x, y};
        queries[count].expected = expected;
        count += 1u;
    }
    if (status == LINE_ERROR || status == LINE_TOO_LONG) {
        (void)fprintf(stderr, "error: cannot read queries CSV row %zu: %s\n", line_number + 1u,
                      path);
        goto cleanup;
    }
    if (count == 0u) {
        (void)fprintf(stderr, "error: queries CSV is empty: %s\n", path);
        goto cleanup;
    }
    *out_queries = queries;
    *out_count = count;
    queries = NULL;
    success = true;

cleanup:
    free(queries);
    if (fclose(stream) != 0) {
        (void)fprintf(stderr, "error: cannot close queries CSV %s: %s\n", path, strerror(errno));
        success = false;
    }
    return success;
}

static const char *kind_string(odt_result_kind kind) {
    switch (kind) {
    case ODT_RESULT_REGION:
        return "region";
    case ODT_RESULT_OUTSIDE:
        return "outside";
    case ODT_RESULT_ERROR:
        return "error";
    }
    return "invalid";
}

static bool same_result(const odt_query_result *first, const odt_query_result *second) {
    return first->kind == second->kind && first->status == second->status &&
           first->region_id == second->region_id;
}

static scalar_result query_one(const odt_generation *generation, const odt_point *point) {
    scalar_result result = {
        {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, 0},
        {0u, 0u},
    };
    odt_status status = odt_query(generation, point, &result.value, &result.stats);

    if (status != ODT_OK) {
        result.value.kind = ODT_RESULT_ERROR;
        result.value.status = status;
        result.value.region_id = 0;
        result.stats.comparisons = 0u;
        result.stats.exact_fallbacks = 0u;
    }
    return result;
}

static void add_result_totals(result_totals *totals, const odt_query_result *result,
                              const odt_query_stats *stats) {
    if (result->kind == ODT_RESULT_REGION) {
        totals->region_count += 1u;
    } else if (result->kind == ODT_RESULT_OUTSIDE) {
        totals->outside_count += 1u;
    } else {
        totals->error_count += 1u;
    }
    if (stats != NULL) {
        totals->comparisons += stats->comparisons;
        totals->exact_fallbacks += stats->exact_fallbacks;
    }
}

static bool json_string(FILE *stream, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;

    if (fputc('"', stream) == EOF) {
        return false;
    }
    while (*cursor != '\0') {
        const unsigned char byte = *cursor++;

        if (byte == (unsigned char)'"' || byte == (unsigned char)'\\') {
            if (fputc('\\', stream) == EOF || fputc((int)byte, stream) == EOF) {
                return false;
            }
        } else if (byte == (unsigned char)'\b') {
            if (fputs("\\b", stream) == EOF) {
                return false;
            }
        } else if (byte == (unsigned char)'\f') {
            if (fputs("\\f", stream) == EOF) {
                return false;
            }
        } else if (byte == (unsigned char)'\n') {
            if (fputs("\\n", stream) == EOF) {
                return false;
            }
        } else if (byte == (unsigned char)'\r') {
            if (fputs("\\r", stream) == EOF) {
                return false;
            }
        } else if (byte == (unsigned char)'\t') {
            if (fputs("\\t", stream) == EOF) {
                return false;
            }
        } else if (byte < 0x20u || byte > 0x7eu) {
            if (fprintf(stream, "\\u%04x", (unsigned int)byte) < 0) {
                return false;
            }
        } else if (fputc((int)byte, stream) == EOF) {
            return false;
        }
    }
    return fputc('"', stream) != EOF;
}

static bool write_semantic_result(FILE *stream, const odt_query_result *result,
                                  const odt_query_stats *stats) {
    if (fputs("{\"kind\":", stream) == EOF || !json_string(stream, kind_string(result->kind)) ||
        fputs(",\"status\":", stream) == EOF ||
        !json_string(stream, odt_status_string(result->status)) ||
        fprintf(stream, ",\"region_id\":%" PRId32, result->region_id) < 0) {
        return false;
    }
    if (stats != NULL &&
        fprintf(stream, ",\"comparisons\":%" PRIu64 ",\"exact_fallbacks\":%" PRIu64,
                stats->comparisons, stats->exact_fallbacks) < 0) {
        return false;
    }
    return fputc('}', stream) != EOF;
}

static bool write_result_rows(const char *path, const query_record *queries, size_t query_count,
                              const scalar_result *scalar, const odt_query_result *batch,
                              const scalar_result *restored) {
    FILE *stream = fopen(path, "w");
    size_t index;
    bool success = false;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open result JSONL %s: %s\n", path, strerror(errno));
        return false;
    }
    for (index = 0u; index < query_count; ++index) {
        if (fputs("{\"schema\":", stream) == EOF ||
            !json_string(stream, ODT_EXAMPLE_RESULT_SCHEMA) ||
            fprintf(stream, ",\"query_id\":%" PRIu64 ",\"class\":", queries[index].query_id) < 0 ||
            !json_string(stream, queries[index].query_class) ||
            fputs(",\"x_hex\":", stream) == EOF || !json_string(stream, queries[index].x_hex) ||
            fputs(",\"y_hex\":", stream) == EOF || !json_string(stream, queries[index].y_hex) ||
            fputs(",\"scalar\":", stream) == EOF ||
            !write_semantic_result(stream, &scalar[index].value, &scalar[index].stats) ||
            fputs(",\"batch\":", stream) == EOF ||
            !write_semantic_result(stream, &batch[index], NULL) ||
            fputs(",\"restored\":", stream) == EOF ||
            !write_semantic_result(stream, &restored[index].value, &restored[index].stats) ||
            fputs("}\n", stream) == EOF) {
            (void)fprintf(stderr, "error: cannot write result JSONL %s\n", path);
            goto cleanup;
        }
    }
    success = true;

cleanup:
    if (fclose(stream) != 0) {
        (void)fprintf(stderr, "error: cannot close result JSONL %s: %s\n", path, strerror(errno));
        success = false;
    }
    return success;
}

static uint64_t count_class(const query_record *queries, size_t query_count,
                            const char *query_class) {
    uint64_t count = 0u;
    size_t index;

    for (index = 0u; index < query_count; ++index) {
        if (strcmp(queries[index].query_class, query_class) == 0) {
            count += 1u;
        }
    }
    return count;
}

static uint64_t mismatch_total(const mismatch_totals *mismatches) {
    return mismatches->scalar_batch + mismatches->scalar_restored + mismatches->expectation;
}

static bool write_totals(FILE *stream, const result_totals *totals) {
    return fprintf(stream,
                   "{\"region\":%" PRIu64 ",\"outside\":%" PRIu64 ",\"error\":%" PRIu64
                   ",\"comparisons\":%" PRIu64 ",\"exact_fallbacks\":%" PRIu64 "}",
                   totals->region_count, totals->outside_count, totals->error_count,
                   totals->comparisons, totals->exact_fallbacks) >= 0;
}

static bool write_summary(FILE *stream, const command_options *options, size_t site_count,
                          const query_record *queries, size_t query_count,
                          const odt_build_stats *build_stats, uint64_t encoded_size,
                          const result_totals *scalar_totals, const result_totals *batch_totals,
                          const result_totals *restored_totals, const mismatch_totals *mismatches) {
    if (fputs("{\"schema\":", stream) == EOF || !json_string(stream, ODT_EXAMPLE_SUMMARY_SCHEMA) ||
        fprintf(stream,
                ",\"inputs\":{\"site_count\":%zu,\"query_count\":%zu,"
                "\"query_classes\":{\"random\":%" PRIu64 ",\"boundary\":%" PRIu64
                ",\"site\":%" PRIu64 ",\"exact_bisector\":%" PRIu64 ",\"outside\":%" PRIu64
                ",\"nonfinite\":%" PRIu64 "}}",
                site_count, query_count, count_class(queries, query_count, "random"),
                count_class(queries, query_count, "boundary"),
                count_class(queries, query_count, "site"),
                count_class(queries, query_count, "exact_bisector"),
                count_class(queries, query_count, "outside"),
                count_class(queries, query_count, "nonfinite")) < 0 ||
        fprintf(stream,
                ",\"tree\":{\"nodes\":%" PRIu64 ",\"leaves\":%" PRIu64 ",\"maximum_depth\":%" PRIu32
                ",\"fragments\":%" PRIu64 ",\"build_work\":%" PRIu64
                ",\"peak_build_bytes\":%" PRIu64 ",\"build_duration_ns\":%" PRIu64
                ",\"encoded_bytes\":%" PRIu64 "}",
                build_stats->node_count, build_stats->leaf_count, build_stats->maximum_depth,
                build_stats->fragment_count, build_stats->build_work, build_stats->peak_build_bytes,
                build_stats->build_duration_ns, encoded_size) < 0 ||
        fputs(",\"results\":{\"scalar\":", stream) == EOF || !write_totals(stream, scalar_totals) ||
        fputs(",\"batch\":", stream) == EOF || !write_totals(stream, batch_totals) ||
        fputs(",\"restored\":", stream) == EOF || !write_totals(stream, restored_totals) ||
        fprintf(stream,
                "},\"mismatches\":{\"scalar_batch\":%" PRIu64 ",\"scalar_restored\":%" PRIu64
                ",\"expectation\":%" PRIu64 ",\"total\":%" PRIu64 "},\"artifacts\":{\"sites\":",
                mismatches->scalar_batch, mismatches->scalar_restored, mismatches->expectation,
                mismatch_total(mismatches)) < 0 ||
        !json_string(stream, options->sites_path) || fputs(",\"queries\":", stream) == EOF ||
        !json_string(stream, options->queries_path) || fputs(",\"results\":", stream) == EOF ||
        !json_string(stream, options->results_path) || fputs(",\"summary\":", stream) == EOF ||
        !json_string(stream, options->summary_path) || fputs(",\"snapshot\":", stream) == EOF ||
        !json_string(stream, options->snapshot_path) || fputs("}}\n", stream) == EOF) {
        return false;
    }
    return !ferror(stream);
}

static bool
write_summary_file(const command_options *options, size_t site_count, const query_record *queries,
                   size_t query_count, const odt_build_stats *build_stats, uint64_t encoded_size,
                   const result_totals *scalar_totals, const result_totals *batch_totals,
                   const result_totals *restored_totals, const mismatch_totals *mismatches) {
    FILE *stream = fopen(options->summary_path, "w");
    bool success;

    if (stream == NULL) {
        (void)fprintf(stderr, "error: cannot open summary JSON %s: %s\n", options->summary_path,
                      strerror(errno));
        return false;
    }
    success = write_summary(stream, options, site_count, queries, query_count, build_stats,
                            encoded_size, scalar_totals, batch_totals, restored_totals, mismatches);
    if (!success) {
        (void)fprintf(stderr, "error: cannot write summary JSON %s\n", options->summary_path);
    }
    if (fclose(stream) != 0) {
        (void)fprintf(stderr, "error: cannot close summary JSON %s: %s\n", options->summary_path,
                      strerror(errno));
        success = false;
    }
    return success;
}

static void print_human_report(const command_options *options, size_t site_count,
                               const query_record *queries, size_t query_count,
                               const scalar_result *scalar, const odt_build_stats *build_stats,
                               uint64_t encoded_size, const result_totals *scalar_totals,
                               const result_totals *batch_totals,
                               const result_totals *restored_totals,
                               const mismatch_totals *mismatches) {
    size_t class_index;

    (void)printf("Oblique Decision Tree example\n");
    (void)printf("generated inputs: sites=%zu queries=%zu\n", site_count, query_count);
    (void)printf("query classes:");
    for (class_index = 0u;
         class_index < sizeof(ODT_EXAMPLE_QUERY_CLASSES) / sizeof(ODT_EXAMPLE_QUERY_CLASSES[0]);
         ++class_index) {
        (void)printf(" %s=%" PRIu64, ODT_EXAMPLE_QUERY_CLASSES[class_index],
                     count_class(queries, query_count, ODT_EXAMPLE_QUERY_CLASSES[class_index]));
    }
    (void)printf("\n");
    (void)printf("tree: nodes=%" PRIu64 " leaves=%" PRIu64 " depth=%" PRIu32 " fragments=%" PRIu64
                 " encoded_bytes=%" PRIu64 " build_ns=%" PRIu64 "\n",
                 build_stats->node_count, build_stats->leaf_count, build_stats->maximum_depth,
                 build_stats->fragment_count, encoded_size, build_stats->build_duration_ns);
    (void)printf("result totals: region=%" PRIu64 " outside=%" PRIu64 " error=%" PRIu64
                 " comparisons=%" PRIu64 " exact_fallbacks=%" PRIu64 "\n",
                 scalar_totals->region_count, scalar_totals->outside_count,
                 scalar_totals->error_count, scalar_totals->comparisons,
                 scalar_totals->exact_fallbacks);
    (void)printf("batch totals: region=%" PRIu64 " outside=%" PRIu64 " error=%" PRIu64
                 " comparisons=%" PRIu64 " exact_fallbacks=%" PRIu64 "\n",
                 batch_totals->region_count, batch_totals->outside_count, batch_totals->error_count,
                 batch_totals->comparisons, batch_totals->exact_fallbacks);
    (void)printf("restored totals: region=%" PRIu64 " outside=%" PRIu64 " error=%" PRIu64
                 " comparisons=%" PRIu64 " exact_fallbacks=%" PRIu64 "\n",
                 restored_totals->region_count, restored_totals->outside_count,
                 restored_totals->error_count, restored_totals->comparisons,
                 restored_totals->exact_fallbacks);
    (void)printf("mismatches: scalar_batch=%" PRIu64 " scalar_restored=%" PRIu64
                 " expectation=%" PRIu64 " total=%" PRIu64 "\n",
                 mismatches->scalar_batch, mismatches->scalar_restored, mismatches->expectation,
                 mismatch_total(mismatches));
    (void)printf("representative rows:\n");
    for (class_index = 0u;
         class_index < sizeof(ODT_EXAMPLE_QUERY_CLASSES) / sizeof(ODT_EXAMPLE_QUERY_CLASSES[0]);
         ++class_index) {
        size_t index;

        for (index = 0u; index < query_count; ++index) {
            if (strcmp(queries[index].query_class, ODT_EXAMPLE_QUERY_CLASSES[class_index]) == 0) {
                (void)printf("  id=%" PRIu64 " class=%s point=(%s,%s) result=%s:%" PRId32
                             " comparisons=%" PRIu64 " exact_fallbacks=%" PRIu64 "\n",
                             queries[index].query_id, queries[index].query_class,
                             queries[index].x_hex, queries[index].y_hex,
                             kind_string(scalar[index].value.kind), scalar[index].value.region_id,
                             scalar[index].stats.comparisons, scalar[index].stats.exact_fallbacks);
                break;
            }
        }
    }
    (void)printf("artifacts:\n");
    (void)printf("  results=%s\n", options->results_path);
    (void)printf("  summary=%s\n", options->summary_path);
    (void)printf("  snapshot=%s\n", options->snapshot_path);
}

static bool allocate_outputs(size_t query_count, odt_point **out_points, scalar_result **out_scalar,
                             odt_query_result **out_batch, scalar_result **out_restored) {
    if (query_count > SIZE_MAX / sizeof(**out_points) ||
        query_count > SIZE_MAX / sizeof(**out_scalar) ||
        query_count > SIZE_MAX / sizeof(**out_batch) ||
        query_count > SIZE_MAX / sizeof(**out_restored)) {
        return false;
    }
    *out_points = malloc(query_count * sizeof(**out_points));
    *out_scalar = calloc(query_count, sizeof(**out_scalar));
    *out_batch = calloc(query_count, sizeof(**out_batch));
    *out_restored = calloc(query_count, sizeof(**out_restored));
    return *out_points != NULL && *out_scalar != NULL && *out_batch != NULL &&
           *out_restored != NULL;
}

int main(int argc, char **argv) {
    command_options options;
    odt_site *sites = NULL;
    query_record *queries = NULL;
    odt_point *points = NULL;
    scalar_result *scalar = NULL;
    odt_query_result *batch = NULL;
    scalar_result *restored = NULL;
    odt_generation *source_generation = NULL;
    odt_generation *restored_generation = NULL;
    odt_build_stats build_stats;
    odt_batch_stats batch_stats;
    result_totals scalar_totals = {0u, 0u, 0u, 0u, 0u};
    result_totals batch_totals = {0u, 0u, 0u, 0u, 0u};
    result_totals restored_totals = {0u, 0u, 0u, 0u, 0u};
    mismatch_totals mismatches = {0u, 0u, 0u};
    uint64_t encoded_size = 0u;
    size_t site_count = 0u;
    size_t query_count = 0u;
    size_t index;
    odt_status status;
    int exit_code = ODT_EXAMPLE_EXECUTION_ERROR;

    if (!parse_options(argc, argv, &options)) {
        print_usage(stderr, argv[0]);
        return ODT_EXAMPLE_EXECUTION_ERROR;
    }
    errno = 0;
    if (!read_sites(options.sites_path, &sites, &site_count) ||
        !read_queries(options.queries_path, &queries, &query_count)) {
        goto cleanup;
    }
    if (!allocate_outputs(query_count, &points, &scalar, &batch, &restored)) {
        (void)fprintf(stderr, "error: cannot allocate query outputs\n");
        goto cleanup;
    }
    for (index = 0u; index < query_count; ++index) {
        points[index] = queries[index].point;
    }

    status = odt_build(&ODT_EXAMPLE_DOMAIN, sites, site_count, NULL, NULL, NULL, &build_stats,
                       &source_generation);
    if (status != ODT_OK) {
        (void)fprintf(stderr, "error: odt_build: %s\n", odt_status_string(status));
        goto cleanup;
    }
    for (index = 0u; index < query_count; ++index) {
        scalar[index] = query_one(source_generation, &points[index]);
        add_result_totals(&scalar_totals, &scalar[index].value, &scalar[index].stats);
    }
    status = odt_query_batch(source_generation, query_count, points, sizeof(*points), batch,
                             sizeof(*batch), &batch_stats);
    if (status != ODT_OK) {
        (void)fprintf(stderr, "error: odt_query_batch: %s\n", odt_status_string(status));
        goto cleanup;
    }
    for (index = 0u; index < query_count; ++index) {
        add_result_totals(&batch_totals, &batch[index], NULL);
    }
    batch_totals.comparisons = batch_stats.comparisons;
    batch_totals.exact_fallbacks = batch_stats.exact_fallbacks;
    if (batch_stats.attempted != (uint64_t)query_count ||
        batch_stats.region_count != batch_totals.region_count ||
        batch_stats.outside_count != batch_totals.outside_count ||
        batch_stats.failed_count != batch_totals.error_count) {
        mismatches.scalar_batch += 1u;
    }

    status = odt_save_file_atomic(source_generation, options.snapshot_path, NULL);
    if (status != ODT_OK) {
        (void)fprintf(stderr, "error: odt_save_file_atomic: %s\n", odt_status_string(status));
        goto cleanup;
    }
    status = odt_load_file(options.snapshot_path, NULL, NULL, &restored_generation);
    if (status != ODT_OK) {
        (void)fprintf(stderr, "error: odt_load_file: %s\n", odt_status_string(status));
        goto cleanup;
    }
    status = odt_generation_get_encoded_size(source_generation, &encoded_size);
    if (status != ODT_OK) {
        (void)fprintf(stderr, "error: odt_generation_get_encoded_size: %s\n",
                      odt_status_string(status));
        goto cleanup;
    }
    for (index = 0u; index < query_count; ++index) {
        restored[index] = query_one(restored_generation, &points[index]);
        add_result_totals(&restored_totals, &restored[index].value, &restored[index].stats);
        if (!same_result(&scalar[index].value, &batch[index])) {
            mismatches.scalar_batch += 1u;
        }
        if (!same_result(&scalar[index].value, &restored[index].value)) {
            mismatches.scalar_restored += 1u;
        }
        if (queries[index].expected.present &&
            !same_result(&scalar[index].value, &queries[index].expected.value)) {
            mismatches.expectation += 1u;
        }
    }

    if (!write_result_rows(options.results_path, queries, query_count, scalar, batch, restored) ||
        !write_summary_file(&options, site_count, queries, query_count, &build_stats, encoded_size,
                            &scalar_totals, &batch_totals, &restored_totals, &mismatches)) {
        goto cleanup;
    }
    if (options.mode == OUTPUT_MACHINE) {
        if (!write_summary(stdout, &options, site_count, queries, query_count, &build_stats,
                           encoded_size, &scalar_totals, &batch_totals, &restored_totals,
                           &mismatches) ||
            fflush(stdout) != 0) {
            (void)fprintf(stderr, "error: cannot write machine output\n");
            goto cleanup;
        }
    } else {
        print_human_report(&options, site_count, queries, query_count, scalar, &build_stats,
                           encoded_size, &scalar_totals, &batch_totals, &restored_totals,
                           &mismatches);
        if (fflush(stdout) != 0) {
            (void)fprintf(stderr, "error: cannot write human output\n");
            goto cleanup;
        }
    }
    exit_code =
        mismatch_total(&mismatches) == 0u ? ODT_EXAMPLE_SUCCESS : ODT_EXAMPLE_SEMANTIC_MISMATCH;

cleanup:
    odt_generation_destroy(restored_generation);
    odt_generation_destroy(source_generation);
    free(restored);
    free(batch);
    free(scalar);
    free(points);
    free(queries);
    free(sites);
    return exit_code;
}
