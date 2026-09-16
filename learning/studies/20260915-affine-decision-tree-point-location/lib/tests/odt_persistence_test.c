#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "odt_persistence_test_support.h"
#include "odt_test_support.h"

#include "odt_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const odt_domain test_domain = {-2.0, -2.0, 2.0, 2.0};
static const odt_site test_sites[] = {
    {-1.0, 0.0, 11},
    {1.0, 0.0, 22},
    {0.0, 1.5, 33},
};

static odt_generation *build_representative(const odt_allocator *allocator) {
    odt_generation *generation = NULL;

    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, allocator, NULL, &generation),
                    ODT_OK);
    return generation;
}

static void test_crc32c_known_vector(void) {
    static const char text[] = "123456789";
    uint32_t state = odt_internal_crc32c_begin();

    state = odt_internal_crc32c_extend(state, text, 4u);
    state = odt_internal_crc32c_extend(state, text + 4u, sizeof(text) - 1u - 4u);
    ODT_TEST_CHECK(odt_internal_crc32c_end(state) == UINT32_C(0xe3069283));
    ODT_TEST_CHECK(odt_internal_crc32c(text, sizeof(text) - 1u) == UINT32_C(0xe3069283));
}

static void test_streaming_round_trip(void) {
    odt_generation *source_generation = build_representative(NULL);
    odt_generation *restored_generation = NULL;
    odt_test_byte_buffer encoded = {0};
    odt_test_memory_source source;
    uint64_t expected_size = 0u;
    uint64_t observed_size = UINT64_MAX;
    uint64_t source_digest;

    ODT_TEST_STATUS(odt_generation_get_encoded_size(source_generation, &expected_size), ODT_OK);
    encoded.reentrant_generation = source_generation;
    {
        odt_sink sink = {&encoded, odt_test_buffer_sink_write};

        ODT_TEST_STATUS(odt_encode(source_generation, NULL, &sink, &observed_size), ODT_OK);
    }
    ODT_TEST_CHECK(observed_size == expected_size);
    ODT_TEST_CHECK(encoded.size == expected_size);
    ODT_TEST_CHECK(encoded.write_calls > 3u);

    memset(&source, 0, sizeof(source));
    source.data = encoded.data;
    source.size = encoded.size;
    source.reentrant_generation = source_generation;
    ODT_TEST_STATUS(odt_test_load_from_memory(&source, NULL, NULL, &restored_generation), ODT_OK);
    ODT_TEST_CHECK(source.maximum_request <= 4096u);
    odt_test_expect_metadata_equal(source_generation, restored_generation);
    source_digest = odt_test_query_digest(source_generation);
    ODT_TEST_CHECK(odt_test_query_digest(restored_generation) == source_digest);

    odt_generation_destroy(restored_generation);
    odt_generation_destroy(source_generation);
    odt_test_byte_buffer_destroy(&encoded);
}

static void test_streaming_failures_are_transactional(void) {
    odt_generation *generation = build_representative(NULL);
    odt_test_byte_buffer encoded = {0};
    odt_test_memory_source source;
    odt_generation *loaded = (odt_generation *)(uintptr_t)1u;
    uint64_t encoded_size = UINT64_MAX;
    odt_sink sink;
    odt_source invalid_source;

    ODT_TEST_STATUS(odt_encode(NULL, NULL, NULL, NULL), ODT_INVALID_ARGUMENT);
    ODT_TEST_STATUS(odt_encode(generation, NULL, NULL, NULL), ODT_INVALID_ARGUMENT);
    sink.context = &encoded;
    sink.write = NULL;
    ODT_TEST_STATUS(odt_encode(generation, NULL, &sink, NULL), ODT_INVALID_ARGUMENT);

    encoded.fail_at_call = 1u;
    sink.write = odt_test_buffer_sink_write;
    ODT_TEST_STATUS(odt_encode(generation, NULL, &sink, &encoded_size), ODT_IO_ERROR);
    ODT_TEST_CHECK(encoded_size == UINT64_MAX);
    encoded.fail_at_call = 0u;
    ODT_TEST_STATUS(odt_test_encode_to_buffer(generation, &encoded), ODT_OK);

    ODT_TEST_STATUS(odt_load(NULL, NULL, NULL, &loaded), ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(loaded == NULL);
    invalid_source.context = NULL;
    invalid_source.encoded_size = (uint64_t)encoded.size;
    invalid_source.read_at = NULL;
    loaded = (odt_generation *)(uintptr_t)1u;
    ODT_TEST_STATUS(odt_load(&invalid_source, NULL, NULL, &loaded), ODT_INVALID_ARGUMENT);
    ODT_TEST_CHECK(loaded == NULL);
    ODT_TEST_STATUS(odt_load(&invalid_source, NULL, NULL, NULL), ODT_INVALID_ARGUMENT);

    memset(&source, 0, sizeof(source));
    source.data = encoded.data;
    source.size = encoded.size;
    source.fail_at_call = 1u;
    loaded = (odt_generation *)(uintptr_t)1u;
    ODT_TEST_STATUS(odt_test_load_from_memory(&source, NULL, NULL, &loaded), ODT_IO_ERROR);
    ODT_TEST_CHECK(loaded == NULL);

    odt_test_byte_buffer_destroy(&encoded);
    odt_generation_destroy(generation);
}

static void test_frozen_fixtures(void) {
    static const struct {
        const char *name;
        odt_test_fixture_kind kind;
    } fixtures[] = {
        {"format_v1_minimal.odt", ODT_TEST_FIXTURE_MINIMAL},
        {"format_v1_representative.odt", ODT_TEST_FIXTURE_REPRESENTATIVE},
    };
    odt_generation *expected_generation = build_representative(NULL);
    size_t index;

    for (index = 0u; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        odt_test_byte_buffer frozen = {0};
        odt_test_byte_buffer independent = {0};
        odt_test_byte_buffer reencoded = {0};
        odt_test_memory_source source;
        odt_generation *generation = NULL;

        odt_test_read_fixture(fixtures[index].name, &frozen);
        odt_test_independent_fixture(fixtures[index].kind, &independent);
        ODT_TEST_CHECK(frozen.size == independent.size);
        ODT_TEST_CHECK(memcmp(frozen.data, independent.data, frozen.size) == 0);

        memset(&source, 0, sizeof(source));
        source.data = frozen.data;
        source.size = frozen.size;
        ODT_TEST_STATUS(odt_test_load_from_memory(&source, NULL, NULL, &generation), ODT_OK);
        ODT_TEST_STATUS(odt_test_encode_to_buffer(generation, &reencoded), ODT_OK);
        ODT_TEST_CHECK(reencoded.size == frozen.size);
        ODT_TEST_CHECK(memcmp(reencoded.data, frozen.data, frozen.size) == 0);
        if (fixtures[index].kind == ODT_TEST_FIXTURE_MINIMAL) {
            const odt_point inside = {0.1, 0.9};
            const odt_point outside = {2.0, 0.5};
            odt_query_result result;

            ODT_TEST_STATUS(odt_query(generation, &inside, &result, NULL), ODT_OK);
            ODT_TEST_CHECK(result.kind == ODT_RESULT_REGION);
            ODT_TEST_CHECK(result.region_id == 7);
            ODT_TEST_STATUS(odt_query(generation, &outside, &result, NULL), ODT_OK);
            ODT_TEST_CHECK(result.kind == ODT_RESULT_OUTSIDE);
        } else {
            ODT_TEST_CHECK(odt_test_query_digest(generation) ==
                           odt_test_query_digest(expected_generation));
        }
        odt_generation_destroy(generation);
        odt_test_byte_buffer_destroy(&reencoded);
        odt_test_byte_buffer_destroy(&independent);
        odt_test_byte_buffer_destroy(&frozen);
    }
    odt_generation_destroy(expected_generation);
}

static void test_allocator_address_independence(void) {
    odt_test_byte_buffer frozen = {0};
    odt_test_byte_buffer first_encoded = {0};
    odt_test_byte_buffer second_encoded = {0};
    odt_test_memory_source first_source;
    odt_test_memory_source second_source;
    odt_test_allocator_state first_state = {.live = true};
    odt_test_allocator_state second_state = {.live = true};
    odt_allocator first_allocator;
    odt_allocator second_allocator;
    odt_generation *first = NULL;
    odt_generation *second = NULL;

    odt_test_read_fixture("format_v1_representative.odt", &frozen);
    memset(&first_source, 0, sizeof(first_source));
    memset(&second_source, 0, sizeof(second_source));
    first_source.data = frozen.data;
    first_source.size = frozen.size;
    second_source.data = frozen.data;
    second_source.size = frozen.size;
    odt_test_counting_allocator_init(&first_allocator, &first_state);
    odt_test_counting_allocator_init(&second_allocator, &second_state);
    ODT_TEST_STATUS(odt_test_load_from_memory(&first_source, NULL, &first_allocator, &first),
                    ODT_OK);
    ODT_TEST_STATUS(odt_test_load_from_memory(&second_source, NULL, &second_allocator, &second),
                    ODT_OK);
    ODT_TEST_CHECK(first != second);
    ODT_TEST_STATUS(odt_test_encode_to_buffer(first, &first_encoded), ODT_OK);
    ODT_TEST_STATUS(odt_test_encode_to_buffer(second, &second_encoded), ODT_OK);
    ODT_TEST_CHECK(first_encoded.size == second_encoded.size);
    ODT_TEST_CHECK(memcmp(first_encoded.data, second_encoded.data, first_encoded.size) == 0);

    odt_generation_destroy(second);
    odt_generation_destroy(first);
    ODT_TEST_CHECK(first_state.live_allocations == 0u);
    ODT_TEST_CHECK(second_state.live_allocations == 0u);
    odt_test_byte_buffer_destroy(&second_encoded);
    odt_test_byte_buffer_destroy(&first_encoded);
    odt_test_byte_buffer_destroy(&frozen);
}

static void test_repeated_builds_encode_identically(void) {
    odt_generation *first = build_representative(NULL);
    odt_generation *second = build_representative(NULL);
    odt_test_byte_buffer first_encoded = {0};
    odt_test_byte_buffer second_encoded = {0};
    odt_build_stats first_stats;
    odt_build_stats second_stats;

    ODT_TEST_STATUS(odt_generation_get_build_stats(first, &first_stats), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_build_stats(second, &second_stats), ODT_OK);
    ODT_TEST_CHECK(first_stats.build_duration_ns != 0u);
    ODT_TEST_CHECK(second_stats.build_duration_ns != 0u);
    ODT_TEST_STATUS(odt_test_encode_to_buffer(first, &first_encoded), ODT_OK);
    ODT_TEST_STATUS(odt_test_encode_to_buffer(second, &second_encoded), ODT_OK);
    ODT_TEST_CHECK(first_encoded.size == second_encoded.size);
    ODT_TEST_CHECK(memcmp(first_encoded.data, second_encoded.data, first_encoded.size) == 0);

    odt_test_byte_buffer_destroy(&second_encoded);
    odt_test_byte_buffer_destroy(&first_encoded);
    odt_generation_destroy(second);
    odt_generation_destroy(first);
}

static void test_file_round_trip(void) {
    char directory[] = "/tmp/odt-persistence-XXXXXX";
    char path[512];
    odt_generation *generation = build_representative(NULL);
    odt_generation *loaded = NULL;
    int length;

    ODT_TEST_CHECK(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/generation.odt", directory);
    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < sizeof(path));
    ODT_TEST_STATUS(odt_save_file_atomic(generation, path, NULL), ODT_OK);
    ODT_TEST_STATUS(odt_load_file(path, NULL, NULL, &loaded), ODT_OK);
    odt_test_expect_metadata_equal(generation, loaded);
    ODT_TEST_CHECK(odt_test_query_digest(generation) == odt_test_query_digest(loaded));

    odt_generation_destroy(loaded);
    odt_generation_destroy(generation);
    ODT_TEST_CHECK(unlink(path) == 0);
    ODT_TEST_CHECK(rmdir(directory) == 0);
}

int main(void) {
    test_crc32c_known_vector();
    test_streaming_round_trip();
    test_streaming_failures_are_transactional();
    test_frozen_fixtures();
    test_allocator_address_independence();
    test_repeated_builds_encode_identically();
    test_file_round_trip();
    return EXIT_SUCCESS;
}
